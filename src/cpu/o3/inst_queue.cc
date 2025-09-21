/*
 * Copyright (c) 2011-2014, 2017-2020 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/o3/inst_queue.hh"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "base/logging.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/fu_pool.hh"
#include "cpu/o3/issue_queue.hh"
#include "cpu/o3/limits.hh"
#include "debug/IQ.hh"
#include "debug/Counters.hh"
#include "debug/Schedule.hh"
#include "enums/OpClass.hh"
#include "params/BaseO3CPU.hh"
#include "sim/core.hh"
#include "sim/cur_tick.hh"

// clang complains about std::set being overloaded with Packet::set if
// we open up the entire namespace std
using std::list;

namespace gem5
{

namespace o3
{

// 功能单元完成事件构造函数
// 用于在指令执行完成后进行回调
InstructionQueue::FUCompletion::FUCompletion(const DynInstPtr &_inst,
    int fu_idx, InstructionQueue *iq_ptr)
    : Event(Stat_Event_Pri, AutoDelete),
      inst(_inst), fuIdx(fu_idx), iqPtr(iq_ptr), freeFU(false)
{
}

// 处理功能单元完成事件
// 当指令在功能单元中执行完成时调用
void
InstructionQueue::FUCompletion::process()
{
    iqPtr->processFUCompletion(inst, -1);
    inst = NULL;
}


// 返回功能单元完成事件的描述
const char *
InstructionQueue::FUCompletion::description() const
{
    return "Functional unit completion";
}

// 缓存缺失load指令的哈希函数
// 用于在哈希表中快速查找缓存缺失的load指令
size_t
InstructionQueue::CacheMissLdInstsHash::operator()(const DynInstPtr& ptr) const
{
    return ptr->getLqIdx();
}

// Store-to-Load转发失败的Load指令构造函数
// 记录因为store数据未准备好而无法转发的load指令
InstructionQueue::STLFFailLdInst::STLFFailLdInst(DynInstPtr inst, InstSeqNum storeSeqNum, bool resolved)
    : inst(inst), storeSeqNum(storeSeqNum), resolved(resolved) {}

// 指令队列构造函数
// 初始化指令队列，包括调度器、内存依赖单元等
InstructionQueue::InstructionQueue(CPU *cpu_ptr, IEW *iew_ptr,
        const BaseO3CPUParams &params)
    : cpu(cpu_ptr),
      iewStage(iew_ptr),
      scheduler(params.scheduler),
      numThreads(params.numThreads),
      totalWidth(8),
      commitToIEWDelay(params.commitToIEWDelay),
      iqStats(cpu, totalWidth),
      iqIOStats(cpu)
{
    const auto &reg_classes = params.isa[0]->regClasses();
    // 设置物理寄存器总数
    // 由于向量寄存器有两种寻址模式，因此需要计算两次
    numPhysRegs = params.numPhysIntRegs + params.numPhysFloatRegs +
                    params.numPhysVecRegs +
                    params.numPhysVecRegs * (
                            reg_classes.at(VecElemClass).numRegs() /
                            reg_classes.at(VecRegClass).numRegs()) +
                    params.numPhysVecPredRegs +
                    params.numPhysCCRegs +
                    params.numPhysRMiscRegs;

    // 初始化内存依赖单元
    // 为每个线程创建独立的内存依赖跟踪单元
    for (ThreadID tid = 0; tid < MaxThreads; tid++) {
        memDepUnit[tid].init(params, tid, cpu_ptr);
        memDepUnit[tid].setIQ(this);
    }

    // 设置调度器的CPU指针和load/store队列
    scheduler->setCPU(cpu_ptr, &iew_ptr->ldstQueue);
    // 重置依赖图，分配物理寄存器数量的节点
    scheduler->resetDepGraph(numPhysRegs);
    // 设置内存依赖单元
    scheduler->setMemDepUnit(memDepUnit);

    // 重置指令队列状态
    resetState();
}

InstructionQueue::~InstructionQueue()
{
}

// 返回指令队列的名称
// 格式为CPU名称.iq
std::string
InstructionQueue::name() const
{
    return cpu->name() + ".iq";
}

InstructionQueue::IQStats::IQStats(CPU *cpu, const unsigned &total_width)
    : statistics::Group(cpu, "iq"),
    ADD_STAT(instsAdded, statistics::units::Count::get(),
             "Number of instructions added to the IQ (excludes non-spec)"),
    ADD_STAT(nonSpecInstsAdded, statistics::units::Count::get(),
             "Number of non-speculative instructions added to the IQ"),
    ADD_STAT(instsIssued, statistics::units::Count::get(),
             "Number of instructions issued"),
    ADD_STAT(intInstsIssued, statistics::units::Count::get(),
             "Number of integer instructions issued"),
    ADD_STAT(floatInstsIssued, statistics::units::Count::get(),
             "Number of float instructions issued"),
    ADD_STAT(branchInstsIssued, statistics::units::Count::get(),
             "Number of branch instructions issued"),
    ADD_STAT(memInstsIssued, statistics::units::Count::get(),
             "Number of memory instructions issued"),
    ADD_STAT(miscInstsIssued, statistics::units::Count::get(),
             "Number of miscellaneous instructions issued"),
    ADD_STAT(squashedInstsIssued, statistics::units::Count::get(),
             "Number of squashed instructions issued"),
    ADD_STAT(squashedInstsExamined, statistics::units::Count::get(),
             "Number of squashed instructions iterated over during squash; "
             "mainly for profiling"),
    ADD_STAT(squashedOperandsExamined, statistics::units::Count::get(),
             "Number of squashed operands that are examined and possibly "
             "removed from graph"),
    ADD_STAT(squashedNonSpecRemoved, statistics::units::Count::get(),
             "Number of squashed non-spec instructions that were removed"),
    ADD_STAT(numIssuedDist, statistics::units::Count::get(),
             "Number of insts issued each cycle"),
    ADD_STAT(statIssuedInstType, statistics::units::Count::get(),
             "Number of instructions issued per FU type, per thread"),
    ADD_STAT(issueRate, statistics::units::Rate<
                statistics::units::Count, statistics::units::Cycle>::get(),
             "Inst issue rate", instsIssued / cpu->baseStats.numCycles),
    ADD_STAT(deferMemInstNum, statistics::units::Count::get(),
             "Number of instructions replayed because of TLB miss"),
    ADD_STAT(fuBusy, statistics::units::Count::get(), "FU busy when requested"),
    ADD_STAT(fuBusyRate, statistics::units::Rate<
                statistics::units::Count, statistics::units::Count>::get(),
             "FU busy rate (busy events/executed inst)")
{
    instsAdded
        .prereq(instsAdded);

    nonSpecInstsAdded
        .prereq(nonSpecInstsAdded);

    instsIssued
        .prereq(instsIssued);

    intInstsIssued
        .prereq(intInstsIssued);

    floatInstsIssued
        .prereq(floatInstsIssued);

    branchInstsIssued
        .prereq(branchInstsIssued);

    memInstsIssued
        .prereq(memInstsIssued);

    miscInstsIssued
        .prereq(miscInstsIssued);

    squashedInstsIssued
        .prereq(squashedInstsIssued);

    squashedInstsExamined
        .prereq(squashedInstsExamined);

    squashedOperandsExamined
        .prereq(squashedOperandsExamined);

    squashedNonSpecRemoved
        .prereq(squashedNonSpecRemoved);
/*
    queueResDist
        .init(Num_OpClasses, 0, 99, 2)
        .name(name() + ".IQ:residence:")
        .desc("cycles from dispatch to issue")
        .flags(total | pdf | cdf )
        ;
    for (int i = 0; i < Num_OpClasses; ++i) {
        queueResDist.subname(i, opClassStrings[i]);
    }
*/
    numIssuedDist
        .init(0,total_width,1)
        .flags(statistics::pdf)
        ;
/*
    dist_unissued
        .init(Num_OpClasses+2)
        .name(name() + ".unissued_cause")
        .desc("Reason ready instruction not issued")
        .flags(pdf | dist)
        ;
    for (int i=0; i < (Num_OpClasses + 2); ++i) {
        dist_unissued.subname(i, unissued_names[i]);
    }
*/
    statIssuedInstType
        .init(cpu->numThreads,enums::Num_OpClass)
        .flags(statistics::total | statistics::pdf | statistics::dist)
        ;
    statIssuedInstType.ysubnames(enums::OpClassStrings);

    //
    //  How long did instructions for a particular FU type wait prior to issue
    //
/*
    issueDelayDist
        .init(Num_OpClasses,0,99,2)
        .name(name() + ".")
        .desc("cycles from operands ready to issue")
        .flags(pdf | cdf)
        ;
    for (int i=0; i<Num_OpClasses; ++i) {
        std::stringstream subname;
        subname << opClassStrings[i] << "_delay";
        issueDelayDist.subname(i, subname.str());
    }
*/
    issueRate
        .flags(statistics::total)
        ;

    fuBusy
        .init(cpu->numThreads)
        .flags(statistics::total)
        ;

    fuBusyRate
        .flags(statistics::total)
        ;
    fuBusyRate = fuBusy / instsIssued;
}

InstructionQueue::IQIOStats::IQIOStats(statistics::Group *parent)
    : statistics::Group(parent, "iq_io"),
    ADD_STAT(intInstQueueReads, statistics::units::Count::get(),
             "Number of integer instruction queue reads"),
    ADD_STAT(intInstQueueWrites, statistics::units::Count::get(),
             "Number of integer instruction queue writes"),
    ADD_STAT(intInstQueueWakeupAccesses, statistics::units::Count::get(),
             "Number of integer instruction queue wakeup accesses"),
    ADD_STAT(fpInstQueueReads, statistics::units::Count::get(),
             "Number of floating instruction queue reads"),
    ADD_STAT(fpInstQueueWrites, statistics::units::Count::get(),
             "Number of floating instruction queue writes"),
    ADD_STAT(fpInstQueueWakeupAccesses, statistics::units::Count::get(),
             "Number of floating instruction queue wakeup accesses"),
    ADD_STAT(vecInstQueueReads, statistics::units::Count::get(),
             "Number of vector instruction queue reads"),
    ADD_STAT(vecInstQueueWrites, statistics::units::Count::get(),
             "Number of vector instruction queue writes"),
    ADD_STAT(vecInstQueueWakeupAccesses, statistics::units::Count::get(),
             "Number of vector instruction queue wakeup accesses"),
    ADD_STAT(intAluAccesses, statistics::units::Count::get(),
             "Number of integer alu accesses"),
    ADD_STAT(fpAluAccesses, statistics::units::Count::get(),
             "Number of floating point alu accesses"),
    ADD_STAT(vecAluAccesses, statistics::units::Count::get(),
             "Number of vector alu accesses")
{
    using namespace statistics;
    intInstQueueReads
        .flags(total);

    intInstQueueWrites
        .flags(total);

    intInstQueueWakeupAccesses
        .flags(total);

    fpInstQueueReads
        .flags(total);

    fpInstQueueWrites
        .flags(total);

    fpInstQueueWakeupAccesses
        .flags(total);

    vecInstQueueReads
        .flags(total);

    vecInstQueueWrites
        .flags(total);

    vecInstQueueWakeupAccesses
        .flags(total);

    intAluAccesses
        .flags(total);

    fpAluAccesses
        .flags(total);

    vecAluAccesses
        .flags(total);
}

// 重置指令队列状态
// 清空所有待处理指令队列和状态计数器
void
InstructionQueue::resetState()
{
    // 重置每个线程的撤销序列号
    for (ThreadID tid = 0; tid < MaxThreads; ++tid) {
        squashedSeqNum[tid] = 0;
    }

    // 清空各种类型的指令队列
    nonSpecInsts.clear();        // 非推测执行指令
    deferredMemInsts.clear();    // 延迟的内存指令
    cacheMissLdInsts.clear();    // 缓存缺失的load指令
    stlfFailLdInsts.clear();     // Store-to-Load转发失败的指令
    blockedMemInsts.clear();     // 被阻塞的内存指令
    retryMemInsts.clear();       // 需要重试的内存指令
    wbOutstanding = 0;           // 未完成的写回操作数量
}

// 设置活跃线程列表
// 用于多线程调度时确定哪些线程是活跃的
void
InstructionQueue::setActiveThreads(list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

// 设置发射到执行阶段的时间缓冲区
// 用于在发射和执行阶段之间传递信息
void
InstructionQueue::setIssueToExecuteQueue(TimeBuffer<IssueStruct> *i2e_ptr)
{
      issueToExecuteQueue = i2e_ptr;
}

// 设置指令调度器
// 调度器负责指令的选择和发射
void
InstructionQueue::setScheduler(Scheduler* scheduler)
{
    this->scheduler = scheduler;
}

// 设置时间缓冲区
// 用于在流水线阶段之间传递信息
void
InstructionQueue::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // 获取从提交阶段到IEW阶段的信息传递线路
    fromCommit = timeBuffer->getWire(-commitToIEWDelay);
}

// 检查指令队列是否已排空
// 用于系统shutdown或切换时确保没有待处理指令
bool
InstructionQueue::isDrained() const
{
    // 检查调度器、执行队列和写回队列是否都为空
    bool drained = scheduler->isDrained() &&
                   instsToExecute.empty() &&
                   wbOutstanding == 0;
    // 检查所有线程的内存依赖单元是否都已排空
    for (ThreadID tid = 0; tid < numThreads; ++tid)
        drained = drained && memDepUnit[tid].isDrained();

    return drained;
}

// 执行排空完整性检查
// 确保所有队列在排空后确实为空
void
InstructionQueue::drainSanityCheck() const
{
    assert(instsToExecute.empty());
    for (ThreadID tid = 0; tid < numThreads; ++tid)
        memDepUnit[tid].drainSanityCheck();
}

// 从另一个指令队列接管
// 用于系统切换或重启时的状态转移
void
InstructionQueue::takeOverFrom()
{
    resetState();
}

// 检查是否有准备好的指令可以发射
// 返回true表示有指令可以被调度执行
bool
InstructionQueue::hasReadyInsts()
{
    return scheduler->hasReadyInsts();
}

// 将新指令插入到指令队列
// disp_seq是分发序列号，用于保持指令的序列性
void
InstructionQueue::insert(const DynInstPtr &new_inst, int disp_seq)
{
    // 根据指令类型更新统计信息
    if (new_inst->isFloating()) {
        iqIOStats.fpInstQueueWrites++;    // 浮点指令队列写入
    } else if (new_inst->isVector()) {
        iqIOStats.vecInstQueueWrites++;   // 向量指令队列写入
    } else {
        iqIOStats.intInstQueueWrites++;   // 整数指令队列写入
    }
    // 确保指令指针有效
    assert(new_inst);

    DPRINTF(IQ, "Adding instruction [sn:%llu] PC %s to the IQ.\n",
            new_inst->seqNum, new_inst->pcState());

    // 将指令插入调度器
    scheduler->insert(new_inst, disp_seq);

    // 更新已添加指令数的统计
    ++iqStats.instsAdded;
}

// 插入非推测执行指令
// 这类指令必须等到提交阶段确认后才能执行（如fence、syscall等）
void
InstructionQueue::insertNonSpec(const DynInstPtr &new_inst)
{
    // @todo: 清理此代码；可以通过设置指令为不能发射状态，然后调用正常插入函数
    if (new_inst->isFloating()) {
        iqIOStats.fpInstQueueWrites++;    // 浮点指令队列写入
    } else if (new_inst->isVector()) {
        iqIOStats.vecInstQueueWrites++;   // 向量指令队列写入
    } else {
        iqIOStats.intInstQueueWrites++;   // 整数指令队列写入
    }

    assert(new_inst);

    // 将非推测指令插入调度器
    scheduler->insertNonSpec(new_inst);
    // 将非推测指令加入映射表，等待提交阶段确认
    nonSpecInsts[new_inst->seqNum] = new_inst;

    DPRINTF(Schedule, "nonSpecInsts size: %lu\n", nonSpecInsts.size());

    DPRINTF(IQ, "Adding non-speculative instruction [sn:%llu] PC %s "
            "to the IQ.\n",
            new_inst->seqNum, new_inst->pcState());

    // instList[new_inst->threadNumber].push_back(new_inst);

    // 更新非推测指令数量统计
    ++iqStats.nonSpecInstsAdded;
}

// 插入内存屏障指令
// 屏障指令需要阻止其他内存指令的无序执行
void
InstructionQueue::insertBarrier(const DynInstPtr &barr_inst)
{
    // 在内存依赖单元中插入屏障指令
    memDepUnit[barr_inst->threadNumber].insertBarrier(barr_inst);

    // 将屏障指令作为非推测指令插入
    insertNonSpec(barr_inst);
}

// 获取下一个要执行的指令
// 从执行队列中取出一个即将执行的指令
DynInstPtr
InstructionQueue::getInstToExecute()
{
    assert(!instsToExecute.empty());
    DynInstPtr inst = std::move(instsToExecute.front());
    instsToExecute.pop_front();
    // 根据指令类型更新读取统计
    if (inst->isFloating()) {
        iqIOStats.fpInstQueueReads++;     // 浮点指令队列读取
    } else if (inst->isVector()) {
        iqIOStats.vecInstQueueReads++;    // 向量指令队列读取
    } else {
        iqIOStats.intInstQueueReads++;    // 整数指令队列读取
    }
    return inst;
}

// 处理功能单元完成事件
// 当指令在功能单元中执行完成后调用
void
InstructionQueue::processFUCompletion(const DynInstPtr &inst, int fu_idx)
{
    DPRINTF(IQ, "Processing FU completion [sn:%llu]\n", inst->seqNum);
    assert(!cpu->switchedOut());
    // CPU可能在等待这个操作完成时进入睡眠状态（极长延迟操作）。
    // 如果需要的话，唤醒CPU。这可能是过度的。
   --wbOutstanding;
    iewStage->wakeCPU();

    // @todo: 确保这些FU完成事件在周期开始时发生，
    // 否则它们可能会在队列中添加太多指令。
    issueToExecuteQueue->access(0)->size++;
    instsToExecute.push_back(inst);
}

// 执行延迟检查
// 根据操作数的值动态计算指令的执行延迟
bool
InstructionQueue::execLatencyCheck(const DynInstPtr& inst, uint32_t& op_latency)
{
    // 前导零位计数函数
    auto clz = [](RegVal val) {
#if defined(__GNUC__) || defined(__clang__)
        return val == 0 ? 64 : __builtin_clzll(val);
#else
        for (int i = 0; i < 64; i++) {
            if (val & (0x1lu << 63)) {
                return i;
            }
            val <<= 1;
        }
        return 64;
#endif
    };


    RegVal rs1;
    RegVal rs2;
    int delay_;
    switch (inst->opClass()) {
        case OpClass::IntDiv:  // 整数除法指令
            // 读取源寄存器的值
            rs1 = cpu->readArchIntReg(inst->srcRegIdx(0).index(),
                                      inst->threadNumber);
            rs2 = cpu->readArchIntReg(inst->srcRegIdx(1).index(),
                                      inst->threadNumber);
            // rs1 / rs2 : 0x80/0x8 ,delay_ = 4
            // 计算rs1和rs2之间的前导零位差值 (rs1 > rs2)
            delay_ = std::max(clz(rs2) - clz(rs1), 0);
            if (rs2 == 1) {
                // rs1 / 1 = rs1，优化情况
                op_latency = 5;
            } else if (rs1 == rs2) {
                // rs1 / rs2 = 1 rem 0，等值除法
                op_latency = 7;
            } else if (clz(rs2) - clz(rs1) < 0) {
                // 如果rs2 > rs1那么rs1/rs2 = 0 rem rs1
                op_latency = 5;
            } else {
                // 基本延迟 + 动态延迟
                // 动态延迟由delay_决定
                op_latency = 7 + delay_ / 4;
            }
            return true;
        case OpClass::FloatSqrt:  // 浮点平方根指令
            rs1 = cpu->readArchFloatReg(inst->srcRegIdx(0).index(),
                                        inst->threadNumber);
            rs2 = cpu->readArchFloatReg(inst->srcRegIdx(1).index(),
                                        inst->threadNumber);
            // 对于特殊值，fsqrt/fdiv可以提早结束
            // 如nan、inf或rs1 == rs2
            switch (inst->staticInst->operWid()) {
                case 32:  // 32位浮点数
                    if (__isnanf(*((float*)(&rs1))) ||
                        __isnanf(*((float*)(&rs2))) ||
                        __isinff(*((float*)(&rs1))) ||
                        __isinff(*((float*)(&rs2)))) {
                        op_latency = 2;  // 特殊值的快速处理
                        break;
                    }
                    op_latency = 8;  // 正常情况下的32位平方根延迟
                    break;
                case 64:  // 64位浮点数
                    if (__isnan(*((double*)(&rs1))) ||
                        __isnan(*((double*)(&rs2))) ||
                        __isinf(*((double*)(&rs1))) ||
                        __isinf(*((double*)(&rs2)))) {
                        op_latency = 2;  // 特殊值的快速处理
                        break;
                    }
                    op_latency = 15; // 正常情况下的64位平方根延迟
                    break;
                default:
                    panic("Unsupported float width\n");
                    return false;
            }
            return true;
        case OpClass::FloatDiv:   // 浮点除法指令
            rs1 = cpu->readArchFloatReg(inst->srcRegIdx(0).index(),
                                        inst->threadNumber);
            rs2 = cpu->readArchFloatReg(inst->srcRegIdx(1).index(),
                                        inst->threadNumber);
            switch (inst->staticInst->operWid()) {
                case 32:  // 32位浮点数
                    if (__isnanf(*((float*)(&rs1))) ||
                        __isnanf(*((float*)(&rs2))) ||
                        __isinff(*((float*)(&rs1))) ||
                        __isinff(*((float*)(&rs2)))) {
                        op_latency = 2;  // 特殊值的快速处理
                        break;
                    }
                    op_latency = 7;  // 正常情况下的32位除法延迟
                    break;
                case 64:  // 64位浮点数
                    if (__isnan(*((double*)(&rs1))) ||
                        __isnan(*((double*)(&rs2))) ||
                        __isinf(*((double*)(&rs1))) ||
                        __isinf(*((double*)(&rs2)))) {
                        op_latency = 2;  // 特殊值的快速处理
                        break;
                    }
                    op_latency = 12; // 正常情况下的64位除法延迟
                    break;
                default:
                    panic("Unsupported float width\n");
                    return false;
            }
            return true;
        default:
            return false;  // 不支持的指令类型
    }
    return false;
}

// @todo: 找到一个更好的方法来从列表中删除被撤销的项目。
// 检查每个列表的顶部项目是否被撤销会浪费时间并强制跳转。
// 调度准备好的指令
// 这是指令队列的核心函数，负责选择和发射准备好的指令
void
InstructionQueue::scheduleReadyInsts()
{
    DPRINTF(IQ, "Attempting to schedule ready instructions from "
            "the IQ.\n");

    IssueStruct *i2e_info = issueToExecuteQueue->access(0);

    DynInstPtr mem_inst;
    // 处理缓存缺失的load指令，尝试重新执行
    while ((mem_inst = getCacheMissInstToExecute())) {
        mem_inst->issueQue->retryMem(mem_inst);
    }

    // 处理延迟的内存指令，尝试重新执行
    while ((mem_inst = getDeferredMemInstToExecute())) {
        mem_inst->issueQue->retryMem(mem_inst);
    }

    // 处理Store-to-Load转发失败的指令，尝试重新执行
    while ((mem_inst = getSTLFFailInstToExecute())) {
        mem_inst->issueQue->retryMem(mem_inst);
    }

    // 检查是否有被缓存阻塞的指令可以执行
    while ((mem_inst = getBlockedMemInstToExecute())) {
        mem_inst->issueQue->retryMem(mem_inst);
    }

    // 有一个指向列表头部的迭代器
    // 只要没有超过带宽或达到列表末尾，
    // 尝试获取一个能够执行这个操作的功能单元。
    // 如果成功，将oldestInst更改为列表的新顶部，将队列放在列表的适当位置。
    // 递增迭代器。
    // 这将避免在没有处理它的FU时尝试调度某个操作类。
    int total_issued = 0;        // 总共发射的指令数
    DynInstPtr issued_inst;
    while ((issued_inst = scheduler->getInstToFU())) {

        // 根据指令类型更新读取统计
        if (issued_inst->isFloating()) {
            iqIOStats.fpInstQueueReads++;
        } else if (issued_inst->isVector()) {
            iqIOStats.vecInstQueueReads++;
        } else {
            iqIOStats.intInstQueueReads++;
        }

        // 如果指令已被撤销，跳过此指令
        if (issued_inst->isSquashed()) {
            ++iqStats.squashedInstsIssued;
            continue;
        }

        // 根据指令类型更新ALU访问统计
        if (issued_inst->isFloating()) {
            iqIOStats.fpAluAccesses++;    // 浮点ALU访问
        } else if (issued_inst->isVector()) {
            iqIOStats.vecAluAccesses++;   // 向量ALU访问
        } else {
            iqIOStats.intAluAccesses++;   // 整数ALU访问
        }

        // 获取操作延迟并进行动态检查
        uint32_t op_latency = scheduler->getOpLatency(issued_inst);
        execLatencyCheck(issued_inst, op_latency);
        assert(op_latency < 64);
        DPRINTF(Schedule, "[sn:%llu] start execute %u cycles\n", issued_inst->seqNum, op_latency);
        // 更新性能计数器，记录指令在功能单元的位置
        cpu->perfCCT->updateInstPos(issued_inst->seqNum, PerfRecord::AtFU);
        // 如果延迟小于等于1周期，或者是load/store指令，直接加入执行队列
        if (op_latency <= 1 || issued_inst->isLoad() || issued_inst->isStore()) {
            i2e_info->size++;
            instsToExecute.push_back(issued_inst);
        }
        else {
            // 否则创建一个延迟执行的事件
            ++wbOutstanding;
            FUCompletion *execution = new FUCompletion(issued_inst, 0, this);
            cpu->schedule(execution, cpu->clockEdge(Cycles(op_latency - 1))-1);
        }
        ++total_issued;
        // 如果这是指令的第一次发射，记录时间戳
        if (issued_inst->firstIssue == -1) {
            issued_inst->firstIssue = curTick();
        }

        // 更新按线程和操作类型分类的发射统计
        iqStats.statIssuedInstType[issued_inst->threadNumber][issued_inst->opClass()]++;
    }

    // 更新发射数量统计
    iqStats.numIssuedDist.sample(total_issued);
    iqStats.instsIssued+= total_issued;

    // 如果发射了任何指令，告诉CPU我们有活动。
    // @todo 如果延迟内存指令的处理方式因翻译变化而改变，
    // 那么应该从下面的代码中删除deferredMemInsts条件。
    if (total_issued || !retryMemInsts.empty() || !deferredMemInsts.empty() ||
       !cacheMissLdInsts.empty() || !stlfFailLdInsts.empty()) {
        cpu->activityThisCycle();   // 通知CPU此周期有活动
    } else {
        DPRINTF(IQ, "Not able to schedule any instructions.\n");
    }
}

// 通知指令已执行
// 在指令执行完成后调用，用于更新内存依赖图
void
InstructionQueue::notifyExecuted(const DynInstPtr &inst)
{
    // 通知内存依赖单元指令已发射
    memDepUnit[inst->threadNumber].issue(inst);
}

// 调度非推测指令执行
// 当提交阶段确认非推测指令可以执行时调用
void
InstructionQueue::scheduleNonSpec(const InstSeqNum &inst)
{
    DPRINTF(IQ, "Marking nonspeculative instruction [sn:%llu] as ready "
            "to execute.\n", inst);

    // 在非推测指令映射中查找指令
    NonSpecMapIt inst_it = nonSpecInsts.find(inst);

    assert(inst_it != nonSpecInsts.end());

    ThreadID tid = (*inst_it).second->threadNumber;

    // 设置指令状态为在提交阶段和可发射
    (*inst_it).second->setAtCommit();
    (*inst_it).second->setCanIssue();

    // 将指令添加到功能单元调度器
    scheduler->addToFU((*inst_it).second);

    // 清空指针并从映射中删除
    (*inst_it).second = NULL;
    nonSpecInsts.erase(inst_it);
}

// 提交指令
// 通知指令队列指令已提交，可以清理相关资源
void
InstructionQueue::commit(const InstSeqNum &inst, ThreadID tid)
{
    DPRINTF(IQ, "[tid:%i] Committing instructions older than [sn:%llu]\n",
            tid,inst);
    // 通知调度器指令已提交
    scheduler->doCommit(inst);
}

// 唤醒依赖指令
// 当一个指令完成后，唤醒所有依赖于它的指令
int
InstructionQueue::wakeDependents(const DynInstPtr &completed_inst)
{
    int dependents = 0;

    // 指令队列这里处理浮点和整数操作
    if (completed_inst->isFloating()) {
        iqIOStats.fpInstQueueWakeupAccesses++;   // 浮点指令队列唤醒访问
    } else if (completed_inst->isVector()) {
        iqIOStats.vecInstQueueWakeupAccesses++;  // 向量指令队列唤醒访问
    } else {
        iqIOStats.intInstQueueWakeupAccesses++;  // 整数指令队列唤醒访问
    }

    // 记录最后一次唤醒依赖的时间
    completed_inst->lastWakeDependents = curTick();

    DPRINTF(IQ, "Waking dependents of completed instruction.\n");

    // 确保指令未被撤销
    assert(!completed_inst->isSquashed());

    // 如果是内存指令，告诉内存依赖单元唤醒依赖于此指令的指令。
    // 同时在此时完成内存指令，因为我们知道它没有问题地执行了。
    ThreadID tid = completed_inst->threadNumber;
    if (completed_inst->isMemRef()) {
        // 完成内存指令
        memDepUnit[tid].completeInst(completed_inst);

        DPRINTF(IQ, "Completing mem instruction PC: %s [sn:%llu]\n",
            completed_inst->pcState(), completed_inst->seqNum);

        // 标记内存操作完成
        completed_inst->memOpDone(true);
    } else if (completed_inst->isReadBarrier() ||
               completed_inst->isWriteBarrier()) {
        // 完成非内存引用的屏障指令
        memDepUnit[tid].completeInst(completed_inst);
    }

    // 遍历所有目标寄存器，唤醒依赖指令
    for (int dest_reg_idx = 0;
         dest_reg_idx < completed_inst->numDestRegs();
         dest_reg_idx++)
    {
        PhysRegIdPtr dest_reg =
            completed_inst->renamedDestIdx(dest_reg_idx);

        // 特殊情况：uniq或控制寄存器。它们不由IQ处理，因此没有依赖图条目。
        if (dest_reg->isFixedMapping()) {
            DPRINTF(IQ, "Reg %d [%s] is part of a fix mapping, skipping\n",
                    dest_reg->index(), dest_reg->className());
            continue;
        }

        // 如果寄存器被钉住，避免唤醒依赖
        dest_reg->decrNumPinnedWritesToComplete();
        if (dest_reg->isPinned())
            completed_inst->setPinnedRegsWritten();

        // 如果寄存器仍有未完成的钉住写入，跳过
        if (dest_reg->getNumPinnedWritesToComplete() != 0) {
            DPRINTF(IQ, "Reg %d [%s] is pinned, skipping\n",
                    dest_reg->index(), dest_reg->className());
            continue;
        }

        // 调试输出：唤醒依赖于此寄存器的指令
        DPRINTF(IQ, "Waking any dependents on register %i (%s).\n",
                dest_reg->index(),
                dest_reg->className());
    }

    return dependents;
}

// 重新调度内存指令
// 当内存指令因为某些原因需要重新执行时调用
void
InstructionQueue::rescheduleMemInst(const DynInstPtr &resched_inst)
{
    DPRINTF(IQ, "Rescheduling mem inst [sn:%llu]\n", resched_inst->seqNum);

    // 重置DTB翻译状态
    resched_inst->translationStarted(false);
    resched_inst->translationCompleted(false);

    // 清除可发射标志并重新调度
    resched_inst->clearCanIssue();
    memDepUnit[resched_inst->threadNumber].reschedule(resched_inst);
}

// 重放内存指令
// 重放由于地址翻译或其他原因被延迟的内存指令
void
InstructionQueue::replayMemInst(const DynInstPtr &replay_inst)
{
    memDepUnit[replay_inst->threadNumber].replay();
}

// 延迟内存指令
// 将内存指令加入延迟队列，等待后续处理
void
InstructionQueue::deferMemInst(const DynInstPtr &deferred_inst)
{
    deferredMemInsts.push_back(deferred_inst);
    iqStats.deferMemInstNum++;  // 更新延迟内存指令数量统计
}

// 缓存缺失的Load指令重放
// 当Load指令因缓存缺失而需要重新执行时调用
void
InstructionQueue::cacheMissLdReplay(const DynInstPtr &deferred_inst)
{
    DPRINTF(IQ, "Get Cache Missed Load, insert to Replay Queue "
            "[sn:%llu]\n", deferred_inst->seqNum);
    // 将缓存缺失的Load指令插入重放队列
    cacheMissLdInsts.insert(deferred_inst);
}

// Store-to-Load转发失败的Load指令重放
// 当Load指令因Store数据未准备好而无法转发时调用
void
InstructionQueue::stlfFailLdReplay(const DynInstPtr &deferred_inst, const InstSeqNum &store_seq_num)
{
    DPRINTF(IQ, "Load[sn:%llu] unable to forward from data unready store[sn:%llu],"
            " insert to Replay Queue\n", deferred_inst->seqNum, store_seq_num);
    // 将STLF失败的Load指令加入重放队列，记录对应的Store序列号
    stlfFailLdInsts.emplace_back(deferred_inst, store_seq_num, false);
}

// 阻塞内存指令
// 当内存指令因缓存系统等原因被阻塞时调用
void
InstructionQueue::blockMemInst(const DynInstPtr &blocked_inst)
{
    // 清除可发射标志
    blocked_inst->clearCanIssue();
    // 将被阻塞的指令加入阻塞队列
    blockedMemInsts.push_back(blocked_inst);
    DPRINTF(IQ, "Memory inst [sn:%llu] PC %s is blocked, will be "
            "reissued later\n", blocked_inst->seqNum,
            blocked_inst->pcState());
}

// 缓存解除阻塞
// 当缓存系统不再阻塞时，重新调度被阻塞的内存指令
void
InstructionQueue::cacheUnblocked()
{
    DPRINTF(IQ, "Cache is unblocked, rescheduling blocked memory "
            "instructions\n");
    // 将被阻塞的指令移动到重试队列
    retryMemInsts.splice(retryMemInsts.end(), blockedMemInsts);
    // 让CPU重新开始运行
    cpu->wakeCPU();
}

// 获取可执行的延迟内存指令
// 遍历延迟队列，查找翻译完成或被撤销的指令
DynInstPtr
InstructionQueue::getDeferredMemInstToExecute()
{
    for (ListIt it = deferredMemInsts.begin(); it != deferredMemInsts.end();
         ++it) {
        // 如果翻译完成或被撤销，返回此指令
        if ((*it)->translationCompleted() || (*it)->isSquashed()) {
            DPRINTF(IQ, "Deferred mem inst [sn:%llu] PC %s is ready to "
                    "execute\n", (*it)->seqNum, (*it)->pcState());
            DynInstPtr mem_inst = std::move(*it);
            deferredMemInsts.erase(it);
            return mem_inst;
        }
        if (!(*it)->translationCompleted()) {
            DPRINTF(
                IQ,
                "Deferred mem inst [sn:%llu] PC %s has not been translated\n",
                (*it)->seqNum, (*it)->pcState());
        }
    }
    return nullptr;  // 没有找到可执行的指令
}

// 获取可执行的缓存缺失Load指令
// 遍历缓存缺失队列，查找不再等待缓存重填或被撤销的指令
DynInstPtr
InstructionQueue::getCacheMissInstToExecute()
{
    for (auto it = cacheMissLdInsts.begin(); it != cacheMissLdInsts.end();
         ++it) {
        // 如果不再等待缓存重填或被撤销，返回此指令
        if (!(*it)->waitingCacheRefill() || (*it)->isSquashed()) {
            DPRINTF(IQ, "CacheMissed load inst [sn:%llu] PC %s is ready to "
                    "execute\n", (*it)->seqNum, (*it)->pcState());
            DynInstPtr mem_inst = *it;
            cacheMissLdInsts.erase(it);
            return mem_inst;
        }
        if ((*it)->waitingCacheRefill()) {
            DPRINTF(
                IQ,
                "CacheMissed load inst [sn:%llu] PC %s has not been waken up "
                "by Dcache\n",
                (*it)->seqNum, (*it)->pcState());
        }
    }
    return nullptr;  // 没有找到可执行的指令
}

// 获取可执行的STLF失败Load指令
// 遍历STLF失败队列，查找已解决或被撤销的指令
DynInstPtr
InstructionQueue::getSTLFFailInstToExecute()
{
    for (auto it = stlfFailLdInsts.begin(); it != stlfFailLdInsts.end();
         ++it) {
        // 如果问题已解决或被撤销，返回此指令
        if ((*it).resolved || (*it).inst->isSquashed()) {
            DPRINTF(IQ, "STDFwdFailed load inst [sn:%llu] PC %s is ready to "
                    "execute\n", (*it).inst->seqNum, (*it).inst->pcState());
            DynInstPtr mem_inst = std::move((*it).inst);
            stlfFailLdInsts.erase(it);
            return mem_inst;
        }
        if (!(*it).resolved) {
            DPRINTF(
                IQ,
                "inst [sn:%llu] PC %s is waiting for store [sn:%llu] to be ready\n",
                (*it).inst->seqNum, (*it).inst->pcState(), (*it).storeSeqNum);
        }
    }
    return nullptr;  // 没有找到可执行的指令
}

// 解决STLF失败问题
// 当Store指令准备好时，标记所有等待此Store的Load指令为已解决
void
InstructionQueue::resolveSTLFFailInst(const InstSeqNum &store_seq_num)
{
    for (auto it = stlfFailLdInsts.begin(); it != stlfFailLdInsts.end();
         ++it) {
        // 查找等待此Store的Load指令并标记为已解决
        if ((*it).storeSeqNum == store_seq_num) {
            (*it).resolved = true;
        }
    }
}

// 获取可执行的被阻塞内存指令
// 从重试队列中取出一个指令
DynInstPtr
InstructionQueue::getBlockedMemInstToExecute()
{
    if (retryMemInsts.empty()) {
        return nullptr;  // 重试队列为空
    } else {
        // 从队列头部取出一个指令
        DynInstPtr mem_inst = std::move(retryMemInsts.front());
        retryMemInsts.pop_front();
        return mem_inst;
    }
}

// 处理内存违例
// 当检测到Load/Store之间的内存违例时调用
void
InstructionQueue::violation(const DynInstPtr &store,
        const DynInstPtr &faulting_load)
{
    iqIOStats.intInstQueueWrites++;  // 更新整数指令队列写入统计
    // 通知内存依赖单元处理违例
    memDepUnit[store->threadNumber].violation(store, faulting_load);
}

// 撤销指令
// 当发生分支预测错误或其他异常时，撤销指定线程的指令
void
InstructionQueue::squash(ThreadID tid)
{
    DPRINTF(IQ, "[tid:%i] Starting to squash instructions in "
            "the IQ.\n", tid);

    // 从时间缓冲区中读取最后一条指令的序列号
    squashedSeqNum[tid] = fromCommit->commitInfo[tid].doneSeqNum;

    // 执行具体的撤销操作
    doSquash(tid);

    // 同时通知内存依赖单元进行撤销
    memDepUnit[tid].squash(squashedSeqNum[tid], tid);
}

// 执行撤销操作
// 具体实现撤销逻辑，清理相关数据结构
void
InstructionQueue::doSquash(ThreadID tid)
{
    // // 从尾部开始。
    // ListIt squash_it = instList[tid].end();
    // --squash_it;

    DPRINTF(IQ, "[tid:%i] Squashing until sequence number %i!\n",
            tid, squashedSeqNum[tid]);
    // 通知调度器进行撤销操作
    scheduler->doSquash(squashedSeqNum[tid]);

    // 遍历非推测指令映射，撤销需要撤销的指令
    for (auto it = nonSpecInsts.begin(); it != nonSpecInsts.end();) {
        // 如果指令的序列号大于撤销序列号，需要被撤销
        if (it->first > squashedSeqNum[tid]) {
            auto& squashed_inst = it->second;
            // 如果指令未发射或是未完成的内存指令
            if (!squashed_inst->isIssued() ||
                (squashed_inst->isMemRef() &&
                !squashed_inst->memOpDone())) {

                // 检查是否为获取-释放(acquire-release)屏障指令
                bool is_acq_rel = squashed_inst->isFullMemBarrier() &&
                            (squashed_inst->isLoad() ||
                            (squashed_inst->isStore() &&
                                !squashed_inst->isStoreConditional()));

                // 从依赖列表中删除指令
                if (is_acq_rel ||
                    (!squashed_inst->isNonSpeculative() &&
                    !squashed_inst->isStoreConditional() &&
                    !squashed_inst->isAtomic() &&
                    !squashed_inst->isReadBarrier() &&
                    !squashed_inst->isWriteBarrier())) {
                    it++;  // 特殊指令，不删除
                } else if (!squashed_inst->isStoreConditional() ||
                        !squashed_inst->isCompleted()) {

                    // 清空指针并从映射中删除
                    (*it).second = NULL;
                    it = nonSpecInsts.erase(it);

                    // 更新被撤销的非推测指令数量统计
                    ++iqStats.squashedNonSpecRemoved;
                }
                else {
                    it++;  // 其他情况，继续遍历
                }
            }
            else {
                it++;  // 指令已发射且完成，继续遍历
            }
        }
        else {
            it++;  // 指令序列号小于撤销序列号，不需要撤销
        }
    }
}

} // namespace o3
} // namespace gem5

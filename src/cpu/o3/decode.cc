/*
 * Copyright (c) 2012, 2014 ARM Limited
 * All rights reserved
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
#include <queue>

#include "cpu/o3/decode.hh"

/**
 * @file decode.cc
 *
 * O3（乱序执行）CPU流水线中译码阶段的实现。
 * 译码阶段的主要职责包括：
 * - 从取指阶段接收指令
 * - 验证取指阶段做出的分支预测
 * - 管理流水线停顿和清空
 * - 将译码后的指令转发给重命名阶段
 * - 处理流水线控制信号（阻塞/解阻塞/清空）
 *
 * 该阶段位于取指和重命名之间，通过早期检测分支预测错误
 * 和管理指令流控制来维持正确的执行流。
 */

#include "arch/generic/pcstate.hh"
#include "base/trace.hh"
#include "config/the_isa.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "debug/Activity.hh"
#include "debug/Decode.hh"
#include "debug/DecoupleBP.hh"
#include "debug/O3PipeView.hh"
#include "debug/Counters.hh"
#include "params/BaseO3CPU.hh"
#include "sim/full_system.hh"

// clang complains about std::set being overloaded with Packet::set if
// we open up the entire namespace std
using std::list;

namespace gem5
{

namespace o3
{

/**
 * 译码阶段构造函数
 * 初始化译码阶段的各种参数和状态
 *
 * @param _cpu 指向CPU对象的指针
 * @param params O3 CPU的参数对象，包含各种配置参数
 */
Decode::Decode(CPU *_cpu, const BaseO3CPUParams &params)
    : cpu(_cpu),
      renameToDecodeDelay(params.renameToDecodeDelay),    // 重命名到译码的延迟
      iewToDecodeDelay(params.iewToDecodeDelay),          // IEW到译码的延迟
      commitToDecodeDelay(params.commitToDecodeDelay),    // 提交到译码的延迟
      fetchToDecodeDelay(params.fetchToDecodeDelay),      // 取指到译码的延迟
      decodeWidth(params.decodeWidth),                    // 译码宽度（每周期可处理的指令数）
      numThreads(params.numThreads),                     // 线程数量
      stats(_cpu)                                         // 统计信息对象
{
    // 检查译码宽度是否超过编译时限制
    if (decodeWidth > MaxWidth)
        fatal("decodeWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             decodeWidth, static_cast<int>(MaxWidth));

    // 计算缓冲区最大大小（基于取指到译码的延迟和取指宽度）
    // TODO: 将此设为参数
    skidBufferMax = (fetchToDecodeDelay + 1) *  params.fetchWidth;

    // 初始化每个线程的状态
    for (int tid = 0; tid < MaxThreads; tid++) {
        stalls[tid] = {false};                    // 停顿状态
        decodeStatus[tid] = Idle;                 // 译码状态设为空闲
        bdelayDoneSeqNum[tid] = 0;               // 分支延迟完成序列号
        squashInst[tid] = nullptr;               // 清空指令指针
        squashAfterDelaySlot[tid] = 0;           // 延迟槽后清空
    }

    // 初始化译码停顿状态数组
    decodeStalls.resize(decodeWidth, StallReason::NoStall);
}

/**
 * 启动译码阶段
 * 在仿真开始时调用，重置译码阶段到初始状态
 */
void
Decode::startupStage()
{
    resetStage();
}

/**
 * 清空特定线程的状态
 * 将指定线程的译码状态重置为空闲，清空停顿信号
 *
 * @param tid 线程ID
 */
void
Decode::clearStates(ThreadID tid)
{
    decodeStatus[tid] = Idle;        // 设置为空闲状态
    stalls[tid].rename = false;      // 清空重命名停顿信号
}

/**
 * 重置译码阶段
 * 将整个译码阶段重置到初始状态，清空所有线程的状态
 */
void
Decode::resetStage()
{
    _status = Inactive;  // 设置阶段为非活动状态

    // 设置状态，确保停顿信号被清空
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        decodeStatus[tid] = Idle;        // 设置为空闲状态
        stalls[tid].rename = false;      // 清空重命名停顿信号
    }
}

/**
 * 获取译码阶段的名称
 * 返回由CPU名称和".decode"组成的字符串
 *
 * @return 译码阶段的完整名称
 */
std::string
Decode::name() const
{
    return cpu->name() + ".decode";
}

Decode::DecodeStats::DecodeStats(CPU *cpu)
    : statistics::Group(cpu, "decode"),
      ADD_STAT(idleCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is idle"),
      ADD_STAT(blockedCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is blocked"),
      ADD_STAT(runCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is running"),
      ADD_STAT(unblockCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is unblocking"),
      ADD_STAT(squashCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is squashing"),
      ADD_STAT(branchResolved, statistics::units::Count::get(),
               "Number of times decode resolved a branch"),
      ADD_STAT(branchMispred, statistics::units::Count::get(),
               "Number of times decode detected a branch misprediction"),
      ADD_STAT(controlMispred, statistics::units::Count::get(),
               "Number of times decode detected an instruction incorrectly "
               "predicted as a control"),
      ADD_STAT(decodedInsts, statistics::units::Count::get(),
               "Number of instructions handled by decode"),
      ADD_STAT(squashedInsts, statistics::units::Count::get(),
               "Number of squashed instructions handled by decode"),
      ADD_STAT(mispredictedByPC, statistics::units::Count::get(),
               "Number of instructions that mispredicted due to pc"),
      ADD_STAT(mispredictedByNPC, statistics::units::Count::get(),
               "Number of instructions that mispredicted due to npc")
{
    idleCycles.prereq(idleCycles);
    blockedCycles.prereq(blockedCycles);
    runCycles.prereq(runCycles);
    unblockCycles.prereq(unblockCycles);
    squashCycles.prereq(squashCycles);
    branchResolved.prereq(branchResolved);
    branchMispred.prereq(branchMispred);
    controlMispred.prereq(controlMispred);
    decodedInsts.prereq(decodedInsts);
    squashedInsts.prereq(squashedInsts);
    mispredictedByPC.flags(statistics::total);
    mispredictedByNPC.flags(statistics::total);
}

/**
 * 设置时间缓冲区
 * 初始化与其他流水线阶段通信的线路连接
 *
 * @param tb_ptr 指向时间缓冲区的指针
 */
void
Decode::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // 设置向取指阶段写入信息的线路
    toFetch = timeBuffer->getWire(0);

    // 创建从时间缓冲区适当位置获取信息的线路
    fromRename = timeBuffer->getWire(-renameToDecodeDelay);  // 从重命名阶段获取信息
    fromIEW = timeBuffer->getWire(-iewToDecodeDelay);        // 从IEW阶段获取信息
    fromCommit = timeBuffer->getWire(-commitToDecodeDelay);  // 从提交阶段获取信息
}

/**
 * 设置译码队列
 * 初始化向重命名阶段传递数据的队列
 *
 * @param dq_ptr 指向译码队列的指针
 */
void
Decode::setDecodeQueue(TimeBuffer<DecodeStruct> *dq_ptr)
{
    decodeQueue = dq_ptr;

    // 设置向译码队列写入信息的线路
    toRename = decodeQueue->getWire(0);
}

/**
 * 设置取指队列
 * 初始化从取指阶段接收数据的队列
 *
 * @param fq_ptr 指向取指队列的指针
 */
void
Decode::setFetchQueue(TimeBuffer<FetchStruct> *fq_ptr)
{
    fetchQueue = fq_ptr;

    // 设置从取指队列读取信息的线路
    fromFetch = fetchQueue->getWire(-fetchToDecodeDelay);
}

/**
 * 设置活动线程列表
 * 设置当前活动的线程ID列表
 *
 * @param at_ptr 指向活动线程列表的指针
 */
void
Decode::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

/**
 * 排空健全性检查
 * 在排空过程中检查所有队列是否为空
 * 确保没有指令遗留在译码阶段
 */
void
Decode::drainSanityCheck() const
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        assert(insts[tid].empty());      // 断言指令队列为空
        assert(skidBuffer[tid].empty()); // 断言缓冲区为空
    }
}

/**
 * 检查是否已排空
 * 检查译码阶段是否完全排空（没有指令在处理）
 *
 * @return 如果已排空则返回true，否则返回false
 */
bool
Decode::isDrained() const
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        // 检查是否有指令在队列中或状态不正常
        if (!insts[tid].empty() || !skidBuffer[tid].empty() ||
                (decodeStatus[tid] != Running && decodeStatus[tid] != Idle))
            return false;
    }
    return true;
}

/**
 * 检查停顿状态
 * 检查指定线程是否在停顿状态
 *
 * @param tid 线程ID
 * @return 如果在停顿状态则返回true，否则返回false
 */
bool
Decode::checkStall(ThreadID tid) const
{
    bool ret_val = false;

    // 检查是否因重命名阶段而停顿
    if (stalls[tid].rename) {
        DPRINTF(Decode,"[tid:%i] Stall from Rename stage detected.\n", tid);
        ret_val = true;
    }

    return ret_val;
}

/**
 * 检查取指指令是否有效
 * 检查是否有从取指阶段来的有效指令
 *
 * @return 如果有有效指令则返回true，否则返回false
 */
bool
Decode::fetchInstsValid()
{
    return fromFetch->size > 0;  // 检查取指队列大小
}

/**
 * 阻塞译码阶段
 * 将指定线程设为阻塞状态，并将当前指令保存到缓冲区
 *
 * @param tid 线程ID
 * @return 如果成功阻塞则返回true，否则返回false
 */
bool
Decode::block(ThreadID tid)
{
    DPRINTF(Decode, "[tid:%i] Blocking.\n", tid);

    // 将当前输入添加到缓冲区，以便在解阻塞时重新处理
    skidInsert(tid);

    // 如果译码状态已经是阻塞或解阻塞，则译码还没有通知取指解阻塞
    // 在这种情况下，不需要告诉取指阻塞
    if (decodeStatus[tid] != Blocked) {
        // 设置状态为阻塞
        decodeStatus[tid] = Blocked;

        if (toFetch->decodeUnblock[tid]) {
            toFetch->decodeUnblock[tid] = false;  // 清空解阻塞信号
        } else {
            toFetch->decodeBlock[tid] = true;     // 设置阻塞信号
            wroteToTimeBuffer = true;             // 标记已写入时间缓冲区
        }

        return true;
    }

    return false;
}

/**
 * 解阻塞译码阶段
 * 尝试解阻塞指定线程，只有在缓冲区为空时才能完成解阻塞
 *
 * @param tid 线程ID
 * @return 如果成功解阻塞则返回true，否则返回false
 */
bool
Decode::unblock(ThreadID tid)
{
    // 只有在缓冲区为空时才算完成解阻塞
    if (skidBuffer[tid].empty()) {
        DPRINTF(Decode, "[tid:%i] Done unblocking.\n", tid);
        toFetch->decodeUnblock[tid] = true;   // 设置解阻塞信号
        wroteToTimeBuffer = true;             // 标记已写入时间缓冲区

        decodeStatus[tid] = Running;          // 设置状态为运行
        return true;
    }

    DPRINTF(Decode, "[tid:%i] Currently unblocking.\n", tid);

    return false;  // 缓冲区不为空，继续解阻塞状态
}

/**
 * 清空流水线（由于分支预测错误）
 * 当在译码阶段检测到分支预测错误时，清空流水线并重定向取指
 *
 * @param inst 导致清空的指令
 * @param tid 线程ID
 */
void
Decode::squash(const DynInstPtr &inst, ThreadID tid)
{
    DPRINTF(Decode, "[tid:%i] [sn:%llu] Squashing due to incorrect branch "
            "prediction detected at decode.\n", tid, inst->seqNum);

    // 发送错误预测信息给取指阶段
    toFetch->decodeInfo[tid].branchMispredict = true;    // 标记分支预测错误
    toFetch->decodeInfo[tid].predIncorrect = true;       // 标记预测不正确
    toFetch->decodeInfo[tid].mispredictInst = inst;      // 记录错误预测的指令
    toFetch->decodeInfo[tid].squash = true;              // 设置清空信号
    toFetch->decodeInfo[tid].doneSeqNum = inst->seqNum;  // 记录清空序列号

    // 设置正确的下一个PC地址
    if (inst->isControl()) {                             // 如果是控制指令
        if (!inst->isReturn()) {                         // 非返回指令
            set(toFetch->decodeInfo[tid].nextPC, *inst->branchTarget());
        } else {                                         // 返回指令
            // 如果是返回指令，目标地址已经在预测目标中设置
            std::unique_ptr<PCStateBase> tgt_ptr(inst->readPredTarg().clone());
            set(toFetch->decodeInfo[tid].nextPC, *tgt_ptr);
        }
    } else {                                             // 非控制指令
        // 设置为下一个顺序执行地址
        std::unique_ptr<PCStateBase> npc_ptr(inst->pcState().clone());
        npc_ptr->as<RiscvISA::PCState>().set(inst->pcState().getFallThruPC());
        set(toFetch->decodeInfo[tid].nextPC, *npc_ptr);
    }

    // 设置分支是否被预测为跳转
    // 注意：使用inst->pcState().branching()可能产生意外结果
    // 当分支被预测为跳转但在BTB中与跳转到下一指令的分支混叠时
    toFetch->decodeInfo[tid].branchTaken = inst->readPredTaken() ||
                                           inst->isUncondCtrl();

    toFetch->decodeInfo[tid].squashInst = inst;          // 记录清空指令

    InstSeqNum squash_seq_num = inst->seqNum;

    // 如果当前处于阻塞状态，可能需要通知取指解阻塞
    if (decodeStatus[tid] == Blocked ||
        decodeStatus[tid] == Unblocking) {
        toFetch->decodeUnblock[tid] = 1;
    }

    // 设置状态为清空状态
    decodeStatus[tid] = Squashing;

    // 清空取指队列中的后续指令
    for (int i=0; i<fromFetch->size; i++) {
        if (fromFetch->insts[i]->threadNumber == tid &&
            fromFetch->insts[i]->seqNum > squash_seq_num) {
            fromFetch->insts[i]->setSquashed();
        }
    }

    // 清空指令列表和缓冲区中的所有指令
    while (!insts[tid].empty()) {
        insts[tid].pop();
    }

    while (!skidBuffer[tid].empty()) {
        skidBuffer[tid].pop();
    }

    // 清空直到这个指令为止的所有指令
    cpu->removeInstsUntil(squash_seq_num, tid);
}

/**
 * 清空流水线（由于其他原因）
 * 清空指定线程的所有指令，通常由于异常或提交阶段的清空信号
 *
 * @param tid 线程ID
 * @return 清空的指令数量
 */
unsigned
Decode::squash(ThreadID tid)
{
    DPRINTF(Decode, "[tid:%i] Squashing.\n",tid);

    // 处理阻塞和解阻塞状态下的清空
    if (decodeStatus[tid] == Blocked ||
        decodeStatus[tid] == Unblocking) {
        if (FullSystem) {
            toFetch->decodeUnblock[tid] = 1;     // 全系统模式下直接解阻塞
        } else {
            // 在系统调用仿真中，在同一周期内可能同时有阻塞和清空
            // 这会导致两个信号都为高电平。在全系统中不应该发生。
            // TODO: 确定这种情况是否仍然发生
            if (toFetch->decodeBlock[tid])
                toFetch->decodeBlock[tid] = 0;       // 清空阻塞信号
            else
                toFetch->decodeUnblock[tid] = 1;     // 设置解阻塞信号
        }
    }

    // 设置状态为清空状态
    decodeStatus[tid] = Squashing;

    // 遍历从取指阶段来的指令并清空它们
    unsigned squash_count = 0;

    for (int i=0; i<fromFetch->size; i++) {
        if (fromFetch->insts[i]->threadNumber == tid) {
            fromFetch->insts[i]->setSquashed();  // 设置指令为清空状态
            squash_count++;
        }
    }

    // 清空指令列表和缓冲区中的所有指令
    while (!insts[tid].empty()) {
        insts[tid].pop();
    }

    while (!skidBuffer[tid].empty()) {
        skidBuffer[tid].pop();
    }

    return squash_count;  // 返回清空的指令数量
}

/**
 * 将指令插入到缓冲区
 * 将当前指令列表中的所有指令移动到缓冲区
 * 通常在阻塞时调用，以保存待处理的指令
 *
 * @param tid 线程ID
 */
void
Decode::skidInsert(ThreadID tid)
{
    DynInstPtr inst = NULL;

    // 将指令列表中的所有指令移动到缓冲区
    while (!insts[tid].empty()) {
        inst = insts[tid].front();           // 获取队列前端指令

        insts[tid].pop();                    // 从指令列表中移除

        assert(tid == inst->threadNumber);   // 断言线程ID匹配

        skidBuffer[tid].push(inst);          // 插入到缓冲区

        DPRINTF(Decode, "Inserting [tid:%d][sn:%lli] PC: %s into decode "
                "skidBuffer %i\n", inst->threadNumber, inst->seqNum,
                inst->pcState(), skidBuffer[tid].size());
    }

    // TODO: 最终需要通过不允许线程超过其缓冲区取指来强制执行此限制
    assert(skidBuffer[tid].size() <= skidBufferMax);
}

/**
 * 检查所有缓冲区是否为空
 * 遍历所有活动线程，检查它们的缓冲区是否都为空
 *
 * @return 如果所有缓冲区都为空则返回true，否则返回false
 */
bool
Decode::skidsEmpty()
{
    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    // 遍历所有活动线程
    while (threads != end) {
        ThreadID tid = *threads++;
        if (!skidBuffer[tid].empty())        // 如果任何一个缓冲区不为空
            return false;
    }

    return true;  // 所有缓冲区都为空
}

/**
 * 更新译码阶段状态
 * 根据各个线程的状态更新译码阶段的整体活动状态
 * 如果有线程在解阻塞，则激活译码阶段
 */
void
Decode::updateStatus()
{
    bool any_unblocking = false;

    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    // 检查是否有线程在解阻塞
    while (threads != end) {
        ThreadID tid = *threads++;

        if (decodeStatus[tid] == Unblocking) {
            any_unblocking = true;
            break;
        }
    }

    // 如果有线程在解阻塞，译码阶段将有活动
    if (any_unblocking) {
        if (_status == Inactive) {
            _status = Active;                    // 设置为活动状态

            DPRINTF(Activity, "Activating stage.\n");

            cpu->activateStage(CPU::DecodeIdx);  // 激活译码阶段
        }
    } else {
        // 如果没有线程在解阻塞，则译码阶段没有内部活动
        // 切换到非活动状态
        if (_status == Active) {
            _status = Inactive;                   // 设置为非活动状态
            DPRINTF(Activity, "Deactivating stage.\n");

            cpu->deactivateStage(CPU::DecodeIdx); // 去激活译码阶段
        }
    }
}

/**
 * 对指令进行分类排序
 * 将从取指阶段来的指令按线程分类并检查是否需要清空
 * 基于版本号来判断指令是否已经被清空
 */
void
Decode::sortInsts()
{
    int insts_from_fetch = fromFetch->size;  // 从取指阶段来的指令数量

    for (int i = 0; i < insts_from_fetch; ++i) {
        const DynInstPtr &inst = fromFetch->insts[i];

        // 检查指令的版本号，如果本地清空版本更新，则标记为清空
        if (localSquashVer.largerThan(inst->getVersion())) {
            inst->setSquashed();
        }

        // 将指令添加到对应线程的指令队列
        insts[inst->threadNumber].push(inst);
    }
}

/**
 * 读取停顿信号
 * 从重命名阶段读取阻塞和解阻塞信号，更新停顿状态
 *
 * @param tid 线程ID
 */
void
Decode::readStallSignals(ThreadID tid)
{
    // 检查重命名阶段的阻塞信号
    if (fromRename->renameBlock[tid]) {
        stalls[tid].rename = true;   // 设置重命名停顿状态
    }

    // 检查重命名阶段的解阻塞信号
    if (fromRename->renameUnblock[tid]) {
        assert(stalls[tid].rename);  // 断言之前确实在停顿
        stalls[tid].rename = false;  // 清空重命名停顿状态
    }
}

bool
Decode::checkSignalsAndUpdate(ThreadID tid)
{
    // Check if there's a squash signal, squash if there is.
    // Check stall signals, block if necessary.
    // If status was blocked
    //     Check if stall conditions have passed
    //         if so then go to unblocking
    // If status was Squashing
    //     check if squashing is not high.  Switch to running this cycle.

    // Update the per thread stall statuses.
    readStallSignals(tid);

    // Check squash signals from commit.
    if (fromCommit->commitInfo[tid].squash) {

        DPRINTF(Decode, "[tid:%i] Squashing instructions due to squash "
                "from commit.\n", tid);

        squash(tid);

        localSquashVer.update(fromCommit->commitInfo[tid].squashVersion.getVersion());
        DPRINTF(Decode, "Updating squash version to %u\n",
                localSquashVer.getVersion());

        return true;
    }

    if (checkStall(tid)) {
        blockReason = fromRename->renameInfo[tid].blockReason;
        return block(tid);
    }

    if (decodeStatus[tid] == Blocked) {
        DPRINTF(Decode, "[tid:%i] Done blocking, switching to unblocking.\n",
                tid);

        decodeStatus[tid] = Unblocking;

        unblock(tid);

        return true;
    }

    if (decodeStatus[tid] == Squashing) {
        // Switch status to running if decode isn't being told to block or
        // squash this cycle.
        DPRINTF(Decode, "[tid:%i] Done squashing, switching to running.\n",
                tid);

        decodeStatus[tid] = Running;

        return false;
    }

    // If we've reached this point, we have not gotten any signals that
    // cause decode to change its status.  Decode remains the same as before.
    return false;
}

/**
 * 译码阶段的主循环函数
 * 每个周期调用一次，负责处理所有活动线程的指令译码
 * 包括信号检查、指令译码、状态更新等
 */
void
Decode::tick()
{
    // 将取指停顿原因传递给重命名阶段
    toRename->fetchStallReason = fromFetch->fetchStallReason;

    wroteToTimeBuffer = false;   // 重置时间缓冲区写入标志

    bool status_change = false;  // 状态改变标志

    toRenameIndex = 0;          // 重置发送给重命名阶段的指令索引

    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    // 对从取指来的指令进行分类
    sortInsts();

    // 检查停顿和清空信号
    while (threads != end) {
        ThreadID tid = *threads++;

        DPRINTF(Decode,"Processing [tid:%i]\n",tid);

        // 检查信号并更新状态
        status_change =  checkSignalsAndUpdate(tid) || status_change;

        // 执行译码操作
        decode(status_change, tid);

        // 设置阻塞原因
        toFetch->decodeInfo[tid].blockReason = blockReason;
    }

    // 如果状态有改变，更新整体状态
    if (status_change) {
        updateStatus();
    }

    // 处理停顿状态和原因
    ThreadID tid = *threads;
    if (stalls[tid].rename) {
        // 有来自重命名的停顿，传递重命名停顿原因
        setAllStalls(fromRename->renameInfo[tid].blockReason);
    } else if (toRenameIndex == 0) {
        // 没有指令发送到重命名阶段
        if (decodeStalls[0] != StallReason::NoStall) {
            setAllStalls(decodeStalls[0]);
        } else {
            // warn("译码有其他停顿原因!");
        }
    } else {
        // 译码没有停顿，传递取指停顿原因(无停顿/取指片段停顿/取指全停顿)
        for (int i = 0; i < decodeStalls.size(); i++) {
            if (i < toRenameIndex) {    // 译码成功，无停顿
                decodeStalls.at(i) = StallReason::NoStall;
            } else {    // 没有指令可译码，传递取指片段停顿
                decodeStalls.at(i) = fromFetch->fetchStallReason.at(i);
            }
        }
    }

    // 将译码停顿原因传递给重命名阶段
    toRename->decodeStallReason = decodeStalls;

    // 如果向时间缓冲区写入了数据，通知CPU此周期有活动
    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");

        cpu->activityThisCycle();
    }
}

/**
 * 执行译码操作
 * 根据当前线程的状态决定如何处理指令
 * - 如果状态是运行或空闲，调用decodeInsts()
 * - 如果状态是解阻塞，缓冲从取指来的指令，继续尝试清空缓冲区
 *
 * @param status_change 状态改变标志的引用
 * @param tid 线程ID
 */
void
Decode::decode(bool &status_change, ThreadID tid)
{
    // 处理不同的译码状态
    if (decodeStatus[tid] == Blocked) {          // 阻塞状态
        ++stats.blockedCycles;                   // 统计阻塞周期
        setAllStalls(blockReason);               // 设置所有停顿原因
    } else if (decodeStatus[tid] == Squashing) { // 清空状态
        ++stats.squashCycles;                    // 统计清空周期
        setAllStalls(StallReason::SquashStall);  // 设置清空停顿原因
    }

    // 译码应该在带宽允许的范围内尽可能多地译码指令，
    // 只要它当前没有被阻塞
    if (decodeStatus[tid] == Running ||          // 运行状态
        decodeStatus[tid] == Idle) {             // 空闲状态
        DPRINTF(Decode, "[tid:%i] Not blocked, so attempting to run "
                "stage.\n",tid);

        decodeInsts(tid);                        // 调用指令译码函数
    } else if (decodeStatus[tid] == Unblocking) { // 解阻塞状态
        // 确保在解阻塞状态下缓冲区中有内容
        assert(!skidsEmpty());

        // 如果状态是解阻塞，则使用缓冲区中的指令
        // 移除这些指令并处理解阻塞的其他部分
        decodeInsts(tid);

        if (fetchInstsValid()) {
            // 将当前输入添加到缓冲区，以便在该阶段解阻塞时重新处理
            skidInsert(tid);
        }

        // 尝试解阻塞并更新状态改变标志
        status_change = unblock(tid) || status_change;
    }
}

/**
 * 执行具体的指令译码操作
 * 该函数是译码阶段的核心，负责处理单个线程的指令译码
 * 包括分支预测验证、指令验证和向重命名阶段发送指令
 *
 * @param tid 线程ID
 */
void
Decode::decodeInsts(ThreadID tid)
{
    // 指令可以来自缓冲区或从取指来的指令列表，取决于译码状态
    int insts_available = decodeStatus[tid] == Unblocking ?
        skidBuffer[tid].size() : insts[tid].size();

    std::queue<StallReason> decode_stalls;       // 译码停顿原因队列

    StallReason breakDecode = StallReason::NoStall;  // 中断译码的原因

    // 如果没有可用的指令，提前退出
    if (insts_available == 0) {
        DPRINTF(Decode, "[tid:%i] Nothing to do, breaking out"
                " early.\n",tid);
        // 是否应该将状态改为空闲？
        ++stats.idleCycles;                     // 统计空闲周期

        // 查找取指阶段的停顿原因
        StallReason stall = StallReason::NoStall;
        for (auto iter : fromFetch->fetchStallReason) {
            if (iter != StallReason::NoStall) {
                stall = iter;
                break;
            }
        }
        setAllStalls(stall);                    // 传递取指停顿原因
        return;
    } else if (decodeStatus[tid] == Unblocking) {
        DPRINTF(Decode, "[tid:%i] Unblocking, removing insts from skid "
                "buffer.\n",tid);
        ++stats.unblockCycles;                  // 统计解阻塞周期
    } else if (decodeStatus[tid] == Running) {
        ++stats.runCycles;                      // 统计运行周期
    }

    // 根据状态选择指令来源：解阻塞时使用缓冲区，否则使用正常指令队列
    std::queue<DynInstPtr>
        &insts_to_decode = decodeStatus[tid] == Unblocking ?
        skidBuffer[tid] : insts[tid];

    DPRINTF(Decode, "[tid:%i] Sending instruction to rename.\n",tid);

    // 向量指令译码限制标志
    // XS-GEM5中的特殊处理：限制向量指令与非向量指令的混合译码
    bool vec_decode_limit = false;

    if (!insts_to_decode.front()->isVector()) {
        vec_decode_limit = true;                // 如果首个指令不是向量指令，则启用限制
    }

    // 主译码循环：在有指令可用且未超过译码带宽时继续处理
    while (insts_available > 0 && toRenameIndex < decodeWidth) {
        assert(!insts_to_decode.empty());       // 断言指令队列不为空

        // XS-GEM5特殊处理：检查向量指令译码限制
        if (vec_decode_limit && insts_to_decode.front()->isVector()) {
            break;  // 如果启用限制且下一个是向量指令，则中断译码
        }

        // 获取下一个要译码的指令
        DynInstPtr inst = std::move(insts_to_decode.front());
        insts_to_decode.pop();

        DPRINTF(Decode, "[tid:%i] Processing instruction [sn:%lli] with "
                "PC %s\n", tid, inst->seqNum, inst->pcState());

        // 检查指令是否已被清空
        if (inst->isSquashed()) {
            DPRINTF(Decode, "[tid:%i] Instruction %i with PC %s is "
                    "squashed, skipping.\n",
                    tid, inst->seqNum, inst->pcState());

            ++stats.squashedInsts;               // 统计清空指令数
            --insts_available;                   // 减少可用指令数
            decode_stalls.push(StallReason::InstSquashed);  // 记录停顿原因
            continue;                            // 跳过此指令
        }

        // 检查指令是否没有源寄存器。将它们标记为可以随时发射。
        // 不确定这个检查应该在这里还是在后续阶段，
        // 但对功能正确性影响不大
        if (inst->numSrcRegs() == 0) {
            inst->setCanIssue();                 // 设置为可发射
        }

        // 当前指令有效，将其添加到译码队列中
        // 下一个指令可能无效，所以检查分支是否预测正确
        toRename->insts[toRenameIndex] = inst;

        ++(toRename->size);                      // 增加发送给重命名的指令数
        ++toRenameIndex;                         // 增加索引
        ++stats.decodedInsts;                    // 统计译码指令数
        --insts_available;                       // 减少可用指令数

        // 更新性能跟踪信息
        cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtDecode);

#if TRACING_ON
        // 记录译码时间信息用于管道视图
        if (debug::O3PipeView) {
            inst->decodeTick = curTick() - inst->fetchTick;
            // DPRINTF(O3PipeView, "Record decode for inst sn:%lu\n",
            //         inst->seqNum);
        }
#endif

        // 确保如果被预测为分支，它确实是一个分支指令
        if (inst->readPredTaken() && !inst->isControl()) {
            // panic("指令被预测为分支但实际不是!");

            ++stats.controlMispred;              // 统计控制流预测错误

            // 可能需要设置某种布尔值，在最后做检查
            squash(inst, inst->threadNumber);    // 清空流水线

            decode_stalls.push(StallReason::InstMisPred);  // 记录预测错误停顿
            breakDecode = StallReason::InstMisPred;        // 设置中断译码原因

            break;                               // 中断译码循环
        }

        // 继续并计算任何PC相对分支
        // 这包括直接无条件控制和被预测为跳转的直接条件控制
        if (inst->isDirectCtrl() &&
           (inst->isUncondCtrl() || inst->readPredTaken()))
        {
            ++stats.branchResolved;              // 统计分支解决数

            // 计算实际分支目标地址
            std::unique_ptr<PCStateBase> target = inst->branchTarget();
            auto &t = target->as<RiscvISA::PCState>();
            auto &pred = inst->readPredTarg().as<RiscvISA::PCState>();

            // XS-GEM5特殊处理：处理无用的NPC问题
            if (t.start_equals(pred) && !t.equals(pred)) {
                DPRINTF(
                    DecoupleBP,
                    "覆盖无用的npc，从 %#lx->%#lx 到 %#lx->%#lx\n",
                    pred.pc(), pred.npc(), t.pc(), t.npc());
                inst->setPredTarg(t);            // 更新预测目标
            }

            // 检查分支目标是否预测正确
            if (*target != inst->readPredTarg()) {
                ++stats.branchMispred;           // 统计分支预测错误

                // 创建目标和预测目标的副本用于比较
                RiscvISA::PCState cpTarget = target->clone()->as<RiscvISA::PCState>();
                RiscvISA::PCState cpPredTarget = inst->readPredTarg().clone()->as<RiscvISA::PCState>();

                // 统计不同类型的预测错误
                if (cpTarget.instAddr() != cpPredTarget.instAddr() && cpTarget.npc() == cpPredTarget.npc()) {
                    ++stats.mispredictedByPC;    // PC预测错误
                } else if (cpTarget.instAddr() == cpPredTarget.instAddr() && cpTarget.npc() != cpPredTarget.npc()) {
                    ++stats.mispredictedByNPC;   // NPC预测错误
                }

                // 可能需要设置某种布尔值，在最后做检查
                squash(inst, inst->threadNumber);// 清空流水线

                decode_stalls.push(StallReason::InstMisPred);  // 记录预测错误停顿
                breakDecode = StallReason::InstMisPred;        // 设置中断译码原因

                DPRINTF(Decode,
                        "[tid:%i] [sn:%llu] 更新预测:"
                        " 错误的预测目标: %s 预测PC: %s\n",
                        tid, inst->seqNum, inst->readPredTarg(), *target);
                // 指令级分支后的微码PC应该为0
                inst->setPredTarg(*target);              // 设置正确的预测目标
                break;                                   // 中断译码循环
            }
        }

        // 未预测的返回指令可以利用RAS结果来获得更早的重定向
        if (inst->isReturn() && !inst->isNonSpeculative() && !inst->readPredTaken()) {
            ++stats.branchMispred;                    // 统计分支预测错误
            decode_stalls.push(StallReason::InstMisPred);
            breakDecode = StallReason::InstMisPred;

            // 返回目标无法在译码阶段计算，因为它是间接分支
            // 需要查询BPU来获得目标
            auto return_addr = fetch_ptr->getPreservedReturnAddr(inst);
            auto target = std::make_unique<RiscvISA::PCState>(return_addr);

            DPRINTF(Decode, "[tid:%i] [sn:%llu] 更新预测:"
                    " 返回未被bp识别: predTaken %d, PredPC: %s 现在PC %s\n",
                    tid, inst->seqNum, inst->readPredTaken(), inst->readPredTarg(), *target);

            inst->setPredTaken(true);                 // 设置为跳转
            inst->setPredTarg(*target);               // 设置预测目标

            // 必须在设置指令真实目标后清空，因为它无法从静态指令计算
            squash(inst, inst->threadNumber);
            break;
        }

        // 处理非推测指令被预测为跳转的情况
        if (inst->isNonSpeculative() && inst->readPredTaken()) {
            // TODO: 重定向到下降执行
            std::unique_ptr<PCStateBase> npc(inst->pcState().clone());
            npc->as<RiscvISA::PCState>().set(inst->pcState().getFallThruPC());
            inst->setPredTaken(false);                // 设置为不跳转
            inst->setPredTarg(*npc);                  // 设置为下一个顺序地址
        }
    }  // 结束主译码循环

    // 如果此阶段完全停顿，设置所有译码停顿状态
    if (!decode_stalls.empty()) {
        setAllStalls(decode_stalls.front());     // 设置停顿原因
        decode_stalls.pop();
    } else if (breakDecode != StallReason::NoStall) {
        setAllStalls(breakDecode);               // 设置中断译码的原因
    }

    // 如果我们没有处理所有指令，那么需要阻塞
    // 并将所有这些指令放入缓冲区
    if (!insts_to_decode.empty()) {
        blockReason = breakDecode;               // 记录阻塞原因
        block(tid);                              // 阻塞该线程
    }

    // 记录译码已向时间缓冲区写入数据用于活动跟踪
    if (toRenameIndex) {
        wroteToTimeBuffer = true;                // 设置写入标志
    }
}

/**
 * 设置所有停顿原因
 * 将所有译码停顿状态设置为相同的停顿原因
 *
 * @param decodeStall 停顿原因
 */
void
Decode::setAllStalls(StallReason decodeStall)
{
    // 将所有译码停顿状态设置为相同的原因
    for (int i = 0;i < decodeStalls.size();i++) {
        decodeStalls.at(i) = decodeStall;
    }
}

} // namespace o3
} // namespace gem5

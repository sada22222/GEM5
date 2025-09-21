/*
 * Copyright (c) 2010-2013, 2018-2019 ARM Limited
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

// @todo: 修复IEW内部各阶段间的即时通信问题
// IEW各阶段间存在明显的延迟（发射到执行），但反向通信却是同时发生的

#include "cpu/o3/iew.hh"

#include <queue>

#include "base/output.hh"
#include "base/stats/info.hh"
#include "config/the_isa.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/fu_pool.hh"
#include "cpu/o3/issue_queue.hh"
#include "cpu/o3/limits.hh"
#include "cpu/timebuf.hh"
#include "debug/Activity.hh"
#include "debug/Counters.hh"
#include "debug/DecoupleBP.hh"
#include "debug/Drain.hh"
#include "debug/IEW.hh"
#include "debug/O3PipeView.hh"
#include "debug/Rename.hh"
#include "params/BaseO3CPU.hh"
#include "sim/core.hh"

namespace gem5
{

namespace o3
{

// IEW（Issue/Execute/Writeback）阶段构造函数
// 初始化发射、执行和写回阶段的所有组件和参数
IEW::IEW(CPU *_cpu, const BaseO3CPUParams &params)
    : dqSize(params.numDQEntries),                          // 分派队列大小
      issueToExecQueue(params.backComSize, params.forwardComSize),  // 发射到执行的通信队列
      cpu(_cpu),                                            // CPU指针
      scheduler(params.scheduler),                          // 指令调度器
      instQueue(_cpu, this, params),                       // 指令队列
      ldstQueue(_cpu, this, params),                       // 加载存储队列
      dispWidth(params.dispWidth),                         // 分派宽度
      commitToIEWDelay(params.commitToIEWDelay),          // 提交到IEW的延迟
      renameToIEWDelay(params.renameToIEWDelay),          // 重命名到IEW的延迟
      enableDispatchStage(params.enableDispatchStage),    // 启用分派阶段
      renameWidth(params.renameWidth),                     // 重命名宽度
      wbNumInst(0),                                        // 写回指令数量
      wbCycle(0),                                          // 写回周期
      wbDelay(params.executeToWriteBackDelay),            // 执行到写回的延迟
      wbWidth(params.wbWidth),                            // 写回宽度
      enableStoreSetTrain(params.enable_storeSet_train),  // 启用存储集训练
      numThreads(params.numThreads),                      // 线程数量
      iewStats(cpu)                                        // 统计信息
{
    // 检查写回宽度是否超过编译时限制
    if (wbWidth > MaxWidth)
        fatal("wbWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             wbWidth, static_cast<int>(MaxWidth));

    // 初始化IEW阶段各部分的状态
    _status = Active;        // IEW整体状态为活跃
    exeStatus = Running;     // 执行状态为运行中
    wbStatus = Idle;         // 写回状态为空闲

    // 建立从发射阶段读取指令的连接
    fromIssue = issueToExecQueue.getWire(0);

    // 指令队列需要发射到执行之间的通信队列
    instQueue.setIssueToExecuteQueue(&issueToExecQueue);

    // 初始化每个线程的状态
    for (ThreadID tid = 0; tid < MaxThreads; tid++) {
        dispatchStatus[tid] = Running;   // 分派状态为运行中
        fetchRedirect[tid] = false;      // 取指重定向标志为false
    }

    updateLSQNextCycle = false;  // 下一周期不更新LSQ

    // 计算滑动缓冲区最大大小：(延迟+1) * 重命名宽度 * 2
    skidBufferMax = (renameToIEWDelay + 1) * params.renameWidth * 2;

    // 调整分派停顿原因向量大小，初始化为无停顿
    dispatchStalls.resize(renameWidth, StallReason::NoStall);

}

// 获取IEW阶段的名称
// 返回CPU名称加上".iew"后缀
std::string
IEW::name() const
{
    return cpu->name() + ".iew";
}

// 注册探测点
// 为性能分析和调试注册各种探测点
void
IEW::regProbePoints()
{
    // 分派探测点：当指令被分派到IEW时触发
    ppDispatch = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Dispatch");
    // 错误预测探测点：当检测到分支预测错误时触发
    ppMispredict = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Mispredict");
    // 执行探测点：当指令开始执行时触发
    ppExecute = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Execute");
    // 准备提交探测点：当指令执行完成并准备提交时触发
    ppToCommit = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "ToCommit");
}

// IEW统计信息构造函数
// 初始化IEW阶段的各种性能统计计数器
IEW::IEWStats::IEWStats(CPU *cpu)
    : statistics::Group(cpu, "iew"),
    ADD_STAT(idleCycles, statistics::units::Cycle::get(),
             "Number of cycles IEW is idle"),                        // IEW空闲周期数
    ADD_STAT(squashCycles, statistics::units::Cycle::get(),
             "Number of cycles IEW is squashing"),                   // IEW撤销周期数
    ADD_STAT(blockCycles, statistics::units::Cycle::get(),
             "Number of cycles IEW is blocking"),                    // IEW阻塞周期数
    ADD_STAT(unblockCycles, statistics::units::Cycle::get(),
             "Number of cycles IEW is unblocking"),                  // IEW解除阻塞周期数
    ADD_STAT(dispatchedInsts, statistics::units::Count::get(),
             "Number of instructions dispatched to IQ"),            // 分派到IQ的指令数
    ADD_STAT(dispSquashedInsts, statistics::units::Count::get(),
             "Number of squashed instructions skipped by dispatch"), // 分派时跳过的撤销指令数
    ADD_STAT(dispLoadInsts, statistics::units::Count::get(),
             "Number of dispatched load instructions"),              // 分派的加载指令数
    ADD_STAT(dispStoreInsts, statistics::units::Count::get(),
             "Number of dispatched store instructions"),             // 分派的存储指令数
    ADD_STAT(dispNonSpecInsts, statistics::units::Count::get(),
             "Number of dispatched non-speculative instructions"),  // 分派的非推测指令数
    ADD_STAT(iqFullEvents, statistics::units::Count::get(),
             "Number of times the IQ has become full, causing a stall"),    // IQ满造成停顿的次数
    ADD_STAT(lsqFullEvents, statistics::units::Count::get(),
             "Number of times the LSQ has become full, causing a stall"),   // LSQ满造成停顿的次数
    ADD_STAT(memOrderViolationEvents, statistics::units::Count::get(),
             "Number of memory order violations"),                   // 内存顺序冲突次数
    ADD_STAT(predictedTakenIncorrect, statistics::units::Count::get(),
             "Number of branches that were predicted taken incorrectly"),   // 错误预测为跳转的分支数
    ADD_STAT(predictedNotTakenIncorrect, statistics::units::Count::get(),
             "Number of branches that were predicted not taken incorrectly"), // 错误预测为不跳转的分支数
    ADD_STAT(branchMispredicts, statistics::units::Count::get(),
             "Number of branch mispredicts detected at execute",    // 执行阶段检测到的分支预测错误数
             predictedTakenIncorrect + predictedNotTakenIncorrect),
    ADD_STAT(dispDist, statistics::units::Count::get(),
             "Number of branch mispredicts detected at execute"),   // 分派分布统计
    executedInstStats(cpu),                                    // 执行指令统计
    ADD_STAT(instsToCommit, statistics::units::Count::get(),
             "Cumulative count of insts sent to commit"),           // 发送到提交的指令累计数
    ADD_STAT(writebackCount, statistics::units::Count::get(),
             "Cumulative count of insts written-back"),             // 写回指令累计数
    ADD_STAT(producerInst, statistics::units::Count::get(),
             "Number of instructions producing a value"),           // 产生值的指令数
    ADD_STAT(consumerInst, statistics::units::Count::get(),
             "Number of instructions consuming a value"),           // 消费值的指令数
    ADD_STAT(wbRate, statistics::units::Rate<
                statistics::units::Count, statistics::units::Cycle>::get(),
             "Insts written-back per cycle"),                       // 每周期写回指令数
    ADD_STAT(wbFanout, statistics::units::Rate<
                statistics::units::Count, statistics::units::Count>::get(),
             "Average fanout of values written-back"),              // 写回值的平均扇出数
    ADD_STAT(stallEvents, statistics::units::Count::get(),
             "Number of events the IEW has stalled"),               // IEW停顿事件数
    ADD_STAT(fetchStallReason, statistics::units::Count::get(),
             "Number of fetch stall reasons each tick (Total)"),   // 每周期取指停顿原因数
    ADD_STAT(decodeStallReason, statistics::units::Count::get(),
             "Number of decode stall reasons each tick (Total)"),  // 每周期译码停顿原因数
    ADD_STAT(renameStallReason, statistics::units::Count::get(),
             "Number of rename stall reasons each tick (Total)"),  // 每周期重命名停顿原因数
    ADD_STAT(dispatchStallReason, statistics::units::Count::get(),
             "Number of dispatch stall reasons each tick (Total)") // 每周期分派停顿原因数
{
    instsToCommit
        .init(cpu->numThreads)
        .flags(statistics::total);

    writebackCount
        .init(cpu->numThreads)
        .flags(statistics::total);

    producerInst
        .init(cpu->numThreads)
        .flags(statistics::total);

    consumerInst
        .init(cpu->numThreads)
        .flags(statistics::total);

    wbRate
        .flags(statistics::total);
    wbRate = writebackCount / cpu->baseStats.numCycles;

    wbFanout
        .flags(statistics::total);
    wbFanout = producerInst / consumerInst;

    stallEvents
        .init(StallEventCount)
        .flags(statistics::total);

    dispDist.init(0,10,1).flags(statistics::nozero);

    std::map < StallEvent, const char* > stall_event_str = {
        { CacheMiss, "CacheMiss" },
        { Translation, "Translation" },
        { ROBWalk, "ROBWalk" },
        { IQFull, "IQFull" },
        { LSQFull, "LSQFull" },
        { DispBWFull, "DispBWFull" }
    };

    for (int i = 0; i < StallEventCount; i++) {
        stallEvents.subname(i, stall_event_str[static_cast<StallEvent>(i)]);
    }

    fetchStallReason
            .init(NumStallReasons)
            .flags(statistics::total | statistics::pdf);

    decodeStallReason
            .init(NumStallReasons)
            .flags(statistics::total | statistics::pdf);

    renameStallReason
            .init(NumStallReasons)
            .flags(statistics::total | statistics::pdf);

    dispatchStallReason
            .init(NumStallReasons)
            .flags(statistics::total | statistics::pdf);

    std::map <StallReason, const char*> stallReasonStr = {
        {StallReason::NoStall, "NoStall"},
        {StallReason::IcacheStall, "IcacheStall"},
        {StallReason::ITlbStall, "ITlbStall"},
        {StallReason::DTlbStall, "DTlbStall"},
        {StallReason::BpStall, "BpStall"},
        {StallReason::IntStall, "IntStall"},
        {StallReason::TrapStall, "TrapStall"},
        {StallReason::FetchFragStall, "FetchFragStall"},
        {StallReason::OtherFragStall, "OtherFragStall"},
        {StallReason::SquashStall, "SquashStall"},
        {StallReason::FetchBufferInvalid, "FetchBufferInvalid"},
        {StallReason::InstMisPred, "InstMisPred"},
        {StallReason::InstSquashed, "InstSquashed"},
        {StallReason::SerializeStall, "SerializeStall"},
        {StallReason::VectorLongExecute, "VectorLongExecute"},
        {StallReason::ScalarLongExecute, "ScalarLongExecute"},
        {StallReason::InstNotReady, "InstNotReady"},
        {StallReason::LoadL1Bound, "LoadL1Bound"},
        {StallReason::LoadL2Bound, "LoadL2Bound"},
        {StallReason::LoadL3Bound, "LoadL3Bound"},
        {StallReason::LoadMemBound, "LoadMemBound"},
        {StallReason::StoreL1Bound, "StoreL1Bound"},
        {StallReason::StoreL2Bound, "StoreL2Bound"},
        {StallReason::StoreL3Bound, "StoreL3Bound"},
        {StallReason::StoreMemBound, "StoreMemBound"},
        {StallReason::MemSquashed, "MemSquashed"},
        {StallReason::Atomic,"Atomic"},
        {StallReason::ResumeUnblock, "ResumeUnblock"},
        {StallReason::CommitSquash, "CommitSquash"},
        {StallReason::OtherStall, "OtherStall"},
        {StallReason::OtherFetchStall, "OtherFetchStall"},
        {StallReason::FTQBubble, "FTQBubble"},
        {StallReason::MemDQBandwidth, "MemDQBandwidth"},
        {StallReason::FVDQBandwidth, "FVDQBandwidth"},
        {StallReason::IntDQBandwidth, "IntDQBandwidth"},
        {StallReason::MemNotReady, "MemNotReady"},
        {StallReason::MemCommitRateLimit, "MemCommitRateLimit"},
        {StallReason::OtherMemStall, "OtherMemStall"},
        {StallReason::VectorReadyButNotIssued, "VectorReadyButNotIssued"},
        {StallReason::ScalarReadyButNotIssued, "ScalarReadyButNotIssued"}
    };

    for (int i = 0;i < NumStallReasons;i++) {
        fetchStallReason.subname(i, stallReasonStr[static_cast<StallReason>(i)]);
        decodeStallReason.subname(i, stallReasonStr[static_cast<StallReason>(i)]);
        renameStallReason.subname(i, stallReasonStr[static_cast<StallReason>(i)]);
        dispatchStallReason.subname(i, stallReasonStr[static_cast<StallReason>(i)]);
    }
}

// 执行指令统计信息构造函数
// 初始化各种类型执行指令的统计计数器
IEW::IEWStats::ExecutedInstStats::ExecutedInstStats(CPU *cpu)
    : statistics::Group(cpu, "executed_inst"),
    ADD_STAT(numInsts, statistics::units::Count::get(),
             "Number of executed instructions"),                     // 执行指令总数
    ADD_STAT(numLoadInsts, statistics::units::Count::get(),
             "Number of load instructions executed"),                // 执行的加载指令数
    ADD_STAT(numSquashedInsts, statistics::units::Count::get(),
             "Number of squashed instructions skipped in execute"),  // 执行时跳过的撤销指令数
    ADD_STAT(numSwp, statistics::units::Count::get(),
             "Number of swp insts executed"),                        // 执行的交换指令数
    ADD_STAT(numNop, statistics::units::Count::get(),
             "Number of nop insts executed"),                        // 执行的空操作指令数
    ADD_STAT(numRefs, statistics::units::Count::get(),
             "Number of memory reference insts executed"),           // 执行的内存引用指令数
    ADD_STAT(numBranches, statistics::units::Count::get(),
             "Number of branches executed"),                         // 执行的分支指令数
    ADD_STAT(numStoreInsts, statistics::units::Count::get(),
             "Number of stores executed"),                           // 执行的存储指令数
    ADD_STAT(numRate, statistics::units::Rate<
                statistics::units::Count, statistics::units::Cycle>::get(),
             "Inst execution rate", numInsts / cpu->baseStats.numCycles)  // 指令执行率
{
    numLoadInsts
        .init(cpu->numThreads)
        .flags(statistics::total);

    numSwp
        .init(cpu->numThreads)
        .flags(statistics::total);

    numNop
        .init(cpu->numThreads)
        .flags(statistics::total);

    numRefs
        .init(cpu->numThreads)
        .flags(statistics::total);

    numBranches
        .init(cpu->numThreads)
        .flags(statistics::total);

    numStoreInsts
        .flags(statistics::total);
    numStoreInsts = numRefs - numLoadInsts;

    numRate
        .flags(statistics::total);
}

// IEW启动阶段初始化
// 在流水线启动时初始化IEW阶段的状态信息
void
IEW::startupStage()
{
    // 为每个线程初始化队列使用状态和可用条目数
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        toRename->iewInfo[tid].usedIQ = true;   // 标记指令队列已使用

        toRename->iewInfo[tid].usedLSQ = true;  // 标记加载存储队列已使用
        // 获取加载队列空闲条目数
        toRename->iewInfo[tid].freeLQEntries =
            ldstQueue.numFreeLoadEntries(tid);
        // 获取存储队列空闲条目数
        toRename->iewInfo[tid].freeSQEntries =
            ldstQueue.numFreeStoreEntries(tid);
    }

    // Initialize the checker's dcache port here
    if (cpu->checker) {
        cpu->checker->setDcachePort(&ldstQueue.getDataPort());
    }

    cpu->activateStage(CPU::IEWIdx);
}

// 清空线程状态
// 重置指定线程的IEW状态信息
void
IEW::clearStates(ThreadID tid)
{
    toRename->iewInfo[tid].usedIQ = true;   // 重置指令队列使用状态

    toRename->iewInfo[tid].usedLSQ = true;  // 重置加载存储队列使用状态
    // 重置加载队列和存储队列的空闲条目数
    toRename->iewInfo[tid].freeLQEntries = ldstQueue.numFreeLoadEntries(tid);
    toRename->iewInfo[tid].freeSQEntries = ldstQueue.numFreeStoreEntries(tid);
}

// 设置时间缓冲区
// 配置IEW阶段与其他流水线阶段通信的时间缓冲区
void
IEW::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // 建立从提交阶段读取信息的连接（考虑延迟）
    fromCommit = timeBuffer->getWire(-commitToIEWDelay);

    // 建立向前级阶段写回信息的连接
    toRename = timeBuffer->getWire(0);  // 向重命名阶段写回

    toFetch = timeBuffer->getWire(0);   // 向取指阶段写回

    // Instruction queue also needs main time buffer.
    instQueue.setTimeBuffer(tb_ptr);
}

// 设置重命名队列
// 配置从重命名阶段读取指令的通信队列
void
IEW::setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr)
{
    renameQueue = rq_ptr;

    // 建立从重命名队列读取信息的连接（考虑延迟）
    fromRename = renameQueue->getWire(-renameToIEWDelay);
}

// 设置IEW队列
// 配置向提交阶段发送指令的通信队列
void
IEW::setIEWQueue(TimeBuffer<IEWStruct> *iq_ptr)
{
    iewQueue = iq_ptr;

    // 建立向提交阶段写入指令的连接
    toCommit = iewQueue->getWire(0);

    // 设置执行旁路和写回连接
    execBypass = iewQueue->getWire(0);      // 执行旁路连接
    execWB = iewQueue->getWire(-wbDelay);   // 写回连接（考虑延迟）
}

// 设置活跃线程列表
// 配置IEW阶段需要处理的活跃线程
void
IEW::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;

    // 将活跃线程信息传递给子组件
    ldstQueue.setActiveThreads(at_ptr);     // 设置加载存储队列的活跃线程
    instQueue.setActiveThreads(at_ptr);     // 设置指令队列的活跃线程
}

// 设置计分板
// 配置用于跟踪寄存器就绪状态的计分板
void
IEW::setScoreboard(Scoreboard *sb_ptr)
{
    scoreboard = sb_ptr;  // 设置计分板指针，用于寄存器依赖跟踪
}

// 检查IEW阶段是否已排空
// 确认所有队列和缓冲区都已清空，用于系统排空操作
bool
IEW::isDrained() const
{
    // 检查加载存储队列和指令队列是否都已排空
    bool drained = ldstQueue.isDrained() && instQueue.isDrained();

    // 检查每个线程的状态
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        // 检查指令缓冲区是否为空
        if (!insts[tid].empty()) {
            DPRINTF(Drain, "%i: Insts not empty.\n", tid);
            drained = false;
        }
        // 检查滑动缓冲区是否为空
        if (!skidBuffer[tid].empty()) {
            DPRINTF(Drain, "%i: Skid buffer not empty.\n", tid);
            drained = false;
        }
        // 检查分派状态是否为运行中
        drained = drained && dispatchStatus[tid] == Running;
    }

    return drained;  // 只有所有条件满足时才认为已排空
}

// 排空健全性检查
// 验证IEW阶段是否正确排空，用于调试
void
IEW::drainSanityCheck() const
{
    assert(isDrained());  // 断言IEW已经排空

    // 检查指令队列和加载存储队列的排空状态
    instQueue.drainSanityCheck();
    ldstQueue.drainSanityCheck();
}

// 接管IEW阶段控制
// 在CPU切换或重启时重置IEW阶段的所有状态
void
IEW::takeOverFrom()
{
    // 重置所有状态
    _status = Active;     // IEW整体状态为活跃
    exeStatus = Running;  // 执行状态为运行中
    wbStatus = Idle;      // 写回状态为空闲

    // 让子组件接管控制
    instQueue.takeOverFrom();
    ldstQueue.takeOverFrom();

    // 启动阶段初始化
    startupStage();
    cpu->activityThisCycle();  // 标记CPU本周期活跃

    // 重置所有线程的状态
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        dispatchStatus[tid] = Running;  // 分派状态为运行中
        fetchRedirect[tid] = false;     // 取指重定向标志为false
    }

    updateLSQNextCycle = false;  // 不在下周期更新LSQ

    // 清空发射到执行的通信队列
    for (int i = 0; i < issueToExecQueue.getSize(); ++i) {
        issueToExecQueue.advance();
    }
}

// 撤销指定线程的所有指令
// 当检测到分支预测错误或异常时撤销后续指令
void
IEW::squash(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Squashing all instructions.\n", tid);

    // 撤销分派队列中序列号大于已提交指令的所有指令
    for (auto& dp : dispQue) {
        for (auto& it : dp) {
            if (it->seqNum > fromCommit->commitInfo[tid].doneSeqNum) {
                it->setSquashed();  // 标记指令为已撤销
            }
        }
    }

    // Tell the IQ to start squashing.
    instQueue.squash(tid);

    // Tell the LDSTQ to start squashing.
    ldstQueue.squash(fromCommit->commitInfo[tid].doneSeqNum, tid);
    updatedQueues = true;

    // Clear the skid buffer in case it has any data in it.
    DPRINTF(IEW,
            "Removing skidbuffer instructions until "
            "[sn:%llu] [tid:%i]\n",
            fromCommit->commitInfo[tid].doneSeqNum, tid);

    while (!skidBuffer[tid].empty()) {
        if (skidBuffer[tid].front()->isLoad()) {
            toRename->iewInfo[tid].dispatchedToLQ++;
        }
        if (skidBuffer[tid].front()->isStore() ||
            skidBuffer[tid].front()->isAtomic()) {
            toRename->iewInfo[tid].dispatchedToSQ++;
        }

        toRename->iewInfo[tid].dispatched++;

        skidBuffer[tid].pop_front();
    }

    emptyRenameInsts(tid);
}

// 因分支预测错误而撤销
// 当检测到分支预测错误时，设置撤销信息并通知其他阶段
void
IEW::squashDueToBranch(const DynInstPtr& inst, ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] [sn:%llu] Squashing from a specific instruction,"
            " PC: %s "
            "\n", tid, inst->seqNum, inst->pcState() );

    // 如果没有正在进行的撤销或当前指令序列号更小，则开始新的撤销
    if (!execWB->squash[tid] ||
            inst->seqNum < execWB->squashedSeqNum[tid]) {
        execWB->squash[tid] = true;                           // 设置撤销标志
        execWB->squashedSeqNum[tid] = inst->seqNum;          // 撤销起始序列号
        execWB->squashedStreamId[tid] = inst->getFsqId();    // 获取FSQ ID
        execWB->squashedTargetId[tid] = inst->getFtqId();    // 获取FTQ ID
        execWB->squashedLoopIter[tid] = inst->getLoopIteration(); // 循环迭代次数
        execWB->branchTaken[tid] = inst->pcState().branching(); // 分支是否跳转

        // 设置正确的PC值
        set(execWB->pc[tid], inst->pcState());
        inst->staticInst->advancePC(*execWB->pc[tid]);

        execWB->mispredictInst[tid] = inst;        // 错误预测的指令
        execWB->includeSquashInst[tid] = false;    // 不包括撤销指令本身

        wroteToTimeBuffer = true;  // 标记已写入时间缓冲区

        DPRINTF(DecoupleBP,
                "Branch misprediction (pc=%#lx) set stream id to %lu, target "
                "id to %lu, loop iter to %u\n",
                execWB->pc[tid]->instAddr(),
                execWB->squashedStreamId[tid],
                execWB->squashedTargetId[tid],
                execWB->squashedLoopIter[tid]);
    }

}

// 因内存序冲突而撤销
// 当检测到内存序冲突时，撤销冲突指令及其后续指令
void
IEW::squashDueToMemOrder(const DynInstPtr& inst, ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Memory violation, squashing violator and younger "
            "insts, PC: %s [sn:%llu].\n", tid, inst->pcState(), inst->seqNum);
    // 需要在比较中包含inst->seqNum来处理角落情况：
    // 当分支预测错误和内存冲突在同一周期对同一指令检测到时，
    // 内存冲突应该优先于分支预测错误，因为它需要将冲突指令本身包含在撤销中
    if (!execWB->squash[tid] ||
            inst->seqNum <= execWB->squashedSeqNum[tid]) {
        execWB->squash[tid] = true;  // 设置撤销标志

        execWB->squashedSeqNum[tid] = inst->seqNum;          // 撤销起始序列号
        execWB->squashedStreamId[tid] = inst->getFsqId();    // FSQ ID
        execWB->squashedTargetId[tid] = inst->getFtqId();    // FTQ ID
        execWB->squashedLoopIter[tid] = inst->getLoopIteration(); // 循环迭代
        set(execWB->pc[tid], inst->pcState());               // 设置PC
        execWB->mispredictInst[tid] = NULL;                  // 清空错误预测指令

        // 内存冲突必须包含冲突指令本身在撤销中
        execWB->includeSquashInst[tid] = true;

        wroteToTimeBuffer = true;  // 标记已写入时间缓冲区

        DPRINTF(DecoupleBP,
                "Memory violation (pc=%#lx) set stream id to %lu, target id "
                "to %lu, loop iter to %u\n",
                execWB->pc[tid]->instAddr(),
                execWB->squashedStreamId[tid],
                execWB->squashedTargetId[tid],
                execWB->squashedLoopIter[tid]);


    }
}

// 阻塞指定线程的IEW阶段
// 当资源不足时阻塞线程，将指令缓存到滑动缓冲区
void
IEW::block(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Blocking.\n", tid);

    // 如果当前不是阻塞状态，通知重命名阶段
    if (dispatchStatus[tid] != Blocked &&
        dispatchStatus[tid] != Unblocking) {
        toRename->iewBlock[tid] = true;  // 设置阻塞信号
        wroteToTimeBuffer = true;        // 标记写入了时间缓冲区
    }

    // 将当前输入的指令添加到滑动缓冲区
    // 这样在解除阻塞时可以重新处理这些指令
    skidInsert(tid);

    dispatchStatus[tid] = Blocked;  // 设置分派状态为阻塞
}

// 解除阻塞指定线程的IEW阶段
// 从滑动缓冲区读取指令并恢复正常处理
void
IEW::unblock(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Reading instructions out of the skid "
            "buffer %u.\n",tid, tid);

    // 如果滑动缓冲区已空，向前级阶段发送解除阻塞信号
    // 同时切换状态为运行中
    if (skidBuffer[tid].empty()) {
        toRename->iewUnblock[tid] = true;  // 通知重命名阶段解除阻塞
        wroteToTimeBuffer = true;          // 标记写入了时间缓冲区
        DPRINTF(IEW, "[tid:%i] Done unblocking.\n",tid);
        dispatchStatus[tid] = Running;     // 设置分派状态为运行中
    }
}

// 唤醒依赖指令
// 当指令完成执行后，唤醒依赖于该指令的其他指令
void
IEW::wakeDependents(const DynInstPtr& inst)
{
    instQueue.wakeDependents(inst);  // 通知指令队列唤醒依赖指令
}

// 重新调度内存指令
// 当内存指令需要重新调度时调用
void
IEW::rescheduleMemInst(const DynInstPtr& inst)
{
    instQueue.rescheduleMemInst(inst);  // 通知指令队列重新调度内存指令
}

// 重放内存指令
// 当内存指令需要重新执行时调用
void
IEW::replayMemInst(const DynInstPtr& inst)
{
    instQueue.replayMemInst(inst);  // 通知指令队列重放内存指令
}

// 阻塞内存指令
// 当内存指令遇到阻塞条件时调用
void
IEW::blockMemInst(const DynInstPtr& inst)
{
    instQueue.blockMemInst(inst);  // 通知指令队列阻塞内存指令
}

// 缓存缺失加载指令重放
// 当加载指令发生缓存缺失时需要重放
void
IEW::cacheMissLdReplay(const DynInstPtr& inst)
{
    instQueue.cacheMissLdReplay(inst);  // 通知指令队列处理缓存缺失重放
}

// 缓存解除阻塞
// 当缓存阻塞解除时调用
void
IEW::cacheUnblocked()
{
    instQueue.cacheUnblocked();  // 通知指令队列缓存已解除阻塞
}

void
IEW::readyToFinish(const DynInstPtr& inst)
{
    // This function should not be called after writebackInsts in a
    // single cycle.  That will cause problems with an instruction
    // being added to the queue to commit without being processed by
    // writebackInsts prior to being sent to commit.

    // First check the time slot that this instruction will write
    // to.  If there are free write ports at the time, then go ahead
    // and write the instruction to that time.  If there are not,
    // keep looking back to see where's the first time there's a
    // free slot.
    while ((*iewQueue)[wbCycle].insts[wbNumInst]) {
        ++wbNumInst;
        if (wbNumInst == wbWidth) {
            ++wbCycle;
            wbNumInst = 0;
        }
    }

#if TRACING_ON
    if (debug::O3PipeView) {
        inst->completeTick = curTick() - inst->fetchTick;
    }
#endif

    scheduler->bypassWriteback(inst);
    inst->completionTick = curTick();

    DPRINTF(IEW, "Current wb cycle: %i, width: %i, numInst: %i\nwbActual:%i\n",
            wbCycle, wbWidth, wbNumInst, wbCycle * wbWidth + wbNumInst);
    // Add finished instruction to queue to commit.
    (*iewQueue)[wbCycle].insts[wbNumInst] = inst;
    (*iewQueue)[wbCycle].size++;
}

// 将指令插入滑动缓冲区
// 在IEW阶段阻塞时，将当前处理的指令保存到滑动缓冲区
void
IEW::skidInsert(ThreadID tid)
{
    DynInstPtr inst = NULL;

    while (!insts[tid].empty()) {
        inst = insts[tid].front();

        insts[tid].pop_front();

        DPRINTF(IEW,"[tid:%i] Inserting [sn:%lli] PC:%s into "
                "dispatch skidBuffer %i\n",tid, inst->seqNum,
                inst->pcState(),tid);

        skidBuffer[tid].push_back(inst);
    }

    assert(skidBuffer[tid].size() <= skidBufferMax &&
           "Skidbuffer Exceeded Max Size");
}

// 获取滑动缓冲区最大深度
// 返回所有线程中滑动缓冲区的最大条目数
int
IEW::skidCount()
{
    int max=0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    // 遍历所有活跃线程，找到滑动缓冲区最大深度
    while (threads != end) {
        ThreadID tid = *threads++;
        unsigned thread_count = skidBuffer[tid].size();
        if (max < thread_count)
            max = thread_count;  // 更新最大深度
    }

    return max;  // 返回最大深度
}

// 检查所有滑动缓冲区是否为空
// 只有当所有线程的滑动缓冲区都为空时才返回true
bool
IEW::skidsEmpty()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    // 检查每个线程的滑动缓冲区
    while (threads != end) {
        ThreadID tid = *threads++;

        if (!skidBuffer[tid].empty())
            return false;  // 只要有一个不为空就返回false
    }

    return true;  // 所有缓冲区都为空
}

// 更新IEW阶段状态
// 根据各线程状态更新IEW整体状态
void
IEW::updateStatus()
{
    bool any_unblocking = false;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    // 检查是否有线程处于解除阻塞状态
    while (threads != end) {
        ThreadID tid = *threads++;

        if (dispatchStatus[tid] == Unblocking) {
            any_unblocking = true;  // 发现有线程在解除阻塞
            break;
        }
    }

    // If there are no ready instructions waiting to be scheduled by the IQ,
    // and there's no stores waiting to write back, and dispatch is not
    // unblocking, then there is no internal activity for the IEW stage.
    instQueue.iqIOStats.intInstQueueReads++;
    if (_status == Active && !instQueue.hasReadyInsts() &&
        !ldstQueue.willWB() && !any_unblocking) {
        DPRINTF(IEW, "IEW switching to idle\n");

        deactivateStage();

        _status = Inactive;
    } else if (_status == Inactive && (instQueue.hasReadyInsts() ||
                                       ldstQueue.willWB() ||
                                       any_unblocking)) {
        // Otherwise there is internal activity.  Set to active.
        DPRINTF(IEW, "IEW switching to active\n");

        activateStage();

        _status = Active;
    }
}

bool
IEW::checkStall(ThreadID tid)
{
    bool ret_val(false);

    if (fromCommit->commitInfo[tid].robSquashing) {
        DPRINTF(IEW,"[tid:%i] Stall from Commit stage detected.\n",tid);
        ret_val = true;
        blockReason = StallReason::CommitSquash;
    }

    return ret_val;
}

// 检查信号并更新状态
// 处理来自提交阶段的撤销和停顿信号，更新分派状态
void
IEW::checkSignalsAndUpdate(ThreadID tid)
{
    // 处理逻辑：
    // 1. 检查是否有撤销信号，如果有则执行撤销
    // 2. 检查停顿信号，如果有则阻塞
    // 3. 如果状态是阻塞，则转向解除阻塞
    // 4. 如果状态是撤销中，检查撤销是否结束，切换到运行状态

    // 检查提交阶段的撤销信号
    if (fromCommit->commitInfo[tid].squash) {
        squash(tid);  // 执行撤销操作
        // 更新本地撤销版本号
        localSquashVer.update(fromCommit->commitInfo[tid].squashVersion.getVersion());
        DPRINTF(IEW, "Updating squash version to %u\n",
                localSquashVer.getVersion());

        // 如果当前处于阻塞或解除阻塞状态，通知重命名阶段解除阻塞
        if (dispatchStatus[tid] == Blocked ||
            dispatchStatus[tid] == Unblocking) {
            toRename->iewUnblock[tid] = true;  // 解除阻塞信号
            wroteToTimeBuffer = true;          // 标记写入时间缓冲区
        }

        dispatchStatus[tid] = Squashing;
        fetchRedirect[tid] = false;
        iewStats.stallEvents[ROBWalk]++;
        setAllStalls(StallReason::CommitSquash);
        return;
    }

    if (fromCommit->commitInfo[tid].robSquashing) {
        DPRINTF(IEW, "[tid:%i] ROB is still squashing.\n", tid);

        dispatchStatus[tid] = Squashing;
        emptyRenameInsts(tid);
        wroteToTimeBuffer = true;
        iewStats.stallEvents[ROBWalk]++;
        setAllStalls(StallReason::CommitSquash);
    }

    if (checkStall(tid)) {
        block(tid);
        dispatchStatus[tid] = Blocked;
        return;
    }

    if (dispatchStatus[tid] == Blocked) {
        // Status from previous cycle was blocked, but there are no more stall
        // conditions.  Switch over to unblocking.
        DPRINTF(IEW, "[tid:%i] Done blocking, switching to unblocking.\n",
                tid);

        dispatchStatus[tid] = Unblocking;

        unblock(tid);

        return;
    }

    if (dispatchStatus[tid] == Squashing) {
        // Switch status to running if rename isn't being told to block or
        // squash this cycle.
        DPRINTF(IEW, "[tid:%i] Done squashing, switching to running.\n",
                tid);

        dispatchStatus[tid] = Running;

        return;
    }
}

// 对来自重命名的指令进行排序
// 按线程ID对指令进行分类，检查撤销版本
void
IEW::sortInsts()
{
    int insts_from_rename = fromRename->size;  // 来自重命名的指令数量
#ifdef DEBUG
    // 调试模式下确保指令缓冲区为空
    for (ThreadID tid = 0; tid < numThreads; tid++)
        assert(insts[tid].empty());
#endif
    // 处理每个来自重命名的指令
    for (int i = 0; i < insts_from_rename; ++i) {
        const DynInstPtr &inst = fromRename->insts[i];
        // 如果本地撤销版本比指令版本新，标记为撤销
        if (localSquashVer.largerThan(inst->getVersion())) {
            inst->setSquashed();  // 设置指令为已撤销
        }
        insts[fromRename->insts[i]->threadNumber].push_back(inst);
    }
}

// 清空来自重命名的指令
// 移除所有从重命名阶段传来的指令，并更新统计
void
IEW::emptyRenameInsts(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Removing incoming rename instructions\n", tid);

    // 遍历并清空所有来自重命名阶段的指令
    while (!insts[tid].empty()) {
        // 统计加载指令数
        if (insts[tid].front()->isLoad()) {
            toRename->iewInfo[tid].dispatchedToLQ++;
        }
        // 统计存储和原子指令数
        if (insts[tid].front()->isStore() ||
            insts[tid].front()->isAtomic()) {
            toRename->iewInfo[tid].dispatchedToSQ++;
        }

        toRename->iewInfo[tid].dispatched++;  // 统计总分派指令数

        insts[tid].pop_front();  // 移除指令
    }
}

// 唤醒CPU
// 通知CPU有任务需要处理
void
IEW::wakeCPU()
{
    cpu->wakeCPU();  // 唤醒CPU进行处理
}

// 标记本周期有活动
// 通知CPU在这个周期有活动发生
void
IEW::activityThisCycle()
{
    DPRINTF(Activity, "Activity this cycle.\n");
    cpu->activityThisCycle();  // 通知CPU本周期有活动
}

// 激活IEW阶段
// 通知CPU激活IEW阶段
void
IEW::activateStage()
{
    DPRINTF(Activity, "Activating stage.\n");
    cpu->activateStage(CPU::IEWIdx);  // 激活IEW阶段
}

// 去激活IEW阶段
// 通知CPU去激活IEW阶段
void
IEW::deactivateStage()
{
    DPRINTF(Activity, "Deactivating stage.\n");
    cpu->deactivateStage(CPU::IEWIdx);
}

// 分派指定线程的指令
// 将来自重命名阶段的指令分派到指令队列和加载存储队列
void
IEW::dispatch(ThreadID tid)
{
    // 分派逻辑：
    // 如果状态是Running或idle：调用dispatchInsts()
    // 如果状态是Unblocking：
    //     缓存来自重命名的指令
    //     继续尝试清空滑动缓冲区
    //     检查停顿条件是否已经过去

    if (dispatchStatus[tid] == Blocked) {
        ++iewStats.blockCycles;
        setAllStalls(blockReason);

    } else if (dispatchStatus[tid] == Squashing) {
        ++iewStats.squashCycles;
        setAllStalls(StallReason::CommitSquash);
    }

    // Dispatch should try to dispatch as many instructions as its bandwidth
    // will allow, as long as it is not currently blocked.
    if (dispatchStatus[tid] == Running ||
        dispatchStatus[tid] == Idle) {
        DPRINTF(IEW, "[tid:%i] Not blocked, so attempting to run "
                "dispatch.\n", tid);

        dispatchInsts(tid);
    } else if (dispatchStatus[tid] == Unblocking) {
        // Make sure that the skid buffer has something in it if the
        // status is unblocking.
        assert(!skidsEmpty());

        // If the status was unblocking, then instructions from the skid
        // buffer were used.  Remove those instructions and handle
        // the rest of unblocking.
        dispatchInsts(tid);

        ++iewStats.unblockCycles;

        if (fromRename->size != 0) {
            // Add the current inputs to the skid buffer so they can be
            // reprocessed when this stage unblocks.
            skidInsert(tid);
        }

        unblock(tid);
    }
}

void
IEW::dispatchInsts(ThreadID tid)
{
    if (enableDispatchStage) {
        dispatchInstFromDispQue(tid);
        classifyInstToDispQue(tid);
    } else {
        dispatchInstFromRename(tid);
    }
}

void
IEW::dispatchInstFromRename(ThreadID tid)
{
    DynInstPtr inst;

    std::deque<DynInstPtr> &insts_to_dispatch =
    dispatchStatus[tid] == Unblocking ?
    skidBuffer[tid] : insts[tid];

    bool canDispatch = true;

    if ((ldstQueue.getFreeLQEntries(tid) < (renameWidth + lastClockLQPopEntries[tid])) ||
        (ldstQueue.getFreeSQEntries(tid) < (renameWidth + lastClockSQPopEntries[tid]))) {
        canDispatch = false;
    }

    bool emptyROB = fromCommit->commitInfo[tid].emptyROB;

    int insts_to_add = insts_to_dispatch.size();
    std::queue<StallReason> dispatch_stalls;
    StallReason breakDispatch = StallReason::NoStall;

    unsigned dispatched = 0;
    int disp_seq = -1;
    if (canDispatch) {
        scheduler->lookahead(insts_to_dispatch);
        while (!insts_to_dispatch.empty()) {
            bool add_to_iq = false;
            auto &inst = insts_to_dispatch.front();
            disp_seq++;
            int ins = cpu->cpuStats.committedInsts.total();
            if (cpu->hasHintDownStream() && ins % 10000 == 1) {
                cpu->hintDownStream->notifyIns(ins);
            }

            if (inst->isSquashed()) {
                ++iewStats.dispSquashedInsts;
                // Tell Rename That An Instruction has been processed
                if (inst->isLoad()) {
                    toRename->iewInfo[tid].dispatchedToLQ++;
                }
                if (inst->isStore() || inst->isAtomic()) {
                    toRename->iewInfo[tid].dispatchedToSQ++;
                }
                toRename->iewInfo[tid].dispatched++;
                insts_to_dispatch.pop_front();

                dispatch_stalls.push(StallReason::InstSquashed);
                continue;
            }

            if ((inst->isSerializeBefore() && !inst->isSerializeHandled()) ? !emptyROB : false) {
                dispatch_stalls.push(StallReason::SerializeStall);
                breakDispatch = StallReason::SerializeStall;
                blockReason = breakDispatch;
                break;
            }

            // Check LSQ if inst is LD/ST
            if ((inst->isAtomic() && ldstQueue.sqFull(tid)) || (inst->isLoad() && ldstQueue.lqFull(tid)) ||
                (inst->isStore() && ldstQueue.sqFull(tid))) {
                DPRINTF(IEW, "[tid:%i] Dispatch: %s has become full.\n", tid, inst->isLoad() ? "LQ" : "SQ");

                iewStats.stallEvents[LSQFull]++;

                ++iewStats.lsqFullEvents;
                dispatch_stalls.push(checkDispatchStall(tid, NumDQ, inst, disp_seq));
                breakDispatch = dispatch_stalls.back();
                blockReason = breakDispatch;
                break;
            }

            // Check VLMergeBuffer if inst is vector memory operation
            if (inst->isVector() && (inst->isLoad() || inst->isStore()) &&
                ldstQueue.vlMergeBufferBlocked(tid)) {
                DPRINTF(IEW, "[tid:%i] Dispatch: VLMergeBuffer is blocked.\n", tid);

                iewStats.stallEvents[LSQFull]++;

                ++iewStats.lsqFullEvents;
                dispatch_stalls.push(checkDispatchStall(tid, NumDQ, inst, disp_seq));
                breakDispatch = dispatch_stalls.back();
                blockReason = breakDispatch;
                break;
            }

            if (!scheduler->ready(inst, disp_seq)) {
                DPRINTF(IEW, "[tid:%i] Dispatch: IQ is full or bwFull.\n", tid);
                iewStats.stallEvents[IQFull]++;
                ++iewStats.iqFullEvents;

                dispatch_stalls.push(checkDispatchStall(tid, NumDQ, inst, disp_seq));
                breakDispatch = dispatch_stalls.back();
                blockReason = breakDispatch;
                break;
            }

            const int numHtmStarts = ldstQueue.numHtmStarts(tid);
            const int numHtmStops = ldstQueue.numHtmStops(tid);
            const int htmDepth = numHtmStarts - numHtmStops;
            if (htmDepth > 0) {
                inst->setHtmTransactionalState(ldstQueue.getLatestHtmUid(tid), htmDepth);
            } else {
                inst->clearHtmTransactionalState();
            }

            if (!inst->isNop() && !inst->isEliminated()) {
                scheduler->addProducer(inst);
            }

            if (inst->isAtomic()) {
                DPRINTF(IEW,
                        "[tid:%i] Dispatch: Memory instruction "
                        "encountered, adding to LSQ.\n",
                        tid);
                ++iewStats.dispStoreInsts;
                ++iewStats.dispNonSpecInsts;
                toRename->iewInfo[tid].dispatchedToSQ++;

                ldstQueue.insertStore(inst);
                inst->setCanCommit();
                instQueue.insertNonSpec(inst);
                add_to_iq = false;
            } else if (inst->isLoad()) {
                DPRINTF(IEW,
                        "[tid:%i] Dispatch: Memory instruction "
                        "encountered, adding to LSQ.\n",
                        tid);
                ++iewStats.dispLoadInsts;
                toRename->iewInfo[tid].dispatchedToLQ++;

                ldstQueue.insertLoad(inst);
                add_to_iq = true;
            } else if (inst->isStore()) {
                DPRINTF(IEW,
                        "[tid:%i] Dispatch: Memory instruction "
                        "encountered, adding to LSQ.\n",
                        tid);
                ++iewStats.dispStoreInsts;

                ldstQueue.insertStore(inst);
                if (inst->isStoreConditional()) {
                    ++iewStats.dispNonSpecInsts;
                    inst->setCanCommit();
                    instQueue.insertNonSpec(inst);
                    add_to_iq = false;
                } else {
                    add_to_iq = true;
                }
                toRename->iewInfo[tid].dispatchedToSQ++;
            } else if (inst->isReadBarrier() || inst->isWriteBarrier()) {
                inst->setCanCommit();
                instQueue.insertBarrier(inst);
                add_to_iq = false;
            } else if (inst->isNop() || inst->isEliminated()) {
                DPRINTF(IEW,
                        "[tid:%i] Dispatch: Nop instruction [sn:%llu] encountered, "
                        "skipping.\n",
                        tid, inst->seqNum);
                inst->setIssued();
                inst->setExecuted();
                inst->setCanCommit();
                iewStats.executedInstStats.numNop[tid]++;
                add_to_iq = false;
            } else {
                assert(!inst->isExecuted());
                add_to_iq = true;
            }

            if (add_to_iq && inst->isNonSpeculative()) {
                DPRINTF(IEW,
                        "[tid:%i] Dispatch: Nonspeculative instruction "
                        "encountered, skipping.\n",
                        tid);
                inst->setCanCommit();
                instQueue.insertNonSpec(inst);
                add_to_iq = false;
            }

            if (add_to_iq) {
                instQueue.insert(inst, disp_seq);
            }
            ppDispatch->notify(inst);

            toRename->iewInfo[tid].dispatched++;
            ++iewStats.dispatchedInsts;

            insts_to_dispatch.pop_front();
            dispatched++;
        }
    }
    iewStats.dispDist.sample(dispatched);

    if (!dispatch_stalls.empty()) {
        setAllStalls(dispatch_stalls.front());
        dispatch_stalls.pop();
    } else if (breakDispatch != StallReason::NoStall) {
        setAllStalls(breakDispatch);
    } else {
        // no totally stall, pass rename stall
        // assert(dispatched != 0);
        for (int i = 0; i < dispatchStalls.size(); i++) {
            if (i < dispatched) {   // dispatch success, no stall
                dispatchStalls.at(i) = StallReason::NoStall;
            } else {    // dispatch no insts, pass rename stall
                if (fromRename->renameStallReason.size() == 0) {    // initialize, no stall
                    dispatchStalls.at(i) = StallReason::NoStall;
                } else {    // not dispatch initialize, pass rename stall
                    dispatchStalls.at(i) = fromRename->renameStallReason.at(i);
                }
            }
        }
    }


    for (int i = 0;i < dispatchStalls.size();i++) {
        DPRINTF(IEW,"[tid:%i] dispatchStalls[%d]=%d\n", tid, i, dispatchStalls.at(i));
    }

    if (!insts_to_dispatch.empty()) {
        DPRINTF(IEW,"[tid:%i] Dispatch: Bandwidth Full. Blocking.\n", tid);
        block(tid);
        iewStats.stallEvents[DispBWFull]++;
        toRename->iewUnblock[tid] = false;
        disp_stall = true;
    } else {
        disp_stall = false;
    }

    if (dispatchStatus[tid] == Idle && insts_to_add) {
        dispatchStatus[tid] = Running;
        updatedQueues = true;
    }

}

// 将指令分类到分派队列
// 根据指令类型将指令分类并添加到相应的分派队列中
void
IEW::classifyInstToDispQue(ThreadID tid)
{
    std::deque<DynInstPtr> &insts_to_dispatch =
        dispatchStatus[tid] == Unblocking ?
        skidBuffer[tid] : insts[tid];

    bool emptyROB = fromCommit->commitInfo[tid].emptyROB;

    int insts_to_add = insts_to_dispatch.size();
    std::queue<StallReason> dispatch_stalls;
    StallReason breakDispatch = StallReason::NoStall;
    unsigned dispatched = 0;
    while (!insts_to_dispatch.empty()) {
        auto& inst = insts_to_dispatch.front();
        int ins = cpu->cpuStats.committedInsts.total();
        if (cpu->hasHintDownStream() && ins % 10000 == 1) {
            cpu->hintDownStream->notifyIns(ins);
        }
        int id = getInstDQType(inst);
        if (dispQue[id].size() < dqSize[id]) {
            if (inst->isSquashed()) {
                ++iewStats.dispSquashedInsts;
                //Tell Rename That An Instruction has been processed
                if (inst->isLoad()) {
                    toRename->iewInfo[tid].dispatchedToLQ++;
                }
                if (inst->isStore() || inst->isAtomic()) {
                    toRename->iewInfo[tid].dispatchedToSQ++;
                }
                toRename->iewInfo[tid].dispatched++;
                insts_to_dispatch.pop_front();

                dispatch_stalls.push(StallReason::InstSquashed);
                continue;
            }

            if ((inst->isSerializeBefore() && !inst->isSerializeHandled()) ? !emptyROB : false) {
                dispatch_stalls.push(StallReason::SerializeStall);
                breakDispatch = StallReason::SerializeStall;
                blockReason = breakDispatch;
                break;
            }

            // hardware transactional memory
            // CPU needs to track transactional state in program order.
            const int numHtmStarts = ldstQueue.numHtmStarts(tid);
            const int numHtmStops = ldstQueue.numHtmStops(tid);
            const int htmDepth = numHtmStarts - numHtmStops;
            if (htmDepth > 0) {
                inst->setHtmTransactionalState(ldstQueue.getLatestHtmUid(tid),
                                                htmDepth);
            } else {
                inst->clearHtmTransactionalState();
            }

            if (inst->isAtomic()) {
                ++iewStats.dispStoreInsts;
                ++iewStats.dispNonSpecInsts;
                toRename->iewInfo[tid].dispatchedToSQ++;
            } else if (inst->isLoad()) {
                ++iewStats.dispLoadInsts;
                toRename->iewInfo[tid].dispatchedToLQ++;
            } else if (inst->isStore()) {
                ++iewStats.dispStoreInsts;
                if (inst->isStoreConditional()) {
                    ++iewStats.dispNonSpecInsts;
                }
                toRename->iewInfo[tid].dispatchedToSQ++;
            }
            toRename->iewInfo[tid].dispatched++;
            ++iewStats.dispatchedInsts;
            dispQue[id].push_back(inst);

            if (!inst->isNop() && !inst->isEliminated()) {
                scheduler->addProducer(inst);
            }

            inst->enterDQTick = curTick();
            cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtDispQue);

            insts_to_dispatch.pop_front();
            dispatched++;
        } else {
            dispatch_stalls.push(checkDispatchStall(tid, id, inst, -1));
            breakDispatch = dispatch_stalls.back();
            blockReason = breakDispatch;
            break;
        }
    }

    if (!dispatch_stalls.empty()) {
        setAllStalls(dispatch_stalls.front());
        dispatch_stalls.pop();
    } else if (breakDispatch != StallReason::NoStall) {
        setAllStalls(breakDispatch);
    } else {
        // no totally stall, pass rename stall
        // assert(dispatched != 0);
        for (int i = 0; i < dispatchStalls.size(); i++) {
            if (i < dispatched) {   // dispatch success, no stall
                dispatchStalls.at(i) = StallReason::NoStall;
            } else {    // dispatch no insts, pass rename stall
                if (fromRename->renameStallReason.size() == 0) {    // initialize, no stall
                    dispatchStalls.at(i) = StallReason::NoStall;
                } else {    // not dispatch initialize, pass rename stall
                    dispatchStalls.at(i) = fromRename->renameStallReason.at(i);
                }
            }
        }
    }


    for (int i = 0;i < dispatchStalls.size();i++) {
        DPRINTF(IEW,"[tid:%i] dispatchStalls[%d]=%d\n", tid, i, dispatchStalls.at(i));
    }

    if (!insts_to_dispatch.empty()) {
        DPRINTF(IEW,"[tid:%i] Dispatch: Bandwidth Full. Blocking.\n", tid);
        block(tid);
        iewStats.stallEvents[DispBWFull]++;
        toRename->iewUnblock[tid] = false;
        disp_stall = true;
    } else {
        disp_stall = false;
    }

    if (dispatchStatus[tid] == Idle && insts_to_add) {
        dispatchStatus[tid] = Running;
        updatedQueues = true;
    }
}

void
IEW::dispatchInstFromDispQue(ThreadID tid)
{
    DynInstPtr inst;
    bool add_to_iq = false;
    int dis_num_inst = 0;

    for (int i = 0; i < NumDQ; i++) {
        int dispatched = 0;
        int disp_seq = -1;
        scheduler->lookahead(dispQue[i]);
        while (!dispQue[i].empty() && dispatched < dispWidth[i]) {
            inst = dispQue[i].front();
            disp_seq++;

            // Check for squashed instructions.
            if (inst->isSquashed()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Squashed instruction encountered, "
                        "not adding to IQ.\n", tid);

                dispQue[i].pop_front();
                continue;
            }

            // Check for ready conditions.(ready: !full && !bwFull )
            if (!scheduler->ready(inst, disp_seq)) {
                DPRINTF(IEW, "[tid:%i] Dispatch: IQ is full or bwFull.\n", tid);

                iewStats.stallEvents[IQFull]++;
                ++iewStats.iqFullEvents;
                break;
            }

            // Check LSQ if inst is LD/ST
            if ((inst->isAtomic() && ldstQueue.sqFull(tid)) ||
                (inst->isLoad() && ldstQueue.lqFull(tid)) ||
                (inst->isStore() && ldstQueue.sqFull(tid))) {
                DPRINTF(IEW, "[tid:%i] Dispatch: %s has become full.\n",tid,
                        inst->isLoad() ? "LQ" : "SQ");

                iewStats.stallEvents[LSQFull]++;

                ++iewStats.lsqFullEvents;
                break;
            }

            // Check VLMergeBuffer if inst is vector memory operation
            if (inst->isVector() && (inst->isLoad() || inst->isStore()) &&
                ldstQueue.vlMergeBufferBlocked(tid)) {
                DPRINTF(IEW, "[tid:%i] Dispatch: VLMergeBuffer is blocked.\n", tid);

                iewStats.stallEvents[LSQFull]++;

                ++iewStats.lsqFullEvents;
                break;
            }

            // Otherwise issue the instruction just fine.
            if (inst->isAtomic()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Memory instruction "
                        "encountered, adding to LSQ.\n", tid);

                // allocate entry in store queue
                ldstQueue.insertStore(inst);

                // AMOs need to be set as "canCommit()"
                // so that commit can process them when they reach the
                // head of commit.
                inst->setCanCommit();
                instQueue.insertNonSpec(inst);
                add_to_iq = false;
            } else if (inst->isLoad()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Memory instruction "
                        "encountered, adding to LSQ.\n", tid);

                // allocate entry in load queue
                ldstQueue.insertLoad(inst);

                add_to_iq = true;

            } else if (inst->isStore()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Memory instruction "
                        "encountered, adding to LSQ.\n", tid);

                // allocate entry in store queue
                ldstQueue.insertStore(inst);

                if (inst->isStoreConditional()) {
                    // Store conditionals need to be set as "canCommit()"
                    // so that commit can process them when they reach the
                    // head of commit.
                    // @todo: This is somewhat specific to Alpha.
                    inst->setCanCommit();
                    instQueue.insertNonSpec(inst);
                    add_to_iq = false;

                } else {
                    add_to_iq = true;
                }
            } else if (inst->isReadBarrier() || inst->isWriteBarrier()) {
                // Same as non-speculative stores.
                inst->setCanCommit();
                instQueue.insertBarrier(inst);
                add_to_iq = false;
            } else if (inst->isNop() || inst->isEliminated()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Nop instruction [sn:%llu] encountered, "
                        "skipping.\n", tid, inst->seqNum);

                inst->setIssued();
                inst->setExecuted();
                inst->setCanCommit();

                iewStats.executedInstStats.numNop[tid]++;

                add_to_iq = false;
            } else {
                assert(!inst->isExecuted());
                add_to_iq = true;
            }

            if (add_to_iq && inst->isNonSpeculative()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Nonspeculative instruction "
                        "encountered, skipping.\n", tid);

                // Same as non-speculative stores.
                inst->setCanCommit();

                // Specifically insert it as nonspeculative.
                instQueue.insertNonSpec(inst);

                add_to_iq = false;
            }

            // If the instruction queue is not full, then add the
            // instruction.
            if (add_to_iq) {
                DPRINTF(IEW, "[tid:%i] Dispatch: [sn:%llu] dispatched to IQ.\n", tid, inst->seqNum);
                instQueue.insert(inst, disp_seq);
            }
            ++dis_num_inst;

            inst->exitDQTick = curTick();

    #if TRACING_ON
            inst->dispatchTick = curTick() - inst->fetchTick;
    #endif
            ppDispatch->notify(inst);

            dispQue[i].pop_front();
            dispatched++;
        }
    }
    iewStats.dispDist.sample(dis_num_inst);
}

void
IEW::printAvailableInsts()
{
    int inst = 0;

    std::cout << "Available Instructions: ";

    while (fromIssue->insts[inst]) {

        if (inst%3==0) std::cout << "\n\t";

        std::cout << "PC: " << fromIssue->insts[inst]->pcState()
             << " TN: " << fromIssue->insts[inst]->threadNumber
             << " SN: " << fromIssue->insts[inst]->seqNum << " | ";

        inst++;

    }

    std::cout << "\n";
}

void
IEW::SquashCheckAfterExe(DynInstPtr inst)
{
    ThreadID tid = inst->threadNumber;

    if (!fetchRedirect[tid] ||
        !execWB->squash[tid] ||
        execWB->squashedSeqNum[tid] > inst->seqNum) {

        // Prevent testing for misprediction on load instructions,
        // that have not been executed.
        bool loadNotExecuted = !inst->isExecuted() && inst->isLoad();

        if (inst->mispredicted() && !loadNotExecuted) {
            fetchRedirect[tid] = true;

            DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                    "Branch mispredict detected.\n",
                    tid, inst->seqNum);
            DPRINTF(IEW, "[tid:%i] [sn:%llu] "
                    "Predicted target was PC: %s\n",
                    tid, inst->seqNum, inst->readPredTarg());
            DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                    "Redirecting fetch to PC: %s\n",
                    tid, inst->seqNum, inst->pcState());
            // If incorrect, then signal the ROB that it must be squashed.
            squashDueToBranch(inst, tid);

            ppMispredict->notify(inst);

            if (inst->readPredTaken()) {
                iewStats.predictedTakenIncorrect++;
            } else {
                iewStats.predictedNotTakenIncorrect++;
            }
        } else if (ldstQueue.violation(tid)) {
            assert(inst->isMemRef());
            // If there was an ordering violation, then get the
            // DynInst that caused the violation.  Note that this
            // clears the violation signal.
            DynInstPtr violator;
            violator = ldstQueue.getMemDepViolator(tid);

            DPRINTF(IEW, "LDSTQ detected a violation. Violator PC: %s "
                    "[sn:%lli], inst PC: %s [sn:%lli]. Addr is: %#x.\n",
                    violator->pcState(), violator->seqNum,
                    inst->pcState(), inst->seqNum, inst->physEffAddr);

            fetchRedirect[tid] = true;

            // Tell the instruction queue that a violation has occured.
            if (enableStoreSetTrain) {
                instQueue.violation(inst, violator);
            }

            // Squash.
            squashDueToMemOrder(violator, tid);

            ++iewStats.memOrderViolationEvents;
        }
    } else {
        // Reset any state associated with redirects that will not
        // be used.
        if (ldstQueue.violation(tid)) {
            assert(inst->isMemRef());

            DynInstPtr violator = ldstQueue.getMemDepViolator(tid);

            DPRINTF(IEW, "LDSTQ detected a violation.  Violator PC: "
                    "%s, inst PC: %s.  Addr is: %#x.\n",
                    violator->pcState(), inst->pcState(),
                    inst->physEffAddr);
            DPRINTF(IEW, "Violation will not be handled because "
                    "already squashing\n");

            ++iewStats.memOrderViolationEvents;
        }
    }
}

// 执行指令
// 从功能单元获取已完成的指令并处理写回
void
IEW::executeInsts()
{
    // 初始化写回计数器
    wbNumInst = 0;  // 本周期写回指令数
    wbCycle = 0;    // 写回周期计数

    // 遍历所有活跃线程
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    // 重置每个线程的取指重定向标志
    while (threads != end) {
        ThreadID tid = *threads++;
        fetchRedirect[tid] = false;  // 清除取指重定向标志
    }

    // Uncomment this if you want to see all available instructions.
    // @todo This doesn't actually work anymore, we should fix it.
//    printAvailableInsts();

    // Execute/writeback any instructions that are available.
    int insts_to_execute = fromIssue->size;
    fromIssue->size = 0;
    int inst_num = 0;
    for (; inst_num < insts_to_execute;
          ++inst_num) {

        DPRINTF(IEW, "Execute: Executing instructions from IQ.\n");

        DynInstPtr inst = instQueue.getInstToExecute();

        DPRINTF(IEW, "Execute: Processing PC %s, [tid:%i] [sn:%llu].\n",
                inst->pcState(), inst->threadNumber,inst->seqNum);

        // Notify potential listeners that this instruction has started
        // executing
        ppExecute->notify(inst);

        // Check if the instruction is squashed; if so then skip it
        if (inst->isSquashed()) {
            DPRINTF(IEW, "Execute: Instruction was squashed. PC: %s, [tid:%i]"
                         " [sn:%llu]\n", inst->pcState(), inst->threadNumber,
                         inst->seqNum);

            // Consider this instruction executed so that commit can go
            // ahead and retire the instruction.
            inst->setExecuted();

            // Not sure if I should set this here or just let commit try to
            // commit any squashed instructions.  I like the latter a bit more.
            inst->setCanCommit();

            ++iewStats.executedInstStats.numSquashedInsts;

            continue;
        }

        Fault fault = NoFault;

        // Execute instruction.
        // Note that if the instruction faults, it will be handled
        // at the commit stage.
        if (inst->isMemRef()) {
            DPRINTF(IEW, "Execute: Calculating address for memory "
                    "reference.\n");

            // Tell the LDSTQ to execute this instruction (if it is a load).
            if (inst->isAtomic()) {
                // AMOs are treated like store requests
                fault = ldstQueue.executeAmo(inst);

                if (inst->isTranslationDelayed() &&
                    fault == NoFault) {
                    // A hw page table walk is currently going on; the
                    // instruction must be deferred.
                    DPRINTF(IEW, "Execute: Delayed translation, deferring "
                            "store.\n");
                    deferMemInst(inst);
                    continue;
                }
            } else if (inst->isLoad()) {
                // add this load inst to loadpipe S0.
                ldstQueue.issueToLoadPipe(inst);
            } else if (inst->isStore()) {
                // add this store inst to storepipe S0.
                ldstQueue.issueToStorePipe(inst);

                // Store conditionals will mark themselves as
                // executed, and their writeback event will add the
                // instruction to the queue to commit.
            } else {
                panic("Unexpected memory type!\n");
            }

        } else {
            // If the instruction has already faulted, then skip executing it.
            // Such case can happen when it faulted during ITLB translation.
            // If we execute the instruction (even if it's a nop) the fault
            // will be replaced and we will lose it.
            if (inst->getFault() == NoFault) {
                inst->execute();
                if (!inst->readPredicate())
                    inst->forwardOldRegs();
            }

            if (!inst->isSplitStoreData()) {
                inst->setExecuted();
                readyToFinish(inst);
            } else {
                DPRINTF(IEW, "Execute: Split store data, [sn:%lli]\n", inst->seqNum);
                // STD is ready, wake up corresponding load if any
                instQueue.resolveSTLFFailInst(inst->seqNum);
                if (inst->sqIt->splitStoreFinish()) {
                    readyToFinish(inst->sqIt->instruction());
                }
            }
        }

        updateExeInstStats(inst);

        if (debug::IEW) {
            inst->printDisassemblyAndResult(cpu->name());
        }

        // Check if branch prediction was correct, if not then we need
        // to tell commit to squash in flight instructions.  Only
        // handle this if there hasn't already been something that
        // redirects fetch in this group of instructions.

        // This probably needs to prioritize the redirects if a different
        // scheduler is used.  Currently the scheduler schedules the oldest
        // instruction first, so the branch resolution order will be correct.
        if (!(inst->isLoad() || inst->isStore() || inst->isSplitStoreData())) {
            // because Load/Store become pipeline execution ,Load/Store will
            // call this in `lsq_unit.cc` after execution
            SquashCheckAfterExe(inst);
        }
    }

    ldstQueue.executePipeSx();

    // Update and record activity if we processed any instructions.
    if (inst_num) {
        if (exeStatus == Idle) {
            exeStatus = Running;
        }

        updatedQueues = true;

        cpu->activityThisCycle();
    }

    // Need to reset this in case a writeback event needs to write into the
    // iew queue.  That way the writeback event will write into the correct
    // spot in the queue.
    wbNumInst = 0;

}

// 写回已完成执行的指令
// 将执行完成的指令写回到重命名阶段并更新计分板
void
IEW::writebackInsts()
{
    // Loop through the head of the time buffer and wake any
    // dependents.  These instructions are about to write back.  Also
    // mark scoreboard that this instruction is finally complete.
    // Either have IEW have direct access to scoreboard, or have this
    // as part of backwards communication.

    for (int inst_num = 0; inst_num < wbWidth &&
             execWB->insts[inst_num]; inst_num++) {
        DynInstPtr inst = execWB->insts[inst_num];
        ThreadID tid = inst->threadNumber;

        if (inst->savedRequest && inst->isLoad()) {
            inst->pf_source = inst->savedRequest->mainReq()->getPFSource();
        }

        DPRINTF(IEW, "Sending instructions to commit, [sn:%lli] PC %s.\n",
                inst->seqNum, inst->pcState());

        iewStats.instsToCommit[tid]++;
        // Notify potential listeners that execution is complete for this
        // instruction.
        ppToCommit->notify(inst);

        // Some instructions will be sent to commit without having
        // executed because they need commit to handle them.
        // E.g. Strictly ordered loads have not actually executed when they
        // are first sent to commit.  Instead commit must tell the LSQ
        // when it's ready to execute the strictly ordered load.
        if (!inst->isSquashed() && inst->isExecuted() &&
                inst->getFault() == NoFault) {

            scheduler->writebackWakeup(inst);
            int dependents = instQueue.wakeDependents(inst);

            for (int i = 0; i < inst->numDestRegs(); i++) {
                // Mark register as ready if not pinned
                if (inst->renamedDestIdx(i)->
                        getNumPinnedWritesToComplete() == 0) {
                    DPRINTF(IEW,"Setting Destination Register %i (%s)\n",
                            inst->renamedDestIdx(i)->index(),
                            inst->renamedDestIdx(i)->className());
                    scoreboard->setReg(inst->renamedDestIdx(i));
                }
            }

            if (dependents) {
                iewStats.producerInst[tid]++;
                iewStats.consumerInst[tid]+= dependents;
            }
            iewStats.writebackCount[tid]++;
        }
    }
}

// IEW阶段时钟周期处理
// IEW阶段每个周期的主要处理逻辑
void
IEW::tick()
{
    // 统计各阶段的停顿原因
    for (int i = 0;i < fromRename->fetchStallReason.size();i++) {
        iewStats.fetchStallReason[fromRename->fetchStallReason[i]]++;
    }

    for (int i = 0;i < fromRename->decodeStallReason.size();i++) {
        iewStats.decodeStallReason[fromRename->decodeStallReason[i]]++;
    }

    for (int i = 0;i < fromRename->renameStallReason.size();i++) {
        iewStats.renameStallReason[fromRename->renameStallReason[i]]++;
    }

    // 初始化本周期的写回计数器
    wbNumInst = 0;  // 写回指令数
    wbCycle = 0;    // 写回周期

    // 重置通信标志
    wroteToTimeBuffer = false;  // 是否写入时间缓冲区
    updatedQueues = false;      // 是否更新了队列

    // 调用调度器和加载存储队列的时钟处理
    scheduler->tick();
    ldstQueue.tick();

    // 对指令进行排序
    sortInsts();

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    // Check stall and squash signals, dispatch any instructions.
    while (threads != end) {
        ThreadID tid = *threads++;

        DPRINTF(IEW,"Issue: Processing [tid:%i]\n",tid);
        lastClockLQPopEntries[tid] = ldstQueue.getAndResetLastLQPopEntries(tid);
        lastClockSQPopEntries[tid] = ldstQueue.getAndResetLastSQPopEntries(tid);
        checkSignalsAndUpdate(tid);
        dispatch(tid);

        toRename->iewInfo[tid].robHeadStallReason = checkDispatchStall(tid, NumDQ, nullptr, -1);
        toRename->iewInfo[tid].lqHeadStallReason =
            ldstQueue.lqEmpty() ? StallReason::NoStall : checkLSQStall(tid, true);
        toRename->iewInfo[tid].sqHeadStallReason =
            ldstQueue.sqEmpty() ? StallReason::NoStall : checkLSQStall(tid, false);
        toRename->iewInfo[tid].blockReason = blockReason;
    }
    for (int i = 0;i < dispatchStalls.size();i++) {
        iewStats.dispatchStallReason[dispatchStalls[i]]++;
    }

    if (exeStatus != Squashing) {
        instQueue.scheduleReadyInsts();

        executeInsts();

        writebackInsts();
    }

    scheduler->issueAndSelect();

    bool broadcast_free_entries = false;

    if (updatedQueues || exeStatus == Running || updateLSQNextCycle) {
        exeStatus = Idle;
        updateLSQNextCycle = false;

        broadcast_free_entries = true;
    }

    // Writeback any stores using any leftover bandwidth.
    ldstQueue.writebackStoreBuffer();

    // Check the committed load/store signals to see if there's a load
    // or store to commit.  Also check if it's being told to execute a
    // nonspeculative instruction.
    // This is pretty inefficient...

    threads = activeThreads->begin();
    while (threads != end) {
        ThreadID tid = (*threads++);

        DPRINTF(IEW,"Processing [tid:%i]\n",tid);

        if (fromCommit->commitInfo[tid].doneMemSeqNum != 0 &&
            !fromCommit->commitInfo[tid].squash &&
            !fromCommit->commitInfo[tid].robSquashing) {

            // Marks some of the entries in the store queue as canWB and
            // they will be moved to the store buffer when appropriate.
            ldstQueue.commitStores(fromCommit->commitInfo[tid].doneMemSeqNum,tid);
            updateLSQNextCycle = true;
        }

        // Update structures based on instructions committed.
        if (fromCommit->commitInfo[tid].doneSeqNum != 0 &&
            !fromCommit->commitInfo[tid].squash &&
            !fromCommit->commitInfo[tid].robSquashing) {

            ldstQueue.commitLoads(fromCommit->commitInfo[tid].doneSeqNum,tid);
            updateLSQNextCycle = true;

            instQueue.commit(fromCommit->commitInfo[tid].doneSeqNum,tid);
        }

        if (fromCommit->commitInfo[tid].nonSpecSeqNum != 0) {

            //DPRINTF(IEW,"NonspecInst from thread %i",tid);
            if (fromCommit->commitInfo[tid].strictlyOrdered) {
                instQueue.replayMemInst(
                    fromCommit->commitInfo[tid].strictlyOrderedLoad);
                fromCommit->commitInfo[tid].strictlyOrderedLoad->setAtCommit();
            } else {
                instQueue.scheduleNonSpec(
                    fromCommit->commitInfo[tid].nonSpecSeqNum);
            }
        }

        if (broadcast_free_entries) {
            toFetch->iewInfo[tid].ldstqCount =
                ldstQueue.getCount(tid);

            toRename->iewInfo[tid].usedIQ = true;
            toRename->iewInfo[tid].usedLSQ = true;

            toRename->iewInfo[tid].freeLQEntries =
                ldstQueue.numFreeLoadEntries(tid);
            toRename->iewInfo[tid].freeSQEntries =
                ldstQueue.numFreeStoreEntries(tid);

            wroteToTimeBuffer = true;
        }

        DPRINTF(IEW, "[tid:%i], Dispatch dispatched %i instructions.\n",
                tid, toRename->iewInfo[tid].dispatched);
    }

    DPRINTF(IEW,"LQ has %i free entries. SQ has %i free entries.\n",
            ldstQueue.numFreeLoadEntries(), ldstQueue.numFreeStoreEntries());

    updateStatus();

    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }
}

// 更新执行指令统计信息
// 根据指令类型更新相应的执行统计计数器
void
IEW::updateExeInstStats(const DynInstPtr& inst)
{
    ThreadID tid = inst->threadNumber;

    // 总执行指令数加1
    iewStats.executedInstStats.numInsts++;

    // 控制指令统计
    if (inst->isControl())
        iewStats.executedInstStats.numBranches[tid]++;  // 分支指令数

    // 内存操作指令统计
    if (inst->isMemRef()) {
        iewStats.executedInstStats.numRefs[tid]++;      // 内存引用指令数

        if (inst->isLoad()) {
            iewStats.executedInstStats.numLoadInsts[tid]++;
        }
    }
}

// 检查分支预测错误
// 检测分支指令是否被错误预测，如果是则触发撤销操作
void
IEW::checkMisprediction(const DynInstPtr& inst)
{
    ThreadID tid = inst->threadNumber;

    // 如果当前没有重定向或者当前指令序列号更大，则检查预测错误
    if (!fetchRedirect[tid] ||
        !execWB->squash[tid] ||
        execWB->squashedSeqNum[tid] > inst->seqNum) {

        if (inst->mispredicted()) {
            fetchRedirect[tid] = true;  // 设置取指重定向标志

            DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                    "Branch mispredict detected.\n",
                    tid, inst->seqNum);
            DPRINTF(IEW, "[tid:%i] [sn:%llu] Predicted target was PC: %s\n",
                    tid, inst->seqNum, inst->readPredTarg());
            DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                    "Redirecting fetch to PC: %s\n",
                    tid, inst->seqNum, inst->pcState());
            // 如果预测错误，通知ROB执行撤销操作
            squashDueToBranch(inst, tid);

            // 更新预测错误统计
            if (inst->readPredTaken()) {
                iewStats.predictedTakenIncorrect++;     // 错误预测为跳转
            } else {
                iewStats.predictedNotTakenIncorrect++;  // 错误预测为不跳转
            }
        }
    }
}

// 取消加载指令
// 当加载指令发生缓存缺失时取消其依赖指令
void
IEW::loadCancel(const DynInstPtr &inst)
{
    scheduler->loadCancel(inst);  // 通知调度器取消加载指令
}

// 存储到加载转发失败重放
// 当存储到加载转发失败时重放加载指令
void
IEW::stlfFailLdReplay(const DynInstPtr &inst, const InstSeqNum &store_seq_num)
{
    instQueue.stlfFailLdReplay(inst, store_seq_num);  // 通知指令队列处理STLF失败
}

// 获取指令队列中的指令数
// 返回当前指令队列中的指令总数
uint32_t
IEW::getIQInsts()
{
    return scheduler->getIQInsts();  // 从调度器获取指令数
}

// 设置所有停顿原因
// 将所有分派停顿原因设置为指定的停顿类型
void
IEW::setAllStalls(StallReason dispatchStall)
{
    // 设置所有分派位置的停顿原因
    for (int i = 0;i < dispatchStalls.size();i++) {
        dispatchStalls.at(i) = dispatchStall;
    }
}

// 检查加载存储指令状态
// 检查指令的各种状态，返回相应的停顿原因
StallReason
IEW::checkLoadStoreInst(DynInstPtr inst)
{
    // 检查指令是否已被撤销
    if (inst->isSquashed()) {
        return StallReason::MemSquashed;  // 内存指令被撤销
    }
    // 检查指令是否已提交
    if (inst->isCommitted()) {
        return StallReason::MemCommitRateLimit;  // 内存指令提交速率限制
    }
    if (inst->isAtomic() || inst->isStoreConditional()) {
        return StallReason::Atomic;
    }
    if (!inst->readyToIssue()){
        return StallReason::MemNotReady;
    }
    assert(inst->isLoad() || inst->isStore());

    if (inst->isIssued() && inst->translationStarted() && !inst->translationCompleted()) {
        return StallReason::DTlbStall;
    }

    bool inFlight = inst->isIssued() && inst->hasPendingCacheReq();
    bool lsuStall = inst->isIssued() && !inst->hasPendingCacheReq();
    //Level of the cache hierachy where this request was responded to
    //e.g. 0:in l1, 1:in l2
    int depth=-1;
    if (inFlight) {
        assert(inst->pendingCacheReq);
        depth = inst->pendingCacheReq->mainReq()->depth;
    }
    assert(depth < 5);
    bool in_l1 = depth == 0;
    bool in_l2 = depth == 1;
    bool in_l3 = depth == 2;
    bool other_stall = depth == -1;
    // maybe soc does not have l3cache
    // so we can not use in_mem = depth==3
    bool in_mem = !(in_l1 ||  in_l2 || in_l3 || other_stall);
    if (inFlight && in_l1) {
        return inst->isLoad() ? StallReason::LoadL1Bound : StallReason::StoreL1Bound;
    } else if (inFlight && in_l2) {
        return inst->isLoad() ? StallReason::LoadL2Bound : StallReason::StoreL2Bound;
    } else if (inFlight && in_l3) {
        return inst->isLoad() ? StallReason::LoadL3Bound : StallReason::StoreL3Bound;
    } else if (inFlight && in_mem) {
        return inst->isLoad() ? StallReason::LoadMemBound : StallReason::StoreMemBound;
    } else if (inFlight && other_stall) {
        return StallReason::OtherMemStall;
    }

    if (lsuStall) {
        return inst->isLoad() ? StallReason::LoadL1Bound : StallReason::StoreL1Bound;
    } else {
        return StallReason::OtherMemStall;
    }
}

StallReason
IEW::dqTypeToReason(DQType dq_type)
{
    switch (dq_type) {
        case DQType::IntDQ:
            return StallReason::IntDQBandwidth;
        case DQType::MemDQ:
            return StallReason::MemDQBandwidth;
        case DQType::FVDQ:
            return StallReason::FVDQBandwidth;
        default:
            panic("Unknown DQType");
    }
}

IEW::DQType
IEW::getInstDQType(const DynInstPtr &inst)
{
    if (inst->isMemRef() || inst->isReadBarrier() || inst->isWriteBarrier() || inst->isNonSpeculative()) {
        return MemDQ;
    }
    if (inst->isFloating() || inst->isVector()) {
        return FVDQ;
    }
    return IntDQ;
}

StallReason
IEW::checkDispatchStall(ThreadID tid, int dq_stall, const DynInstPtr &dispatch_inst, int disp_seq) {
    DynInstPtr head_inst = rob->readHeadInst(tid);
    if (head_inst == rob->dummyInst) {
        if (dq_stall != NumDQ) {
            // this call is from dispatch to classify the reason why an instr cannot be dispatched
            return dqTypeToReason(static_cast<DQType>(dq_stall));
        } else {  // this call is to tell rename the stall status
            return StallReason::NoStall;
        }
    }

    if (dq_stall != NumDQ && disp_seq >= 0) {
        // get dq head inst
        assert(dispQue[dq_stall].size());
        auto &dq_head = dispQue[dq_stall].front();
        bool ready = !scheduler->ready(dq_head, disp_seq);
        if (getInstDQType(dispatch_inst) == getInstDQType(dq_head) && !ready) {
            return dqTypeToReason(static_cast<DQType>(dq_stall));
        }

        if (dispatch_inst->isStore() && !ldstQueue.sqFull()) {
            // store cannot be dispatched while sq is not full
            return StallReason::MemDQBandwidth;
        }
        if (dispatch_inst->isLoad() && !(ldstQueue.lqFull() || rob->isFull() || !ready)) {
            return StallReason::MemDQBandwidth;
        }
        if (dispatch_inst->isAtCommit() && !(ldstQueue.lqFull() || ldstQueue.sqFull())) {
            return StallReason::MemDQBandwidth;
        }

        if ((dispatch_inst->isFloating() || dispatch_inst->isVector()) &&
            !(rob->isFull() || !ready)) {
            return StallReason::FVDQBandwidth;
        }

        if (dispatch_inst->isInteger() && !(rob->isFull() || !ready)) {
            return StallReason::IntDQBandwidth;
        }
    }

    assert(head_inst);

    if (head_inst->readyTick == -1) {
        DPRINTF(Counters, "IEW: [tid:%i] [sn:%llu] "
                "Dispatch: Instruction not ready. nonSpeculative:%d\n",
                tid, head_inst->seqNum, head_inst->isNonSpeculative());
        if (head_inst->isNonSpeculative()) {
            return StallReason::SerializeStall;
        } else if (head_inst->isLoad() && ldstQueue.lqFull(tid)) {
            return checkLSQStall(tid, true);
        } else if ((head_inst->isStore() || head_inst->isAtomic()) && ldstQueue.sqFull(tid)) {
            return checkLSQStall(tid, false);
        } else {
            return StallReason::InstNotReady;
        }
    }

    if (head_inst->isLoad() || head_inst->isStore() || head_inst->isAtomic()) {
        return checkLoadStoreInst(head_inst);
    } else {
        if (head_inst->firstIssue != -1) {
            if (head_inst->isVector()) {
                return StallReason::VectorLongExecute;
            } else {
                return StallReason::ScalarLongExecute;
            }
        } else {
            if (head_inst->isVector()) {
                return StallReason::VectorReadyButNotIssued;
            } else {
                return StallReason::ScalarReadyButNotIssued;
            }
        }
    }

    return StallReason::OtherStall;
}

StallReason
IEW::checkLSQStall(ThreadID tid, bool isLoad)
{
    DynInstPtr head_inst = ldstQueue.getLSQHeadInst(tid, isLoad);
    return checkLoadStoreInst(head_inst);
}

void
IEW::setRob(ROB *rob)
{
    this->rob = rob;
}

} // namespace o3
} // namespace gem5

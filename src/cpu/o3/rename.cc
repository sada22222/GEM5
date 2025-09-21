/*
 * Copyright (c) 2010-2012, 2014-2019 ARM Limited
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

#include "cpu/o3/rename.hh"

/**
 * @file rename.cc
 *
 * O3（乱序执行）CPU流水线中重命名阶段的实现。
 * 重命名阶段是现代乱序处理器的关键组件，主要职责包括：
 * - 寄存器重命名：消除WAR和WAW冲突，支持乱序执行
 * - ROB（重排序缓冲区）管理：分配ROB条目跟踪指令完成状态
 * - 物理寄存器分配：将架构寄存器映射到物理寄存器
 * - 资源管理：管理各种执行资源（整数/浮点/向量寄存器等）
 * - 串行化处理：处理需要串行执行的特殊指令
 * - 流水线控制：处理停顿、阻塞和清空信号
 *
 * 该阶段位于译码和发射之间，通过寄存器重命名技术显著提升
 * 指令级并行性，是实现高性能乱序执行的核心。
 *
 * XS-GEM5特殊功能：
 * - 支持RISC-V向量扩展的寄存器重命名
 * - 与Xiangshan处理器架构的物理寄存器文件集成
 * - 针对RISC-V指令集的特殊优化
 */

#include <list>

#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/reg_class.hh"
#include "debug/Activity.hh"
#include "debug/O3PipeView.hh"
#include "debug/Rename.hh"
#include "debug/Counters.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

/**
 * 重命名阶段构造函数
 * 初始化重命名阶段的各种参数和状态，包括延迟、宽度、线程管理等
 *
 * @param _cpu 指向CPU对象的指针
 * @param params O3 CPU的参数对象，包含各种配置参数
 */
Rename::Rename(CPU *_cpu, const BaseO3CPUParams &params)
    : cpu(_cpu),
      iewToRenameDelay(params.iewToRenameDelay),           // IEW到重命名的延迟
      decodeToRenameDelay(params.decodeToRenameDelay),     // 译码到重命名的延迟
      commitToRenameDelay(params.commitToRenameDelay),     // 提交到重命名的延迟
      renameWidth(params.renameWidth),                     // 重命名宽度（每周期可处理的指令数）
      releaseWidth(params.phyregReleaseWidth),             // 物理寄存器释放宽度
      numThreads(params.numThreads),                       // 线程数量
      stats(_cpu)                                           // 统计信息对象
{
    // 检查重命名宽度是否超过编译时限制
    if (renameWidth > MaxWidth)
        fatal("renameWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             renameWidth, static_cast<int>(MaxWidth));

    // 计算缓冲区最大大小（基于译码到重命名的延迟和译码宽度）
    // TODO: 将此设为参数
    skidBufferMax = (decodeToRenameDelay + 1) * params.decodeWidth * 2;

    // 初始化每个线程的状态和资源
    for (uint32_t tid = 0; tid < MaxThreads; tid++) {
        renameStatus[tid] = Idle;             // 重命名状态设为空闲
        renameMap[tid] = nullptr;             // 重命名映射表指针
        instsInProgress[tid] = 0;             // 正在处理的指令数
        loadsInProgress[tid] = 0;             // 正在处理的加载指令数
        storesInProgress[tid] = 0;            // 正在处理的存储指令数
        freeEntries[tid] = {0, 0, 0};         // 各类型空闲条目数
        emptyROB[tid] = true;                 // ROB空闲标志
        stalls[tid] = {false, false};         // 停顿状态
        serializeInst[tid] = nullptr;         // 串行化指令指针
        serializeOnNextInst[tid] = false;     // 下一条指令串行化标志
    }

    // 初始化重命名停顿状态数组
    renameStalls.resize(renameWidth, StallReason::NoStall);
}

/**
 * 获取重命名阶段的名称
 * 返回由CPU名称和".rename"组成的字符串
 *
 * @return 重命名阶段的完整名称
 */
std::string
Rename::name() const
{
    return cpu->name() + ".rename";
}

Rename::RenameStats::RenameStats(statistics::Group *parent)
    : statistics::Group(parent, "rename"),
      ADD_STAT(squashCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is squashing"),
      ADD_STAT(idleCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is idle"),
      ADD_STAT(blockCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is blocking"),
      ADD_STAT(serializeStallCycles, statistics::units::Cycle::get(),
               "count of cycles rename stalled for serializing inst"),
      ADD_STAT(runCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is running"),
      ADD_STAT(unblockCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is unblocking"),
      ADD_STAT(renamedInsts, statistics::units::Count::get(),
               "Number of instructions processed by rename"),
      ADD_STAT(squashedInsts, statistics::units::Count::get(),
               "Number of squashed instructions processed by rename"),
      ADD_STAT(ROBFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to ROB full"),
      ADD_STAT(IQFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to IQ full"),
      ADD_STAT(LQFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to LQ full" ),
      ADD_STAT(SQFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to SQ full"),
      ADD_STAT(fullRegistersEvents, statistics::units::Count::get(),
               "Number of times there has been no free registers"),
      ADD_STAT(renamedOperands, statistics::units::Count::get(),
               "Number of destination operands rename has renamed"),
      ADD_STAT(lookups, statistics::units::Count::get(),
               "Number of register rename lookups that rename has made"),
      ADD_STAT(intLookups, statistics::units::Count::get(),
               "Number of integer rename lookups"),
      ADD_STAT(fpLookups, statistics::units::Count::get(),
               "Number of floating rename lookups"),
      ADD_STAT(vecLookups, statistics::units::Count::get(),
               "Number of vector rename lookups"),
      ADD_STAT(vecPredLookups, statistics::units::Count::get(),
               "Number of vector predicate rename lookups"),
      ADD_STAT(committedMaps, statistics::units::Count::get(),
               "Number of HB maps that are committed"),
      ADD_STAT(undoneMaps, statistics::units::Count::get(),
               "Number of HB maps that are undone due to squashing"),
      ADD_STAT(serializing, statistics::units::Count::get(),
               "count of serializing insts renamed"),
      ADD_STAT(tempSerializing, statistics::units::Count::get(),
               "count of temporary serializing insts renamed"),
      ADD_STAT(skidInsts, statistics::units::Count::get(),
               "count of insts added to the skid buffer"),
      ADD_STAT(moveEliminated, statistics::units::Count::get(),
               "count of insts eliminated by move elimination"),
      ADD_STAT(constantFolded, statistics::units::Count::get(),
               "count of insts eliminated by constant folding"),
      ADD_STAT(stallEvents, statistics::units::Count::get(),
               "count of stall events")
{
    squashCycles.prereq(squashCycles);
    idleCycles.prereq(idleCycles);
    blockCycles.prereq(blockCycles);
    serializeStallCycles.flags(statistics::total);
    runCycles.prereq(idleCycles);
    unblockCycles.prereq(unblockCycles);

    renamedInsts.prereq(renamedInsts);
    squashedInsts.prereq(squashedInsts);

    ROBFullEvents.prereq(ROBFullEvents);
    IQFullEvents.prereq(IQFullEvents);
    LQFullEvents.prereq(LQFullEvents);
    SQFullEvents.prereq(SQFullEvents);
    fullRegistersEvents.prereq(fullRegistersEvents);

    renamedOperands.prereq(renamedOperands);
    lookups.prereq(lookups);
    intLookups.prereq(intLookups);
    fpLookups.prereq(fpLookups);
    vecLookups.prereq(vecLookups);
    vecPredLookups.prereq(vecPredLookups);

    committedMaps.prereq(committedMaps);
    undoneMaps.prereq(undoneMaps);
    serializing.flags(statistics::total);
    tempSerializing.flags(statistics::total);
    skidInsts.flags(statistics::total);
    moveEliminated.flags(statistics::total);
    constantFolded.flags(statistics::total);

    stallEvents.init(StallEventCount).flags(statistics::total);
    std::map < StallEvent, const char* > stall_event_str = {
        { ROBWalk, "ROBWalk"},
        { IEWStall, "IEWStall"},
        { ROBFull, "ROBFull"},
        { IQFull, "IQFull"},
        { LSQFull, "LSQFull"},
        { RegFull, "RegFull"},
        { SerializeInst, "SerializeInst"},
        { BWFull, "BWFull"},
    };

    for (int i = 0; i < StallEventCount; i++) {
        stallEvents.subname(i, stall_event_str[static_cast<StallEvent>(i)]);
    }
}

void
Rename::regProbePoints()
{
    ppRename = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Rename");
    ppSquashInRename = new ProbePointArg<SeqNumRegPair>(cpu->getProbeManager(),
                                                        "SquashInRename");
}

/**
 * 设置时间缓冲区
 * 初始化与其他流水线阶段通信的线路连接
 *
 * @param tb_ptr 指向时间缓冲区的指针
 */
void
Rename::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // 设置从时间缓冲区读取信息的线路，来自IEW阶段
    fromIEW = timeBuffer->getWire(-iewToRenameDelay);

    // 设置从时间缓冲区读取信息的线路，来自提交阶段
    fromCommit = timeBuffer->getWire(-commitToRenameDelay);

    // 设置向前级阶段写入信息的线路
    toDecode = timeBuffer->getWire(0);
}

/**
 * 设置重命名队列
 * 初始化向后续阶段传递数据的队列
 *
 * @param rq_ptr 指向重命名队列的指针
 */
void
Rename::setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr)
{
    renameQueue = rq_ptr;

    // 设置向后续阶段写入信息的线路
    toIEW = renameQueue->getWire(0);
}

/**
 * 设置译码队列
 * 初始化从译码阶段接收数据的队列
 *
 * @param dq_ptr 指向译码队列的指针
 */
void
Rename::setDecodeQueue(TimeBuffer<DecodeStruct> *dq_ptr)
{
    decodeQueue = dq_ptr;

    // 设置从译码阶段获取信息的线路
    fromDecode = decodeQueue->getWire(-decodeToRenameDelay);
}

/**
 * 启动重命名阶段
 * 在仿真开始时调用，重置重命名阶段到初始状态
 */
void
Rename::startupStage()
{
    resetStage();
}

/**
 * 清空特定线程的状态
 * 重置指定线程的重命名状态，更新资源计数器
 *
 * @param tid 线程ID
 */
void
Rename::clearStates(ThreadID tid)
{
    renameStatus[tid] = Idle;    // 设置为空闲状态

    // 更新各种资源的空闲条目数
    freeEntries[tid].lqEntries = iew_ptr->ldstQueue.numFreeLoadEntries(tid);   // 加载队列空闲条目
    freeEntries[tid].sqEntries = iew_ptr->ldstQueue.numFreeStoreEntries(tid);  // 存储队列空闲条目
    freeEntries[tid].robEntries = commit_ptr->numROBFreeEntries(tid);          // ROB空闲条目
    emptyROB[tid] = true;        // ROB为空

    stalls[tid].iew = false;     // 清空 IEW 停顿信号
    serializeInst[tid] = NULL;   // 清空串行化指令指针

    // 重置在进行中的指令计数器
    instsInProgress[tid] = 0;    // 正在处理的指令数
    loadsInProgress[tid] = 0;    // 正在处理的加载指令数
    storesInProgress[tid] = 0;   // 正在处理的存储指令数

    serializeOnNextInst[tid] = false;  // 清空下一条指令串行化标志
}

/**
 * 重置重命名阶段
 * 将整个重命名阶段重置到初始状态，清空所有线程的状态和资源计数器
 */
void
Rename::resetStage()
{
    _status = Inactive;          // 设置阶段为非活动状态

    resumeSerialize = false;     // 清空恢复串行化标志
    resumeUnblocking = false;    // 清空恢复解阻塞标志

    // 从各个阶段直接获取空闲条目数
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        renameStatus[tid] = Idle;    // 设置为空闲状态

        // 更新各种资源的空闲条目数
        freeEntries[tid].lqEntries =
            iew_ptr->ldstQueue.numFreeLoadEntries(tid);   // 加载队列空闲条目
        freeEntries[tid].sqEntries =
            iew_ptr->ldstQueue.numFreeStoreEntries(tid);  // 存储队列空闲条目
        freeEntries[tid].robEntries = commit_ptr->numROBFreeEntries(tid);  // ROB空闲条目
        emptyROB[tid] = true;        // ROB为空

        stalls[tid].iew = false;     // 清空IEW停顿信号
        serializeInst[tid] = NULL;   // 清空串行化指令指针

        // 重置在进行中的指令计数器
        instsInProgress[tid] = 0;    // 正在处理的指令数
        loadsInProgress[tid] = 0;    // 正在处理的加载指令数
        storesInProgress[tid] = 0;   // 正在处理的存储指令数

        serializeOnNextInst[tid] = false;  // 清空下一条指令串行化标志
    }
}

/**
 * 设置活动线程列表
 * 设置当前活动的线程ID列表
 *
 * @param at_ptr 指向活动线程列表的指针
 */
void
Rename::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}


/**
 * 设置重命名映射表
 * 为每个线程设置统一的重命名映射表，用于管理架构寄存器到物理寄存器的映射
 *
 * @param rm_ptr 指向重命名映射表数组的指针
 */
void
Rename::setRenameMap(UnifiedRenameMap rm_ptr[MaxThreads])
{
    // 为每个线程设置重命名映射表指针
    for (ThreadID tid = 0; tid < numThreads; tid++)
        renameMap[tid] = &rm_ptr[tid];
}

/**
 * 设置空闲列表
 * 设置统一的空闲列表，用于管理可用的物理寄存器
 *
 * @param fl_ptr 指向空闲列表的指针
 */
void
Rename::setFreeList(UnifiedFreeList *fl_ptr)
{
    freeList = fl_ptr;
}

/**
 * 设置记分板
 * 设置记分板指针，用于跟踪物理寄存器的就绪状态
 *
 * @param _scoreboard 指向记分板的指针
 */
void
Rename::setScoreboard(Scoreboard *_scoreboard)
{
    scoreboard = _scoreboard;
}

/**
 * 检查重命名阶段是否已排空
 * 检查所有线程是否都完成了处理，没有指令在缓冲区中
 *
 * @return 如果已排空则返回true，否则返回false
 */
bool
Rename::isDrained() const
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        // 检查每个线程是否完成排空
        if (instsInProgress[tid] != 0 ||            // 有指令在处理中
            !historyBuffer[tid].empty() ||          // 历史缓冲区不为空
            !skidBuffer[tid].empty() ||             // 缓冲区不为空
            !insts[tid].empty() ||                  // 指令队列不为空
            (renameStatus[tid] != Idle && renameStatus[tid] != Running))  // 状态不正常
            return false;
    }
    return true;  // 所有线程都已排空
}

/**
 * 从其他CPU模式接管
 * 在CPU模式切换时调用，重置重命名阶段状态
 */
void
Rename::takeOverFrom()
{
    resetStage();
}

/**
 * 排空健全性检查
 * 在排空过程中验证所有缓冲区和计数器都为空/零
 * 确保没有数据遗留在重命名阶段
 */
void
Rename::drainSanityCheck() const
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        assert(historyBuffer[tid].empty());  // 断言历史缓冲区为空
        assert(insts[tid].empty());          // 断言指令队列为空
        assert(skidBuffer[tid].empty());     // 断言缓冲区为空
        assert(instsInProgress[tid] == 0);   // 断言在处理指令数为零
    }
}

/**
 * 清空流水线指令
 * 当检测到分支预测错误或异常时，清空指定线程的后续指令
 *
 * @param squash_seq_num 清空序列号，比这个序列号更新的指令都会被清空
 * @param tid 线程ID
 */
void
Rename::squash(const InstSeqNum &squash_seq_num, ThreadID tid)
{
    DPRINTF(Rename, "[tid:%i] [squash sn:%llu] Squashing instructions.\n",
        tid,squash_seq_num);

    // 如果重命名之前被阻塞或解阻塞，则清空停顿信号
    // 如果仍需要阻塞，阻塞应该在下一周期发生，
    // 由于清空，应该有空间容纳所有内容
    if (renameStatus[tid] == Blocked ||             // 阻塞状态
        renameStatus[tid] == Unblocking) {          // 解阻塞状态
        toDecode->renameUnblock[tid] = 1;           // 通知译码阶段解阻塞

        resumeSerialize = false;                    // 不恢复串行化
        serializeInst[tid] = NULL;                  // 清空串行化指令
    } else if (renameStatus[tid] == SerializeStall) {  // 串行化停顿状态
        if (serializeInst[tid]->seqNum <= squash_seq_num) {
            // 串行化指令也被清空，清空后恢复串行化
            DPRINTF(Rename, "[tid:%i] [squash sn:%llu] "
                "Rename will resume serializing after squash\n",
                tid,squash_seq_num);
            resumeSerialize = true;                 // 标记恢复串行化
            assert(serializeInst[tid]);
        } else {
            // 串行化指令没有被清空，取消串行化
            resumeSerialize = false;
            toDecode->renameUnblock[tid] = 1;       // 通知译码阶段解阻塞

            serializeInst[tid] = NULL;              // 清空串行化指令
        }
    }

    // 设置状态为清空状态
    renameStatus[tid] = Squashing;

    // 清空来自译码阶段的指令
    for (int i=0; i<fromDecode->size; i++) {
        if (fromDecode->insts[i]->threadNumber == tid &&
            fromDecode->insts[i]->seqNum > squash_seq_num) {
            fromDecode->insts[i]->setSquashed();    // 标记指令为清空
            wroteToTimeBuffer = true;               // 标记已写入时间缓冲区
        }
    }

    // 清空指令列表和缓冲区，以防其中有指令
    insts[tid].clear();                             // 清空指令列表
    skidBuffer[tid].clear();                        // 清空缓冲区

    // 调用具体的清空操作
    doSquash(squash_seq_num, tid);
}

/**
 * 重命名阶段的主循环函数
 * 每个周期调用一次，负责处理所有活动线程的指令重命名
 * 包括信号检查、指令重命名、资源管理等
 */
void
Rename::tick()
{
    // 将取指和译码停顿原因传递给IEW阶段
    toIEW->fetchStallReason = fromDecode->fetchStallReason;
    toIEW->decodeStallReason = fromDecode->decodeStallReason;

    wroteToTimeBuffer = false;   // 重置时间缓冲区写入标志
    blockThisCycle = false;      // 重置当前周期阻塞标志
    bool status_change = false;  // 状态改变标志
    toIEWIndex = 0;             // 重置发送给IEW阶段的指令索引

    // 对从译码来的指令进行分类排序
    sortInsts();

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    // 检查停顿和清空信号
    while (threads != end) {
        ThreadID tid = *threads++;

        DPRINTF(Rename, "Processing [tid:%i]\n", tid);

        // 检查信号并更新状态
        status_change = checkSignalsAndUpdate(tid) || status_change;

        // 执行具体的重命名操作
        rename(status_change, tid);

        // 设置阻塞原因信息
        toDecode->renameInfo[tid].blockReason = blockReason;
    }

    // 处理停顿状态和原因传递
    if (stalls[*threads].iew) {
        // 有来自IEW的停顿，传递IEW停顿原因
        setAllStalls(fromIEW->iewInfo[*threads].blockReason);
    } else if (toIEWIndex == 0) {
        // 没有指令发送到IEW阶段
        if (renameStalls[0] != StallReason::NoStall) {
            setAllStalls(renameStalls[0]);
        } else {
            // warn("重命名有其他停顿原因!");
        }
    } else {
        // 重命名没有停顿，传递译码停顿原因(无停顿或译码停顿)
        // assert(renamed_insts != 0);
        for (int i = 0; i < renameStalls.size(); i++) {
            if (i < toIEWIndex) {    // 重命名成功，无停顿
                renameStalls.at(i) = StallReason::NoStall;
            } else {    // 没有指令可重命名，传递译码停顿
                renameStalls.at(i) = fromDecode->decodeStallReason.at(i);
            }
        }
    }

    // 将重命名停顿原因传递给IEW阶段
    toIEW->renameStallReason = renameStalls;

    // 如果状态有改变，更新整体状态
    if (status_change) {
        updateStatus();
    }

    // 如果向时间缓冲区写入了数据或有物理寄存器需要释放，通知CPU此周期有活动
    if (wroteToTimeBuffer || releaseSeq < finalCommitSeq) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }

    // 处理物理寄存器释放
    threads = activeThreads->begin();

    // 更新释放序列号，根据释放宽度推进
    if (releaseSeq + releaseWidth < finalCommitSeq) {
        releaseSeq += releaseWidth;      // 按宽度递增
    } else {
        releaseSeq = finalCommitSeq;     // 设置为最终提交序列号
    }

    // 为每个活动线程处理历史缓冲区清理
    while (threads != end) {
        ThreadID tid = *threads++;

        // 从历史缓冲区中移除已释放的条目
        removeFromHistory(releaseSeq, tid);

        // 如果此周期有提交，则doneSeqNum将>0
        if (fromCommit->commitInfo[tid].doneSeqNum != 0 &&
            !fromCommit->commitInfo[tid].squash &&
            renameStatus[tid] != Squashing) {

            finalCommitSeq = fromCommit->commitInfo[tid].doneSeqNum;  // 更新最终提交序列号
            // 更新释放序列号为历史缓冲区最后一个条目的序列号
            releaseSeq = historyBuffer->empty() ? 0 : historyBuffer[tid].back().instSeqNum;
        }
    }

    // 更新进度计数器
    // TODO: 将此部分制作成updateProgress函数
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        // 减去已分发的指令数
        instsInProgress[tid] -= fromIEW->iewInfo[tid].dispatched;      // 总指令数
        loadsInProgress[tid] -= fromIEW->iewInfo[tid].dispatchedToLQ;  // 加载指令数
        storesInProgress[tid] -= fromIEW->iewInfo[tid].dispatchedToSQ; // 存储指令数

        // 确保计数器不会变为负数
        assert(loadsInProgress[tid] >= 0);
        assert(storesInProgress[tid] >= 0);
        assert(instsInProgress[tid] >=0);
    }
}

/**
 * 执行重命名操作
 * 根据当前线程的状态决定如何处理指令
 * - 如果状态是运行或空闲，调用renameInsts()
 * - 如果状态是解阻塞，缓冲从译码来的指令，继续尝试清空缓冲区
 *
 * @param status_change 状态改变标志的引用
 * @param tid 线程ID
 */
void
Rename::rename(bool &status_change, ThreadID tid)
{
    // 处理不同的重命名状态
    if (renameStatus[tid] == Blocked) {          // 阻塞状态
        ++stats.blockCycles;                     // 统计阻塞周期
        setAllStalls(blockReason);               // 设置所有停顿原因
    } else if (renameStatus[tid] == Squashing) { // 清空状态
        ++stats.squashCycles;                    // 统计清空周期
        setAllStalls(StallReason::SquashStall);  // 设置清空停顿原因
    } else if (renameStatus[tid] == SerializeStall) {  // 串行化停顿状态
        ++stats.serializeStallCycles;            // 统计串行化停顿周期
        setAllStalls(StallReason::SerializeStall);  // 设置串行化停顿原因
        // If we are currently in SerializeStall and resumeSerialize
        // was set, then that means that we are resuming serializing
        // this cycle.  Tell the previous stages to block.
        // 处理恢复串行化的情况
        if (resumeSerialize) {
            resumeSerialize = false;                 // 清空恢复串行化标志
            blockReason = StallReason::SerializeStall;  // 设置阻塞原因
            block(tid);                              // 阻塞该线程
            toDecode->renameUnblock[tid] = false;    // 取消解阻塞信号
        }
    } else if (renameStatus[tid] == Unblocking) {    // 解阻塞状态
        // 处理恢复解阻塞的情况
        if (resumeUnblocking) {
            blockReason = StallReason::ResumeUnblock;   // 设置恢复解阻塞原因
            block(tid);                              // 重新阻塞该线程
            resumeUnblocking = false;                // 清空恢复解阻塞标志
            toDecode->renameUnblock[tid] = false;    // 取消解阻塞信号
        }
    }

    // 根据状态执行具体的重命名操作
    if (renameStatus[tid] == Running ||              // 运行状态
        renameStatus[tid] == Idle) {                 // 空闲状态
        DPRINTF(Rename,
                "[tid:%i] "
                "Not blocked, so attempting to run stage.\n",
                tid);

        renameInsts(tid);                            // 调用指令重命名函数
    } else if (renameStatus[tid] == Unblocking) {    // 解阻塞状态
        renameInsts(tid);                            // 继续处理指令重命名

        if (validInsts()) {
            // 将当前输入添加到缓冲区，以便在该阶段解阻塞时重新处理
            skidInsert(tid);
        }

        // 如果切换到阻塞状态，则可能存在整体状态改变
        status_change = unblock(tid) || status_change || blockThisCycle;
    }
}

/**
 * 检查是否可以进行重命名操作
 * 检查指定数量的指令是否有足够的资源进行重命名
 * 包括物理寄存器、ROB条目、加载/存储队列等资源
 *
 * @param tid 线程ID
 * @param num_insts 需要检查的指令数量
 * @return 如果可以重命名则返回true，否则返回false
 */
bool
Rename::canRename(ThreadID tid, int num_insts)
{
    // 初始化各类型物理寄存器的需求数量
    std::vector<int> demand_phy_regs(RMiscRegClass + 1, 0);

    // 根据状态选择指令来源：解阻塞时使用缓冲区，否则使用正常指令队列
    InstQueue &insts_to_rename = renameStatus[tid] == Unblocking ?
        skidBuffer[tid] : insts[tid];

    // 计算这`num_insts`条指令所需要的物理寄存器
    for (int i = 0; i < num_insts; i++) {
        DynInstPtr inst = insts_to_rename.at(i);

        // 检查加载指令的加载队列资源
        if (inst->isLoad()) {
            if (calcFreeLQEntries(tid) <= 0) {
                break;  // 加载队列已满，停止检查
            }
        } else if (inst->isStore() || inst->isAtomic()) {
            // 检查存储指令的存储队列资源
            if (calcFreeSQEntries(tid) <= 0) {
                break;  // 存储队列已满，停止检查
            }
        }

        // 跳过已清空的指令
        if (inst->isSquashed()) {
            continue;
        }

        // 检查前串行化指令
        if (inst->isSerializeBefore() && !inst->isSerializeHandled()) {
            break;  // 遇到未处理的前串行化指令，停止检查
        }

        // 统计各类型寄存器的需求数量
        for (int j = 0; j < RMiscRegClass + 1 ; j++) {
            demand_phy_regs[j] += inst->numDestRegs((RegClassType)j);
        }

        // 检查后串行化指令
        if ((inst->isStoreConditional() || inst->isSerializeAfter()) &&
            !inst->isSerializeHandled()) {
            break;  // 遇到未处理的后串行化指令，停止检查
        }
    }

    // 如果总需求寄存器数少于重命名宽度，则设置为重命名宽度。
    // 在实际硬件中，由于时序约束，
    // 我们只能在最坏情况下评估重命名是否可以进行。
    for (int i = 0; i < RMiscRegClass + 1; i++) {
        switch (i) {
            case IntRegClass:    // 整数寄存器类型
            case FloatRegClass:  // 浮点寄存器类型
            case VecRegClass:
            case RMiscRegClass:
                demand_phy_regs[i] = std::max(demand_phy_regs[i], (int)renameWidth);
                break;
            default:
                break;
        }
    }

    // check if the demand registers can be satisfied or not
    for (int i = 0; i < RMiscRegClass + 1; i++) {
        if (demand_phy_regs[i] > renameMap[tid]->numFreeEntries((RegClassType)i)) {
            return false;
        }
    }
    return true;
}

/**
 * 执行具体的指令重命名操作
 * 该函数是重命名阶段的核心，负责处理单个线程的指令重命名
 * 包括资源检查、寄存器重命名、ROB分配等核心操作
 *
 * @param tid 线程ID
 */
void
Rename::renameInsts(ThreadID tid)
{
    // 指令可以来自缓冲区或从译码来的指令队列，取决于状态
    int insts_available = renameStatus[tid] == Unblocking ?
        skidBuffer[tid].size() : insts[tid].size();

    int instsAvailable = insts_available;  // 保存原始可用数量

    // 检查译码队列中是否有可用指令
    // 如果没有可重命名的指令，则不做任何操作
    if (insts_available == 0) {
        DPRINTF(Rename, "[tid:%i] Nothing to do, breaking out early.\n",
                tid);
        // 是否应该将状态改为空闲？
        ++stats.idleCycles;              // 统计空闲周期

        // 查找译码阶段的停顿原因
        StallReason stall = StallReason::NoStall;
        for (auto iter : fromDecode->decodeStallReason) {
            if (iter != StallReason::NoStall) {
                stall = iter;
                break;
            }
        }
        setAllStalls(stall);             // 传递译码停顿原因

        return;
    } else if (renameStatus[tid] == Unblocking) {
        ++stats.unblockCycles;           // 统计解阻塞周期
    } else if (renameStatus[tid] == Running) {
        ++stats.runCycles;               // 统计运行周期
    }

    // 需要对空闲条目数进行不同的计算
    int free_rob_entries = calcFreeROBEntries(tid);      // 计算ROB空闲条目
    int free_iq_entries  = calcFreeIQEntries(tid);       // 计算指令队列空闲条目
    int min_free_entries = free_rob_entries;             // 初始化为 ROB 空闲数

    DPRINTF(Rename, "free_rob_entries=%d, free_iq_entries=%d, insts_available=%d\n",
            free_rob_entries, free_iq_entries, insts_available);

    FullSource source = ROB;                             // 限制来源，初始为ROB

    // 找到最小的空闲条目数，这就是我们的瓶颈
    if (free_iq_entries < min_free_entries) {
        min_free_entries = free_iq_entries;              // 更新最小空闲数
        source = IQ;                                     // 更新限制来源为指令队列
    }

    // 检查是否还有空闲空间
    if (min_free_entries <= 0) {
        DPRINTF(Rename,
                "[tid:%i] Blocking due to no free ROB/IQ/ entries.\n"
                "ROB has %i free entries.\n"
                "IQ has %i free entries.\n",
                tid, free_rob_entries, free_iq_entries);

        blockThisCycle = true;                           // 设置当前周期阻塞标志
        block(tid);                                      // 阻塞该线程
        incrFullStat(source);                            // 增加资源充满统计
        setAllStalls(checkRenameStallFromIEW(tid));      // 设置停顿原因

        return;  // 早期返回，无法继续重命名
    } else if (min_free_entries < insts_available) {
        // 空闲条目不足以处理所有可用指令，限制处理数量
        DPRINTF(Rename,
                "[tid:%i] "
                "Will have to block this cycle. "
                "%i insts available, "
                "but only %i insts can be renamed due to ROB/IQ/LSQ limits.\n",
                tid, insts_available, min_free_entries);

        insts_available = min_free_entries;              // 限制可处理指令数
        blockThisCycle = true;                           // 设置当前周期阻塞标志
        blockReason = checkRenameStallFromIEW(tid);      // 设置阻塞原因
        incrFullStat(source);                            // 增加资源充满统计
    }

    // 根据状态选择指令来源：解阻塞时使用缓冲区，否则使用正常指令队列
    InstQueue &insts_to_rename = renameStatus[tid] == Unblocking ?
        skidBuffer[tid] : insts[tid];

    DPRINTF(Rename,
            "[tid:%i] "
            "%i available instructions to send iew.\n",
            tid, insts_available);

    DPRINTF(Rename,
            "[tid:%i] "
            "%i insts pipelining from Rename | "
            "%i insts dispatched to IQ last cycle.\n",
            tid, instsInProgress[tid], fromIEW->iewInfo[tid].dispatched);

    // 处理下一条指令的串行化（如果必要）
    if (serializeOnNextInst[tid]) {
        if (emptyROB[tid] && instsInProgress[tid] == 0) {
            // ROB已经为空，不需要串行化
            serializeOnNextInst[tid] = false;
        } else if (!insts_to_rename.empty()) {
            // 将下一条指令设置为前串行化
            insts_to_rename.front()->setSerializeBefore();
        }
    }

    int renamed_insts = 0;                               // 已重命名指令计数器
    std::queue<StallReason> rename_stalls;               // 重命名停顿原因队列
    StallReason breakRename = StallReason::NoStall;      // 中断重命名的原因

    // 在这里检查是否有足够的目标寄存器可以重命名
    // 否则阻塞
    bool can_rename = canRename(tid, std::min((int)renameWidth, insts_available));
    if (!can_rename) {
        DPRINTF(Rename, "[tid:%i] Blocking due to no free physical registers to rename to.\n", tid);
        blockThisCycle = true;                           // 设置阻塞标志
        ++stats.fullRegistersEvents;                     // 统计寄存器充满事件
    }

    // 主重命名循环：在可以重命名且有指令可用且未超过重命名宽度时继续处理
    while (can_rename && insts_available > 0 &&  toIEWIndex < renameWidth) {
        DPRINTF(Rename, "[tid:%i] Sending instructions to IEW.\n", tid);

        assert(!insts_to_rename.empty());            // 断言指令队列不为空

        DynInstPtr inst = insts_to_rename.front();   // 获取下一条指令

        // 对于所有类型的指令，首先检查ROB和IQ
        // 对于加载指令，检查LQ大小并考虑在飞加载数
        // 对于存储指令，检查SQ大小并考虑在飞存储数

        // 检查加载指令的资源
        if (inst->isLoad()) {
            if (calcFreeLQEntries(tid) <= 0) {
                DPRINTF(Rename, "[tid:%i] Cannot rename due to no free LQ\n",
                        tid);
                source = LQ;                         // 设置限制来源为加载队列
                incrFullStat(source);                // 增加资源充满统计
                rename_stalls.push(checkRenameStallFromIEW(tid));  // 记录停顿原因
                breakRename = checkRenameStallFromIEW(tid);        // 设置中断原因
                break;                               // 中断重命名循环
            }
        }

        // 检查存储或原子指令的资源
        if (inst->isStore() || inst->isAtomic()) {
            if (calcFreeSQEntries(tid) <= 0) {
                DPRINTF(Rename, "[tid:%i] Cannot rename due to no free SQ\n",
                        tid);
                source = SQ;                         // 设置限制来源为存储队列
                incrFullStat(source);                // 增加资源充满统计
                rename_stalls.push(checkRenameStallFromIEW(tid));  // 记录停顿原因
                breakRename = checkRenameStallFromIEW(tid);        // 设置中断原因
                break;                               // 中断重命名循环
            }
        }

        insts_to_rename.pop_front();                 // 从队列中移除指令

        // 如果在解阻塞状态，记录从缓冲区移除指令
        if (renameStatus[tid] == Unblocking) {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "Removing [sn:%llu] PC:%s from rename skidBuffer\n",
                    tid, inst->seqNum, inst->pcState());
        }

        // 检查指令是否已被清空
        // 清空的指令无需重命名，直接跳过并继续处理下一条
        if (inst->isSquashed()) {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "instruction %i with PC %s is squashed, skipping.\n",
                    tid, inst->seqNum, inst->pcState());

            ++stats.squashedInsts;                   // 统计清空指令数
            --insts_available;                       // 减少可用指令数
            rename_stalls.push(StallReason::InstSquashed);  // 记录停顿原因
            continue;                                // 跳过此指令
        }

        DPRINTF(Rename,
                "[tid:%i] "
                "Processing instruction [sn:%llu] with PC %s.\n",
                tid, inst->seqNum, inst->pcState());

        assert(renameMap[tid]->canRename(inst));     // 断言可以重命名

        // 处理后串行化/前串行化指令
        // 后串行化将下一条指令标记为前串行化
        // 前串行化使指令在重命名中等待直到ROB为空

        // 在此模型中，IPR访问是前串行化指令，
        // 条件存储是后串行化指令。
        // 这主要是由于缺乏对这两类指令的乱序操作支持。

        // 处理前串行化指令
        if (inst->isSerializeBefore() && !inst->isSerializeHandled()) {
            DPRINTF(Rename, "Serialize before instruction encountered.\n");

            if (!inst->isTempSerializeBefore()) {
                stats.serializing++;                // 统计串行化指令数
                inst->setSerializeHandled();         // 标记串行化已处理
            } else {
                stats.tempSerializing++;            // 统计临时串行化数
            }

            // 将状态改为串行化停顿，以便其他阶段知道阻塞原因
            renameStatus[tid] = SerializeStall;
            serializeInst[tid] = inst;               // 记录串行化指令
            blockThisCycle = true;                   // 设置当前周期阻塞
            blockReason = StallReason::SerializeStall;  // 设置阻塞原因
            rename_stalls.push(StallReason::SerializeStall);  // 记录停顿原因
            breakRename = StallReason::SerializeStall;        // 设置中断原因

            break;  // 中断重命名循环
        } else if ((inst->isStoreConditional() || inst->isSerializeAfter()) &&
                   !inst->isSerializeHandled()) {
            // 处理后串行化指令
            DPRINTF(Rename, "Serialize after instruction encountered.\n");

            stats.serializing++;                    // 统计串行化数
            inst->setSerializeHandled();             // 标记串行化已处理
            serializeAfter(insts_to_rename, tid);    // 处理后串行化逻辑
        }

        // 执行源寄存器重命名
        // 将指令的源寄存器从架构寄存器映射到物理寄存器
        renameSrcRegs(inst, inst->threadNumber);

        // 执行目标寄存器重命名
        // 为指令的目标寄存器分配新的物理寄存器
        renameDestRegs(inst, inst->threadNumber);

        // 更新性能跟踪器中的指令位置
        cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtRename);

        // 增加相应的在飞指令计数器
        if (inst->isAtomic() || inst->isStore()) {
            storesInProgress[tid]++;             // 存储指令在飞计数
        } else if (inst->isLoad()) {
            loadsInProgress[tid]++;              // 加载指令在飞计数
        }

        ++renamed_insts;                         // 增加已重命名指令计数

        // 通知监听器：此指令的源寄存器和目标寄存器已完成重命名
        ppRename->notify(inst);

        // 将指令放入发送给IEW阶段的队列中
        toIEW->insts[toIEWIndex] = inst;
        ++(toIEW->size);

        // 递增发送给IEW的指令索引
        ++toIEWIndex;

        // 减少可用指令数量
        --insts_available;
    }

    // 处理重命名停顿信息
    if (!rename_stalls.empty()) {
        setAllStalls(rename_stalls.front());     // 设置停顿状态
        rename_stalls.pop();                     // 移除已处理的停顿原因
    } else if (breakRename != StallReason::NoStall) {
        setAllStalls(breakRename);               // 设置中断重命名的停顿状态
    }

    // 更新统计信息
    instsInProgress[tid] += renamed_insts;       // 增加线程的在飞指令数
    stats.renamedInsts += renamed_insts;         // 更新全局重命名指令统计

    // 如果写入了时间缓冲区，记录此事件
    if (toIEWIndex) {
        wroteToTimeBuffer = true;
    }

    // 检查是否还有指令未重命名
    // 如果有，则阻塞当前周期
    if (insts_available) {
        blockThisCycle = true;                   // 设置阻塞标志
        blockReason = breakRename;               // 设置阻塞原因
    }

    if (blockThisCycle) {
        block(tid);
        toDecode->renameUnblock[tid] = false;
    }
}

/**
 * 将指令插入到缓冲区（skid buffer）中
 * 当重命名阶段被阻塞时，来自解码阶段的指令需要临时存储在缓冲区中
 * 等到重命名阶段解除阻塞后，这些指令会从缓冲区中取出继续处理
 * @param tid 线程ID
 */
void
Rename::skidInsert(ThreadID tid)
{
    DynInstPtr inst = NULL;

    // 将所有来自解码阶段的指令移动到缓冲区中
    while (!insts[tid].empty()) {
        inst = insts[tid].front();           // 获取队列前端指令

        insts[tid].pop_front();              // 从原队列中移除指令

        assert(tid == inst->threadNumber);   // 断言线程ID匹配

        DPRINTF(Rename, "[tid:%i] Inserting [sn:%llu] PC: %s into Rename "
                "skidBuffer\n", tid, inst->seqNum, inst->pcState());

        ++stats.skidInsts;                   // 统计缓冲区指令数

        skidBuffer[tid].push_back(inst);     // 将指令加入缓冲区
    }

    // 检查缓冲区是否溢出
    if (skidBuffer[tid].size() > skidBufferMax) {
        InstQueue::iterator it;
        warn("Skidbuffer contents:\n");
        // 打印缓冲区内容用于调试
        for (it = skidBuffer[tid].begin(); it != skidBuffer[tid].end(); it++) {
            warn("[tid:%i] %s [sn:%llu].\n", tid,
                    (*it)->staticInst->disassemble(
                        inst->pcState().instAddr()),
                    (*it)->seqNum);
        }
        panic("Skidbuffer Exceeded Max Size");  // 缓冲区溢出，触发panic
    }
}

/**
 * 对来自解码阶段的指令进行分类处理
 * 检查指令是否需要被清空，或者将有效指令分发到对应线程的队列中
 */
void
Rename::sortInsts()
{
    int insts_from_decode = fromDecode->size;    // 获取从解码阶段来的指令数量

    // 遍历所有从解码阶段来的指令
    for (int i = 0; i < insts_from_decode; ++i) {
        const DynInstPtr &inst = fromDecode->insts[i];

        // 检查指令是否需要被清空（基于版本号比较）
        if (localSquashVer.largerThan(inst->getVersion())) {
            inst->setSquashed();             // 将指令标记为已清空
        } else {
            // 将有效指令加入对应线程的指令队列
            insts[inst->threadNumber].push_back(inst);
        }

#if TRACING_ON
        // 如果启用了流水线视图追踪，记录重命名阶段的时间戳
        if (debug::O3PipeView) {
            inst->renameTick = curTick() - inst->fetchTick;
        }
#endif
    }
}

/**
 * 检查所有线程的缓冲区是否为空
 * 用于判断重命名阶段是否可以从阻塞状态中恢复
 * @return true 如果所有线程的缓冲区都为空，否则返回false
 */
bool
Rename::skidsEmpty()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    // 遍历所有活跃线程
    while (threads != end) {
        ThreadID tid = *threads++;

        // 如果任何一个线程的缓冲区不为空，返回false
        if (!skidBuffer[tid].empty())
            return false;
    }

    // 所有线程的缓冲区都为空
    return true;
}

/**
 * 更新重命名阶段的活动状态
 * 根据线程的解阻塞状态来决定重命名阶段是否应该处于活跃状态
 * 这影响CPU的整体活动管理和电源状态
 */
void
Rename::updateStatus()
{
    bool any_unblocking = false;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    // 检查是否有任何线程处于解阻塞状态
    while (threads != end) {
        ThreadID tid = *threads++;

        if (renameStatus[tid] == Unblocking) {
            any_unblocking = true;
            break;
        }
    }

    // 如果有线程在解阻塞，重命名阶段需要保持活跃状态
    if (any_unblocking) {
        if (_status == Inactive) {
            _status = Active;                // 激活重命名阶段

            DPRINTF(Activity, "Activating stage.\n");

            cpu->activateStage(CPU::RenameIdx);  // 通知CPU激活重命名阶段
        }
    } else {
        // 如果没有线程在解阻塞，重命名阶段可以进入非活跃状态
        // 切换到非活跃状态以节省功耗
        if (_status == Active) {
            _status = Inactive;              // 设置为非活跃状态
            DPRINTF(Activity, "Deactivating stage.\n");

            cpu->deactivateStage(CPU::RenameIdx);  // 通知CPU停用重命名阶段
        }
    }
}

/**
 * 阻塞指定线程的重命名操作
 * 当后续阶段无法接收更多指令时调用此函数
 * @param tid 要阻塞的线程ID
 * @return true 如果成功设置为阻塞状态，false 否则
 */
bool
Rename::block(ThreadID tid)
{
    DPRINTF(Rename, "[tid:%i] Blocking.\n", tid);

    // 将当前输入指令添加到缓冲区中，以便在解除阻塞时可以重新处理
    skidInsert(tid);

    // 只有当前面的阶段不认为重命名已经被阻塞时，才向后发送阻塞信号
    if (renameStatus[tid] != Blocked) {
        // 如果resumeUnblocking被设置，说明我们在清空期间解除了阻塞
        // 但现在处于解阻塞状态，需要告诉前面的阶段进行阻塞
        if (resumeUnblocking || renameStatus[tid] != Unblocking) {
            toDecode->renameBlock[tid] = true;      // 向解码阶段发送阻塞信号
            toDecode->renameUnblock[tid] = false;   // 清除解阻塞信号
            wroteToTimeBuffer = true;               // 标记已写入时间缓冲区
        }

        // 重命名不能从SerializeStall直接转换到Blocked状态
        // 否则它不知道如何完成串行化停顿
        if (renameStatus[tid] != SerializeStall) {
            // 设置状态为阻塞
            renameStatus[tid] = Blocked;
            return true;
        }
    }

    return false;
}

/**
 * 尝试解除指定线程的重命名阻塞状态
 * 当缓冲区为空且不处于串行化停顿时，可以完成解阻塞操作
 * @param tid 要解阻塞的线程ID
 * @return true 如果成功解除阻塞，false 否则
 */
bool
Rename::unblock(ThreadID tid)
{
    DPRINTF(Rename, "[tid:%i] Trying to unblock.\n", tid);

    // 当缓冲区为空且不处于串行化停顿时，重命名完成解阻塞
    if (skidBuffer[tid].empty() && renameStatus[tid] != SerializeStall) {

        DPRINTF(Rename, "[tid:%i] Done unblocking.\n", tid);

        toDecode->renameUnblock[tid] = true;    // 向解码阶段发送解阻塞信号
        wroteToTimeBuffer = true;               // 标记已写入时间缓冲区

        renameStatus[tid] = Running;            // 设置状态为运行状态
        return true;
    }

    return false;
}

/**
 * 尝试释放物理寄存器
 * 在指令清空时调用，减少寄存器引用计数并在适当时将其放回空闲列表
 * @param preg 要释放的物理寄存器指针
 */
void
Rename::tryFreePReg(PhysRegIdPtr preg)
{
    const auto preg_idx = preg->flatIndex();
    // 如果引用计数已为0或寄存器类型无效，直接返回
    if (preg->getRef() == 0 || preg->classValue() == InvalidRegClass) {
        return;
    }

    preg->decRef();                          // 减少引用计数
    if (preg->getRef() == 0) {
        // 如果引用计数降为0，将物理寄存器放回空闲列表
        DPRINTF(Rename, "Really free up p%i on squash with ref=%i\n", preg_idx,
                preg->getRef());
        freeList->addReg(preg);              // 将寄存器添加到空闲列表
    } else {
        DPRINTF(Rename, "Not to free up p%i on squash for ref=%i\n",
                preg->flatIndex(), preg->getRef());
    }
}

/**
 * 执行指令清空操作
 * 撤销被清空指令的寄存器重命名映射，恢复之前的寄存器状态
 * @param squashed_seq_num 被清空指令的序列号
 * @param tid 线程ID
 */
void
Rename::doSquash(const InstSeqNum &squashed_seq_num, ThreadID tid)
{
    auto hb_it = historyBuffer[tid].begin();

    // 系统调用清空所有指令后，历史缓冲区可能为空
    // 但ROB可能仍在清空指令
    // 遍历最近的指令，撤销它们的映射并释放寄存器
    while (!historyBuffer[tid].empty() &&
           hb_it->instSeqNum > squashed_seq_num) {
        assert(hb_it != historyBuffer[tid].end());

        DPRINTF(Rename,
                "[tid:%i] Removing history entry with sequence "
                "number %i (archReg: %d, newPhysReg: %s, prevPhysReg: %s).\n",
                tid, hb_it->instSeqNum, hb_it->archReg.index(),
                hb_it->newPhysReg.toString(),
                hb_it->prevPhysReg.toString());

        // 只有在真正发生了重命名时才撤销映射
        // 特殊寄存器（如misc寄存器和零寄存器）实际上没有重命名
        // 可以通过新映射和旧映射相同来识别
        // 虽然更新重命名表只是浪费时间，但我们绝不能将这些寄存器放入空闲列表
        if (hb_it->newPhysReg != hb_it->prevPhysReg) {
            // 告诉重命名映射表将架构寄存器设置为之前重命名的物理寄存器
            renameMap[tid]->setEntry(hb_it->archReg, hb_it->prevPhysReg);
            if (hb_it->newPhysReg.PhyReg() != hb_it->prevPhysReg.PhyReg()) {
                tryFreePReg(hb_it->newPhysReg.PhyReg());  // 尝试释放新的物理寄存器
            }
        }

        // 通知监听器需要移除寄存器映射，因为映射的指令被清空了
        // 注意这是在hb_it递增之前完成的
        ppSquashInRename->notify(std::make_pair(hb_it->instSeqNum,
                                                hb_it->newPhysReg.PhyReg()));

        historyBuffer[tid].erase(hb_it++);   // 从历史缓冲区中删除条目

        ++stats.undoneMaps;                  // 统计撤销的映射数量
    }
}

/**
 * 从历史缓冲区中移除已提交的指令记录
 * 当指令提交时，其旧的物理寄存器可以被释放，因为不再需要回滚
 * @param inst_seq_num 已提交指令的序列号
 * @param tid 线程ID
 */
void
Rename::removeFromHistory(InstSeqNum inst_seq_num, ThreadID tid)
{
    DPRINTF(Rename, "[tid:%i] Removing a committed instruction from the "
            "history buffer %u (size=%i), until [sn:%llu].\n",
            tid, tid, historyBuffer[tid].size(), inst_seq_num);

    auto hb_it = historyBuffer[tid].end();

    --hb_it;                                 // 从历史缓冲区末尾开始

    // 检查历史缓冲区是否为空
    if (historyBuffer[tid].empty()) {
        DPRINTF(Rename, "[tid:%i] History buffer is empty.\n", tid);
        return;
    } else if (hb_it->instSeqNum > inst_seq_num) {
        // 遇到旧的序列号，可能是系统调用导致的
        DPRINTF(Rename, "[tid:%i] [sn:%llu] "
                "Old sequence number encountered. "
                "Ensure that a syscall happened recently.\n",
                tid, inst_seq_num);
        return;
    }

    // 提交直到（包括）提交序列号的所有重命名
    // 某些或甚至所有已提交的指令可能没有重命名历史记录
    // 如果它们没有被重命名的目标寄存器
    while (!historyBuffer[tid].empty() &&
           hb_it != historyBuffer[tid].end() &&
           hb_it->instSeqNum <= inst_seq_num) {

        DPRINTF(Rename,
                "[tid:%i] try to free up older rename of reg %s (%s), "
                "[sn:%llu].\n",
                tid, hb_it->prevPhysReg.toString(),
                hb_it->prevPhysReg.PhyReg()->className(),
                hb_it->instSeqNum);

        // 不释放特殊物理寄存器，如misc和零寄存器
        // 这些寄存器可以通过新映射与旧映射相同来识别
        if (hb_it->newPhysReg.PhyReg() != hb_it->prevPhysReg.PhyReg()) {
            tryFreePReg(hb_it->prevPhysReg.PhyReg());  // 释放旧的物理寄存器
        }

        ++stats.committedMaps;               // 统计已提交的映射数量

        historyBuffer[tid].erase(hb_it--);   // 从历史缓冲区删除并向前移动
    }
}

/**
 * 对指令的源寄存器进行重命名
 * 将架构寄存器映射到对应的物理寄存器，并检查寄存器是否就绪
 * @param inst 要重命名的指令
 * @param tid 线程ID
 */
void
Rename::renameSrcRegs(const DynInstPtr &inst, ThreadID tid)
{
    gem5::ThreadContext *tc = inst->tcBase();
    UnifiedRenameMap *map = renameMap[tid];
    unsigned num_src_regs = inst->numSrcRegs();

    // 从源操作数获取架构寄存器编号，并重定向到正确的物理寄存器
    for (int src_idx = 0; src_idx < num_src_regs; src_idx++) {
        const RegId& src_reg = inst->srcRegIdx(src_idx);
        VirtRegId renamed_reg;

        // 查找架构寄存器对应的物理寄存器
        renamed_reg = map->lookup(tc->flattenRegId(src_reg));

        // 根据寄存器类型更新统计信息
        switch (src_reg.classValue()) {
          case InvalidRegClass:
            break;
          case IntRegClass:
            stats.intLookups++;              // 整型寄存器查找统计
            break;
          case FloatRegClass:
            stats.fpLookups++;               // 浮点寄存器查找统计
            break;
          case VecRegClass:
          case VecElemClass:
            stats.vecLookups++;              // 向量寄存器查找统计
            break;
          case VecPredRegClass:
            stats.vecPredLookups++;          // 向量谓词寄存器查找统计
            break;
          case CCRegClass:
          case RMiscRegClass:
          case MiscRegClass:
            break;

          default:
            panic("Invalid register class: %d.", src_reg.classValue());
        }

        DPRINTF(Rename,
                "[tid:%i] "
                "Looking up %s arch reg x%i, got %s\n",
                tid, src_reg.className(), src_reg.index(),
                renamed_reg.toString());

        // 设置指令的源寄存器重命名结果
        inst->renameSrcReg(src_idx, renamed_reg);

        // 检查物理寄存器是否就绪
        if (scoreboard->getReg(renamed_reg.PhyReg())) {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "Register %d (flat: %d) (%s) is ready.\n",
                    tid, renamed_reg.PhyReg()->index(), renamed_reg.PhyReg()->flatIndex(),
                    renamed_reg.PhyReg()->className());

            inst->markSrcRegReady(src_idx);  // 标记源寄存器就绪
        } else {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "Register %d (flat: %d) (%s) is not ready.\n",
                    tid, renamed_reg.PhyReg()->index(), renamed_reg.PhyReg()->flatIndex(),
                    renamed_reg.PhyReg()->className());
        }

        ++stats.lookups;                     // 总查找次数统计
    }
}

/**
 * 对指令的目标寄存器进行重命名
 * 为每个目标寄存器分配新的物理寄存器，并支持移动消除和常量折叠优化
 * @param inst 要重命名的指令
 * @param tid 线程ID
 */
void
Rename::renameDestRegs(const DynInstPtr &inst, ThreadID tid)
{
    gem5::ThreadContext *tc = inst->tcBase();
    UnifiedRenameMap *map = renameMap[tid];
    unsigned num_dest_regs = inst->numDestRegs();

    // 重命名目标寄存器
    for (int dest_idx = 0; dest_idx < num_dest_regs; dest_idx++) {
        const RegId& dest_reg = inst->destRegIdx(dest_idx);
        UnifiedRenameMap::RenameInfo rename_result;

        RegId flat_dest_regid = tc->flattenRegId(dest_reg);
        flat_dest_regid.setNumPinnedWrites(dest_reg.getNumPinnedWrites());

        VirtRegId bypass_reg;
        bool inc_ref_of_last_dest_phy_reg = false;

        // 移动指令消除优化
        if (inst->isMov()) {
            bypass_reg =
                map->lookup(tc->flattenRegId(inst->srcRegIdx(0)));
            DPRINTF(Rename, "Find the last reg p%i renamed for mv x%i, x%i\n",
                    bypass_reg.PhyReg()->flatIndex(), dest_reg.index(),
                    inst->srcRegIdx(0).index());
            inc_ref_of_last_dest_phy_reg = true;
            inst->setEmptyMov();                 // 标记为空移动指令
            DPRINTF(Rename, "[sn:%llu] Inst is nop: %i, is move: %i\n", inst->seqNum, inst->isNop(),
                    inst->isMov());
            stats.moveEliminated++;             // 统计消除的移动指令
        }
        // 常量折叠优化：处理立即数加法
        else if (inst->isAddImm() &&
            (cpu->enableConstantFolding || (cpu->enableMovImmElimination && inst->srcRegIdx(0).isZeroReg()))) {
            bypass_reg =
                map->lookup(tc->flattenRegId(inst->srcRegIdx(0)));

            // 只对ADD类型的立即数操作进行常量折叠
            if (!bypass_reg.IEOper() || bypass_reg.IEOper()->type == IEOperand::Type::ADD) {
                if (bypass_reg.IEOper()) {
                    // 累加立即数值
                    IEOperPtr ie_op = new IEOperand(IEOperand::Type::ADD,
                        bypass_reg.IEOper()->imm + inst->staticInst->getImm());
                    bypass_reg.setIEOper(ie_op);
                } else {
                    // 创建新的立即数操作
                    IEOperPtr ie_op = new IEOperand(IEOperand::Type::ADD, inst->staticInst->getImm());
                    bypass_reg.setIEOper(ie_op);
                }
                inc_ref_of_last_dest_phy_reg = true;

                inst->setConstantFolded();       // 标记为常量折叠指令
                DPRINTF(Rename, "[sn:%llu] Inst constant folded, "
                    "virtRegId: %s\n", inst->seqNum, bypass_reg.toString());
                stats.constantFolded++;         // 统计常量折叠次数
            }
        }

        // 执行实际的重命名操作
        rename_result = map->rename(flat_dest_regid, bypass_reg);

        inst->flattenedDestIdx(dest_idx, flat_dest_regid);

        // 如果不是移动/常量折叠优化，则将新分配的寄存器标记为未就绪
        if (!inc_ref_of_last_dest_phy_reg) {
            scoreboard->unsetReg(rename_result.first.PhyReg());
        }

        DPRINTF(Rename, "[tid:%i] %s arch reg x%i (%s) from %s to %s.\n", tid,
                inc_ref_of_last_dest_phy_reg ? "Mov" : "Rename",
                dest_reg.index(), dest_reg.className(),
                rename_result.second.toString(),
                rename_result.first.toString());

        // 记录重命名信息以便保持历史记录
        RenameHistory hb_entry(inst->seqNum, flat_dest_regid,
                               rename_result.first,
                               rename_result.second);

        historyBuffer[tid].push_front(hb_entry);    // 添加到历史缓冲区

        DPRINTF(Rename, "[tid:%i] [sn:%llu] "
                "Adding instruction to history buffer (size=%i).\n",
                tid,(*historyBuffer[tid].begin()).instSeqNum,
                historyBuffer[tid].size());

        // 告知指令将相应的目标寄存器重命名为新的物理寄存器
        // 并记录同一逻辑寄存器之前重命名到的物理寄存器
        inst->renameDestReg(dest_idx,
                            rename_result.first,     // 新的物理寄存器
                            rename_result.second);   // 之前的物理寄存器

        ++stats.renamedOperands;                     // 统计重命名的操作数数量
    }
}

/**
 * 计算指定线程的空闲ROB条目数
 * @param tid 线程ID
 * @return 可用的ROB条目数量
 */
int
Rename::calcFreeROBEntries(ThreadID tid)
{
    int num_free = freeEntries[tid].robEntries -
                  (instsInProgress[tid] - fromIEW->iewInfo[tid].dispatched);

    //DPRINTF(Rename,"[tid:%i] %i rob free\n",tid,num_free);

    return num_free;
}

/**
 * 计算指定线程的空闲指令队列条目数
 * 简化实现：如果IEW阶段停顿则返回0，否则返回100
 * @param tid 线程ID
 * @return 可用的IQ条目数量
 */
int
Rename::calcFreeIQEntries(ThreadID tid)
{
    return iew_ptr->dispStall() ? 0 : 100;
}

/**
 * 计算指定线程的空闲加载队列条目数
 * @param tid 线程ID
 * @return 可用的LQ条目数量
 */
int
Rename::calcFreeLQEntries(ThreadID tid)
{
        int num_free = freeEntries[tid].lqEntries -
            (loadsInProgress[tid] - fromIEW->iewInfo[tid].dispatchedToLQ);
        DPRINTF(Rename,
                "calcFreeLQEntries: free lqEntries: %d, loadsInProgress: %d, "
                "loads dispatchedToLQ: %d\n",
                freeEntries[tid].lqEntries, loadsInProgress[tid],
                fromIEW->iewInfo[tid].dispatchedToLQ);
        return num_free;
}

/**
 * 计算指定线程的空闲存储队列条目数
 * @param tid 线程ID
 * @return 可用的SQ条目数量
 */
int
Rename::calcFreeSQEntries(ThreadID tid)
{
        int num_free = freeEntries[tid].sqEntries -
            (storesInProgress[tid] - fromIEW->iewInfo[tid].dispatchedToSQ);
        DPRINTF(Rename, "calcFreeSQEntries: free sqEntries: %d, "
                "storesInProgress: %d, stores dispatchedToSQ: %d\n",
                freeEntries[tid].sqEntries, storesInProgress[tid],
                fromIEW->iewInfo[tid].dispatchedToSQ);
        return num_free;
}

/**
 * 计算来自解码阶段的有效指令数量
 * 排除已被清空的指令
 * @return 有效指令的数量
 */
unsigned
Rename::validInsts()
{
    unsigned inst_count = 0;

    for (int i=0; i<fromDecode->size; i++) {
        if (!fromDecode->insts[i]->isSquashed())
            inst_count++;                    // 统计未被清空的指令
    }

    return inst_count;
}

/**
 * 读取来自IEW阶段的停顿信号
 * 更新对应线程的停顿状态
 * @param tid 线程ID
 */
void
Rename::readStallSignals(ThreadID tid)
{
    if (fromIEW->iewBlock[tid]) {
        stalls[tid].iew = true;              // 设置IEW停顿标志
    }

    if (fromIEW->iewUnblock[tid]) {
        assert(stalls[tid].iew);
        stalls[tid].iew = false;             // 清除IEW停顿标志
    }
}

/**
 * 检查指定线程是否需要停顿
 * 检查各种资源和状态以确定重命名阶段是否应该阻塞
 * @param tid 线程ID
 * @return true 如果需要停顿，false 否则
 */
bool
Rename::checkStall(ThreadID tid)
{
    bool ret_val = false;

    // 检查IEW阶段是否发出停顿信号
    if (stalls[tid].iew) {
        DPRINTF(Rename,"[tid:%i] Stall from IEW stage detected.\n", tid);

        blockReason = fromIEW->iewInfo[tid].blockReason;
        ret_val = true;
    }
    // 检查ROB是否已满
    else if (calcFreeROBEntries(tid) <= 0) {
        DPRINTF(Rename,"[tid:%i] Stall: ROB has 0 free entries.\n", tid);
        blockReason = checkRenameStallFromIEW(tid);
        ret_val = true;
    }
    // 检查指令队列是否已满
    else if (calcFreeIQEntries(tid) <= 0) {
        DPRINTF(Rename,"[tid:%i] Stall: IQ has 0 free entries.\n", tid);
        blockReason = checkRenameStallFromIEW(tid);
        ret_val = true;
    }
    // 检查加载存储队列是否都已满
    else if (calcFreeLQEntries(tid) <= 0 && calcFreeSQEntries(tid) <= 0) {
        DPRINTF(Rename,"[tid:%i] Stall: LSQ has 0 free entries.\n", tid);
        blockReason = checkRenameStallFromIEW(tid);
        ret_val = true;
    }
    // 检查重命名映射表是否已满
    else if (renameMap[tid]->numFreeEntries() <= 0) {
        DPRINTF(Rename,"[tid:%i] Stall: RenameMap has 0 free entries.\n", tid);
        blockReason = checkRenameStallFromIEW(tid);
        ret_val = true;
    }
    // 检查串行化停顿状态
    else if (renameStatus[tid] == SerializeStall &&
               (!emptyROB[tid] || instsInProgress[tid])) {
        DPRINTF(Rename,"[tid:%i] Stall: Serialize stall and ROB is not "
                "empty.\n",
                tid);
        blockReason = StallReason::SerializeStall;
        ret_val = true;
    }

    return ret_val;
}

/**
 * 读取并更新空闲硬件资源条目信息
 * 从IEW和Commit阶段获取最新的资源使用情况
 * @param tid 线程ID
 */
void
Rename::readFreeEntries(ThreadID tid)
{
    // 从IEW阶段读取加载存储队列的空闲条目信息
    if (fromIEW->iewInfo[tid].usedLSQ) {
        freeEntries[tid].lqEntries = fromIEW->iewInfo[tid].freeLQEntries;
        freeEntries[tid].sqEntries = fromIEW->iewInfo[tid].freeSQEntries;
    }

    // 从Commit阶段读取ROB的空闲条目信息
    if (fromCommit->commitInfo[tid].usedROB) {
        freeEntries[tid].robEntries =
            fromCommit->commitInfo[tid].freeROBEntries;
        emptyROB[tid] = fromCommit->commitInfo[tid].emptyROB;
        if (emptyROB[tid])
            DPRINTF(Rename, "[tid:%i] ROB is empty now.\n", tid);
    }

    // 输出详细的资源使用统计信息
    DPRINTF(Rename, "[tid:%i] Free ROB: %i, "
                    "Free LQ: %i, Free SQ: %i, FreeRM %i(%i %i %i %i %i %i)\n",
            tid,
            freeEntries[tid].robEntries,         // 空闲ROB条目
            freeEntries[tid].lqEntries,          // 空闲LQ条目
            freeEntries[tid].sqEntries,          // 空闲SQ条目
            renameMap[tid]->numFreeEntries(),    // 总空闲寄存器
            renameMap[tid]->numFreeEntries(IntRegClass),      // 整型寄存器
            renameMap[tid]->numFreeEntries(FloatRegClass),    // 浮点寄存器
            renameMap[tid]->numFreeEntries(VecRegClass),      // 向量寄存器
            renameMap[tid]->numFreeEntries(VecElemClass),     // 向量元素寄存器
            renameMap[tid]->numFreeEntries(VecPredRegClass),  // 向量谓词寄存器
            renameMap[tid]->numFreeEntries(CCRegClass));      // 条件码寄存器

    DPRINTF(Rename, "[tid:%i] %i instructions not yet in ROB\n",
            tid, instsInProgress[tid]);
}

/**
 * 检查信号并更新线程状态
 * 处理清空信号、停顿信号和各种状态转换
 * @param tid 线程ID
 * @return true 如果状态发生变化需要特殊处理，false 可继续正常处理
 */
bool
Rename::checkSignalsAndUpdate(ThreadID tid)
{
    // 检查是否有清空信号，如果有则执行清空
    // 检查停顿信号，必要时进行阻塞
    // 如果状态是阻塞状态：
    //     检查停顿条件是否解除
    //         如果是则转到解阻塞状态
    // 如果状态是清空状态：
    //     检查清空是否完成，切换到运行状态
    // 如果状态是串行化停顿：
    //     检查ROB是否为空且没有指令在飞向ROB

    readFreeEntries(tid);                    // 读取空闲资源信息
    readStallSignals(tid);                   // 读取停顿信号

    // 处理来自Commit阶段的清空信号
    if (fromCommit->commitInfo[tid].squash) {
        DPRINTF(Rename, "[tid:%i] Squashing instructions due to squash from "
                "commit.\n", tid);

        squash(fromCommit->commitInfo[tid].doneSeqNum, tid);    // 执行清空操作

        // 更新本地清空版本号
        localSquashVer.update(fromCommit->commitInfo[tid].squashVersion.getVersion());
        DPRINTF(Rename, "Updating squash version to %u\n",
                localSquashVer.getVersion());

        return true;                         // 清空处理完成，需要特殊处理
    }

    // 检查是否需要停顿
    if (checkStall(tid)) {
        return block(tid);                   // 执行阻塞操作
    }

    // 处理阻塞状态转换
    if (renameStatus[tid] == Blocked) {
        DPRINTF(Rename, "[tid:%i] Done blocking, switching to unblocking.\n",
                tid);

        renameStatus[tid] = Unblocking;      // 转换到解阻塞状态

        unblock(tid);                        // 执行解阻塞操作

        return true;                         // 状态转换完成
    }

    // 处理清空状态转换
    if (renameStatus[tid] == Squashing) {
        // 如果重命名在此周期没有被要求阻塞或清空，则切换状态到运行
        if (resumeSerialize) {
            DPRINTF(Rename,
                    "[tid:%i] Done squashing, switching to serialize.\n", tid);

            renameStatus[tid] = SerializeStall;    // 转到串行化停顿状态
            return true;
        } else if (resumeUnblocking) {
            DPRINTF(Rename,
                    "[tid:%i] Done squashing, switching to unblocking.\n",
                    tid);
            renameStatus[tid] = Unblocking;        // 转到解阻塞状态
            return true;
        } else {
            DPRINTF(Rename, "[tid:%i] Done squashing, switching to running.\n",
                    tid);
            renameStatus[tid] = Running;           // 转到运行状态
            return false;                          // 可以继续正常处理
        }
    }

    // 处理串行化停顿状态
    if (renameStatus[tid] == SerializeStall) {
        // 当ROB为空时停顿结束
        DPRINTF(Rename, "[tid:%i] Done with serialize stall, switching to "
                "unblocking.\n", tid);

        DynInstPtr serial_inst = serializeInst[tid];

        renameStatus[tid] = Unblocking;            // 转到解阻塞状态

        unblock(tid);                              // 执行解阻塞操作

        DPRINTF(Rename, "[tid:%i] Processing instruction [%lli] with "
                "PC %s.\n", tid, serial_inst->seqNum, serial_inst->pcState());

        // 将指令放入队列中处理
        serial_inst->clearSerializeBefore();    // 清除串行化标志

        // 根据缓冲区状态选择放入位置
        if (!skidBuffer[tid].empty()) {
            skidBuffer[tid].push_front(serial_inst);    // 放入缓冲区前端
        } else {
            insts[tid].push_front(serial_inst);         // 放入指令队列前端
        }

        DPRINTF(Rename, "[tid:%i] Instruction must be processed by rename."
                " Adding to front of list.\n", tid);

        serializeInst[tid] = NULL;               // 清空串行化指令指针

        return true;                             // 状态转换完成
    }

    // 如果到达这里，说明没有收到任何导致重命名改变状态的信号
    // 重命名保持与之前相同的状态
    return false;
}

/**
 * 处理后串行化操作
 * 将下一条指令标记为前串行化指令
 * @param inst_list 指令列表
 * @param tid 线程ID
 */
void
Rename::serializeAfter(InstQueue &inst_list, ThreadID tid)
{
    if (inst_list.empty()) {
        // 标记下一条指令需要串行化
        serializeOnNextInst[tid] = true;
        return;
    }

    // 将下一条指令设置为前串行化指令
    inst_list.front()->setSerializeBefore();
}

/**
 * 增加资源充满事件统计
 * 根据资源类型更新相应的统计计数器
 * @param source 资源类型（ROB、IQ、LQ、SQ）
 */
void
Rename::incrFullStat(const FullSource &source)
{
    switch (source) {
      case ROB:
        ++stats.ROBFullEvents;               // ROB充满事件
        stats.stallEvents[ROBFull]++;        // ROB停顿事件
        break;
      case IQ:
        ++stats.IQFullEvents;                // IQ充满事件
        stats.stallEvents[IQFull]++;         // IQ停顿事件
        break;
      case LQ:
        ++stats.LQFullEvents;                // LQ充满事件
        stats.stallEvents[LSQFull]++;        // LSQ停顿事件
        break;
      case SQ:
        ++stats.SQFullEvents;                // SQ充满事件
        stats.stallEvents[LSQFull]++;        // LSQ停顿事件
        break;
      default:
        panic("Rename full stall stat should be incremented for a reason!");
        break;
    }
}

/**
 * 输出重命名历史缓冲区内容
 * 用于调试和分析重命名操作历史
 */
void
Rename::dumpHistory()
{
    std::list<RenameHistory>::iterator buf_it;

    // 遍历所有线程的历史缓冲区
    for (ThreadID tid = 0; tid < numThreads; tid++) {

        buf_it = historyBuffer[tid].begin();

        // 输出每个历史条目的详细信息
        while (buf_it != historyBuffer[tid].end()) {
            cprintf("Seq num: %i\nArch reg[%s]: %i New phys reg:"
                    " %i[%s] Old phys reg: %i[%s]\n",
                    (*buf_it).instSeqNum,                    // 指令序列号
                    (*buf_it).archReg.className(),           // 架构寄存器类型
                    (*buf_it).archReg.index(),               // 架构寄存器索引
                    (*buf_it).newPhysReg.PhyReg()->index(),  // 新物理寄存器索引
                    (*buf_it).newPhysReg.PhyReg()->className(),  // 新物理寄存器类型
                    (*buf_it).prevPhysReg.PhyReg()->index(), // 旧物理寄存器索引
                    (*buf_it).prevPhysReg.PhyReg()->className()); // 旧物理寄存器类型

            buf_it++;                                // 移动到下一个条目
        }
    }
}

void
Rename::setAllStalls(StallReason renameStall)
{
    for (int i = 0;i < renameStalls.size();i++) {
        renameStalls.at(i) = renameStall;
    }
}

StallReason
Rename::checkRenameStallFromIEW(ThreadID tid)
{
    StallReason robHeadStallReason = fromIEW->iewInfo[tid].robHeadStallReason;

    if (robHeadStallReason == StallReason::NoStall) {
        if (calcFreeLQEntries(tid) <= 0) {
            return fromIEW->iewInfo[tid].lqHeadStallReason;
        } else if (calcFreeSQEntries(tid) <= 0) {
            return fromIEW->iewInfo[tid].sqHeadStallReason;
        } else {
            return robHeadStallReason;
        }
    } else {
        return robHeadStallReason;
    }
}

} // namespace o3
} // namespace gem5

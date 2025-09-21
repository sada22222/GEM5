/*
 * Copyright (c) 2011-2012, 2014, 2017-2019, 2021 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
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
 * Copyright (c) 2005-2006 The Regents of The University of Michigan
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

#include "cpu/o3/lsq.hh"

#include <algorithm>
#include <cassert>
#include <csignal>
#include <cstdint>
#include <list>
#include <string>

#include "arch/riscv/insts/vector.hh"
#include "base/compiler.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/iew.hh"
#include "cpu/o3/limits.hh"
#include "debug/Drain.hh"
#include "debug/Fetch.hh"
#include "debug/Hint.hh"
#include "debug/HtmCpu.hh"
#include "debug/LSQ.hh"
#include "debug/PacketSender.hh"
#include "debug/Schedule.hh"
#include "debug/StoreBuffer.hh"
#include "debug/TagReadFail.hh"
#include "debug/Writeback.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "mem/request.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

// DcachePort构造函数：初始化LSQ的数据缓存端口
// _lsq: 指向LSQ的指针
// _cpu: 指向CPU的指针
// 该端口用于LSQ与数据缓存之间的通信
LSQ::DcachePort::DcachePort(LSQ *_lsq, CPU *_cpu) :
    RequestPort(_cpu->name() + ".dcache_port", _cpu), lsq(_lsq), cpu(_cpu)
{}

// 静态成员：维护所有SingleDataRequest的全局列表
// 用于追踪和管理所有单数据请求对象
std::list<LSQ::SingleDataRequest*> LSQ::SingleDataRequest::singleList;

// LSQ构造函数：初始化Load-Store Queue
// cpu_ptr: CPU指针
// iew_ptr: IEW阶段指针
// params: O3 CPU参数
LSQ::LSQ(CPU *cpu_ptr, IEW *iew_ptr, const BaseO3CPUParams &params)
    : cpu(cpu_ptr), iewStage(iew_ptr),
      _cacheBlocked(false),                                      // 缓存阻塞标志
      cacheStorePorts(params.cacheStorePorts), usedStorePorts(0),  // Store端口配置和使用计数
      cacheLoadPorts(params.cacheLoadPorts), usedLoadPorts(0),     // Load端口配置和使用计数
      recentlyloadAddr(8),                                       // 最近访问的Load地址LRU缓存（8项）
      enableBankConflictCheck(params.BankConflictCheck),         // Bank冲突检查使能
      sbufferBankWriteAccurately(params.sbufferBankWriteAccurately),  // Store Buffer精确写Bank标志
      _enableLdMissReplay(params.EnableLdMissReplay),            // Load Miss重放使能
      _enablePipeNukeCheck(params.EnablePipeNukeCheck),          // 流水线Nuke检查使能
      _storeWbStage(params.StoreWbStage),                        // Store写回阶段（2-4）
      waitingForStaleTranslation(false),                         // 等待过时地址翻译标志
      staleTranslationWaitTxnId(0),                              // 过时翻译等待事务ID
      lsqPolicy(params.smtLSQPolicy),                            // SMT LSQ策略（Dynamic/Partitioned/Threshold）
      LQEntries(params.LQEntries),                               // Load Queue项数
      SQEntries(params.SQEntries),                               // Store Queue项数
      maxLQEntries(maxLSQAllocation(lsqPolicy, LQEntries, params.numThreads,
                  params.smtLSQThreshold)),                      // 最大LQ项数（考虑SMT策略）
      maxSQEntries(maxLSQAllocation(lsqPolicy, SQEntries, params.numThreads,
                  params.smtLSQThreshold)),                      // 最大SQ项数（考虑SMT策略）
      dcachePort(this, cpu_ptr),                                 // 数据缓存端口
      numThreads(params.numThreads)                              // 线程数量
{
    // 检查线程数量有效性
    assert(numThreads > 0 && numThreads <= MaxThreads);
    // 验证Load Miss重放和流水线Nuke检查的配置一致性
    if (!_enableLdMissReplay && _enablePipeNukeCheck) {
        panic("LSQ can not support pipeline nuke replay when EnableLdMissReplay is False");
    }
    // Store写回阶段必须在2-4之间
    assert(_storeWbStage >= 2 && _storeWbStage <= 4);

    //**********************************************
    //************ 处理SMT参数 *********************
    //**********************************************

    /* 运行SMT策略检查 */
        // Dynamic策略：动态分配LQ/SQ资源
        if (lsqPolicy == SMTQueuePolicy::Dynamic) {
        DPRINTF(LSQ, "LSQ sharing policy set to Dynamic\n");
    // Partitioned策略：静态分区，每个线程固定分配
    } else if (lsqPolicy == SMTQueuePolicy::Partitioned) {
        DPRINTF(Fetch, "LSQ sharing policy set to Partitioned: "
                "%i entries per LQ | %i entries per SQ\n",
                maxLQEntries,maxSQEntries);
    // Threshold策略：基于阈值的动态分配
    } else if (lsqPolicy == SMTQueuePolicy::Threshold) {

        assert(params.smtLSQThreshold > params.LQEntries);
        assert(params.smtLSQThreshold > params.SQEntries);

        DPRINTF(LSQ, "LSQ sharing policy set to Threshold: "
                "%i entries per LQ | %i entries per SQ\n",
                maxLQEntries,maxSQEntries);
    } else {
        panic("Invalid LSQ sharing policy. Options are: Dynamic, "
                    "Partitioned, Threshold");
    }

    // 预留线程数量的空间
    thread.reserve(numThreads);
    // TODO: 将load/store流水线阶段数参数化
    // 为每个线程创建LSQUnit
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        // 创建LSQUnit，传入各项配置参数：
        // - Load/Store Queue大小
        // - Store Buffer配置（项数、驱逐阈值、非活跃阈值）
        // - 流水线阶段数（Load/Store）
        // - RAR/RAW队列配置（项数、出队宽度）
        // - 完成宽度（Load/Store）
        thread.emplace_back(maxLQEntries, maxSQEntries, params.SbufferEntries,
            params.SbufferEvictThreshold, params.storeBufferInactiveThreshold,
            params.LdPipeStages, params.StPipeStages, params.RARQEntries, params.RAWQEntries,
            params.RARDequeuePerCycle, params.RAWDequeuePerCycle, params.LoadCompletionWidth,
            params.StoreCompletionWidth);
        // 初始化LSQUnit，传入CPU、IEW、参数、LSQ指针和线程ID
        thread[tid].init(cpu, iew_ptr, params, this, tid);
        // 设置数据缓存端口
        thread[tid].setDcachePort(&dcachePort);
    }

    // 初始化Bank占用状态数组（8个Bank）
    bankOccupied.resize(8, false);
}


// 返回LSQ的名称，格式为"iewStage名称.lsq"
std::string
LSQ::name() const
{
    return iewStage->name() + ".lsq";
}

// 设置活跃线程列表
// at_ptr: 指向活跃线程ID列表的指针
void
LSQ::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
    assert(activeThreads != 0);
}

// 排空健全性检查：验证LSQ是否已完全排空
void
LSQ::drainSanityCheck() const
{
    assert(isDrained());

    // 检查每个线程的LSQUnit是否已排空
    for (ThreadID tid = 0; tid < numThreads; tid++)
        thread[tid].drainSanityCheck();
}

// 检查LSQ是否已排空（LQ和SQ都为空）
bool
LSQ::isDrained() const
{
    bool drained(true);

    // Load Queue必须为空
    if (!lqEmpty()) {
        DPRINTF(Drain, "Not drained, LQ not empty.\n");
        drained = false;
    }

    // Store Queue必须为空
    if (!sqEmpty()) {
        DPRINTF(Drain, "Not drained, SQ not empty.\n");
        drained = false;
    }

    return drained;
}

// 接管LSQ状态：用于CPU切换或恢复
void
LSQ::takeOverFrom()
{
    // 重置端口使用计数
    usedStorePorts = 0;
    _cacheBlocked = false;

    // 每个线程的LSQUnit执行接管
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        thread[tid].takeOverFrom();
    }
}

// LSQ每周期执行：处理端口限制和tick所有LSQUnit
void
LSQ::tick()
{
    // 如果Load端口达到上限且缓存未阻塞，则通知IEW阶段缓存已解除阻塞
    // 这允许重新发射因端口限制而被阻塞的Load指令
    if (usedLoadPorts == cacheLoadPorts && !_cacheBlocked)
        iewStage->cacheUnblocked();

    // 重置端口使用计数
    usedLoadPorts = 0;
    usedStorePorts = 0;

    // 对所有活跃线程的LSQUnit执行tick
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        thread[tid].tick();
    }

}

// 清除地址追踪信息：Bank占用状态和最近访问的Load地址
void
LSQ::clearAddresses()
{
    std::fill(bankOccupied.begin(), bankOccupied.end(), false);
    recentlyloadAddr.clear();
}

// Load Bank冲突检查：检测是否存在Bank冲突
// vaddr: 虚拟地址
// 返回值: true表示存在Bank冲突
bool
LSQ::loadBankConflictedCheck(Addr vaddr)
{
    bool now_bank_conflict = false;
    // 地址位域划分：
    // [12:6]   - setIndex（Cache Set索引）
    // [5:3]    - bankIndex（Bank索引）
    // [2:0]    - dataOffset（数据偏移）
    const uint64_t cacheBankmask = 0b1111111111000;  // 屏蔽低3位
    const int bankIndex = bankNum(vaddr);

    if (enableBankConflictCheck) {
        // 检查最近访问地址缓存中是否有相同的Cache Line
        if (recentlyloadAddr.contains((vaddr & cacheBankmask))) {
            recentlyloadAddr.get((vaddr & cacheBankmask));
            return false;  // 最近访问过，不认为是冲突
        }
        // 检查Bank是否已被占用
        auto bank_occupied = bankOccupied.at(bankIndex);
        if (bank_occupied) {
            now_bank_conflict = true;  // Bank已占用，存在冲突

        } else {
            bank_occupied = true;  // 标记Bank为占用
            recentlyloadAddr.insert((vaddr & cacheBankmask), {});  // 记录访问
        }
    }
    return now_bank_conflict;
}

// 获取指定线程的Load Queue空闲项数
unsigned
LSQ::getFreeLQEntries(ThreadID tid)
{
    return thread[tid].numFreeLoadEntries();
}

// 获取指定线程的Store Queue空闲项数
unsigned
LSQ::getFreeSQEntries(ThreadID tid)
{
    return thread[tid].numFreeStoreEntries();
}

// 获取并重置指定线程上个周期从LQ弹出的项数
unsigned
LSQ::getAndResetLastLQPopEntries(ThreadID tid)
{
    return thread[tid].getAndResetLastClockLQPopEntries();
}

// 获取并重置指定线程上个周期从SQ弹出的项数
unsigned
LSQ::getAndResetLastSQPopEntries(ThreadID tid)
{
    return thread[tid].getAndResetLastClockSQPopEntries();
}

// 检查缓存是否被阻塞
bool
LSQ::cacheBlocked() const
{
    return _cacheBlocked;
}

// 设置缓存阻塞状态
void
LSQ::cacheBlocked(bool v)
{
    _cacheBlocked = v;
}

// 检查缓存端口是否可用
// is_load: true表示Load端口，false表示Store端口
// 返回值: true表示端口可用
bool
LSQ::cachePortAvailable(bool is_load) const
{
    bool ret;
    if (is_load) {
        ret  = usedLoadPorts < cacheLoadPorts;  // Load端口未满
    } else {
        ret  = usedStorePorts < cacheStorePorts;  // Store端口未满
    }
    return ret;
}

// 标记缓存端口为忙碌（增加使用计数）
// is_load: true表示Load端口，false表示Store端口
void
LSQ::cachePortBusy(bool is_load)
{
    assert(cachePortAvailable(is_load));
    if (is_load) {
        usedLoadPorts++;    // 增加Load端口使用计数
    } else {
        usedStorePorts++;   // 增加Store端口使用计数
    }
}

// 将Load指令插入Load Queue
// load_inst: Load指令的动态实例
void
LSQ::insertLoad(const DynInstPtr &load_inst)
{
    ThreadID tid = load_inst->threadNumber;

    thread[tid].insertLoad(load_inst);
}

// 将Store指令插入Store Queue
// store_inst: Store指令的动态实例
void
LSQ::insertStore(const DynInstPtr &store_inst)
{
    ThreadID tid = store_inst->threadNumber;

    thread[tid].insertStore(store_inst);
}

// 将指令发射到Load流水线
// inst: 待发射的指令
void
LSQ::issueToLoadPipe(const DynInstPtr &inst)
{
    ThreadID tid = inst->threadNumber;

    thread[tid].issueToLoadPipe(inst);
}

// 将指令发射到Store流水线
// inst: 待发射的指令
void
LSQ::issueToStorePipe(const DynInstPtr &inst)
{
    ThreadID tid = inst->threadNumber;

    thread[tid].issueToStorePipe(inst);
}

// 执行Store流水线的地址计算阶段（Sx阶段）
// 对所有活跃线程执行
void
LSQ::executePipeSx()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        thread[tid].executePipeSx();
    }
}

// 执行原子内存操作（AMO）
// inst: 原子操作指令
// 返回值: 可能产生的Fault
Fault
LSQ::executeAmo(const DynInstPtr &inst)
{
    ThreadID tid = inst->threadNumber;

    return thread[tid].executeAmo(inst);
}

// 提交Load指令
// youngest_inst: 最年轻指令的序列号
// tid: 线程ID
void
LSQ::commitLoads(InstSeqNum &youngest_inst, ThreadID tid)
{
    thread.at(tid).commitLoads(youngest_inst);
}

// 提交Store指令
// youngest_inst: 最年轻指令的序列号
// tid: 线程ID
void
LSQ::commitStores(InstSeqNum &youngest_inst, ThreadID tid)
{
    thread.at(tid).commitStores(youngest_inst);
}

// 写回Store Buffer到缓存
// 1. 清除地址追踪信息（在Load发送packet之后，Store Buffer发送packet之前）
// 2. 对所有活跃线程执行Store Buffer驱逐和卸载操作
void
LSQ::writebackStoreBuffer()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    // 在Load发送packets之后
    // 在sbuffer发送packets之前
    clearAddresses();

    while (threads != end) {
        ThreadID tid = *threads++;

        thread[tid].storeBufferEvictToCache();  // Store Buffer驱逐到缓存
        thread[tid].offloadToStoreBuffer();     // 卸载Store到Store Buffer
    }
}

// Squash（清空）指定线程中序列号大于squashed_num的所有指令
// squashed_num: 被squash的指令序列号
// tid: 线程ID
void
LSQ::squash(const InstSeqNum &squashed_num, ThreadID tid)
{
    thread.at(tid).squash(squashed_num);
}

bool
LSQ::violation()
{
    /* Answers: Does Anybody Have a Violation?*/
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (thread[tid].violation())
            return true;
    }

    return false;
}

bool LSQ::violation(ThreadID tid) { return thread.at(tid).violation(); }

DynInstPtr
LSQ::getMemDepViolator(ThreadID tid)
{
    return thread.at(tid).getMemDepViolator();
}

int
LSQ::getLoadHead(ThreadID tid)
{
    return thread.at(tid).getLoadHead();
}

InstSeqNum
LSQ::getLoadHeadSeqNum(ThreadID tid)
{
    return thread.at(tid).getLoadHeadSeqNum();
}

int
LSQ::getStoreHead(ThreadID tid)
{
    return thread.at(tid).getStoreHead();
}

InstSeqNum
LSQ::getStoreHeadSeqNum(ThreadID tid)
{
    return thread.at(tid).getStoreHeadSeqNum();
}

int LSQ::getCount(ThreadID tid) { return thread.at(tid).getCount(); }

int LSQ::numLoads(ThreadID tid) { return thread.at(tid).numLoads(); }

int LSQ::anyInflightLoadsNotComplete()
{
    int l1miss = 0, l2miss = 0, l3miss = 0, any = 0;
    for (auto it : thread.at(0).inflightLoads) {
        if (it->isAnyOutstandingRequest()) {
            if (it->mainReq()->depth == 1) {
                l1miss = 1;
            }
            if (it->mainReq()->depth == 2) {
                l2miss = 1 << 1;
            }
            if (it->mainReq()->depth == 3) {
                l3miss = 1 << 2;
            }
            any = 1 << 3;
        }
    }
    return l1miss | l2miss | l3miss | any;
}

bool
LSQ::anyStoreNotExecute()
{
    for (auto& it : thread.at(0).storeQueue) {
        if (!it.instruction()->isIssued()) {
            return true;
        }
    }
    return false;
}

int LSQ::numStores(ThreadID tid) { return thread.at(tid).numStores(); }

int
LSQ::numHtmStarts(ThreadID tid) const
{
    if (tid == InvalidThreadID)
        return 0;
    else
        return thread[tid].numHtmStarts();
}
int
LSQ::numHtmStops(ThreadID tid) const
{
    if (tid == InvalidThreadID)
        return 0;
    else
        return thread[tid].numHtmStops();
}

void
LSQ::resetHtmStartsStops(ThreadID tid)
{
    if (tid != InvalidThreadID)
        thread[tid].resetHtmStartsStops();
}

uint64_t
LSQ::getLatestHtmUid(ThreadID tid) const
{
    if (tid == InvalidThreadID)
        return 0;
    else
        return thread[tid].getLatestHtmUid();
}

void
LSQ::setLastRetiredHtmUid(ThreadID tid, uint64_t htmUid)
{
    if (tid != InvalidThreadID)
        thread[tid].setLastRetiredHtmUid(htmUid);
}

// 接收请求重试信号：当缓存端口变为可用时调用
void
LSQ::recvReqRetry()
{
    iewStage->cacheUnblocked();  // 通知IEW阶段缓存已解除阻塞
    cacheBlocked(false);         // 清除缓存阻塞标志

    // 对所有活跃线程重试失败的请求
    for (ThreadID tid : *activeThreads) {
        thread[tid].recvRetry();
    }
}


// 接收时序响应：处理从缓存返回的响应packet
// pkt: 响应packet
// 返回值: true表示成功处理
bool
LSQ::recvTimingResp(PacketPtr pkt)
{
    // 检查是否有错误
    if (pkt->isError())
        DPRINTF(LSQ, "Got error packet back for address: %#X\n",
                pkt->getAddr());

    // 从packet的senderState中恢复LSQRequest
    LSQRequest *request = dynamic_cast<LSQRequest*>(pkt->senderState);
    panic_if(!request, "Got packet back with unknown sender state\n");

    // 将响应转发给对应线程的LSQUnit处理
    thread[request->_port.lsqID].recvTimingResp(pkt);

    if (pkt->isInvalidate()) {
        // This response also contains an invalidate; e.g. this can be the case
        // if cmd is ReadRespWithInvalidate.
        //
        // The calling order between completeDataAccess and checkSnoop matters.
        // By calling checkSnoop after completeDataAccess, we ensure that the
        // fault set by checkSnoop is not lost. Calling writeback (more
        // specifically inst->completeAcc) in completeDataAccess overwrites
        // fault, and in case this instruction requires squashing (as
        // determined by checkSnoop), the ReExec fault set by checkSnoop would
        // be lost otherwise.

        DPRINTF(LSQ, "received invalidation with response for addr:%#x\n",
                pkt->getAddr());

        for (ThreadID tid = 0; tid < numThreads; tid++) {
            thread[tid].checkSnoop(pkt);
        }
    }

    if (request->isNormalLd() &&
        !request->instruction()->cacheHit()) {
        // if cache miss, the packet must be delete
        assert(request->isReleased());
        assert(request->_numOutstandingPackets == 1);
    }

    request->packetReplied();

    if (waitingForStaleTranslation) {
        checkStaleTranslations();
    }

    return true;
}

void
LSQ::recvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(LSQ, "received pkt for addr:%#x %s\n", pkt->getAddr(),
            pkt->cmdString());

    // must be a snoop
    if (pkt->isInvalidate()) {
        DPRINTF(LSQ, "received invalidation for addr:%#x\n",
                pkt->getAddr());
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            thread[tid].checkSnoop(pkt);
        }
    } else if (pkt->req && pkt->req->isTlbiExtSync()) {
        DPRINTF(LSQ, "received TLBI Ext Sync\n");
        assert(!waitingForStaleTranslation);

        waitingForStaleTranslation = true;
        staleTranslationWaitTxnId = pkt->req->getExtraData();

        for (auto& unit : thread) {
            unit.startStaleTranslationFlush();
        }

        // In case no units have pending ops, just go ahead
        checkStaleTranslations();
    }
}

void
LSQ::recvFunctionalCustomSignal(PacketPtr pkt, int sig)
{
    if (sig <= 0) {
        return;
    }
    DPRINTF(LSQ, "recvFunctionalCustomSignal: Resp type: %d\n", sig);

    LSQRequest *request = nullptr;
    if (sig != DcacheRespType::Bus_Clear) {
        // Bus_Clear event does not need request info
        request = dynamic_cast<LSQRequest*>(pkt->getPrimarySenderState());
        panic_if(!request, "Got packet back with unknown sender state\n");
    }

    if (sig == DcacheRespType::Miss || sig == DcacheRespType::Block_Not_Ready) {
        DPRINTF(LSQ, "[sn:%ld] CacheMiss: %d, BlockUnready: %d\n",
                request->instruction()->seqNum,
                sig == DcacheRespType::Miss,
                sig == DcacheRespType::Block_Not_Ready);
    } else if (sig == DcacheRespType::Hint) {
        // get cache miss load replay hint
        request->recvFunctionalCustomSignal(pkt);
    } else if (sig == DcacheRespType::Bus_Clear) {
        assert(pkt->cmd == MemCmd::CustomBusClear);
        // Data block is ready in Dcache, data on bus can be cleared now
        Addr busClearBlkAddr = pkt->getAddr();
        DPRINTF(Hint, "Bus Clear\n");
        DPRINTF(LSQ, "Bus_Clear, clear address: %#lx, bus size: %d\n", busClearBlkAddr, bus.size());
        for (auto it = bus.begin(); it != bus.end();) {
            auto [seqNum, addr] = *it;
            if ((addr & ~((uint64_t)cpu->cacheLineSize() - 1)) == busClearBlkAddr) {
                it = bus.erase(it);
                DPRINTF(LSQ, " erased bus: [sn:%ld] addr: %#lx\n", seqNum, addr);
            } else {
                it++;
            }
        }
        panic_if(bus.size() > getLQEntries(), "elements on bus should never be greater than LQ size");
    } else {
        panic("unsupported sig %d in recvFunctionalCustomSignal\n", sig);
    }
}

void*
LSQ::getCPUPtr() {
    return (void *) cpu;
}

int
LSQ::getCount()
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += getCount(tid);
    }

    return total;
}

int
LSQ::numLoads()
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += numLoads(tid);
    }

    return total;
}

int
LSQ::numStores()
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += thread[tid].numStores();
    }

    return total;
}

unsigned
LSQ::numFreeLoadEntries()
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += thread[tid].numFreeLoadEntries();
    }

    return total;
}

unsigned
LSQ::numFreeStoreEntries()
{
    unsigned total = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        total += thread[tid].numFreeStoreEntries();
    }

    return total;
}

unsigned
LSQ::numFreeLoadEntries(ThreadID tid)
{
        return thread[tid].numFreeLoadEntries();
}

unsigned
LSQ::numFreeStoreEntries(ThreadID tid)
{
        return thread[tid].numFreeStoreEntries();
}

bool
LSQ::isFull()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!(thread[tid].lqFull() || thread[tid].sqFull()))
            return false;
    }

    return true;
}

bool
LSQ::isFull(ThreadID tid)
{
    //@todo: Change to Calculate All Entries for
    //Dynamic Policy
    if (lsqPolicy == SMTQueuePolicy::Dynamic)
        return isFull();
    else
        return thread[tid].lqFull() || thread[tid].sqFull();
}

bool
LSQ::isEmpty() const
{
    return lqEmpty() && sqEmpty();
}

bool
LSQ::lqEmpty() const
{
    std::list<ThreadID>::const_iterator threads = activeThreads->begin();
    std::list<ThreadID>::const_iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!thread[tid].lqEmpty())
            return false;
    }

    return true;
}

bool
LSQ::sqEmpty() const
{
    std::list<ThreadID>::const_iterator threads = activeThreads->begin();
    std::list<ThreadID>::const_iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!thread[tid].sqEmpty())
            return false;
    }

    return true;
}

bool
LSQ::lqFull()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!thread[tid].lqFull())
            return false;
    }

    return true;
}

bool
LSQ::lqFull(ThreadID tid)
{
    //@todo: Change to Calculate All Entries for
    //Dynamic Policy
    if (lsqPolicy == SMTQueuePolicy::Dynamic)
        return lqFull();
    else
        return thread[tid].lqFull();
}

bool
LSQ::sqFull()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!sqFull(tid))
            return false;
    }

    return true;
}

bool
LSQ::sqFull(ThreadID tid)
{
     //@todo: Change to Calculate All Entries for
    //Dynamic Policy
    if (lsqPolicy == SMTQueuePolicy::Dynamic)
        return sqFull();
    else
        return thread[tid].sqFull();
}

bool
LSQ::vlMergeBufferBlocked()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        if (thread[tid].vlLoadMergeBuffer.isBlocked() ||
            thread[tid].vlStoreMergeBuffer.isBlocked())
            return true;
    }

    return false;
}

bool
LSQ::vlMergeBufferBlocked(ThreadID tid)
{
    //@todo: Change to Calculate All Entries for
    //Dynamic Policy
    if (lsqPolicy == SMTQueuePolicy::Dynamic)
        return vlMergeBufferBlocked();
    else
        return thread[tid].vlLoadMergeBuffer.isBlocked() ||
               thread[tid].vlStoreMergeBuffer.isBlocked();
}

const DynInstPtr&
LSQ::getLSQHeadInst(ThreadID tid, bool isLoad)
{
    if (isLoad) {
        assert(!thread[tid].loadQueue.empty());
        return thread[tid].loadQueue.front().instruction();
    } else {
        assert(!thread[tid].storeQueue.empty());
        return thread[tid].storeQueue.front().instruction();
    }
}

bool
LSQ::isStalled()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!thread[tid].isStalled())
            return false;
    }

    return true;
}

bool
LSQ::isStalled(ThreadID tid)
{
    if (lsqPolicy == SMTQueuePolicy::Dynamic)
        return isStalled();
    else
        return thread[tid].isStalled();
}

bool
LSQ::hasStoresToWB()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (hasStoresToWB(tid))
            return true;
    }

    return false;
}

bool
LSQ::hasStoresToWB(ThreadID tid)
{
    return thread.at(tid).hasStoresToWB();
}

bool LSQ::flushAllStores(ThreadID tid)
{
    thread.at(tid).flushStoreBuffer();
    bool t = thread.at(tid).hasStoresToWB() == 0 && thread.at(tid).storeBufferEmpty();
    return t;
}

int
LSQ::numStoresToSbuffer(ThreadID tid)
{
    return thread.at(tid).numStoresToSbuffer();
}

bool
LSQ::willWB()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (willWB(tid))
            return true;
    }

    return false;
}

bool
LSQ::willWB(ThreadID tid)
{
    return thread.at(tid).willWB();
}

void
LSQ::dumpInsts() const
{
    std::list<ThreadID>::const_iterator threads = activeThreads->begin();
    std::list<ThreadID>::const_iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        thread[tid].dumpInsts();
    }
}

void
LSQ::dumpInsts(ThreadID tid) const
{
    thread.at(tid).dumpInsts();
}

// 检查访问是否地址未对齐
// inst: 指令实例
// request: LSQ请求
// 返回值: true表示地址未对齐
bool
LSQ::isMisaligned(const DynInstPtr& inst, LSQRequest* request)
{
    // 根据Load/Store类型确定异常码
    auto code = inst->isLoad() ? RiscvISA::ExceptionCode::LOAD_ADDR_MISALIGNED
                                              : RiscvISA::ExceptionCode::STORE_ADDR_MISALIGNED;
    // 非向量指令且访问大小>1字节时，检查地址是否按访问大小对齐
    if (!inst->isVector() && request->mainReq()->getSize() > 1 &&
        request->mainReq()->getVaddr() % request->mainReq()->getSize() != 0) {
        DPRINTF(LSQUnit, "[sn:%lld] misaligned: size: %u, Addr: %#lx, code: %d\n",
                inst->seqNum, request->mainReq()->getSize(), request->mainReq()->getVaddr(), code);
        // 设置地址错误Fault
        inst->getFault() = std::make_shared<RiscvISA::AddressFault>(request->mainReq()->getVaddr(),
                                                                    request->mainReq()->getgPaddr(), code);
        return true;
    }
    return false;
}

// 推送访存请求到LSQ：创建并初始化LSQRequest
// inst: 指令实例
// isLoad: true表示Load，false表示Store
// data: 数据缓冲区指针
// size: 访问大小
// addr: 虚拟地址
// flags: 请求标志
// res: 结果指针（用于原子操作）
// amo_op: 原子操作函子
// byte_enable: 字节使能掩码
// 返回值: 可能产生的Fault
Fault
LSQ::pushRequest(const DynInstPtr& inst, bool isLoad, uint8_t *data,
        unsigned int size, Addr addr, Request::Flags flags, uint64_t *res,
        AtomicOpFunctorPtr amo_op, const std::vector<bool>& byte_enable)
{
    // 这个请求可能是load、store或原子操作
    // 原子请求会有对应的原子内存操作函子指针
    [[maybe_unused]] bool isAtomic = !isLoad && amo_op;

    ThreadID tid = cpu->contextToThread(inst->contextId());
    auto cacheLineSize = cpu->cacheLineSize();
    // 检查是否需要burst传输（跨Cache Line访问）
    bool needs_burst = transferNeedsBurst(addr, size, cacheLineSize);
    LSQRequest* request = nullptr;

    // 跨Cache Line的原子请求当前不支持
    // 因为缓存无法保证跨Cache Line的原子操作的原子性
    // 对于x86等支持跨Cache Line原子指令的ISA，需要修改缓存以支持
    // 对两个Cache Line的原子更新。当前不支持这种跨行更新。
    assert(!isAtomic || (isAtomic && !needs_burst));

    // 检查是否为HTM命令或TLBI命令（这些是特殊的Load类指令）
    const bool htm_cmd = isLoad && (flags & Request::HTM_CMD);
    const bool tlbi_cmd = isLoad && (flags & Request::TLBI_CMD);

    // 检查地址翻译是否已开始
    if (inst->translationStarted()) {
        // 地址翻译已开始，使用保存的request
        request = inst->savedRequest;
        assert(request);
    } else {
        // 地址翻译未开始，根据请求类型创建新的request
        if (htm_cmd || tlbi_cmd) {
            // HTM命令或TLBI命令：使用虚拟地址0x0和大小8的特殊请求
            assert(addr == 0x0lu);
            assert(size == 8);
            request = new UnsquashableDirectRequest(&thread[tid], inst, flags);
        } else if (needs_burst) {
            // 跨Cache Line访问：需要Split请求
            request = new SplitDataRequest(&thread[tid], inst, isLoad, addr, size, flags, data, res);
        } else {
            // 单个Cache Line内访问：使用Single请求
            request = new SingleDataRequest(&thread[tid], inst, isLoad, addr, size, flags, data, res,
                                            std::move(amo_op));
        }
        assert(request);
        request->_byteEnable = byte_enable;  // 设置字节使能
        inst->setRequest();                   // 标记指令有request
        request->taskId(cpu->taskId());      // 设置任务ID

        // 如果是严格顺序的Load，可能有来自之前执行尝试的Fault
        // 清除之前的Fault
        inst->getFault() = NoFault;

        // 发起地址翻译
        request->initiateTranslation();
    }


    // Store指令（非原子操作）临时将数据保存在memData中
    if (!isLoad && !isAtomic) {
        inst->memData = new uint8_t[size];
        memcpy(inst->memData, data, size);
    }

    /* 这里是指令获取effAddr的位置 */
    /* 只有原子类型可以在此阶段尝试向缓存发送请求 */
    if (request->isTranslationComplete()) {
        if (request->isMemAccessRequired()) {
            // 设置有效地址和大小
            inst->effAddr = request->getVaddr();
            inst->effSize = size;
            inst->effAddrValid(true);

            // 如果有checker，创建验证用的请求副本
            if (cpu->checker) {
                inst->reqToVerify = std::make_shared<Request>(*request->req());
            }

            // 原子指令立即执行访问
            if (inst->isAtomic()) {
                Fault fault;
                if (isLoad)
                    fault = read(request, inst->lqIdx);
                else
                    fault = write(request, data, inst->sqIdx);
                // inst->getFault()此时可能已有multi-access split请求的第一个fault
                // 只有当出现其他类型fault（如re-exec）时才覆盖
                if (fault != NoFault)
                    inst->getFault() = fault;
            }
        } else if (isLoad) {
            // 不需要内存访问的Load（如谓词为false）
            inst->setMemAccPredicate(false);
            // Commit阶段需要清理。标记指令已执行
            inst->setExecuted();
        }
        // 检查地址对齐
        if (isMisaligned(inst, request)) {
            // inst->getFault()在isMisaligned()中设置
            return inst->getFault();
        }
    }

    // 设置trace数据
    if (inst->traceData)
        inst->traceData->setMem(addr, size, flags);

    return inst->getFault();
}

// SingleDataRequest构造函数：创建单数据请求（单个Cache Line内的访问）
// port: LSQUnit指针
// inst: 指令实例
// isLoad: Load/Store标志
// addr: 虚拟地址
// size: 访问大小
// flags_: 请求标志
// data: 数据指针
// res: 结果指针
// amo_op: 原子操作函子
LSQ::SingleDataRequest::SingleDataRequest(
    LSQUnit* port, const DynInstPtr& inst,
    bool isLoad, const Addr& addr, const uint32_t& size,
    const Request::Flags& flags_, PacketDataPtr data,
    uint64_t* res, AtomicOpFunctorPtr amo_op) :
    LSQRequest(port, inst, isLoad, addr, size, flags_, data, res,
                std::move(amo_op)) {
    port->numSingleRequest++;         // 增加SingleRequest计数
    singleList.push_back(this);       // 加入全局SingleRequest列表
    assert(port->numSingleRequest <= 400);  // 最多400个Single请求
}

// SingleDataRequest析构函数：清理资源并从列表中移除
LSQ::SingleDataRequest::~SingleDataRequest(){
    assert(_port.numSingleRequest > 0);
    _port.numSingleRequest--;         // 减少SingleRequest计数
    singleList.remove(this);          // 从全局列表中移除
}

void
LSQ::SingleDataRequest::finish(const Fault &fault, const RequestPtr &request,
        gem5::ThreadContext* tc, BaseMMU::Mode mode)
{
    _fault.push_back(fault);
    numInTranslationFragments = 0;
    numTranslatedFragments = 1;
    /* If the instruction has been squahsed, let the request know
     * as it may have to self-destruct. */
    _inst->translatedTick = curTick();
    if (_inst->isSquashed()) {
        squashTranslation();
    } else {
        _inst->strictlyOrdered(request->isStrictlyOrdered());

        flags.set(Flag::TranslationFinished);
        if (fault == NoFault) {
            _inst->physEffAddr = request->getPaddr();
            _inst->memReqFlags = request->getFlags();
            if (request->isCondSwap()) {
                assert(_res);
                request->setExtraData(*_res);
            }
            setState(State::Request);
        } else {
            setState(State::Fault);
        }

        LSQRequest::_inst->fault = fault;
        LSQRequest::_inst->translationCompleted(true);
        DPRINTF(LSQ, "Translation of inst %llu notified as completed\n",
                LSQRequest::_inst->seqNum);
    }
}

// SplitDataRequest构造函数：创建分片数据请求（跨Cache Line访问）
// port: LSQUnit指针
// inst: 指令实例
// isLoad: Load/Store标志
// addr: 虚拟地址
// size: 访问大小
// flags_: 请求标志
// data: 数据指针
// res: 结果指针
LSQ::SplitDataRequest::SplitDataRequest(LSQUnit* port, const DynInstPtr& inst, bool isLoad, const Addr& addr,
                                        const uint32_t& size, const Request::Flags& flags_, PacketDataPtr data,
                                        uint64_t* res)
    : LSQRequest(port, inst, isLoad, addr, size, flags_, data, res, nullptr),
      numFragments(0),              // 片段数量
      numReceivedPackets(0),        // 已接收的packet数量
      _mainReq(nullptr),            // 主请求（聚合所有片段）
      _mainPacket(nullptr)          // 主packet
{
    port->numSplitRequest++;        // 增加SplitRequest计数
    assert(port->numSplitRequest <= 400);  // 最多400个Split请求
    flags.set(Flag::IsSplit);       // 设置IsSplit标志
}

// SplitDataRequest析构函数：清理资源
LSQ::SplitDataRequest::~SplitDataRequest()
{
    assert(_port.numSplitRequest > 0);
    _port.numSplitRequest--;        // 减少SplitRequest计数
    if (_mainReq) {
        _mainReq = nullptr;         // 释放主请求
    }
    if (_mainPacket) {
        delete _mainPacket;         // 删除主packet
        _mainPacket = nullptr;
    }
}

// SplitDataRequest完成地址翻译的回调
// fault: 翻译产生的Fault
// req: 完成翻译的请求片段
// tc: 线程上下文
// mode: MMU模式
void
LSQ::SplitDataRequest::finish(const Fault &fault, const RequestPtr &req,
        gem5::ThreadContext* tc, BaseMMU::Mode mode)
{
    // 查找完成翻译的请求片段在_reqs中的索引
    int i;
    for (i = 0; i < _reqs.size() && _reqs[i] != req; i++);
    assert(i < _reqs.size());
    _fault[i] = fault;  // 记录该片段的Fault

    // 更新翻译计数器
    numInTranslationFragments--;  // 正在翻译的片段数减1
    numTranslatedFragments++;     // 已翻译的片段数加1

    // 如果翻译成功，将片段的标志合并到主请求
    if (fault == NoFault)
        _mainReq->setFlags(req->getFlags());

    // 检查是否所有片段都已完成翻译
    if (numTranslatedFragments == _reqs.size()) {
        _inst->translatedTick = curTick();
        if (_inst->isSquashed()) {
            // 指令已被squash，取消翻译
            squashTranslation();
        } else {
            _inst->strictlyOrdered(_mainReq->isStrictlyOrdered());
            flags.set(Flag::TranslationFinished);  // 标记翻译完成
            _inst->translationCompleted(true);

            // 检查是否有任何片段翻译失败
            for (i = 0; i < _fault.size() && _fault[i] == NoFault; i++);
            if (i > 0) {
                // 至少有一个片段翻译成功
                _inst->physEffAddr = LSQRequest::req()->getPaddr();
                _inst->memReqFlags = _mainReq->getFlags();
                if (_mainReq->isCondSwap()) {
                    // 条件交换指令，设置额外数据
                    assert (i == _fault.size());
                    assert(_res);
                    _mainReq->setExtraData(*_res);
                }
                if (i == _fault.size()) {
                    // 所有片段翻译成功
                    _inst->fault = NoFault;
                    setState(State::Request);
                } else {
                  // 部分片段翻译失败
                  _inst->fault = _fault[i];
                  setState(State::PartialFault);
                }
            } else {
                // 第一个片段就翻译失败
                _inst->fault = _fault[0];
                setState(State::Fault);
            }
        }

    }
}

// SingleDataRequest发起地址翻译
void
LSQ::SingleDataRequest::initiateTranslation()
{
    assert(_reqs.size() == 0);

    // 添加请求（单个片段）
    addReq(_addr, _size, _byteEnable);

    // 设置XS元数据：记录指令地址
    _inst->xsMeta->instAddr = _inst->pcState().instAddr();

    if (_reqs.size() > 0) {
        // 设置请求元数据
        _reqs.back()->setReqInstSeqNum(_inst->seqNum);      // 指令序列号
        _reqs.back()->setXsMetadata(Request::XsMetadata(_inst->xsMeta));  // XS元数据
        _reqs.back()->taskId(_taskId);                      // 任务ID
        _inst->translationStarted(true);                    // 标记翻译已开始
        setState(State::Translation);                        // 设置状态为Translation
        flags.set(Flag::TranslationStarted);                // 设置翻译开始标志

        // 保存request到指令中
        _inst->savedRequest = this;
        // 发送片段0到MMU进行翻译
        sendFragmentToTranslation(0);
    } else {
        // 没有生成请求（如字节使能全为false），设置内存访问谓词为false
        _inst->setMemAccPredicate(false);
    }
}

// 返回SplitDataRequest的主packet
PacketPtr
LSQ::SplitDataRequest::mainPacket()
{
    return _mainPacket;
}

// 返回SplitDataRequest的主请求
RequestPtr
LSQ::SplitDataRequest::mainReq()
{
    return _mainReq;
}

// SplitDataRequest发起地址翻译：将跨Cache Line的访问分片
void
LSQ::SplitDataRequest::initiateTranslation()
{
    auto cacheLineSize = _port.cacheLineSize();
    Addr base_addr = _addr;
    // 计算下一个Cache Line的起始地址
    Addr next_addr = addrBlockAlign(_addr + cacheLineSize, cacheLineSize);
    // 计算最终地址所在Cache Line的起始地址
    Addr final_addr = addrBlockAlign(_addr + _size, cacheLineSize);
    uint32_t size_so_far = 0;

    // 创建主请求，包含完整的访问信息
    _mainReq = std::make_shared<Request>(base_addr,
                _size, _flags, _inst->requestorId(),
                _inst->pcState().instAddr(), _inst->contextId());
    _mainReq->setByteEnable(_byteEnable);

    // 设置XS元数据
    _inst->xsMeta->instAddr = _inst->pcState().instAddr();

    // Paddr在_mainReq中不使用，但我们会在finish()中通过调用setFlags()
    // 将子请求的标志累积到_mainReq中。
    // setFlags()假设paddr已设置，所以在这里设置paddr有效位
    // 以避免从finish()调用时可能的assert
    _mainReq->setPaddr(0);

    /* 处理前缀部分，可能未对齐 */
    auto it_start = _byteEnable.begin();
    auto it_end = _byteEnable.begin() + (next_addr - base_addr);
    addReq(base_addr, next_addr - base_addr,
                     std::vector<bool>(it_start, it_end));
    size_so_far = next_addr - base_addr;

    /* 现在已对齐到块边界，读取完整的块 */
    base_addr = next_addr;
    while (base_addr != final_addr) {
        auto it_start = _byteEnable.begin() + size_so_far;
        auto it_end = _byteEnable.begin() + size_so_far + cacheLineSize;
        addReq(base_addr, cacheLineSize,
                         std::vector<bool>(it_start, it_end));
        size_so_far += cacheLineSize;
        base_addr += cacheLineSize;
    }

    /* 处理尾部 */
    if (size_so_far < _size) {
        auto it_start = _byteEnable.begin() + size_so_far;
        auto it_end = _byteEnable.end();
        addReq(base_addr, _size - size_so_far,
                         std::vector<bool>(it_start, it_end));
    }

    if (_reqs.size() > 0) {
        /* 设置请求并发送到翻译单元 */
        for (auto& r: _reqs) {
            r->setReqInstSeqNum(_inst->seqNum);
            r->setXsMetadata(Request::XsMetadata(_inst->xsMeta));
            r->taskId(_taskId);
        }

        _inst->translationStarted(true);
        setState(State::Translation);
        flags.set(Flag::TranslationStarted);
        _inst->savedRequest = this;
        numInTranslationFragments = 0;
        numTranslatedFragments = 0;
        _fault.resize(_reqs.size());

        // 发送所有片段到MMU进行翻译
        for (uint32_t i = 0; i < _reqs.size(); i++) {
            sendFragmentToTranslation(i);
        }
    } else {
        // 没有生成请求，设置内存访问谓词为false
        _inst->setMemAccPredicate(false);
    }
}

// SbufferRequest构造函数：创建Store Buffer请求
// cpu: CPU指针
// port: LSQUnit指针
// blockpaddr: 块物理地址
// data: 数据指针
LSQ::SbufferRequest::SbufferRequest(CPU* cpu, LSQUnit* port, Addr blockpaddr, uint8_t* data)
    : LSQRequest(port, nullptr, false, 0, port->cacheLineSize(), 0, data,
                 nullptr, nullptr, false),
      cpu(cpu) {
    port->numSBufferRequest++;  // 增加SbufferRequest计数
    assert(port->numSBufferRequest <= port->sbufferEntries);  // 不超过Store Buffer项数
}

// SbufferRequest析构函数
LSQ::SbufferRequest::~SbufferRequest() {
    assert(_port.numSBufferRequest > 0);
    _port.numSBufferRequest--;  // 减少SbufferRequest计数
}

// 添加Store Buffer请求
// blockVaddr: 块虚拟地址
// blockPaddr: 块物理地址
// byteEnable: 字节使能
void
LSQ::SbufferRequest::addReq(Addr blockVaddr, Addr blockPaddr, const std::vector<bool> byteEnable)
{
    // 创建请求，使用物理地址
    auto req = std::make_shared<Request>(
        blockPaddr, _port.cacheLineSize(), Request::Flags(),
        cpu->dataRequestorId());
    req->setContext(cpu->getContext(_port.lsqID)->contextId());
    req->setByteEnable(byteEnable);

    _reqs.push_back(req);
}

// LSQRequest构造函数（简化版本）：用于基本初始化
// port: LSQUnit指针
// inst: 指令实例
// isLoad: Load/Store标志
LSQ::LSQRequest::LSQRequest(
        LSQUnit *port, const DynInstPtr& inst, bool isLoad) :
    _state(State::NotIssued),                    // 初始状态：未发射
    _port(*port), _inst(inst), _data(nullptr),
    _res(nullptr), _addr(0), _size(0), _flags(0),
    _numOutstandingPackets(0), _amo_op(nullptr),
    _sbufferBypass(false)                        // Store Buffer旁路标志
{

    flags.set(Flag::IsLoad, isLoad);
    if (_inst) {
        // 需要写回到寄存器的指令：SC、AMO、Load
        flags.set(Flag::WriteBackToRegister,
                _inst->isStoreConditional() || _inst->isAtomic() ||
                _inst->isLoad());
        flags.set(Flag::IsAtomic, _inst->isAtomic());
        install();  // 安装到LQ或SQ
    }
}

// LSQRequest构造函数（完整版本）：用于创建具体的访存请求
// port: LSQUnit指针
// inst: 指令实例
// isLoad: Load/Store标志
// addr: 虚拟地址
// size: 访问大小
// flags_: 请求标志
// data: 数据指针
// res: 结果指针
// amo_op: 原子操作函子
// stale_translation: 过时翻译标志
LSQ::LSQRequest::LSQRequest(
        LSQUnit *port, const DynInstPtr& inst, bool isLoad,
        const Addr& addr, const uint32_t& size, const Request::Flags& flags_,
        PacketDataPtr data, uint64_t* res, AtomicOpFunctorPtr amo_op,
        bool stale_translation)
    : _state(State::NotIssued),
    numTranslatedFragments(0),                   // 已翻译的片段数
    numInTranslationFragments(0),                // 正在翻译的片段数
    _port(*port), _inst(inst), _data(data),
    _res(res), _addr(addr), _size(size),
    _flags(flags_),
    _numOutstandingPackets(0),                   // 未完成的packet数
    _amo_op(std::move(amo_op)),                  // 原子操作函子
    _hasStaleTranslation(stale_translation),     // 是否有过时翻译
    _sbufferBypass(false)                        // Store Buffer旁路标志
{

    flags.set(Flag::IsLoad, isLoad);
    if (_inst) {
        // 需要写回到寄存器的指令：SC、AMO、Load
        flags.set(Flag::WriteBackToRegister,
                _inst->isStoreConditional() || _inst->isAtomic() ||
                _inst->isLoad());
        flags.set(Flag::IsAtomic, _inst->isAtomic());
        flags.set(Flag::IsHInst, _inst->isHInst());  // Hypervisor指令标志
        install();  // 安装到LQ或SQ
    }

}

// 安装LSQRequest到Load Queue或Store Queue
void
LSQ::LSQRequest::install()
{
    if (isLoad()) {
        // Load指令安装到Load Queue
        _port.loadQueue[_inst->lqIdx].setRequest(this);
    } else {
        // Store、StoreConditional和Atomic指令安装到Store Queue
        _port.storeQueue[_inst->sqIdx].setRequest(this);
    }
}

// 检查指令是否已被squash
bool LSQ::LSQRequest::squashed() const { return _inst->isSquashed(); }

// 添加请求片段：创建Request对象
// addr: 地址
// size: 大小
// byte_enable: 字节使能
void
LSQ::LSQRequest::addReq(Addr addr, unsigned size,
           const std::vector<bool>& byte_enable)
{
    // 只有当字节使能中有激活的元素时才创建请求
    if (isAnyActiveElement(byte_enable.begin(), byte_enable.end())) {
        auto req = std::make_shared<Request>(
                addr, size, _flags, _inst->requestorId(),
                _inst->pcState().instAddr(), _inst->contextId(),
                std::move(_amo_op));
        req->setByteEnable(byte_enable);

        /* 如果请求标记为NO_ACCESS，设置本地访问器 */
        if (_flags.isSet(Request::NO_ACCESS)) {
            req->setLocalAccessor(
                [this, req](gem5::ThreadContext *tc, PacketPtr pkt) -> Cycles
                {
                    // HTM（Hardware Transactional Memory）事务处理
                    if ((req->isHTMStart() || req->isHTMCommit())) {
                        auto& inst = this->instruction();
                        assert(inst->inHtmTransactionalState());
                        pkt->setHtmTransactional(
                            inst->getHtmTransactionUid());
                    }
                    return Cycles(1);
                }
            );
        }

        _reqs.push_back(req);
    }
}

// 前递数据：从Store Buffer或Store Queue前递数据到Load
void
LSQ::LSQRequest::forward()
{
    // 只有Load且需要写回寄存器的指令才前递
    if (!isLoad() || !needWBToRegister()) return;
    DPRINTF(StoreBuffer, "sbuffer/storeQue forward data\n");

    // 从Store Buffer前递数据
    for (auto& p : SBforwardPackets)
    {
        _sbufferBypass = true;
        _inst->memData[p.idx] = p.byte;
    }

    // 从Store Queue前递数据
    for (auto& p : SQforwardPackets) {
        _sbufferBypass = true;
        _inst->memData[p.idx] = p.byte;
    }
}

LSQ::LSQRequest::~LSQRequest()
{
    if (isAnyOutstandingRequest()) {
        warn("numInTranslationFragments = %u, _numOutstandingPackets = %u\n",
             numInTranslationFragments, _numOutstandingPackets);
        std::raise(SIGINT);
    }
    assert(!isAnyOutstandingRequest());
    if (_inst && _inst->savedRequest == this) {
        DPRINTF(LSQ, "inst [sn:%llu] Deleting LSQRequest, savedRequest\n", _inst->seqNum);
         _inst->savedRequest = nullptr;
    }

    for (auto r: _packets)
        delete r;
};

ContextID
LSQ::LSQRequest::contextId() const
{
    return _inst->contextId();
}

void
LSQ::LSQRequest::sendFragmentToTranslation(int i)
{
    numInTranslationFragments++;
    if (_inst->isHInst()){
        req(i)->setHInst(_inst->isHInst());
    }
    _port.getMMUPtr()->translateTiming(req(i), _inst->thread->getTC(),
            this, isLoad() ? BaseMMU::Read : BaseMMU::Write);
}

void
LSQ::SingleDataRequest::markAsStaleTranslation()
{
    // If this element has been translated and is currently being requested,
    // then it may be stale
    if ((!flags.isSet(Flag::Complete)) &&
        (!flags.isSet(Flag::Discarded)) &&
        (flags.isSet(Flag::TranslationStarted))) {
        _hasStaleTranslation = true;
    }

    DPRINTF(LSQ, "SingleDataRequest %d 0x%08x isBlocking:%d\n",
        (int)_state, (uint32_t)flags, _hasStaleTranslation);
}

void
LSQ::SplitDataRequest::markAsStaleTranslation()
{
    // If this element has been translated and is currently being requested,
    // then it may be stale
    if ((!flags.isSet(Flag::Complete)) &&
        (!flags.isSet(Flag::Discarded)) &&
        (flags.isSet(Flag::TranslationStarted))) {
        _hasStaleTranslation = true;
    }

    DPRINTF(LSQ, "SplitDataRequest %d 0x%08x isBlocking:%d\n",
        (int)_state, (uint32_t)flags, _hasStaleTranslation);
}

// SbufferRequest接收时序响应：Store Buffer请求完成
// pkt: 响应packet
// 返回值: true表示成功处理
bool
LSQ::SbufferRequest::recvTimingResp(PacketPtr pkt)
{
    // 打印Store Buffer驱逐完成信息
    DPRINTF(StoreBuffer,
            "Sbuffer Req::recvTimingResp: entry[%#x]\n",
            _packets[0]->getAddr());
    assert(_numOutstandingPackets == 1);
    flags.set(Flag::Complete);           // 标记请求完成
    assert(pkt == _packets.front());
    _port.completeSbufferEvict(pkt);     // 完成Store Buffer驱逐
    discard();                            // 丢弃请求
    return true;
}

// SingleDataRequest接收时序响应：处理单数据请求的响应
// pkt: 响应packet
// 返回值: true表示成功处理
bool
LSQ::SingleDataRequest::recvTimingResp(PacketPtr pkt)
{
    LSQ* lsq = this->_port.getLsq();
    bool isNormalLd = this->isNormalLd();
    bool enableLdMissReplay = lsq->enableLdMissReplay();
    // 在1个周期内收到的所有响应都是Cache命中
    bool cacheHit = LSQRequest::_inst->getCpuPtr()->ticksToCycles(curTick() - pkt->sendTick) <= 1;
    // 打印指令号、请求地址和packet地址
    DPRINTF(LSQ, "Single Req::recvTimingResp: inst: %llu, pkt: %#lx, isLoad: %d, "
                "isLLSC: %d, isUncache: %d, isCachehit: %d, data: %d\n",
                pkt->req->getReqInstSeqNum(), pkt->getAddr(), isLoad(), mainReq()->isLLSC(),
                mainReq()->isUncacheable(), cacheHit, *(pkt->getPtr<uint64_t*>()));

    // 从在途Load列表中移除
    if (isLoad()) {
        auto it = std::find(lsqUnit()->inflightLoads.begin(), lsqUnit()->inflightLoads.end(), this);
        if (it != lsqUnit()->inflightLoads.end()) {
            lsqUnit()->inflightLoads.erase(it);
        }
    }

    assert(_numOutstandingPackets == 1);
    // 如果启用了Load Miss重放机制且是普通Load
    if (enableLdMissReplay && isNormalLd) {
        DPRINTF(Hint, "[sn:%ld] Recv TimingResp\n", pkt->req->getReqInstSeqNum());
        if (cacheHit) {
            DPRINTF(LSQ, "[sn:%ld] %s hit\n", _inst->seqNum, "cache");
            // Cache命中，后续处理将在s2阶段进行
            instruction()->setCacheHit();
        } else if (LSQRequest::_inst->waitingCacheRefill()) {
            // Miss的数据已准备好在LSQ侧数据总线上，唤醒重放队列中的missed load
            // 这里处理missed的早期唤醒
            DPRINTF(LSQ, "[sn:%ld] waitingCacheRefill\n", pkt->req->getReqInstSeqNum());
            LSQRequest::_inst->waitingCacheRefill(false);
            discard();
        } else {
            DPRINTF(LSQ, "[sn:%ld] addToBus\n", _inst->seqNum);
            // Cache miss refill，使数据在数据总线上保持稳定
            lsq->bus[_inst->seqNum] = pkt->getAddr();
            _port.getStats()->busAppendTimes++;
            discard();
        }
    } else {
        // 当enableLdMissReplay为false时，指令的具体执行阶段未知，所以在这里完成
        flags.set(Flag::Complete);
        assert(pkt == _packets.front());
        assert(pkt == mainPacket());
        assemblePackets();
        _hasStaleTranslation = false;
    }
    // 清除待处理的缓存请求
    LSQRequest::_inst->hasPendingCacheReq(false);
    LSQRequest::_inst->pendingCacheReq = nullptr;
    return true;
}

// SplitDataRequest接收时序响应：处理分片数据请求的响应
// pkt: 响应packet
// 返回值: true表示成功处理
bool
LSQ::SplitDataRequest::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(LSQ, "Spilt Req::recvTimingResp: inst: %llu, pkt: %#lx\n", pkt->req->getReqInstSeqNum(),
            pkt->getAddr());
    // 查找响应packet在packet列表中的索引
    uint32_t pktIdx = 0;
    while (pktIdx < _packets.size() && pkt != _packets[pktIdx])
        pktIdx++;
    assert(pktIdx < _packets.size());
    numReceivedPackets++;  // 增加已接收packet计数

    // 检查是否所有片段都已接收完成
    if (numReceivedPackets == _packets.size()) {
        flags.set(Flag::Complete);        // 标记请求完成
        assemblePackets();                 // 组装所有片段
        _hasStaleTranslation = false;
        LSQRequest::_inst->hasPendingCacheReq(false);
        LSQRequest::_inst->pendingCacheReq = nullptr;
    }
    return true;
}

// SbufferRequest接收功能自定义信号（空实现）
void
LSQ::SbufferRequest::recvFunctionalCustomSignal(PacketPtr pkt) {}

// SingleDataRequest接收功能自定义信号：处理Cache Miss Hint
// pkt: 信号packet
void
LSQ::SingleDataRequest::recvFunctionalCustomSignal(PacketPtr pkt)
{
    LSQ* lsq = this->_port.getLsq();
    bool isNormalLd = this->isNormalLd();
    bool enableLdMissReplay = lsq->enableLdMissReplay();
    if (enableLdMissReplay && isNormalLd && LSQRequest::_inst->waitingCacheRefill()) {
        // 接收自定义Hint，在recvTimingResp之前提前唤醒Cache missed的load
        DPRINTF(LSQ, "SingleDataRequest::CustomResp: inst: %llu, pkt: %#lx\n", pkt->req->getReqInstSeqNum(),
            pkt->getAddr());
        DPRINTF(Hint, "[sn:%ld] Recv Hint\n", pkt->req->getReqInstSeqNum());
        LSQRequest::_inst->waitingCacheRefill(false);
    }
}

// SplitDataRequest接收功能自定义信号（空实现）
void
LSQ::SplitDataRequest::recvFunctionalCustomSignal(PacketPtr pkt) {}


// SingleDataRequest组装packets：前递数据并完成数据访问
void
LSQ::SingleDataRequest::assemblePackets()
{
    forward();                               // 执行数据前递
    _port.completeDataAccess(mainPacket()); // 完成数据访问
}

// SplitDataRequest组装packets：合并所有片段并完成数据访问
void
LSQ::SplitDataRequest::assemblePackets()
{
    // 创建响应packet（Load为Read，Store为Write）
    PacketPtr resp = isLoad()
        ? Packet::createRead(_mainReq)
        : Packet::createWrite(_mainReq);
    // 设置数据指针
    if (isLoad())
        resp->dataStatic(_inst->memData);
    else
        resp->dataStatic(_data);
    resp->senderState = this;
    forward();                           // 执行数据前递
    _port.completeDataAccess(resp);      // 完成数据访问
    delete resp;                         // 删除响应packet
}

// SbufferRequest构建packets：创建Store Buffer写packet
void
LSQ::SbufferRequest::buildPackets()
{
    if (_packets.size() == 0) {
        PacketPtr pkt = Packet::createWrite(_reqs[0]);
        pkt->dataStatic(_data);
        pkt->senderState = this;
        _packets.push_back(pkt);
    }
}

// SingleDataRequest构建packets：创建单个packet
void
LSQ::SingleDataRequest::buildPackets()
{
    /* 重试不创建新packet */
    if (_packets.size() == 0) {
        // 根据Load/Store创建Read/Write packet
        _packets.push_back(
                isLoad()
                    ?  Packet::createRead(req())
                    :  Packet::createWrite(req()));
        _packets.back()->dataStatic(_inst->memData);
        _packets.back()->senderState = this;
        DPRINTF(PacketSender, "Set packet %#lx senderState to %#lx\n", _packets.back(), this);

        // 硬件事务内存（HTM）
        // 如果请求源自事务中（不一定是HtmCmd），则packet应标记为事务性的
        if (_inst->inHtmTransactionalState()) {
            _packets.back()->setHtmTransactional(
                _inst->getHtmTransactionUid());

            DPRINTF(HtmCpu,
              "HTM %s pc=0x%lx - vaddr=0x%lx - paddr=0x%lx - htmUid=%u\n",
              isLoad() ? "LD" : "ST",
              _inst->pcState().instAddr(),
              _packets.back()->req->hasVaddr() ?
                  _packets.back()->req->getVaddr() : 0lu,
              _packets.back()->getAddr(),
              _inst->getHtmTransactionUid());
        }
    }
    assert(_packets.size() == 1);
}

// SplitDataRequest构建packets：为每个片段创建packet
void
LSQ::SplitDataRequest::buildPackets()
{
    /* 额外数据？？ */
    Addr base_address = _addr;

    if (_packets.size() == 0) {
        /* 创建新packets */
        // Load操作创建主packet
        if (isLoad()) {
            _mainPacket = Packet::createRead(_mainReq);
            _mainPacket->dataStatic(_inst->memData);

            // 硬件事务内存（HTM）
            // 如果请求源自事务中，packet应标记为事务性的
            if (_inst->inHtmTransactionalState()) {
                _mainPacket->setHtmTransactional(
                    _inst->getHtmTransactionUid());
                DPRINTF(HtmCpu,
                  "HTM LD.0 pc=0x%lx-vaddr=0x%lx-paddr=0x%lx-htmUid=%u\n",
                  _inst->pcState().instAddr(),
                  _mainPacket->req->hasVaddr() ?
                      _mainPacket->req->getVaddr() : 0lu,
                  _mainPacket->getAddr(),
                  _inst->getHtmTransactionUid());
            }
        }
        // 为每个无故障的请求片段创建packet
        for (int i = 0; i < _reqs.size() && _fault[i] == NoFault; i++) {
            RequestPtr req = _reqs[i];
            PacketPtr pkt = isLoad() ? Packet::createRead(req)
                                     : Packet::createWrite(req);
            ptrdiff_t offset = req->getVaddr() - base_address;
            if (isLoad()) {
                // Load：静态数据指针（指向memData的偏移）
                pkt->dataStatic(_inst->memData + offset);
            } else {
                // Store：动态分配数据（需要拷贝）
                uint8_t* req_data = new uint8_t[req->getSize()];
                std::memcpy(req_data,
                        _inst->memData + offset,
                        req->getSize());
                pkt->dataDynamic(req_data);
            }
            pkt->senderState = this;
            _packets.push_back(pkt);

            // 硬件事务内存（HTM）
            // 如果请求源自事务中，packet应标记为事务性的
            if (_inst->inHtmTransactionalState()) {
                _packets.back()->setHtmTransactional(
                    _inst->getHtmTransactionUid());
                DPRINTF(HtmCpu,
                  "HTM %s.%d pc=0x%lx-vaddr=0x%lx-paddr=0x%lx-htmUid=%u\n",
                  isLoad() ? "LD" : "ST",
                  i+1,
                  _inst->pcState().instAddr(),
                  _packets.back()->req->hasVaddr() ?
                      _packets.back()->req->getVaddr() : 0lu,
                  _packets.back()->getAddr(),
                  _inst->getHtmTransactionUid());
            }
        }
    }
    assert(_packets.size() > 0);
}

// SbufferRequest发送packet到缓存
// 返回值: true表示成功发送
bool
LSQ::SbufferRequest::sendPacketToCache()
{
    assert(_numOutstandingPackets == 0);
    bool success = _port.sbufferSendPacket(_packets.at(0));
    DPRINTF(StoreBuffer, "Sbuffer Req::sendPacketToCache: entry[%#x]\n", _packets[0]->getAddr());
    if (success) {
        _numOutstandingPackets = 1;  // 标记有1个未完成的packet
    }

    return success;
}

// SingleDataRequest发送packet到缓存
// 返回值: true表示成功发送
bool
LSQ::SingleDataRequest::sendPacketToCache()
{
    assert(_numOutstandingPackets == 0);
    bool bank_conflict = false;      // Bank冲突标志
    bool tag_read_fail = false;      // Tag读取失败标志
    bool success = lsqUnit()->trySendPacket(isLoad(), _packets.at(0), bank_conflict, tag_read_fail);
    if (success) {
        if (isLoad()) {
            // Load请求添加到在途Load列表
            assert(lsqUnit()->inflightLoads.size() < lsqUnit()->numLoads() + 4);
            lsqUnit()->inflightLoads.emplace_back(this);
        }

        if (!bank_conflict) {
            // 没有Bank冲突，标记有未完成的packet
            _numOutstandingPackets = 1;
            LSQRequest::_inst->hasPendingCacheReq(true);
            LSQRequest::_inst->pendingCacheReq = this;
            DPRINTF(LSQ, "sendPacketToCache success [sn:%llu], pkt: %#lx\n",
                    _inst->seqNum, _packets[0]->getAddr());
        }
    }
    // 处理Bank冲突：设置重放标志
    if (bank_conflict) {
        instruction()->setBankConflicyReplay();
        DPRINTF(LoadPipeline, "Load [sn:%ld] setBankConflicyReplay\n",
                _inst->seqNum);
    }
    // 处理Tag读取失败：调度重放
    if (tag_read_fail) {
        DPRINTF(TagReadFail, "sendPacketToCache fails addr: %lx\n", _packets.at(0)->getAddr());
        lsqUnit()->tagReadFailReplaySchedule();
    }
    return success;
}

// SplitDataRequest发送packet到缓存：尝试发送所有片段
// 返回值: true表示所有片段都已发送
bool
LSQ::SplitDataRequest::sendPacketToCache()
{
    /* 尝试发送packets */
    bool bank_conflict = false;
    bool tag_read_fail = false;
    // 逐个发送未发送的packet
    while (numReceivedPackets + _numOutstandingPackets < _packets.size()) {
        bool success = lsqUnit()->trySendPacket(isLoad(), _packets.at(numReceivedPackets + _numOutstandingPackets),
                                                bank_conflict, tag_read_fail);
        if (success) {
            _numOutstandingPackets++;
        } else {
            break;  // 发送失败，停止
        }
    }
    // 处理Bank冲突
    if (bank_conflict) {
        lsqUnit()->bankConflictReplaySchedule();
    }
    // 处理Tag读取失败
    if (tag_read_fail) {
        lsqUnit()->tagReadFailReplaySchedule();
    }

    // 检查是否所有片段都已发送
    if (_numOutstandingPackets == _packets.size()) {
        LSQRequest::_inst->hasPendingCacheReq(true);
        LSQRequest::_inst->pendingCacheReq = this;
        return true;
    }
    return false;
}

// SingleDataRequest处理本地访问（Local Access）
// 用于处理不需要通过缓存层次结构的特殊内存访问
// 参数: thread - 线程上下文, pkt - 访问packet
// 返回值: 访问延迟周期数
Cycles
LSQ::SingleDataRequest::handleLocalAccess(
        gem5::ThreadContext *thread, PacketPtr pkt)
{
    return pkt->req->localAccessor(thread, pkt);
}

// SplitDataRequest处理本地访问：为每个分片处理本地访问
// 参数: thread - 线程上下文, mainPkt - 主packet
// 返回值: 所有分片中最大的访问延迟
Cycles
LSQ::SplitDataRequest::handleLocalAccess(
        gem5::ThreadContext *thread, PacketPtr mainPkt)
{
    Cycles delay(0);
    unsigned offset = 0;

    // 遍历所有请求分片
    for (auto r: _reqs) {
        // 为每个分片创建packet
        PacketPtr pkt =
            new Packet(r, isLoad() ? MemCmd::ReadReq : MemCmd::WriteReq);
        pkt->dataStatic(mainPkt->getPtr<uint8_t>() + offset);
        // 执行本地访问
        Cycles d = r->localAccessor(thread, pkt);
        // 记录最大延迟
        if (d > delay)
            delay = d;
        offset += r->getSize();
        delete pkt;
    }
    return delay;
}

// SingleDataRequest检查是否命中指定的Cache块
// 用于Snoop一致性协议，检查LSQ中的请求是否访问了指定的Cache Line
// 参数: blockAddr - 块地址, blockMask - 块掩码
// 返回值: true表示命中该Cache块
bool
LSQ::SingleDataRequest::isCacheBlockHit(Addr blockAddr, Addr blockMask)
{
    return ( (LSQRequest::_reqs[0]->getPaddr() & blockMask) == blockAddr);
}

/**
 * Cache可能会探测Load-Store Queue以强制执行内存顺序保证。
 * 此方法通过提供一种将Snoop消息与LSQ跟踪的请求进行比较的机制来支持探测。
 *
 * 一致性模型必须强制执行顺序约束。例如，TSO必须防止内存重排序，
 * 除了Store可以在Load之后重排序。重排序限制会通过减少内存级并行性来
 * 负面影响性能。然而，核心可以通过生成推测性Load来恢复性能。
 * 如果采取预防措施来处理无效的内存顺序，推测性Load可以在不影响正确性的
 * 情况下发射。Load队列必须在内存模型违规时进行squash。
 * 当块所有权被授予另一个核心或Load队列无法准确监视该块时，可能会发生内存模型违规。
 */
// SplitDataRequest检查是否命中指定的Cache块
// 用于多片段请求的Cache一致性检查
bool
LSQ::SplitDataRequest::isCacheBlockHit(Addr blockAddr, Addr blockMask)
{
    bool is_hit = false;
    for (auto &r: _reqs) {
       /**
        * Load-Store Queue处理部分故障，这使此方法变得复杂。
        * 必须比较请求和Snoop之间的物理地址。某些请求可能没有有效的物理地址，
        * 因为部分故障可能有未完成的地址翻译。因此，在比较块命中之前，
        * 必须检查有效请求地址的存在。如果有效请求地址不存在，
        * 我们假设不需要流水线squash。
        */
        if (r->hasPaddr() && (r->getPaddr() & blockMask) == blockAddr) {
            is_hit = true;
            break;
        }
    }
    return is_hit;
}

// DcachePort接收时序响应：转发到LSQ处理
bool
LSQ::DcachePort::recvTimingResp(PacketPtr pkt)
{
    return lsq->recvTimingResp(pkt);
}

// DcachePort接收Snoop请求：处理Cache一致性协议的探测请求
// 检查地址监视器并转发到LSQ
void
LSQ::DcachePort::recvTimingSnoopReq(PacketPtr pkt)
{
    // 遍历所有线程，检查是否有地址监视器监视此地址
    for (ThreadID tid = 0; tid < cpu->numThreads; tid++) {
        if (cpu->getCpuAddrMonitor(tid)->doMonitor(pkt)) {
            cpu->wakeup(tid);  // 唤醒等待该地址的线程
        }
    }
    lsq->recvTimingSnoopReq(pkt);
}

// DcachePort接收功能性自定义信号：转发到LSQ处理
void
LSQ::DcachePort::recvFunctionalCustomSignal(PacketPtr pkt, int sig)
{
    lsq->recvFunctionalCustomSignal(pkt, sig);
}

// DcachePort返回CPU指针：用于外部访问CPU对象
void*
LSQ::DcachePort::recvGetCPUPtr()
{
    return (void *) (lsq->cpu);
}

// DcachePort接收请求重试信号：转发到LSQ处理
void
LSQ::DcachePort::recvReqRetry()
{
    lsq->recvReqRetry();
}

// UnsquashableDirectRequest构造函数：创建不可squash的直接请求
// 用于HTM（Hardware Transactional Memory）和TLBI（TLB Invalidate）等特殊操作
// 这些操作不能被squash，且不需要真实的地址翻译
LSQ::UnsquashableDirectRequest::UnsquashableDirectRequest(
    LSQUnit* port,
    const DynInstPtr& inst,
    const Request::Flags& flags_) :
    SingleDataRequest(port, inst, true, 0x0lu, 8, flags_,
        nullptr, nullptr, nullptr)
{
}

// UnsquashableDirectRequest发起地址翻译（实际不执行翻译）
// 特殊命令（HTM/TLBI）被实现为Load，以避免对CPU和内存接口进行重大更改
// 虚拟地址和物理地址使用虚拟值0x00
// 实际上不会发生地址翻译
void
LSQ::UnsquashableDirectRequest::initiateTranslation()
{
    // 特殊命令被实现为Load，以避免对CPU和内存接口进行重大更改
    // 虚拟地址和物理地址使用虚拟值0x00
    // 实际上不会发生地址翻译

    assert(_reqs.size() == 0);

    addReq(_addr, _size, _byteEnable);

    _inst->xsMeta->instAddr = _inst->pcState().instAddr();

    if (_reqs.size() > 0) {
        // 设置请求元数据
        _reqs.back()->setReqInstSeqNum(_inst->seqNum);
        _reqs.back()->setXsMetadata(Request::XsMetadata(_inst->xsMeta));
        _reqs.back()->taskId(_taskId);
        _reqs.back()->setPaddr(_addr);  // 设置虚拟物理地址
        _reqs.back()->setInstCount(_inst->getCpuPtr()->totalInsts());

        // 设置指令状态
        _inst->strictlyOrdered(_reqs.back()->isStrictlyOrdered());
        _inst->fault = NoFault;
        _inst->physEffAddr = _reqs.back()->getPaddr();
        _inst->memReqFlags = _reqs.back()->getFlags();
        _inst->savedRequest = this;

        // 标记翻译已开始和完成（虽然实际上没有翻译）
        flags.set(Flag::TranslationStarted);
        flags.set(Flag::TranslationFinished);

        _inst->translationStarted(true);
        _inst->translationCompleted(true);

        setState(State::Request);
    } else {
        panic("unexpected behaviour in initiateTranslation()");
    }
}

// UnsquashableDirectRequest标记过期翻译
// HTM/TLBI操作不进行地址翻译，因此不会有过期翻译
void
LSQ::UnsquashableDirectRequest::markAsStaleTranslation()
{
    // HTM/TLBI操作不进行地址翻译，因此不会有过期翻译
    _hasStaleTranslation = false;
}

// UnsquashableDirectRequest完成翻译回调
// 不应该被调用，因为这类请求不进行真实翻译
void
LSQ::UnsquashableDirectRequest::finish(const Fault &fault,
        const RequestPtr &req, gem5::ThreadContext* tc,
        BaseMMU::Mode mode)
{
    panic("unexpected behaviour - finish()");
}

// 检查过期地址翻译：处理TLBI同步完成
// TLBI（TLB Invalidate）操作需要等待所有线程完成同步
void
LSQ::checkStaleTranslations()
{
    assert(waitingForStaleTranslation);

    DPRINTF(LSQ, "Checking pending TLBI sync\n");
    // 检查所有线程队列是否完成
    for (const auto& unit : thread) {
        if (unit.checkStaleTranslations())
            return;  // 仍有线程未完成，继续等待
    }
    DPRINTF(LSQ, "No threads have blocking TLBI sync\n");

    // 所有线程队列都已提交其同步操作
    // => 向sequencer发送RubyRequest
    auto req = Request::createMemManagement(
        Request::TLBI_EXT_SYNC_COMP,
        cpu->dataRequestorId());
    req->setExtraData(staleTranslationWaitTxnId);
    PacketPtr pkt = Packet::createRead(req);

    // TODO - 为这些响应保留一些信用？
    if (!dcachePort.sendTimingReq(pkt)) {
        panic("Couldn't send TLBI_EXT_SYNC_COMP message");
    }

    // 清除等待标志
    waitingForStaleTranslation = false;
    staleTranslationWaitTxnId = 0;
}

// LSQ执行Load操作：将Load请求转发到对应线程的LSQUnit
// 参数: request - LSQ请求, load_idx - Load在Load Queue中的索引
// 返回值: 访问故障（如果有）
Fault
LSQ::read(LSQRequest* request, ssize_t load_idx)
{
    assert(request->req()->contextId() == request->contextId());
    ThreadID tid = cpu->contextToThread(request->req()->contextId());

    return thread.at(tid).read(request, load_idx);
}

// LSQ执行Store操作：将Store请求转发到对应线程的LSQUnit
// 参数: request - LSQ请求, data - 待写入数据, store_idx - Store在Store Queue中的索引
// 返回值: 访问故障（如果有）
Fault
LSQ::write(LSQRequest* request, uint8_t *data, ssize_t store_idx)
{
    ThreadID tid = cpu->contextToThread(request->req()->contextId());

    return thread.at(tid).write(request, data, store_idx);
}

} // namespace o3
} // namespace gem5

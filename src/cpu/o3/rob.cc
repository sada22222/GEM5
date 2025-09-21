/*
 * Copyright (c) 2012 ARM Limited
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

#include "cpu/o3/rob.hh"

#include <list>

#include "base/logging.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "debug/Fetch.hh"
#include "debug/ROB.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

// ROB分配组策略 - 无压缩：每条指令分配一个组
bool
ROB::allocateGroup_none(const DynInstPtr inst, ThreadID tid)
{
    return true; // 不需要组分配
}

// ROB分配组策略 - 昆明湖V2：Load/Store/控制指令独占一个组
// 用于ROB压缩，将多条指令打包成一个组以节省ROB项
bool
ROB::allocateGroup_kmhv2(const DynInstPtr inst, ThreadID tid)
{
    auto& groups = threadGroups[tid];
    auto& prev = instList[tid].back();

    // Load/Store/控制指令独占一个组
    bool alloc = false;
    if (groups.empty()) [[unlikely]] {
        alloc = true;
    } else if (inst->isMemRef() || inst->isControl() || inst->isNonSpeculative()) {
        alloc = true;  // 当前指令是特殊指令，分配新组
    } else if (prev->isMemRef() || prev->isControl() ||
               prev->isNonSpeculative()) {
        alloc = true;  // 前一条指令是特殊指令，分配新组
    } else if (prev->ftqId != inst->ftqId) {
        alloc = true;  // 不同取指队列ID，分配新组
    } else if (lastInsertCycle != cpu->curCycle()) {
        // 不同周期，分配新组
        alloc = true;
    } else if (groups.back() >= instsPerGroup) {
        alloc = true;  // 当前组已满，分配新组
    }
    return alloc;
}

// ROB分配组策略 - MohBoE：Load/Store在组头，控制指令在组尾
bool
ROB::allocateGroup_MohBoE(const DynInstPtr inst, ThreadID tid)
{
    auto& groups = threadGroups[tid];
    auto& prev = instList[tid].back();

    // Load/Store在组头
    // 控制指令在组尾
    bool alloc = false;
    if (groups.empty()) [[unlikely]] {
        alloc = true;
    } else if (inst->isMemRef() || inst->isNonSpeculative()) {
        alloc = true;  // Load/Store分配新组
    } else if (prev->isControl()) {
        alloc = true;  // 前一条是控制指令，分配新组
    } else if (lastInsertCycle != cpu->curCycle()) {
        // 不同周期，分配新组
        alloc = true;
    } else if (groups.back() >= instsPerGroup) {
        alloc = true;  // 当前组已满，分配新组
    }
    return alloc;
}

// ROB分配组策略 - 昆明湖V3：简化的分组策略
bool
ROB::allocateGroup_kmhv3(const DynInstPtr inst, ThreadID tid)
{
    auto& groups = threadGroups[tid];
    auto& prev = instList[tid].back();

    bool alloc = false;
    if (groups.empty()) [[unlikely]] {
        alloc = true;
    } else if (lastInsertCycle != cpu->curCycle()) {
        // 不同周期，分配新组
        alloc = true;
    } else if (groups.back() >= instsPerGroup) {
        alloc = true;  // 当前组已满，分配新组
    }
    return alloc;
}

// ROB构造函数：初始化ReOrder Buffer（重排序缓冲区）
// ROB用于维护指令的程序顺序，支持乱序执行和顺序提交
ROB::ROB(CPU *_cpu, const BaseO3CPUParams &params)
    : robPolicy(params.smtROBPolicy),
      robWalkPolicy(params.robWalkPolicy),
      cpu(_cpu),
      numEntries(params.numROBEntries),
      instsPerGroup(params.CROB_instPerGroup),
      rollbackWidth(params.squashWidth),
      replayWidth(params.replayWidth),
      constSquashCycle(params.ConstSquashCycle),
      numInstsInROB(0),
      numThreads(params.numThreads),
      stats(_cpu)
{
    //确定ROB策略
    if (robPolicy == SMTQueuePolicy::Dynamic) {
        //设置最大项数为ROB总容量
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = numEntries;
        }

    } else if (robPolicy == SMTQueuePolicy::Partitioned) {
        DPRINTF(Fetch, "ROB sharing policy set to Partitioned\n");

        //@todo:如果part_amt不能整除，需要修正
        int part_amt = numEntries / numThreads;

        //平均分配ROB
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = part_amt;
        }

    } else if (robPolicy == SMTQueuePolicy::Threshold) {
        DPRINTF(Fetch, "ROB sharing policy set to Threshold\n");

        int threshold =  params.smtROBThreshold;;

        //按阈值分配
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = threshold;
        }
    }

    assert((robWalkPolicy == ROBWalkPolicy::Rollback && rollbackWidth > 0)
           || (robWalkPolicy == ROBWalkPolicy::Replay && replayWidth > 0)
           || (robWalkPolicy == ROBWalkPolicy::ConstCycle && constSquashCycle > 0)
        //    || robWalkPolicy == ROBWalkPolicy::NaiveCpt
        //    || robWalkPolicy == ROBWalkPolicy::ConfidentCpt
           );

    for (ThreadID tid = numThreads; tid < MaxThreads; tid++) {
        maxEntries[tid] = 0;
    }

    // 根据ROB压缩策略选择分组函数
    switch(params.RobCompressPolicy) {
        case ROBCompressPolicy::none:
            allocateNewGroup = &ROB::allocateGroup_none;
            instsPerGroup = 1;
            break;
        case ROBCompressPolicy::kmhv2:
            allocateNewGroup = &ROB::allocateGroup_kmhv2;
            break;
        case ROBCompressPolicy::MohBoE:
            allocateNewGroup = &ROB::allocateGroup_MohBoE;
            break;
        case ROBCompressPolicy::kmhv3:
            allocateNewGroup = &ROB::allocateGroup_kmhv3;
            break;
        default:
            panic("Unknown ROB compression policy");
            break;
    }

    resetState();
}

// ROB重置状态：清空所有线程的ROB状态
void
ROB::resetState()
{
    for (ThreadID tid = 0; tid  < MaxThreads; tid++) {
        threadGroups[tid].clear();
        squashIt[tid] = instList[tid].end();
        squashedSeqNum[tid] = 0;
        doneSquashing[tid] = true;
    }
    numInstsInROB = 0;

    // 初始化"全局"ROB头尾指针为无效指针
    head = instList[0].end();
    tail = instList[0].end();
}

// ROB返回名称
std::string
ROB::name() const
{
    return cpu->name() + ".rob";
}

// ROB设置活跃线程：设置活跃线程列表指针
void
ROB::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    DPRINTF(ROB, "Setting active threads list pointer.\n");
    activeThreads = at_ptr;
}

// ROB排空完整性检查：确保所有线程的指令列表为空
void
ROB::drainSanityCheck() const
{
    for (ThreadID tid = 0; tid  < numThreads; tid++)
        assert(instList[tid].empty());
    assert(isEmpty());
}

// ROB接管：从检查点恢复时重置状态
void
ROB::takeOverFrom()
{
    resetState();
}

// ROB重置项数：根据活跃线程数重新分配ROB项
void
ROB::resetEntries()
{
    if (robPolicy != SMTQueuePolicy::Dynamic || numThreads > 1) {
        auto active_threads = activeThreads->size();

        std::list<ThreadID>::iterator threads = activeThreads->begin();
        std::list<ThreadID>::iterator end = activeThreads->end();

        while (threads != end) {
            ThreadID tid = *threads++;

            if (robPolicy == SMTQueuePolicy::Partitioned) {
                maxEntries[tid] = numEntries / active_threads;
            } else if (robPolicy == SMTQueuePolicy::Threshold &&
                       active_threads == 1) {
                maxEntries[tid] = numEntries;
            }
        }
    }
}

// ROB项数量：返回每个线程分配的ROB项数
int
ROB::entryAmount(ThreadID num_threads)
{
    if (robPolicy == SMTQueuePolicy::Partitioned) {
        return numEntries / num_threads;
    } else {
        return 0;
    }
}

// ROB统计指令数：统计所有线程的指令总数
int
ROB::countInsts()
{
    int total = 0;

    for (ThreadID tid = 0; tid < numThreads; tid++)
        total += countInsts(tid);

    return total;
}

// ROB统计指令数：返回指定线程的指令数
size_t
ROB::countInsts(ThreadID tid)
{
    return instList[tid].size();
}

// ROB统计组内指令数：统计指定组数的指令总数
uint32_t
ROB::countInstsOfGroups(int groups)
{
    int sum = 0;
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        auto it = threadGroups[tid].begin();
        for (int i = 0; i < groups && it != threadGroups[tid].end(); i++, it++) {
            sum += *it;
        }
    }
    return sum;
}

// ROB提交组：提交指定线程的ROB头部组
void
ROB::commitGroup(const DynInstPtr inst, ThreadID tid)
{
    assert(!threadGroups[tid].empty());

    if (threadGroups[tid].front() == 1) {
        threadGroups[tid].pop_front();  // 组内只有一条指令，删除整个组
    } else {
        threadGroups[tid].front()--;    // 组内还有其他指令，减少计数
    }
}

// ROB清除组：清除指定线程的ROB尾部组（用于squash）
void
ROB::squashGroup(const DynInstPtr inst, ThreadID tid)
{
    assert(!threadGroups[tid].empty());

    if (threadGroups[tid].back() == 1) {
        threadGroups[tid].pop_back();   // 组内只有一条指令，删除整个组
    } else {
        threadGroups[tid].back()--;     // 组内还有其他指令，减少计数
    }
}

// ROB插入指令：将指令插入到ROB尾部
void
ROB::insertInst(const DynInstPtr &inst)
{
    assert(inst);

    stats.writes++;

    DPRINTF(ROB, "Adding inst PC %s to the ROB.\n", inst->pcState());

    assert(numInstsInROB <= numEntries * instsPerGroup);

    ThreadID tid = inst->threadNumber;

    // 分配组
    bool alloc = (this->*allocateNewGroup)(inst, tid);
    lastInsertCycle = cpu->curCycle();
    if (alloc) {
        // 分配新组
        if (!threadGroups[tid].empty()) [[likely]] {
            stats.instPergroup.sample(threadGroups[tid].back());
        }
        threadGroups[tid].push_back(1);
    } else {
        // 添加到现有组
        assert(threadGroups[tid].back() < instsPerGroup);
        threadGroups[tid].back()++;
    }

    instList[tid].push_back(inst);

    //如果这是ROB中的第一条指令，设置head迭代器
    if (numInstsInROB == 0) {
        head = instList[tid].begin();
        assert((*head) == inst);
    }

    //必须递减迭代器才能实际有效，因为__.end()实际指向最后一条指令之后
    tail = instList[tid].end();
    tail--;

    inst->setInROB();

    ++numInstsInROB;

    assert((*tail) == inst);

    DPRINTF(ROB, "[tid:%i] Now has %d instructions.\n", tid,
            threadGroups[tid].size());
}

// ROB退休头指令：提交并删除ROB头部指令
void
ROB::retireHead(ThreadID tid)
{
    stats.writes++;

    assert(numInstsInROB > 0);

    // 获取ROB头部指令并从列表中删除
    InstIt head_it = instList[tid].begin();

    DynInstPtr head_inst = std::move(*head_it);
    instList[tid].erase(head_it);

    assert(head_inst->readyToCommit());
    assert(!head_inst->isSquashed());

    DPRINTF(ROB, "[tid:%i] Retiring head instruction, "
            "instruction PC %s, [sn:%llu]\n", tid, head_inst->pcState(),
            head_inst->seqNum);

    --numInstsInROB;

    //更新组大小
    commitGroup(head_inst, tid);

    head_inst->clearInROB();
    head_inst->setCommitted();

    //更新"全局"ROB头部
    updateHead();

    // @todo: 如果退休的指令是ROB中的唯一指令，需要特殊处理；
    // 否则tail迭代器将变为无效。
    cpu->removeFrontInst(head_inst);
}

// ROB检查头部组是否就绪：检查头部组的所有指令是否可以提交
bool
ROB::isHeadGroupReady(ThreadID tid)
{
    stats.reads++;

    if (!threadGroups[tid].empty() && threadGroups[tid].front() != 0) {
        auto it = instList[tid].begin();

        for (int i = 0; i < threadGroups[tid].front(); i++, it++) {
            auto& inst = *it;
            // 第一条指令必须readyToCommit
            if (!inst->readyToCommit()) {
                return false;
            }

            // 如果组中有屏障、非推测指令或故障
            // 这个组必须被提交
            if (inst->readyToCommit() && (!inst->isExecuted() || inst->faulted())) {
                return true;
            }
        }
        return true;
    }

    return false;
}

// ROB获取头部组最后完成的序列号：返回头部组中最后一条已执行指令的序列号
InstSeqNum
ROB::getHeadGroupLastDoneSeq(ThreadID tid)
{
    if (!threadGroups[tid].empty() && threadGroups[tid].front() != 0) {
        auto it = instList[tid].begin();
        InstSeqNum seqnum = 0;
        for (int i = 0; i < threadGroups[tid].front(); i++, it++) {
            auto& inst = *it;
            if (!inst->readyToCommit() || !inst->isExecuted() || inst->faulted()) {
                break;
            }
            seqnum = inst->seqNum;
        }
        return seqnum;
    }
    return 0;
}

// ROB空闲项数：返回指定线程的空闲ROB项数
unsigned
ROB::numFreeEntries(ThreadID tid)
{
    return maxEntries[tid] - threadGroups[tid].size();
}

// ROB执行Squash：清除指定线程ROB中的错误推测指令
void
ROB::doSquash(ThreadID tid)
{
    stats.writes++;
    DPRINTF(ROB, "[tid:%i] Squashing instructions until [sn:%llu].\n",
            tid, squashedSeqNum[tid]);

    assert(squashIt[tid] != instList[tid].end());

    if ((*squashIt[tid])->seqNum < squashedSeqNum[tid]) {
        DPRINTF(ROB, "[tid:%i] Done squashing instructions.\n",
                tid);

        squashIt[tid] = instList[tid].end();

        doneSquashing[tid] = true;
        return;
    }

    bool robTailUpdate = false;

    assert(dynSquashWidth);
    unsigned int num_insts_to_squash = dynSquashWidth;

    // 如果CPU正在退出，清除所有被告知的指令
    // 即使超过squashWidth。
    // 将数量设置为项数（最大值）。
    if (cpu->isThreadExiting(tid))
    {
        num_insts_to_squash = numEntries * instsPerGroup;
    }

    for (int numSquashed = 0;
         numSquashed < num_insts_to_squash &&
         squashIt[tid] != instList[tid].end() &&
         (*squashIt[tid])->seqNum > squashedSeqNum[tid];
         ++numSquashed)
    {
        DPRINTF(ROB, "[tid:%i] Squashing instruction PC %s, seq num %i.\n",
                (*squashIt[tid])->threadNumber,
                (*squashIt[tid])->pcState(),
                (*squashIt[tid])->seqNum);

        // Mark the instruction as squashed, and ready to commit so that
        // it can drain out of the pipeline.
        (*squashIt[tid])->setSquashed();

        (*squashIt[tid])->setCanCommit();

        // printf("[ROB] squash seqNum %ld\n", (*squashIt[tid])->seqNum);

        auto prevIt = std::prev(squashIt[tid]);
        --numInstsInROB;

        //Update Group Size
        squashGroup(*squashIt[tid], tid);

        (*squashIt[tid])->clearInROB();
        // head_inst->setCommitted();
        cpu->removeFrontInst(*squashIt[tid]);

        if (instList[tid].empty() || squashIt[tid] == instList[tid].begin()) {
            DPRINTF(ROB, "Reached head of instruction list while "
                    "squashing.\n");

            instList[tid].erase(squashIt[tid]);

            squashIt[tid] = instList[tid].end();

            doneSquashing[tid] = true;

            return;
        }

        InstIt tail_thread = instList[tid].end();
        tail_thread--;

        if ((*squashIt[tid]) == (*tail_thread))
            robTailUpdate = true;

        instList[tid].erase(squashIt[tid]);

        squashIt[tid] = prevIt;
    }


    // Check if ROB is done squashing.
    if ((*squashIt[tid])->seqNum <= squashedSeqNum[tid]) {
        DPRINTF(ROB, "[tid:%i] Done squashing instructions.\n",
                tid);

        squashIt[tid] = instList[tid].end();

        doneSquashing[tid] = true;
    }

    if (robTailUpdate) {
        updateTail();
    }
}


// ROB更新头部：更新全局ROB头部指针为最老的指令
void
ROB::updateHead()
{
    InstSeqNum lowest_num = 0;
    bool first_valid = true;

    // @todo: 通过ROB或CPU设置ActiveThreads
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (instList[tid].empty())
            continue;

        if (first_valid) {
            head = instList[tid].begin();
            lowest_num = (*head)->seqNum;
            first_valid = false;
            continue;
        }

        InstIt head_thread = instList[tid].begin();

        DynInstPtr head_inst = (*head_thread);

        assert(head_inst != 0);

        if (head_inst->seqNum < lowest_num) {
            head = head_thread;
            lowest_num = head_inst->seqNum;
        }
    }

    if (first_valid) {
        head = instList[0].end();
    }

}

// ROB更新尾部：更新全局ROB尾部指针为最新的指令
void
ROB::updateTail()
{
    tail = instList[0].end();
    bool first_valid = true;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (instList[tid].empty()) {
            continue;
        }

        // 如果这是第一个有效的，直接赋值而不比较
        if (first_valid) {
            tail = instList[tid].end();
            tail--;
            first_valid = false;
            continue;
        }

        // 如果此线程的tail比当前"tail high"更新
        // 则分配新tail
        InstIt tail_thread = instList[tid].end();
        tail_thread--;

        if ((*tail_thread)->seqNum > (*tail)->seqNum) {
            tail = tail_thread;
        }
    }
}


// ROB Squash：启动指定线程的ROB清除操作
void
ROB::squash(InstSeqNum squash_num, ThreadID tid)
{
    if (isEmpty(tid)) {
        DPRINTF(ROB, "Does not need to squash due to being empty "
                "[sn:%llu]\n",
                squash_num);

        return;
    }

    DPRINTF(ROB, "Starting to squash within the ROB.\n");

    robStatus[tid] = ROBSquashing;

    doneSquashing[tid] = false;

    squashedSeqNum[tid] = squash_num;

    // TODO: 找到需要清除的指令数和未提交指令数
    unsigned total_inst_to_squash = 0;
    for (auto it = instList[tid].begin(); it != instList[tid].end(); ++it) {
        if ((*it)->seqNum > squash_num) {
            total_inst_to_squash++;
        }
    }
    unsigned num_uncommited_inst = instList[tid].size() - total_inst_to_squash;

    dynSquashWidth = computeDynSquashWidth(num_uncommited_inst, total_inst_to_squash);

    if (!instList[tid].empty()) {
        InstIt tail_thread = instList[tid].end();
        tail_thread--;

        squashIt[tid] = tail_thread;

        doSquash(tid);
    }
}

// ROB计算动态Squash宽度：根据不同的ROB遍历策略计算squash宽度
unsigned
ROB::computeDynSquashWidth(unsigned uncommitted_insts, unsigned to_squash)
{
    unsigned dyn_squash_width = 0;
    double expected_cycles;
    switch (robWalkPolicy) {
        case ROBWalkPolicy::Rollback:
            // 回滚策略：使用固定的回滚宽度
            dyn_squash_width = rollbackWidth;
            DPRINTF(ROB, "Recovery with rollback, walk ROB with width %u\n", dyn_squash_width);
            break;

        case ROBWalkPolicy::Replay:
            // 重放策略：根据重放宽度计算squash宽度
            expected_cycles =
                std::max(2.0, ((double)uncommitted_insts / replayWidth));
            dyn_squash_width = ceil((double)to_squash / expected_cycles);
            dyn_squash_width = std::max(dyn_squash_width, 1u);
            DPRINTF(
                ROB,
                "Recovery with replay, walk ROB with width %u in %f cycles\n",
                dyn_squash_width, expected_cycles);
            break;

        case ROBWalkPolicy::ConstCycle:
            // 恒定周期策略：在固定周期内完成squash
            dyn_squash_width = ceil((double) to_squash / (double) constSquashCycle);
            dyn_squash_width = std::max(dyn_squash_width, rollbackWidth);
            DPRINTF(ROB, "Recovery with const cycle, walk ROB with width %u\n", dyn_squash_width);
            break;

        default:
            break;
    }
    return dyn_squash_width;
}

// ROB读取头部指令：返回指定线程的ROB头部指令
const DynInstPtr&
ROB::readHeadInst(ThreadID tid)
{
    if (!threadGroups[tid].empty() && threadGroups[tid].front() != 0) {
        assert(instList[tid].size() > 0);
        InstIt head_thread = instList[tid].begin();

        assert((*head_thread)->isInROB());

        return *head_thread;
    } else {
        return dummyInst;
    }
}

// ROB读取尾部指令：返回指定线程的ROB尾部指令
DynInstPtr
ROB::readTailInst(ThreadID tid)
{
    InstIt tail_thread = instList[tid].end();
    tail_thread--;

    return *tail_thread;
}

// ROB统计信息构造函数：初始化ROB统计计数器
ROB::ROBStats::ROBStats(statistics::Group *parent)
  : statistics::Group(parent, "rob"),
    ADD_STAT(reads, statistics::units::Count::get(),
        "The number of ROB reads"),
    ADD_STAT(writes, statistics::units::Count::get(),
        "The number of ROB writes"),
    ADD_STAT(instPergroup, statistics::units::Count::get())
{
    instPergroup.init(0, 8, 1).flags(statistics::nozero);
}

// ROB查找指令：在指定线程的ROB中查找指定序列号的指令
DynInstPtr
ROB::findInst(ThreadID tid, InstSeqNum squash_inst)
{
    for (InstIt it = instList[tid].begin(); it != instList[tid].end(); it++) {
        if ((*it)->seqNum == squash_inst) {
            return *it;
        }
    }
    return NULL;
}

} // namespace o3
} // namespace gem5

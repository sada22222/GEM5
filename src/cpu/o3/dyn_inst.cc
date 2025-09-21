/*
 * Copyright (c) 2010-2011, 2021 ARM Limited
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
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
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

#include "cpu/o3/dyn_inst.hh"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "arch/riscv/insts/mem.hh"
#include "base/intmath.hh"
#include "debug/DynInst.hh"
#include "debug/IQ.hh"
#include "debug/O3PipeView.hh"

namespace gem5
{

namespace o3
{

// 动态指令构造函数
// 创建一个新的动态指令实例，初始化所有必要的数据结构
DynInst::DynInst(const Arrays &arrays, const StaticInstPtr &static_inst,
        const StaticInstPtr &_macroop, InstSeqNum seq_num, CPU *_cpu)
    : seqNum(seq_num),                    // 指令序列号
      staticInst(static_inst),            // 静态指令指针
      xsMeta(new XsDynInstMeta()),        // XiangShan扩展元数据
      cpu(_cpu),                          // CPU指针
      _numSrcs(arrays.numSrcs),           // 源操作数数量
      _numDests(arrays.numDests),         // 目标操作数数量
      _flatDestIdx(arrays.flatDestIdx),   // 平坦化目标寄存器索引
      _destIdx(arrays.destIdx),           // 目标寄存器索引
      _prevDestIdx(arrays.prevDestIdx),   // 前一个目标寄存器索引
      _srcIdx(arrays.srcIdx),             // 源寄存器索引
      _readySrcIdx(arrays.readySrcIdx),   // 就绪源寄存器索引
      macroop(_macroop)                   // 宏操作指针
{
    // 初始化源寄存器就绪状态位图（每8个源寄存器用1个字节表示）
    std::fill(_readySrcIdx, _readySrcIdx + (numSrcs() + 7) / 8, 0);

    // 重置指令状态位图
    status.reset();

    // 初始化指令标志
    instFlags.reset();
    instFlags[RecordResult] = true;     // 记录结果标志
    instFlags[Predicate] = true;        // 预测标志
    instFlags[MemAccPredicate] = true;  // 内存访问预测标志

#ifndef NDEBUG
    // 调试模式：增加指令计数器
    ++cpu->instcount;

    // 如果指令数量过多，进行调试输出并断言
    if (cpu->instcount > 1500) {
#ifdef DEBUG
        cpu->dumpInsts();  // 转储指令信息
        dumpSNList();      // 转储序列号列表
#endif
        assert(cpu->instcount <= 1500);  // 断言指令数量不超过1500
    }

    DPRINTF(DynInst,
        "DynInst: [sn:%lli] Instruction created. Instcount for %s = %i\n",
        seqNum, cpu->name(), cpu->instcount);
#endif

#ifdef DEBUG
    // 调试模式：将序列号添加到CPU的序列号列表中
    cpu->snList.insert(seqNum);
#endif

}

// 带PC状态的动态指令构造函数
// 创建动态指令并设置PC和预测PC
DynInst::DynInst(const Arrays &arrays, const StaticInstPtr &static_inst,
        const StaticInstPtr &_macroop, const PCStateBase &_pc,
        const PCStateBase &pred_pc, InstSeqNum seq_num, CPU *_cpu)
    : DynInst(arrays, static_inst, _macroop, seq_num, _cpu)  // 调用基础构造函数
{
    set(pc, _pc);          // 设置指令PC
    set(predPC, pred_pc);  // 设置预测PC
}

// 简化版动态指令构造函数
// 用于创建测试或模拟用的动态指令，无CPU关联
DynInst::DynInst(const Arrays &arrays, const StaticInstPtr &_staticInst,
        const StaticInstPtr &_macroop)
    : DynInst(arrays, _staticInst, _macroop, 0, nullptr)  // 序列号为0，无CPU
{}

/*
 * 自定义"new"操作符
 * 使用默认的"new"操作符为DynInst分配空间，同时额外分配一些字节
 * 为DynInst需要的额外结构预留空间。这样只需要一次堆分配就能获取
 * 所有这些结构的空间，从而节省时间并提高性能。
 *
 * 当使用new分配DynInst时，编译器会调用这个"new"操作符，"count"参数
 * 设置为存储DynInst所需的字节数。我们最终会调用默认的new操作符来获取
 * 这些字节，但在此之前，我们会填充"count"以便为DynInst需要的一些结构
 * 提供额外空间。我们考虑了这些结构的绝对大小和对齐要求。
 *
 * 一旦获得了足够大的缓冲区来容纳DynInst本身和这些额外结构，
 * 我们就会使用就地new构造这些额外的部分。这会在我们为它们创建的
 * 空间中就地构造这些结构。
 *
 * 接下来，我们返回缓冲区作为操作符的结果。编译器接受该缓冲区
 * 并使用DynInst构造函数在其开始处构造DynInst。
 *
 * 为了避免必须两次计算这些额外结构的位置（一次是为它们分配空间并
 * 初始化，然后在DynInst构造函数中再次计算），我们还传入了一个
 * 名为"arrays"的结构，其中包含指向它们的指针。"arrays"的字段在
 * 此操作符中初始化，然后在DynInst构造函数中使用。
 */
void *
DynInst::operator new(size_t count, Arrays &arrays)
{
    // 为简洁起见的便利变量
    const auto num_dests = arrays.numDests;  // 目标寄存器数量
    const auto num_srcs = arrays.numSrcs;    // 源寄存器数量

    // 计算所有组件的存储位置
    uintptr_t inst = 0;         // DynInst实例的起始位置（偏移量为0）
    size_t inst_size = count;   // DynInst实例的大小

    // 计算平坦化目标寄存器索引数组的位置和大小
    uintptr_t flat_dest_idx = roundUp(inst + inst_size, alignof(RegId));
    size_t flat_dest_idx_size = sizeof(*arrays.flatDestIdx) * num_dests;

    // 计算目标寄存器索引数组的位置和大小
    uintptr_t dest_idx =
        roundUp(flat_dest_idx + flat_dest_idx_size, alignof(VirtRegId));
    size_t dest_idx_size = sizeof(*arrays.destIdx) * num_dests;

    // 计算前一个目标寄存器索引数组的位置和大小
    uintptr_t prev_dest_idx =
        roundUp(dest_idx + dest_idx_size, alignof(VirtRegId));
    size_t prev_dest_idx_size = sizeof(*arrays.prevDestIdx) * num_dests;

    // 计算源寄存器索引数组的位置和大小
    uintptr_t src_idx =
        roundUp(prev_dest_idx + prev_dest_idx_size, alignof(VirtRegId));
    size_t src_idx_size = sizeof(*arrays.srcIdx) * num_srcs;

    // 计算就绪源寄存器索引位图的位置和大小
    uintptr_t ready_src_idx =
        roundUp(src_idx + src_idx_size, alignof(uint8_t));
    size_t ready_src_idx_size =
        sizeof(*arrays.readySrcIdx) * ((num_srcs + 7) / 8);  // 每8个源寄存器用1字节

    // 计算总共需要的空间大小
    size_t total_size = ready_src_idx + ready_src_idx_size;

    // 实际分配内存
    uint8_t *buf = (uint8_t *)::operator new(total_size);

    // 用指向所有数组的指针填充"arrays"结构
    arrays.flatDestIdx = (RegId *)(buf + flat_dest_idx);        // 平坦化目标寄存器索引
    arrays.destIdx = (VirtRegId *)(buf + dest_idx);             // 目标寄存器索引
    arrays.prevDestIdx = (VirtRegId *)(buf + prev_dest_idx);    // 前一个目标寄存器索引
    arrays.srcIdx = (VirtRegId *)(buf + src_idx);               // 源寄存器索引
    arrays.readySrcIdx = (uint8_t *)(buf + ready_src_idx);      // 就绪源寄存器位图

    // 使用placement new初始化所有额外的组件
    new (arrays.flatDestIdx) RegId[num_dests];      // 平坦化目标寄存器ID数组
    new (arrays.destIdx) VirtRegId[num_dests];      // 目标虚拟寄存器ID数组
    new (arrays.prevDestIdx) VirtRegId[num_dests];  // 前一个目标虚拟寄存器ID数组
    new (arrays.srcIdx) VirtRegId[num_srcs];        // 源虚拟寄存器ID数组
    new (arrays.readySrcIdx) uint8_t[num_srcs];     // 就绪源寄存器位图数组

    return buf;
}

// 自定义delete操作符
// 由于使用了自定义的"new"操作符分配了比DynInst对象本身更多的字节，
// AddressSanitizer会抛出new-delete-type-mismatch错误。
// 添加自定义的delete函数可以消除这个误报
void
DynInst::operator delete(void *ptr)
{
    ::operator delete(ptr);  // 调用默认的delete操作符
}

// 动态指令析构函数
// 销毁动态指令实例，清理所有相关资源
DynInst::~DynInst()
{
    /*
     * DynInst占用的缓冲区还包含它指向的一些结构。我们需要手动调用它们的析构函数
     * 以确保它们被适当清理，但我们不需要显式释放它们的内存，因为这部分内存是
     * DynInst缓冲区的一部分，并且已经作为删除DynInst的一部分被释放。
     */
    // 销毁所有目标寄存器相关的结构
    for (int i = 0; i < _numDests; i++) {
        _flatDestIdx[i].~RegId();       // 销毁平坦化寄存器ID
        _destIdx[i].~VirtRegId();       // 销毁目标虚拟寄存器ID
        _prevDestIdx[i].~VirtRegId();   // 销毁前一个目标虚拟寄存器ID
    }

    // 销毁所有源寄存器虚拟ID结构
    for (int i = 0; i < _numSrcs; i++)
        _srcIdx[i].~VirtRegId();

    // 销毁就绪源寄存器位图（每8个源寄存器用1个字节）
    for (int i = 0; i < ((_numSrcs + 7) / 8); i++)
        _readySrcIdx[i].~uint8_t();

#if TRACING_ON
    // O3流水线视图跟踪：输出指令在各个流水线阶段的时间戳
    if (debug::O3PipeView) {
        Tick fetch = fetchTick;
        // 如果指令在跟踪窗口外取指，fetchTick可能为-1
        if (fetch != -1) {
            Tick val;
            // 输出流水线活动查看器需要的信息
            DPRINTFR(O3PipeView, "O3PipeView:fetch:%llu:0x%08llx:%d:%llu:%s\n",
                     fetch,                                    // 取指时间
                     pcState().instAddr(),                     // 指令地址
                     pcState().microPC(),                      // 微指令PC
                     seqNum,                                   // 序列号
                     staticInst->disassemble(pcState().instAddr()));  // 反汇编

            // 输出各个流水线阶段的时间戳
            val = (decodeTick == -1) ? 0 : fetch + decodeTick;
            DPRINTFR(O3PipeView, "O3PipeView:decode:%llu\n", val);     // 译码阶段
            val = (renameTick == -1) ? 0 : fetch + renameTick;
            DPRINTFR(O3PipeView, "O3PipeView:rename:%llu\n", val);     // 重命名阶段
            val = (dispatchTick == -1) ? 0 : fetch + dispatchTick;
            DPRINTFR(O3PipeView, "O3PipeView:dispatch:%llu\n", val);   // 分发阶段
            val = (issueTick == -1) ? 0 : fetch + issueTick;
            DPRINTFR(O3PipeView, "O3PipeView:issue:%llu\n", val);      // 发射阶段
            val = (completeTick == -1) ? 0 : fetch + completeTick;
            DPRINTFR(O3PipeView, "O3PipeView:complete:%llu\n", val);   // 完成阶段
            val = (commitTick == -1) ? 0 : fetch + commitTick;

            // 存储指令的特殊时间戳
            Tick valS = (storeTick == -1) ? 0 : fetch + storeTick;
            DPRINTFR(O3PipeView, "O3PipeView:retire:%llu:store:%llu\n",
                    val, valS);  // 退休阶段和存储时间
        }
    }
#endif

    delete [] memData;   // 删除内存数据
    delete traceData;    // 删除跟踪数据
    fault = NoFault;     // 重置故障状态

#ifndef NDEBUG
    // 调试模式：减少指令计数器
    --cpu->instcount;

    // 调试输出：记录指令销毁信息
    DPRINTF(DynInst,
        "DynInst: [sn:%lli] Instruction destroyed. Instcount for %s = %i\n",
        seqNum, cpu->name(), cpu->instcount);
#endif
#ifdef DEBUG
    // 调试模式：从CPU的序列号列表中移除该指令
    cpu->snList.erase(seqNum);
#endif
};


#ifdef DEBUG
// 调试函数：转储序列号列表
// 输出所有未销毁的指令序列号，用于调试内存泄漏问题
void
DynInst::dumpSNList()
{
    std::set<InstSeqNum>::iterator sn_it = cpu->snList.begin();

    int count = 0;
    while (sn_it != cpu->snList.end()) {
        // 输出每个未销毁指令的序列号
        cprintf("%i: [sn:%lli] not destroyed\n", count, (*sn_it));
        count++;
        sn_it++;
    }
}
#endif

// 转储指令信息到控制台
// 以格式化的方式输出指令的线程号、PC地址和反汇编代码
void
DynInst::dump()
{
    cprintf("T%d : %#08d `", threadNumber, pc->instAddr());  // 输出线程号和PC地址
    std::cout << staticInst->disassemble(pc->instAddr());    // 输出反汇编代码
    cprintf("'\n");                                          // 换行
}

// 转储指令信息到字符串
// 将指令的线程号、PC地址和反汇编代码格式化为字符串输出
void
DynInst::dump(std::string &outstring)
{
    std::ostringstream s;
    s << "T" << threadNumber << " : 0x" << pc->instAddr() << " "  // 线程号和PC地址
      << staticInst->disassemble(pc->instAddr());                 // 反汇编代码

    outstring = s.str();  // 将格式化结果存储到输出字符串
}

// 标记源寄存器就绪（无参数版本）
// 递增就绪寄存器计数，如果所有源寄存器都就绪则设置可发射标志
void
DynInst::markSrcRegReady()
{
    DPRINTF(IQ, "[sn:%lli] has %d ready out of %d sources. RTI %d)\n",
            seqNum, readyRegs+1, numSrcRegs(), readyToIssue());
    // 如果递增后的就绪寄存器数等于总源寄存器数，则设置可发射标志
    if (++readyRegs == numSrcRegs()) {
        setCanIssue();
    }
}

// 标记指定索引的源寄存器就绪
// 设置对应源寄存器的就绪位，然后调用无参数版本更新整体状态
void
DynInst::markSrcRegReady(RegIndex src_idx)
{
    readySrcIdx(src_idx, true);  // 设置指定源寄存器索引的就绪位
    markSrcRegReady();           // 调用无参数版本更新整体就绪状态
}

// 清除指定索引源寄存器的就绪状态
// 清除对应源寄存器的就绪位，递减就绪计数并清除可发射标志
void
DynInst::clearSrcRegReady(RegIndex src_idx)
{
    assert(readySrcIdx(src_idx));    // 断言该源寄存器当前是就绪的
    readySrcIdx(src_idx, false);     // 清除指定源寄存器索引的就绪位
    readyRegs--;                     // 递减就绪寄存器计数
    clearCanIssue();                 // 清除可发射标志
}

// 重置就绪源寄存器数量
// 设置就绪寄存器数量为指定值，并重置所有就绪位图
void DynInst::resetNumSrcRegReady(uint8_t n) {
    readyRegs = n;                           // 设置就绪寄存器数量
    if (n < numSrcRegs()) {                  // 如果就绪数小于总数
        clearCanIssue();                     // 清除可发射标志
    }
    // 重置就绪源寄存器位图（每8个源寄存器用1个字节）
    memset(_readySrcIdx, 0, (numSrcs() + 7) / 8);
}

// 设置指令为被压制状态
// 标记指令被压制，并处理固定寄存器的重命名状态
void
DynInst::setSquashed()
{
    status.set(Squashed);           // 设置压制状态位
    xsMeta->squashed = true;        // 设置XiangShan元数据中的压制标志

    // 如果固定寄存器未重命名或压制处理已完成，则直接返回
    if (!isPinnedRegsRenamed() || isPinnedRegsSquashDone())
        return;

    // 该指令已经被重命名，所以可能会再次通过重命名阶段
    //（例如，如果压制是由于内存访问顺序违例引起的）。
    // 重置所有固定目标寄存器的写入计数器，以确保在可能的重新重命名时
    // 它们处于一致状态。这也确保了如果发生重新重命名，目标寄存器
    // 将固定到相同的物理寄存器。
    for (int idx = 0; idx < numDestRegs(); idx++) {
        PhysRegIdPtr phys_dest_reg = renamedDestIdx(idx);  // 获取重命名的目标寄存器
        if (phys_dest_reg->isPinned()) {                   // 如果寄存器被固定
            phys_dest_reg->incrNumPinnedWrites();          // 递增固定写入次数
            if (isPinnedRegsWritten())                      // 如果固定寄存器已写入
                phys_dest_reg->incrNumPinnedWritesToComplete();  // 递增待完成的固定写入次数
        }
    }
    setPinnedRegsSquashDone();  // 设置固定寄存器压制处理完成标志
}

// 执行指令
// 调用静态指令的execute方法执行指令逻辑，处理线程上下文的压制控制
Fault
DynInst::execute()
{
    // @todo: 相当复杂的方式来避免在指令执行期间使用TC时发生压制
    //（特别是对于具有使用TC副作用的指令）。需要修复这个问题。
    bool no_squash_from_TC = thread->noSquashFromTC;  // 保存原始的TC压制设置
    thread->noSquashFromTC = true;                    // 临时禁用TC压制

    fault = staticInst->execute(this, traceData);     // 执行静态指令

    thread->noSquashFromTC = no_squash_from_TC;       // 恢复原始的TC压制设置

    return fault;  // 返回故障状态
}

// 发起内存访问
// 对于内存指令，启动内存访问操作的第一阶段（地址计算等）
Fault
DynInst::initiateAcc()
{
    // @todo: 相当复杂的方式来避免在指令执行期间使用TC时发生压制
    //（特别是对于具有使用TC副作用的指令）。需要修复这个问题。
    bool no_squash_from_TC = thread->noSquashFromTC;  // 保存原始的TC压制设置
    thread->noSquashFromTC = true;                    // 临时禁用TC压制

    fault = staticInst->initiateAcc(this, traceData); // 发起内存访问

    thread->noSquashFromTC = no_squash_from_TC;       // 恢复原始的TC压制设置

    return fault;  // 返回故障状态
}

// 完成内存访问
// 对于内存指令，完成内存访问操作的第二阶段（数据返回处理等）
Fault
DynInst::completeAcc(PacketPtr pkt)
{
    // @todo: 相当复杂的方式来避免在指令执行期间使用TC时发生压制
    //（特别是对于具有使用TC副作用的指令）。需要修复这个问题。
    bool no_squash_from_TC = thread->noSquashFromTC;  // 保存原始的TC压制设置
    thread->noSquashFromTC = true;                    // 临时禁用TC压制

    // 如果启用了检查器，处理条件存储指令的验证
    if (cpu->checker) {
        if (isStoreConditional()) {  // 如果是条件存储指令
            reqToVerify->setExtraData(pkt->req->getExtraData());  // 设置额外验证数据
        }
    }

    fault = staticInst->completeAcc(pkt, this, traceData);  // 完成内存访问

    // 如果没有故障，处理条件存储的结果
    if (fault == NoFault) {
        if (isStoreConditional()) {  // 如果是条件存储指令
            // 根据包的额外数据设置锁定写入成功状态
            lockedWriteSuccess(pkt->req->getExtraData() != 0);
        }
    }

    thread->noSquashFromTC = no_squash_from_TC;  // 恢复原始的TC压制设置

    return fault;  // 返回故障状态
}

// 构建存储地址微操作
// 将当前指令转换为存储地址微操作，用于分离存储地址和存储数据操作
void DynInst::buildStoreAddrUop()
{
    assert(staticInst->isSplitStoreAddr());  // 断言这是分离的存储地址指令
    assert(numSrcRegs() == 2);               // 断言有2个源寄存器
    // 将自身转换为存储地址微操作

    // 标记地址就绪
    if (!this->readySrcIdx(1)) this->markSrcRegReady(1);  // 如果地址源寄存器未就绪，标记为就绪
    // 将第二个源寄存器重命名为无效，因为地址微操作不需要数据源
    this->renameSrcReg(1, VirtRegId(UnifiedRenameMap::getInvalid()));
}

// 创建存储数据微操作
// 为分离的存储指令创建对应的存储数据微操作
DynInstPtr DynInst::createStoreDataUop()
{
    assert(staticInst->isSplitStoreAddr());  // 断言这是分离的存储地址指令

    // 设置存储数据微操作的数组参数
    Arrays arrays;
    arrays.numSrcs = 1;   // 存储数据微操作只有1个源寄存器（数据）
    arrays.numDests = 0;  // 存储数据微操作没有目标寄存器

    // 创建RISC-V存储数据静态指令
    StaticInstPtr stdinst = new RiscvISA::StoreData(this->staticInst);

    // 创建存储数据动态指令
    DynInstPtr stduop = new (arrays) DynInst(arrays, stdinst, macroop, this->seqNum, cpu);

    stduop->thread = this->thread;                             // 设置线程指针
    stduop->renameSrcReg(0, this->extRenamedSrcIdx(1));       // 重命名源寄存器（数据源）

    // 如果原指令的数据源已就绪，标记新微操作的源寄存器就绪
    if (this->readySrcIdx(1)) {
        stduop->markSrcRegReady(0);
    }

    // 复制存储队列相关信息
    stduop->sqIdx = this->sqIdx;  // 存储队列索引
    stduop->sqIt = this->sqIt;    // 存储队列迭代器

    return stduop;  // 返回创建的存储数据微操作
}

// 触发异常陷阱
// 当指令执行出现故障时，通知CPU处理异常
void
DynInst::trap(const Fault &fault)
{
    cpu->trap(fault, threadNumber, staticInst);  // 调用CPU的trap方法处理异常
}

// 发起内存读取请求
// 创建并向CPU推送内存读取请求，支持字节使能控制
Fault
DynInst::initiateMemRead(Addr addr, unsigned size, Request::Flags flags,
                               const std::vector<bool> &byte_enable)
{
    assert(byte_enable.size() == size);  // 断言字节使能向量大小与请求大小匹配
    return cpu->pushRequest(
        dynamic_cast<DynInstPtr::PtrType>(this),  // 动态指令指针
        /* ld */ true,                            // 标记为加载操作
        nullptr,                                  // 无数据（读取操作）
        size,                                     // 请求大小
        addr,                                     // 内存地址
        flags,                                    // 请求标志
        nullptr,                                  // 无结果指针
        nullptr,                                  // 无原子操作
        byte_enable);                             // 字节使能向量
}

// 发起内存管理命令请求
// 创建并向CPU推送内存管理命令（如缓存刷新、内存屏障等）
Fault
DynInst::initiateMemMgmtCmd(Request::Flags flags)
{
    const unsigned int size = 8;  // 内存管理命令的默认大小
    return cpu->pushRequest(
            dynamic_cast<DynInstPtr::PtrType>(this),  // 动态指令指针
            /* ld */ true,                            // 标记为加载类型（内存管理命令）
            nullptr,                                  // 无数据
            size,                                     // 请求大小
            0x0ul,                                    // 地址为0（内存管理命令通常不需要具体地址）
            flags,                                    // 请求标志
            nullptr,                                  // 无结果指针
            nullptr,                                  // 无原子操作
            std::vector<bool>(size, true));          // 全部字节使能
}

// 发起内存写入请求
// 创建并向CPU推送内存写入请求，支持字节使能控制和结果返回
Fault
DynInst::writeMem(uint8_t *data, unsigned size, Addr addr,
                        Request::Flags flags, uint64_t *res,
                        const std::vector<bool> &byte_enable)
{
    assert(byte_enable.size() == size);  // 断言字节使能向量大小与请求大小匹配
    return cpu->pushRequest(
        dynamic_cast<DynInstPtr::PtrType>(this),  // 动态指令指针
        /* st */ false,                           // 标记为存储操作
        data,                                     // 要写入的数据
        size,                                     // 请求大小
        addr,                                     // 内存地址
        flags,                                    // 请求标志
        res,                                      // 结果指针（用于条件存储）
        nullptr,                                  // 无原子操作
        byte_enable);                             // 字节使能向量
}

// 发起原子内存操作请求
// 创建并向CPU推送原子内存操作请求（如原子加、交换等）
Fault
DynInst::initiateMemAMO(Addr addr, unsigned size, Request::Flags flags,
                              AtomicOpFunctorPtr amo_op)
{
    // 原子内存指令还没有要写入内存的数据，因为原子操作将直接在缓存/内存中执行。
    // 因此，它的`data`字段是nullptr。
    // 原子内存请求需要将它们的`amo_op`字段传递到缓存/内存
    return cpu->pushRequest(
            dynamic_cast<DynInstPtr::PtrType>(this),  // 动态指令指针
            /* atomic */ false,                       // 标记为原子操作（非普通存储）
            nullptr,                                  // 无数据（原子操作在缓存中直接执行）
            size,                                     // 请求大小
            addr,                                     // 内存地址
            flags,                                    // 请求标志
            nullptr,                                  // 无结果指针
            std::move(amo_op),                        // 原子操作函数对象
            std::vector<bool>(size, true));          // 全部字节使能
}
// 打印指令反汇编和执行结果
// 用于提交跟踪，输出指令的详细执行信息包括时间戳和结果
void
DynInst::printDisassemblyAndResult(const std::string &site) const
{
    // 输出基本的指令信息：调用位置、序列号、PC、各阶段时间戳、反汇编
    DPRINTF(CommitTrace, "%s [sn:%lu pc:%#lx] enDqT: %lu, exDqT: %lu, readyT: %lu, CompleT:%lu, %s",
            site.c_str(),                              // 调用位置
            seqNum,                                    // 指令序列号
            pcState().instAddr(),                      // PC地址
            enterDQTick,                               // 进入分发队列时间
            exitDQTick,                                // 退出分发队列时间
            readyTick,                                 // 就绪时间
            completionTick,                            // 完成时间
            staticInst->disassemble(pcState().instAddr()));  // 反汇编代码

    // 如果有指令执行结果，输出结果值
    if (instResult.size() > 0) {
        DPRINTFR(CommitTrace, ", res: %#lx", instResult.front().as<uint64_t>());
    }
    // 如果是向量指令且有目标寄存器，输出向量寄存器值
    else if (numDestRegs() > 0 && isVector()) {
        uint64_t val[RiscvISA::NumVecElemPerVecReg];   // 向量寄存器值数组
        cpu->getArchReg(destRegIdx(0), val, threadNumber);  // 获取目标向量寄存器值
        std::string s_val;
        // 从高位到低位格式化向量值（大端序显示）
        for (int j = RiscvISA::NumVecElemPerVecReg - 1; j >= 0; j--) {
            s_val += csprintf("%016lx", val[j]);       // 格式化为16进制
            if (j != 0) {
                s_val += "_";                          // 元素间用下划线分隔
            }
        }
        DPRINTFR(CommitTrace, ", res: %s", s_val);
    }

    // 如果是内存引用指令，输出物理地址
    if (isMemRef()) {
        DPRINTFR(CommitTrace, ", paddr: %#lx", physEffAddr);
    }

    DPRINTFR(CommitTrace, "\n");  // 输出换行
}

} // namespace o3
} // namespace gem5

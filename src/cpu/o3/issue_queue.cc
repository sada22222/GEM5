#include "cpu/o3/issue_queue.hh"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <queue>
#include <stack>
#include <string>
#include <vector>

#include "base/logging.hh"
#include "base/stats/group.hh"
#include "base/stats/info.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/func_unit.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/reg_class.hh"
#include "debug/Counters.hh"
#include "debug/Dispatch.hh"
#include "debug/Schedule.hh"
#include "enums/OpClass.hh"
#include "params/BaseO3CPU.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

// 弹出指令的宏定义
// 从指令队列中移除指令，更新计数器并释放选择器资源
#define POPINST(x)                        \
    do {                                  \
        assert(instNum != 0);             \
        assert(opNum[x->opClass()] != 0); \
        opNum[x->opClass()]--;            \
        instNum--;                        \
        selector->deallocate(x);          \
    } while (0)

// 将指令推入就绪队列的宏定义
// 按照选择策略（序列号）有序插入就绪队列
#define READYQ_PUSH(x)                                                                    \
    do {                                                                                  \
        (x)->setInReadyQ();                                                               \
        auto& readyQ = readyQclassify[(x)->opClass()];                                    \
        auto it = std::lower_bound(readyQ->begin(), readyQ->end(), (x), select_policy()); \
        readyQ->insert(it, (x));                                                          \
    } while (0)

// 必须与FUScheduler.py保持一致
// rfTypePortId = 寄存器文件类型ID + 端口ID
#define MAXVAL_TYPEPORTID (1 << (2 + 4))  // [5:4]是类型ID，[3:0]是端口ID
#define RF_GET_PRIORITY(x) ((x)&0b11)      // 获取优先级
#define RF_GET_TYPEPORTID(x) (((x) >> 2) & 0b111111) // 获取类型端口ID
#define RF_GET_PORTID(x) (((x) >> 2) & 0b1111)       // 获取端口ID
#define RF_GET_TYPEID(x) (((x) >> 6) & 0b11)         // 获取类型ID
#define RF_GET_RDWR(x) (((x) >> 8) & 0b1)            // 获取读写标志

#define RF_MAKE_TYPEPORTID(t, p) (((t) << 4) | (p)) // 构造类型端口ID

#define RF_INTID 0  // 整数寄存器文件ID
#define RF_FPID 1   // 浮点寄存器文件ID

namespace gem5
{

namespace o3
{

// 发射端口构造函数
// 初始化发射端口，设置支持的操作类型掩码
IssuePort::IssuePort(const IssuePortParams& params) : SimObject(params), rp(params.rp), fu(params.fu)
{
    // 遍历所有功能单元，设置支持的操作类型掩码
    for (auto it0 : params.fu) {
        for (auto it1 : it0->opDescList) {
            mask.set(it1->opClass);
        }
    }
}

// 基本选择器的选择函数
// 返回最旧的指令（默认策略）
ReadyQue::iterator
BaseSelector::select(ReadyQue::iterator begin, int portid)
{
    // 返回最旧的指令
    return begin;
}

// 年龄选择器设置父对象
// 初始化选择器，设置调度器和发射队列指针
void
PAgeSelector::setparent(Scheduler* scheduler, IssueQue* iq)
{
    BaseSelector::setparent(scheduler, iq);

    // 检查发射队列大小是否能被组大小整除
    panic_if(iq->iqsize % numInstperGroup != 0,
             "POldSelector: IssueQue size % numInstperGroup != 0, "
             "size: %d, numInstperGroup: %d\n",
             iq->iqsize, numInstperGroup);
    iqselectQ = &iq->selectQ;
    // 初始化空闲列表
    for (int i = 0; i < iq->iqsize; i++) {
        freelist.push_back(i);
    }
}

// 为指令分配标签
// 从空闲列表中分配一个IQ标签
void
PAgeSelector::allocate(const DynInstPtr& inst)
{
    assert(!freelist.empty());
    // 从空闲列表前端获取一个标签
    inst->iqtag = freelist.front();
    freelist.pop_front();
}

// 释放指令标签
// 将IQ标签返还给空闲列表
void
PAgeSelector::deallocate(const DynInstPtr& inst)
{
    assert(inst->iqtag >= 0 && inst->iqtag < (int)freelist.size());
    // 将标签返还给空闲列表
    freelist.push_back(inst->iqtag);
    inst->iqtag = -1;  // 重置标签
}

// 年龄选择器的选择函数
// 选择没有组冲突的最旧指令
ReadyQue::iterator
PAgeSelector::select(ReadyQue::iterator begin, int portid)
{
    if (iqselectQ->empty()) {
        // 如果选择队列为空，返回最旧的指令
        return begin;
    } else {
        // TODO: 加速搜索
        for (auto it = begin; it != end; it++) {
            auto& inst = *it;

            // 检查是否有组冲突
            bool no_group_conflict = true;
            for (auto sit = iqselectQ->begin(); sit != iqselectQ->end(); sit++) {
                // 检查组冲突
                if ((inst->iqtag % numInstperGroup) == (sit->second->iqtag % numInstperGroup)) {
                    no_group_conflict = false;
                    break;
                }
            }

            // 如果没有组冲突，返回此指令
            if (no_group_conflict) {
                return it;
            }
        }
        return end;
    }
}

// 发射队列选择策略
// 按照指令序列号排序（较小的在前）
bool
IssueQue::select_policy::operator()(const DynInstPtr& a, const DynInstPtr& b) const
{
    return a->seqNum < b->seqNum;
}

// 向发射流中推入指令
// 将指令添加到发射流的末尾
void
IssueQue::IssueStream::push(const DynInstPtr& inst)
{
    assert(size < 8);  // 确保不超过最大容量
    insts[size++] = inst;
}

// 从发射流中弹出指令
// 从发射流的末尾移除并返回指令
DynInstPtr
IssueQue::IssueStream::pop()
{
    assert(size > 0);  // 确保流不为空
    return insts[--size];
}

// 发射队列统计信息构造函数
// 初始化各种统计计数器
IssueQue::IssueQueStats::IssueQueStats(statistics::Group* parent, IssueQue* que, std::string name)
    : Group(parent, name.c_str()),
      ADD_STAT(retryMem, statistics::units::Count::get(), "count of load/store retry"),      // 内存指令重试数
      ADD_STAT(canceledInst, statistics::units::Count::get(), "count of canceled insts"),   // 被取消指令数
      ADD_STAT(loadmiss, statistics::units::Count::get(), "count of load miss"),            // load缺失数
      ADD_STAT(arbFailed, statistics::units::Count::get(), "count of arbitration failed"), // 仲裁失败数
      ADD_STAT(issueOccupy, statistics::units::Count::get(), "count of replayQ blocked"),  // 重放队列阻塞数
      ADD_STAT(insertDist, statistics::units::Count::get(), "distruibution of insert"),     // 插入分布
      ADD_STAT(issueDist, statistics::units::Count::get(), "distruibution of issue"),       // 发射分布
      ADD_STAT(portissued, statistics::units::Count::get(), "count each port issues"),      // 每个端口发射数
      ADD_STAT(portBusy, statistics::units::Count::get(), "count each port busy cycles"),   // 每个端口忙周期数
      ADD_STAT(avgInsts, statistics::units::Count::get(), "average insts")                  // 平均指令数
{
    // 初始化各种统计分布和标志
    insertDist.init(que->inports + 1).flags(statistics::nozero);   // 插入端口数+1
    issueDist.init(que->outports + 1).flags(statistics::nozero);   // 发射端口数+1
    portissued.init(que->outports).flags(statistics::nozero);      // 发射端口数
    portBusy.init(que->outports).flags(statistics::nozero);        // 发射端口数
    retryMem.flags(statistics::nozero);
    canceledInst.flags(statistics::nozero);
    loadmiss.flags(statistics::nozero);
    arbFailed.flags(statistics::nozero);
    issueOccupy.flags(statistics::nozero);
}

// 发射队列构造函数
// 初始化发射队列的各个组件和参数
IssueQue::IssueQue(const IssueQueParams& params)
    : SimObject(params),
      inports(params.inports),                    // 输入端口数
      outports(params.oports.size()),            // 输出端口数
      iqsize(params.size),                       // 队列大小
      scheduleToExecDelay(params.scheduleToExecDelay), // 调度到执行的延迟
      iqname(params.name),                       // 队列名称
      inflightIssues(scheduleToExecDelay, 0),    // 正在飞行的发射指令
      selector(params.sel)                       // 指令选择器
{
    // 获取发射和功能单元的时间缓冲线
    toIssue = inflightIssues.getWire(0);                      // 当前周期发射
    toFu = inflightIssues.getWire(-scheduleToExecDelay);      // 延迟后发射到FU
    if (outports > 8) {
        panic("%s: outports > 8 is not supported\n", iqname);
    }

    // 初始化各种数据结构
    opNum.resize(enums::Num_OpClass, 0);         // 每种操作类型的指令数
    portBusy.resize(outports, 0);                // 端口忙状态

    intRdRfTPI.resize(outports);                 // 整数读寄存器文件端口
    fpRdRfTPI.resize(outports);                  // 浮点读寄存器文件端口
    intWrRfTPI.resize(outports);                 // 整数写寄存器文件端口

    readyQs.resize(outports, nullptr);           // 每个端口的就绪队列

    readyQclassify.resize(Num_OpClasses, nullptr); // 按操作类型分类的就绪队列
    opPipelined.resize(Num_OpClasses, false);      // 操作是否为流水线方式

    // 使用映射表管理就绪队列，相同操作类型掩码共享队列
    std::unordered_map<std::bitset<Num_OpClasses>, ReadyQue*> readyQmap;
    for (int i = 0; i < outports; i++) {
        auto oport = params.oports[i];

        int wr_pri = -1;  // 写端口优先级
        // 遍历此输出端口的所有寄存器文件端口
        for (auto rfp : oport->rp) {
            int rf_type = RF_GET_TYPEID(rfp);           // 寄存器文件类型
            int rf_portPri = RF_GET_PRIORITY(rfp);      // 端口优先级
            int is_wr = RF_GET_RDWR(rfp);               // 是否为写端口
            int rf_typeportid = RF_GET_TYPEPORTID(rfp); // 类型端口ID

            assert(rf_portPri < (1 << 2));     // 优先级使用2位
            assert(rf_typeportid < (1 << 6));  // 类型端口ID使用6位

            auto rf_typeportid_pair = std::make_pair(rf_typeportid, rf_portPri);

            if (is_wr) {
                // 处理写端口
                if (rf_type == RF_INTID) {
                    intWrRfTPI[i].push_back(rf_typeportid_pair);
                } else {
                    panic("%s: Unknown write RF type %d\n", iqname, rf_type);
                }
                if (rf_portPri > 1) {
                    panic("Num of write arbitration RF port greater than 2 are not supported \n");
                }

                wr_pri = rf_portPri;
            } else {
                // 处理读端口
                if (rf_type == RF_INTID) {
                    intRdRfTPI[i].push_back(rf_typeportid_pair);  // 整数读端口
                } else if (rf_type == RF_FPID) {
                    fpRdRfTPI[i].push_back(rf_typeportid_pair);   // 浮点读端口
                } else {
                    panic("%s: Unknown RF type %d\n", iqname, rf_type);
                }
            }

            if (wr_pri != -1 && wr_pri != rf_portPri) {
                // 如果有写RF，所有读RF必须具有相同的优先级
                panic("%s: Found write RF priority with different other's priority\n", iqname);
            }
        }

        // 输出端口的安全检查
        for (int j = i + 1; j < outports; j++) {
            if ((oport->mask != params.oports[j]->mask) && (oport->mask & params.oports[j]->mask).any()) {
                panic("%s: Found the same opClass in different FU, portid: %d and %d\n", iqname, i, j);
            }
        }
        // 将功能单元描述符添加到集合中
        fuDescs.insert(fuDescs.begin(), oport->fu.begin(), oport->fu.end());

        // 查找或创建就绪队列
        auto it = readyQmap.find(oport->mask);
        ReadyQue* t = nullptr;
        if (it == readyQmap.end()) {
            // 创建新的就绪队列
            t = new ReadyQue;
            readyQmap[oport->mask] = t;
        } else {
            // 使用现有的队列
            t = it->second;
        }
        readyQs[i] = t;

        // 检查是否支持load/store流水线访问
        bool storePipeAcc = false, loadPipeAcc = false;
        for (auto fu : oport->fu) {
            for (auto op : fu->opDescList) {
                // 设置操作类型到就绪队列的映射
                readyQclassify[op->opClass] = t;
                opPipelined[op->opClass] = op->pipelined;

                // 检查是否为Load操作
                if (op->opClass >= MemReadOp && op->opClass <= VectorWholeRegisterLoadOp) {
                    loadPipeAcc = true;
                }
                // 检查是否为Store操作
                if (op->opClass >= MemWriteOp && op->opClass <= VectorWholeRegisterStoreOp) {
                    storePipeAcc = true;
                }
            }
        }

        // 统计Load和Store流水线数量
        if (loadPipeAcc)
            numLoadPipe++;
        if (storePipeAcc)
            numStorePipe++;
    }
}
}

// 设置CPU指针
// 初始化CPU指针并创建统计对象
void
IssueQue::setCPU(CPU* cpu)
{
    this->cpu = cpu;
    _name = cpu->name() + ".scheduler." + getName();
    // 创建发射队列统计对象
    iqstats = new IssueQueStats(cpu, this, "scheduler." + this->getName());
}

// 重置依赖图
// 调整依赖图的大小以适应物理寄存器数量
void
IssueQue::resetDepGraph(int numPhysRegs)
{
    subDepGraph.resize(numPhysRegs);
}

// 检查计分板
// 检查指令的源寄存器是否可以通过旁路网络获取数据
bool
IssueQue::checkScoreboard(const DynInstPtr& inst)
{
    for (int i = 0; i < inst->numSrcRegs(); i++) {
        auto src = inst->renamedSrcIdx(i);
        if (src->isFixedMapping()) [[unlikely]] {
            continue;
        }
        // 检查旁路数据是否就绪
        if (!scheduler->bypassScoreboard[src->flatIndex()]) [[unlikely]] {
            auto dst_inst = scheduler->getInstByDstReg(src->flatIndex());
            if (!dst_inst || !dst_inst->isLoad()) {
                panic("dst[sn:%llu] is not load", dst_inst->seqNum);
            }
            DPRINTF(Schedule, "[sn:%llu] %s can't get data from bypassNetwork, dst inst: %s\n", inst->seqNum,
                    inst->srcRegIdx(i), dst_inst->genDisassembly());
            // 取消load指令
            scheduler->loadCancel(dst_inst);
            return false;
        }
    }
    return true;
}

// 将指令添加到功能单元
// 将已选中的指令发送到功能单元执行
void
IssueQue::addToFu(const DynInstPtr& inst)
{
    if (inst->isIssued()) [[unlikely]] {
        panic("%s [sn:%llu] has alreayd been issued\n", enums::OpClassStrings[inst->opClass()], inst->seqNum);
    }
    // 标记指令为已发射
    inst->setIssued();
    // 从队列中移除指令
    POPINST(inst);
    // 添加到功能单元
    scheduler->addToFU(inst);
}

// 发射指令到功能单元
// 将调度好的指令发射到功能单元执行
void
IssueQue::issueToFu()
{
    int size = toFu->size;      // 待发射指令数量
    int replayed = 0;           // 已重放指令数
    int issued = 0;             // 已发射指令数

    int issuedLoad = 0;         // 已发射load数
    int issuedStore = 0;        // 已发射store数

    // 先处理重放指令
    for (; !replayQ.empty() && replayed < outports; replayed++) {
        auto& inst = replayQ.front();

        // 检查load流水线限制
        if (inst->isLoad()) {
            if (issuedLoad >= numLoadPipe) {
                break;
            }
            issuedLoad++;
        }
        // 检查store流水线限制
        if (inst->isStore()) {
            if (issuedStore >= numStorePipe) {
                break;
            }
            issuedStore++;
        }

        scheduler->addToFU(inst);
        DPRINTF(Schedule, "[sn:%llu] replayed to FU\n", inst->seqNum);
        replayQ.pop();
        issued++;
    }

    // 处理正常调度的指令
    for (int i = 0; i < size; i++) {
        auto inst = toFu->pop();
        if (!inst) {
            continue;
        }
        // 检查是否超过端口或流水线限制
        if ((i + replayed >= outports) || (inst->isLoad() && (issuedLoad >= numLoadPipe)) ||
            (inst->isStore() && (issuedStore >= numStorePipe))) {
            inst->clearScheduled();
            // 仅针对load/store指令，重新加入就绪队列
            READYQ_PUSH(inst);
            DPRINTF(Schedule, "[sn:%llu] issue failed due to being occupied\n", inst->seqNum);
            continue;
        }
        // 检查计分板
        if (!checkScoreboard(inst)) {
            continue;
        }

        // 更新已发射计数器
        if (inst->isLoad()) {
            issuedLoad++;
        }
        if (inst->isStore()) {
            issuedStore++;
        }
        addToFu(inst);
        // 更新性能计数器
        cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtIssueReadReg);
        issued++;
    }

    // 更新统计信息
    if (issued > 0) {
        iqstats->issueDist[issued]++;   // 发射数量分布
    }
    if (replayed) {
        iqstats->issueOccupy += replayed;  // 重放阻塞数
    }
}

// 重试内存指令
// 将失败的内存指令加入重放队列
void
IssueQue::retryMem(const DynInstPtr& inst)
{
    assert(!inst->isNonSpeculative());
    iqstats->retryMem++;  // 更新重试统计
    DPRINTF(Schedule, "retry %s [sn:%llu]\n", enums::OpClassStrings[inst->opClass()], inst->seqNum);
    replayQ.push(inst);   // 加入重放队列
}

// 检查队列是否空闲
// 返回true表示有指令在等待发射
bool
IssueQue::idle()
{
    bool idle = false;
    // 检查所有就绪队列是否有指令
    for (auto it : readyQs) {
        if (it->size()) {
            idle = true;
        }
    }
    // 检查重放队列是否有指令
    idle |= replayQ.size() > 0;
    return idle;
}

// 标记内存依赖完成
// 当内存指令的依赖问题解决后调用
void
IssueQue::markMemDepDone(const DynInstPtr& inst)
{
    assert(inst->isMemRef());
    DPRINTF(Schedule, "[sn:%llu] has solved memdependency\n", inst->seqNum);
    // 标记内存依赖已解决
    inst->setMemDepDone();
    // 尝试将指令加入就绪队列
    addIfReady(inst);
}

// 唤醒依赖指令
// 当指令完成后，唤醒依赖于其结果的指令
void
IssueQue::wakeUpDependents(const DynInstPtr& inst, bool speculative)
{
    if (speculative && inst->canceled()) [[unlikely]] {
        return;
    }
    // 遍历所有目标寄存器
    for (int i = 0; i < inst->numDestRegs(); i++) {
        PhysRegIdPtr dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping() || dst->getNumPinnedWritesToComplete() != 1) [[unlikely]] {
            continue;
        }
        // 将寄存器加入缓存
        scheduler->regCache.insert(dst->flatIndex(), {});
        DPRINTF(Schedule, "was %s woken by p%lu [sn:%llu]\n", speculative ? "spec" : "wb", dst->flatIndex(),
                inst->seqNum);
        // 获取依赖图中的消费者指令
        auto& depgraph = subDepGraph[dst->flatIndex()];
        for (auto& it : depgraph) {
            int srcIdx = it.first;
            auto& consumer = it.second;
            if (consumer->readySrcIdx(srcIdx)) {
                continue;
            }
            // 标记源寄存器就绪
            consumer->markSrcRegReady(srcIdx);


            DPRINTF(Schedule, "[sn:%llu] src%d was woken\n", consumer->seqNum, srcIdx);
            // 尝试将消费者指令加入就绪队列
            addIfReady(consumer);
        }

        // 如果不是推测性唤醒，清空依赖图
        if (!speculative) {
            depgraph.clear();
        }
    }
}

// 如果准备好则添加到就绪队列
// 检查指令是否准备好发射，如果是则加入就绪队列
void
IssueQue::addIfReady(const DynInstPtr& inst)
{
    if (inst->readyToIssue()) {
        // 记录指令准备好的时间
        if (inst->readyTick == -1) {
            inst->readyTick = curTick();
            DPRINTF(Counters, "set readyTick at addIfReady\n");
        }

        // 将指令添加到适当的就绪列表
        if (inst->isMemRef()) {
            if (inst->memDepSolved()) {
                DPRINTF(Schedule, "memRef Dependency was solved can issue\n");
            } else {
                DPRINTF(Schedule, "memRef Dependency was not solved can't issue\n");
                return;
            }
        }

        DPRINTF(Schedule, "[sn:%llu] add to readyInstsQue\n", inst->seqNum);
        // 清除取消标志
        inst->clearCancel();
        // 如果不在就绪队列中，则添加
        if (!inst->inReadyQ()) {
            READYQ_PUSH(inst);
        }
    }
}

// 取消指令
// 取消尚未发射的指令，清理相关资源
void
IssueQue::cancel(const DynInstPtr& inst)
{
    // 只能取消尚未发射的指令
    assert(!inst->isIssued());

    // 标记指令为被取消
    inst->setCancel();
    if (inst->isScheduled() && !opPipelined[inst->opClass()]) {
        inst->clearScheduled();
        portBusy[inst->issueportid] = 0;  // 释放端口
    }

    iqstats->canceledInst++;  // 更新取消统计
}

// 选择指令
// 从就绪队列中选择指令进行调度
void
IssueQue::selectInst()
{
    selectQ.clear();  // 清空选择队列
    // 遍历所有输出端口
    for (int pi = 0; pi < outports; pi++) {
        auto readyQ = readyQs[pi];
        selector->begin(readyQ);
        // 使用选择器选择指令
        for (auto it = selector->select(readyQ->begin(), pi); it != readyQ->end(); it = selector->select(it, pi)) {
            auto& inst = *it;
            // 如果指令被取消，从队列中移除
            if (inst->canceled()) {
                inst->clearInReadyQ();
                it = readyQ->erase(it);
                continue;
            }

            // 检查端口是否忙（根据操作延迟）
            if (!(portBusy[pi] &
                  (scheduler->getCorrectedOpLat(inst) > 63 ? 0 : 1llu << scheduler->getCorrectedOpLat(inst)))) {
                DPRINTF(Schedule, "[sn %ld] was selected\n", inst->seqNum);

                // 获取寄存器文件写端口
                for (int i = 0; i < inst->numDestRegs(); i++) {
                    auto pdst = inst->renamedDestIdx(i);
                    if (pdst->isFixedMapping()) [[unlikely]]
                        continue;
                    std::pair<int, int> rfTypePortId;
                    // 写端口与目标寄存器一一对应
                    if (pdst->isIntReg() && intWrRfTPI[pi].size() > i) {
                        rfTypePortId = intWrRfTPI[pi][i];
                        scheduler->useRfWrPort(inst, pdst, rfTypePortId.first, rfTypePortId.second);
                    }
                }


                // 获取寄存器文件读端口
                for (int i = 0; i < inst->numSrcRegs(); i++) {
                    PhysRegIdPtr psrc = inst->renamedSrcIdx(i);
                    if (psrc->isFixedMapping())
                        continue;
                    std::pair<int, int> rfTypePortId;
                    // 读端口与源寄存器一一对应
                    if (psrc->isIntReg() && intRdRfTPI[pi].size() > i) {
                        // TX动态端口优化：如果src0在寄存器缓存中，src1可以借用src0的端口
                        if (enableMainRdpOpt && i == 1 &&
                            scheduler->regCache.contains(inst->renamedSrcIdx(0)->flatIndex())) {
                            rfTypePortId = intRdRfTPI[pi][0]; // 借用src0的端口
                        } else {
                            rfTypePortId = intRdRfTPI[pi][i];
                        }
                        scheduler->useRfRdPort(inst, psrc, rfTypePortId.first, rfTypePortId.second);
                    } else if (psrc->isFloatReg() && fpRdRfTPI[pi].size() > i) {
                        rfTypePortId = fpRdRfTPI[pi][i];
                        scheduler->useRfRdPort(inst, psrc, rfTypePortId.first, rfTypePortId.second);
                    }
                }

                // 将指令加入选择队列
                selectQ.push_back(std::make_pair(pi, inst));
                inst->clearInReadyQ();
                readyQ->erase(it);
                break;
            } else {
                iqstats->portBusy[pi]++;  // 端口忙统计
            }

            it++;
        }
    }
}

// 调度指令
// 将选中的指令进行最终调度，处理仲裁结果
void
IssueQue::scheduleInst()
{
    // 这里是发射阶段0
    for (auto& info : selectQ) {
        auto& pi = info.first;    // 发射端口ID
        auto& inst = info.second; // 指令
        if (inst->canceled()) {
            DPRINTF(Schedule, "[sn:%llu] was canceled\n", inst->seqNum);
        } else if (inst->arbFailed()) {
            // 仲裁失败，重新加入就绪队列
            DPRINTF(Schedule, "[sn:%llu] arbitration failed, retry\n", inst->seqNum);
            iqstats->arbFailed++;
            assert(inst->readyToIssue());

            READYQ_PUSH(inst);
        } else [[likely]] {
            // 没有冲突，成功调度
            DPRINTF(Schedule, "[sn:%llu] no conflict, scheduled\n", inst->seqNum);
            iqstats->portissued[pi]++;  // 更新端口发射统计
            inst->setScheduled();
            toIssue->push(inst);        // 添加到发射流
            inst->issueportid = pi;

            // 设置端口忙状态
            if (!opPipelined[inst->opClass()]) {
                portBusy[pi] = -1ll;  // 非流水线操作，完全阻塞
            } else if (scheduler->getCorrectedOpLat(inst) > 1) {
                portBusy[pi] |= 1ll << scheduler->getCorrectedOpLat(inst);
            }

            // 推测性唤醒依赖指令
            scheduler->specWakeUpDependents(inst, this);
            // 更新性能计数器
            cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtIssueArb);
        }
        inst->clearArbFailed();
    }
}

// 发射队列的tick函数
// 每个周期调用，更新统计和调度指令
void
IssueQue::tick()
{
    // 更新平均指令数统计
    iqstats->avgInsts = instNum;

    // 更新插入分布统计
    if (instNumInsert > 0) {
        iqstats->insertDist[instNumInsert]++;
    }
    instNumInsert = 0;

    // 调度指令
    scheduleInst();
    // 推进飞行中的发射指令
    inflightIssues.advance();

    // 更新端口忙状态（右移一位）
    for (auto& t : portBusy) {
        t = t >> 1;
    }
}

// 检查发射队列是否准备好接受新指令
// 返回true表示可以插入新指令
bool
IssueQue::ready()
{
    bool bwFull = instNumInsert >= inports;  // 带宽已满
    bool full = (instNum >= iqsize) || (replayQ.size() > replayQsize); // 队列已满
    if (bwFull) {
        DPRINTF(Schedule, "can't insert more due to inports exhausted\n");
    }
    if (full) {
        DPRINTF(Schedule, "has full!\n");
    }
    return !full && !bwFull;
}

// 插入指令到发射队列
// 将新指令添加到发射队列并建立依赖关系
void
IssueQue::insert(const DynInstPtr& inst)
{
    assert(instNum < iqsize);
    opNum[inst->opClass()]++;  // 更新操作类型计数
    instNum++;                 // 更新总指令数
    instNumInsert++;           // 更新本周期插入数

    // 更新性能计数器
    cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtIssueQue);

    DPRINTF(Schedule, "[sn:%llu] %s insert into %s\n", inst->seqNum, enums::OpClassStrings[inst->opClass()], iqname);
    // 为指令分配选择器资源
    selector->allocate(inst);
    inst->issueQue = this;
    instList.emplace_back(inst);
    // 建立依赖关系
    bool addToDepGraph = false;
    for (int i = 0; i < inst->numSrcRegs(); i++) {
        auto src = inst->renamedSrcIdx(i);
        if (!inst->readySrcIdx(i) && !src->isFixedMapping()) {
            // 检查计分板
            if (scheduler->scoreboard[src->flatIndex()]) {
                inst->markSrcRegReady(i);
            } else {
                // 检查早期计分板
                if (scheduler->earlyScoreboard[src->flatIndex()]) {
                    inst->markSrcRegReady(i);
                }
                DPRINTF(Schedule, "[sn:%llu] src p%d add to depGraph\n", inst->seqNum, src->flatIndex());
                // 添加到依赖图
                subDepGraph[src->flatIndex()].push_back({i, inst});
                addToDepGraph = true;
            }
        }
    }

    if (!addToDepGraph) {
        assert(inst->readyToIssue());
    }


    /** 对于内存相关指令，使用内存依赖预测来决定是否可以乱序执行。
     * -- 通过依赖检查：指令可以被调度。
     * -- 依赖检查失败：在store地址计算完成后调度。
     */
    if (inst->isMemRef()) {
        // 插入并检查内存依赖
        scheduler->memDepUnit[inst->threadNumber].insert(inst);
    } else {
        addIfReady(inst);
    }
}

// 插入非推测指令
// 将非推测执行指令添加到队列
void
IssueQue::insertNonSpec(const DynInstPtr& inst)
{
    DPRINTF(Schedule, "[sn:%llu] insertNonSpec into %s\n", inst->seqNum, iqname);
    inst->issueQue = this;
    if (inst->isMemRef()) {
        // 如果是内存指令，插入到内存依赖单元
        scheduler->memDepUnit[inst->threadNumber].insertNonSpec(inst);
    }
}

// 执行提交操作
// 移除已提交的指令
void
IssueQue::doCommit(const InstSeqNum seqNum)
{
    // 移除所有序列号小于等于提交序列号的指令
    while (!instList.empty() && instList.front()->seqNum <= seqNum) {
        assert(instList.front()->isIssued());
        instList.pop_front();
    }
}

// 执行撤销操作
// 撤销所有序列号大于指定值的指令
void
IssueQue::doSquash(const InstSeqNum seqNum)
{
    // 遍历指令列表，撤销需要撤销的指令
    for (auto it = instList.begin(); it != instList.end();) {
        if ((*it)->seqNum > seqNum) {
            // 如果指令尚未发射，从队列中移除
            if (!(*it)->isIssued()) {
                POPINST((*it));
                (*it)->setIssued();
            }
            // 如果指令已调度且是非流水线操作，释放端口
            if ((*it)->isScheduled() && (*it)->issueportid >= 0 && !opPipelined[(*it)->opClass()]) {
                portBusy.at((*it)->issueportid) = 0;
            }

            // 设置指令状态
            (*it)->setSquashedInIQ();
            (*it)->setCanCommit();
            (*it)->clearScheduled();
            (*it)->setCancel();
            it = instList.erase(it);
            assert(instList.size() >= instNum);
        } else {
            it++;
        }
    }

    // 清理飞行中的被撤销指令
    for (int i = 0; i <= getIssueStages(); i++) {
        int size = inflightIssues[-i].size;
        for (int j = 0; j < size; j++) {
            auto& inst = inflightIssues[-i].insts[j];
            if (inst && inst->isSquashed()) {
                inst = nullptr;  // 清除被撤销的指令
            }
        }
    }

    // 清理依赖图中的被撤销指令
    for (auto& entrys : subDepGraph) {
        for (auto it = entrys.begin(); it != entrys.end();) {
            if ((*it).second->isSquashed()) {
                it = entrys.erase(it);  // 移除被撤销的指令
            } else {
                it++;
            }
        }
    }
}

// 推测唤醒完成事件构造函数
// 用于延迟唤醒依赖指令
Scheduler::SpecWakeupCompletion::SpecWakeupCompletion(const DynInstPtr& inst, IssueQue* to,
                                                      PendingWakeEventsType* owner)
    : Event(Stat_Event_Pri, AutoDelete), inst(inst), owner(owner), to_issue_queue(to)
{
}

// 处理推测唤醒完成事件
// 当延迟到达时，唤醒依赖指令
void
Scheduler::SpecWakeupCompletion::process()
{
    // 唤醒依赖指令
    to_issue_queue->wakeUpDependents(inst, true);
    // 从等待事件集合中移除
    (*owner)[inst->seqNum].erase(this);
}

// 返回推测唤醒完成事件的描述
const char*
Scheduler::SpecWakeupCompletion::description() const
{
    return "Spec wakeup completion";
}

// 调度器统计信息构造函数
// 初始化各种性能统计计数器
Scheduler::SchedulerStats::SchedulerStats(statistics::Group* parent)
    : statistics::Group(parent),
      ADD_STAT(exec_stall_cycle, "SUM(OpsExecuted[= FEW])"),
      ADD_STAT(memstall_any_load,
               "Cycles with no uops executed and at least X in-flight load that is not completed yet"),
      ADD_STAT(memstall_any_store, "Cycles with few uops executed and no more stores can be issued"),
      ADD_STAT(memstall_l1miss,
               "Cycles with no uops executed and at least X in-flight load that has missed the L1-cache"),
      ADD_STAT(memstall_l2miss,
               "Cycles with no uops executed and at least X in-flight load that has missed the L2-cache"),
      ADD_STAT(memstall_l3miss,
               "Cycles with no uops executed and at least X in-flight load that has missed the L3-cache")
{
}

// 调度器分发策略
// 优先选择指令数较少的发射队列
bool
Scheduler::disp_policy::operator()(IssueQue* a, IssueQue* b) const
{
    // 数量小的优先
    int p0 = a->opNum[disp_op];
    int p1 = b->opNum[disp_op];
    return p0 < p1;
}

// 调度器构造函数
// 初始化调度器的各个组件和参数
Scheduler::Scheduler(const SchedulerParams& params)
    : SimObject(params), old_disp(params.useOldDisp), stats(this), issueQues(params.IQs)
{
    // 初始化各种数据结构
    dispTable.resize(enums::OpClass::Num_OpClass);     // 分发表
    opExecTimeTable.resize(enums::OpClass::Num_OpClass, 1); // 操作执行时间表
    opPipelined.resize(enums::OpClass::Num_OpClass, false);  // 操作流水线标志

    // 初始化检查器和计数器
    boost::dynamic_bitset<> opChecker(enums::Num_OpClass, 0);  // 操作类型检查器
    std::vector<int> rdRfportChecker(MAXVAL_TYPEPORTID, 0);    // 读端口检查器
    std::vector<int> wrRfportChecker(MAXVAL_TYPEPORTID, 0);    // 写端口检查器
    int maxRdTypePortId = 0;
    int maxWrTypePortId = 0;
    // 初始化所有发射队列
    for (int i = 0; i < issueQues.size(); i++) {
        issueQues[i]->setIQID(i);
        issueQues[i]->scheduler = this;
        combinedFus += issueQues[i]->outports;  // 统计总功能单元数
        panic_if(issueQues[i]->fuDescs.size() == 0, "Empty config IssueQue: " + issueQues[i]->getName());
        // 遍历功能单元，建立操作类型到发射队列的映射
        for (auto fu : issueQues[i]->fuDescs) {
            for (auto op : fu->opDescList) {
                opExecTimeTable[op->opClass] = op->opLat;         // 设置执行时间
                opPipelined[op->opClass] = op->pipelined;         // 设置流水线标志
                dispTable[op->opClass].push_back(issueQues[i]);  // 添加到分发表
                opChecker.set(op->opClass);                      // 标记已配置
            }
        }

        // read port check
        for (auto& rfTypePortId : issueQues[i]->intRdRfTPI) {
            for (auto& typePortId : rfTypePortId) {
                maxRdTypePortId = std::max(maxRdTypePortId, typePortId.first);
                rdRfportChecker[typePortId.first] += 1;
            }
        }
        for (auto& rfTypePortId : issueQues[i]->fpRdRfTPI) {
            for (auto& typePortId : rfTypePortId) {
                maxRdTypePortId = std::max(maxRdTypePortId, typePortId.first);
                rdRfportChecker[typePortId.first] += 1;
            }
        }

        // write port check
        for (auto& rfTypePortId : issueQues[i]->intWrRfTPI) {
            for (auto& typePortId : rfTypePortId) {
                maxWrTypePortId = std::max(maxWrTypePortId, typePortId.first);
                wrRfportChecker[typePortId.first] += 1;
            }
        }
    }
    maxRdTypePortId += 1;
    maxWrTypePortId += 1;
    assert(maxRdTypePortId <= MAXVAL_TYPEPORTID);
    assert(maxWrTypePortId <= MAXVAL_TYPEPORTID);
    rdRfPortOccupancy.resize(maxRdTypePortId, {nullptr, 0});
    wrRfPortOccupancy.resize(maxWrTypePortId, {nullptr, 0, 0});

    // Set TX dynamic read port optimization for all IssueQues
    setMainRdpOpt(params.enableMainRdpOpt);

    if (opChecker.count() != enums::Num_OpClass) {
        for (int i = 0; i < enums::Num_OpClass; i++) {
            if (!opChecker[i]) {
                warn("No config for opClass: %s\n", enums::OpClassStrings[i]);
            }
        }
    }

    wakeMatrix.resize(issueQues.size());
    auto findIQbyname = [this](std::string name) -> IssueQue* {
        IssueQue* ret = nullptr;
        for (auto it : this->issueQues) {
            if (it->getName().compare(name) == 0) {
                if (ret) {
                    panic("has duplicate IQ name: %s\n", name);
                }
                ret = it;
            }
        }
        warn_if(!ret, "can't find IQ by name: %s\n", name);
        return ret;
    };
    if (params.xbarWakeup) {
        for (auto srcIQ : issueQues) {
            for (auto dstIQ : issueQues) {
                wakeMatrix[srcIQ->getId()].push_back(dstIQ);
                DPRINTF(Schedule, "build wakeup channel: %s -> %s\n", srcIQ->getName(), dstIQ->getName());
            }
        }
    } else {
        for (auto it : params.specWakeupNetwork) {
            for (auto src : it->srcIQs) {
                auto srcIQ = findIQbyname(src);
                if (srcIQ) {
                    for (auto dstIQname : it->dstIQs) {
                        auto dstIQ = findIQbyname(dstIQname);
                        if (dstIQ) {
                            wakeMatrix[srcIQ->getId()].push_back(dstIQ);
                            DPRINTF(Schedule, "build wakeup channel: %s -> %s\n", srcIQ->getName(), dstIQ->getName());
                        }
                    }
                }
            }
        }
    }

    assert(dispTable[MemWriteOp].size() == dispTable[StoreDataOp].size());

    dispSeqVec.resize(64);
}

// 设置CPU和LSQ指针
// 为调度器配置所需的CPU和加载/存储队列引用
void
Scheduler::setCPU(CPU* cpu, LSQ* lsq)
{
    this->cpu = cpu;
    this->lsq = lsq;
    for (auto it : issueQues) {
        it->setCPU(cpu);
    }
}

// 重置依赖图
// 调整计分板大小并初始化所有物理寄存器为就绪状态
void
Scheduler::resetDepGraph(uint64_t numPhysRegs)
{
    scoreboard.resize(numPhysRegs, true);
    bypassScoreboard.resize(numPhysRegs, true);
    earlyScoreboard.resize(numPhysRegs, true);
    for (auto it : issueQues) {
        it->resetDepGraph(numPhysRegs);
    }
}

// 将指令添加到功能单元
// 指令从发射队列发射到功能单元进行执行
void
Scheduler::addToFU(const DynInstPtr& inst)
{
#if TRACING_ON
    inst->issueTick = curTick() - inst->fetchTick;
#endif
    inst->clearCancel();
    DPRINTF(Schedule, "%s [sn:%llu] add to FUs\n", enums::OpClassStrings[inst->opClass()], inst->seqNum);
    instsToFu.push_back(inst);
}

// 调度器时钟周期处理
// 每个周期更新端口占用状态并处理发射队列操作
void
Scheduler::tick()
{
    // we need to update portBusy counter each cycle
    cpu->activateStage(CPU::IEWIdx);
    for (auto it : issueQues) {
        it->tick();
    }
}

// 发射和选择指令
// 先完成所有指令的发射，再进行下一轮的选择操作
void
Scheduler::issueAndSelect()
{
    // must wait for all insts was issued
    for (auto it : issueQues) {
        it->selectInst();
    }

    std::fill(rdRfPortOccupancy.begin(), rdRfPortOccupancy.end(), std::make_pair(nullptr, 0));
    std::fill(wrRfPortOccupancy.begin(), wrRfPortOccupancy.end(), std::make_tuple(nullptr, 0, 0));

    for (auto it : issueQues) {
        it->issueToFu();
    }
    if (instsToFu.size() < intel_fewops) {
        stats.exec_stall_cycle++;
        if (lsq->anyStoreNotExecute())
            stats.memstall_any_store++;
    }
    if (instsToFu.size() == 0) {
        int misslevel = lsq->anyInflightLoadsNotComplete();
        if (misslevel != 0)
            stats.memstall_any_load++;
        if ((misslevel & ((1 << 1) - 1)) == ((1 << 1) - 1))
            stats.memstall_l1miss++;
        if ((misslevel & ((1 << 2) - 1)) == ((1 << 2) - 1))
            stats.memstall_l2miss++;
        if ((misslevel & ((1 << 3) - 1)) == ((1 << 3) - 1))
            stats.memstall_l3miss++;
    }
}

// 前瞻性调度检查
// 检查指定的指令队列是否可以被调度
void
Scheduler::lookahead(std::deque<DynInstPtr>& insts)
{
    if (old_disp) {
        // donothing
    } else {
        uint8_t disp_op_num[Num_OpClasses];
        std::memset(disp_op_num, 0, Num_OpClasses);
        int i = 0;
        for (auto& it : insts) {
            auto& iqs = dispTable[it->opClass()];
            std::sort(iqs.begin(), iqs.end(), disp_policy(it->opClass()));
            if (it->isSplitStoreAddr()) {
                auto& iqs = dispTable[StoreDataOp];
                std::sort(iqs.begin(), iqs.end(), disp_policy(StoreDataOp));
            }

            dispSeqVec[i] = disp_op_num[it->opClass()] % dispTable[it->opClass()].size();
            disp_op_num[it->opClass()]++;
            i++;
        }
    }
}

// 检查指令是否准备就绪
// 判断指令是否可以被分发到发射队列
bool
Scheduler::ready(const DynInstPtr& inst, int disp_seq)
{
    if (inst->staticInst->isSplitStoreAddr() && !ready(StoreDataOp, disp_seq)) {
        return false;
    }

    auto& iqs = dispTable[inst->opClass()];
    assert(!iqs.empty());

    if (old_disp) [[unlikely]] {
        for (auto iq : iqs) {
            if (iq->ready()) {
                return true;
            }
        }
    } else {
        if (iqs[dispSeqVec.at(disp_seq)]->ready()) {
            return true;
        }
    }

    DPRINTF(Schedule, "IQ not ready, opclass: %s\n", enums::OpClassStrings[inst->opClass()]);
    return false;
}

// 检查特定操作类型是否准备就绪
// 判断指定操作类型的发射队列是否可以接受新指令
bool
Scheduler::ready(OpClass op, int disp_seq)
{
    auto& iqs = dispTable[op];
    assert(!iqs.empty());

    if (old_disp) {
        for (auto iq : iqs) {
            if (iq->ready()) {
                return true;
            }
        }
    } else {
        if (iqs[dispSeqVec.at(disp_seq)]->ready()) {
            return true;
        }
    }

    DPRINTF(Schedule, "IQ not ready, opclass: %s\n", enums::OpClassStrings[op]);
    return false;
}

// 根据目标寄存器索引获取指令
// 查找产生指定物理寄存器的指令
DynInstPtr
Scheduler::getInstByDstReg(RegIndex flatIdx)
{
    for (auto iq : issueQues) {
        for (auto& inst : iq->instList) {
            if (inst->numDestRegs() > 0 && inst->renamedDestIdx(0)->flatIndex() == flatIdx) {
                return inst;
            }
        }
    }
    return nullptr;
}

// 添加生产者指令
// 将指令标记为数据生产者，更新计分板
void
Scheduler::addProducer(const DynInstPtr& inst)
{
    DPRINTF(Schedule, "[sn:%llu] addProdecer\n", inst->seqNum);
    // 遍历所有目标寄存器
    for (int i = 0; i < inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping()) {
            continue;
        }
        // 将目标寄存器标记为不就绪
        scoreboard[dst->flatIndex()] = false;       // 正常计分板
        bypassScoreboard[dst->flatIndex()] = false; // 旁路计分板
        earlyScoreboard[dst->flatIndex()] = false;  // 早期计分板
        DPRINTF(Schedule, "mark scoreboard p%lu not ready\n", dst->flatIndex());
    }
}

void
Scheduler::insert(const DynInstPtr& inst, int disp_seq)
{
    if (inst->isSplitStoreAddr()) {
        auto stduop = inst->createStoreDataUop();
        this->insert(stduop, disp_seq);
        // transform self to storeAddruop
        inst->buildStoreAddrUop();
    }

    auto& iqs = dispTable[inst->opClass()];

    if (old_disp) {
        bool insert = false;
        std::sort(iqs.begin(), iqs.end(), disp_policy(inst->opClass()));
        for (auto iq : iqs) {
            if (iq->ready()) {
                insert = true;
                iq->insert(inst);
                break;
            }
        }
        panic_if(!insert, "can't find ready IQ to insert");
    } else {
        assert(iqs[dispSeqVec.at(disp_seq)]->ready());
        iqs[dispSeqVec.at(disp_seq)]->insert(inst);
    }

    DPRINTF(Schedule, "[sn:%llu] dispatch: %s\n", inst->seqNum, inst->staticInst->disassemble(0));
}

void
Scheduler::insertNonSpec(const DynInstPtr& inst)
{
    auto& iqs = dispTable[inst->opClass()];

    for (auto iq : iqs) {
        if (iq->ready()) {
            iq->insertNonSpec(inst);
            break;
        }
    }
}

void
Scheduler::specWakeUpDependents(const DynInstPtr& inst, IssueQue* from_issue_queue)
{
    if (!opPipelined[inst->opClass()] || inst->numDestRegs() == 0 || inst->isLoad()) {
        return;
    }

    for (auto to : wakeMatrix[from_issue_queue->getId()]) {
        int oplat = getCorrectedOpLat(inst);
        int wakeDelay = oplat - 1;
        assert(oplat < 64);
        int diff = std::abs(from_issue_queue->getIssueStages() - to->getIssueStages());
        if (from_issue_queue->getIssueStages() > to->getIssueStages()) {
            wakeDelay += diff;
        } else if (wakeDelay >= diff) {
            wakeDelay -= diff;
        }

        DPRINTF(Schedule, "[sn:%llu] %s create wakeupEvent to %s, delay %d cycles\n", inst->seqNum,
                from_issue_queue->getName(), to->getName(), wakeDelay);
        if (wakeDelay == 0) {
            to->wakeUpDependents(inst, true);
            if (!(inst->isFloating() || inst->isVector())) {
                for (int i = 0; i < inst->numDestRegs(); i++) {
                    PhysRegIdPtr dst = inst->renamedDestIdx(i);
                    if (dst->isFixedMapping()) [[unlikely]] {
                        continue;
                    }
                    earlyScoreboard[dst->flatIndex()] = true;
                }
            }
        } else {
            auto wakeEvent = new SpecWakeupCompletion(inst, to, &specWakeEvents);
            // track these pending events
            specWakeEvents[inst->seqNum].insert(wakeEvent);
            cpu->schedule(wakeEvent, cpu->clockEdge(Cycles(wakeDelay)) - 1);
        }
    }
}

void
Scheduler::specWakeUpFromLoadPipe(const DynInstPtr& inst)
{
    assert(inst->isLoad());
    auto from_issue_queue = inst->issueQue;
    for (auto to : wakeMatrix[from_issue_queue->getId()]) {

        DPRINTF(Schedule, "[sn:%llu] %s create wakeupEvent to %s at loadpipe, no delay\n", inst->seqNum,
                from_issue_queue->getName(), to->getName());
        to->wakeUpDependents(inst, true);

        for (int i = 0; i < inst->numDestRegs(); i++) {
            PhysRegIdPtr dst = inst->renamedDestIdx(i);
            if (dst->isFixedMapping()) [[unlikely]] {
                continue;
            }
            earlyScoreboard[dst->flatIndex()] = true;
        }
    }
}

DynInstPtr
Scheduler::getInstToFU()
{
    if (instsToFu.empty()) {
        return DynInstPtr(nullptr);
    }
    auto ret = instsToFu.back();
    instsToFu.pop_back();
    return ret;
}

// 检查寄存器文件端口是否忙碌
// 检查指定类型端口是否被更高优先级的指令占用
bool
Scheduler::checkRfPortBusy(int typePortId, int pri)
{
    if (rdRfPortOccupancy[typePortId].first && rdRfPortOccupancy[typePortId].second > pri) {
        return false;
    }
    return true;
}

// 使用寄存器文件读端口
// 为指令分配寄存器文件读端口，处理端口仲裁
void
Scheduler::useRfRdPort(const DynInstPtr& inst, const PhysRegIdPtr& regid, int typePortId, int pri)
{
    if (regid->is(IntRegClass)) {
        if (regCache.contains(regid->flatIndex())) {
            regCache.get(regid->flatIndex());
            return;
        }
    }
    assert(typePortId < rdRfPortOccupancy.size());
    auto& t_inst = rdRfPortOccupancy[typePortId].first;
    auto& t_pri = rdRfPortOccupancy[typePortId].second;

    if (t_inst) {
        if (t_pri < pri) {  // smaller is higher priority
            // inst arbitration failure
            inst->setArbFailed();
            DPRINTF(Schedule, "[sn:%llu] arbitration failure, typePortId %d occupied by [sn:%llu]\n", inst->seqNum,
                    typePortId, t_inst->seqNum);
            return;
        } else {
            // t_inst arbitration failure
            t_inst->setArbFailed();
            DPRINTF(Schedule, "[sn:%llu] arbitration failure, typePortId %d occupied by [sn:%llu]\n", t_inst->seqNum,
                    typePortId, inst->seqNum);
        }
    }

    t_inst = inst;
    t_pri = pri;
}

// 使用寄存器文件写端口
// 为指令分配寄存器文件写端口，处理端口仲裁和延迟
void
Scheduler::useRfWrPort(const DynInstPtr& inst, const PhysRegIdPtr& regid, int typePortId, int pri)
{
    assert(typePortId < wrRfPortOccupancy.size());

    auto& t_inst = std::get<0>(wrRfPortOccupancy[typePortId]);
    auto& t_pri = std::get<1>(wrRfPortOccupancy[typePortId]);
    auto& t_lat = std::get<2>(wrRfPortOccupancy[typePortId]);
    int lat = getCorrectedOpLat(inst);

    if (t_inst) {
        if ((t_lat == lat) && (t_pri < pri)) {  // smaller is higher priority
            // inst arbitration failure
            inst->setArbFailed();
            DPRINTF(Schedule, "[sn:%llu] arbitration failure, typePortId %d occupied by [sn:%llu]\n", inst->seqNum,
                    typePortId, t_inst->seqNum);
            return;
        } else {
            // t_inst arbitration failure
            t_inst->setArbFailed();
            DPRINTF(Schedule, "[sn:%llu] arbitration failure, typePortId %d occupied by [sn:%llu]\n", t_inst->seqNum,
                    typePortId, inst->seqNum);
        }
    }

    t_inst = inst;
    t_pri = pri;
    t_lat = lat;
}

// 加载指令取消
// 当加载指令发生缓存缺失时，取消其所有消费者指令
void
Scheduler::loadCancel(const DynInstPtr& inst)
{
    DPRINTF(Schedule, "[sn:%llu] %s cache miss, cancel consumers\n", inst->seqNum,
            enums::OpClassStrings[inst->opClass()]);
    if (inst->issueQue) {
        inst->issueQue->iqstats->loadmiss++;
    }

    dfs.push(inst);
    while (!dfs.empty()) {
        auto top = dfs.top();
        dfs.pop();
        // clear pending wake events scheduled by top
        auto& pendingEvents = specWakeEvents[top->seqNum];
        for (auto it = pendingEvents.begin(); it != pendingEvents.end(); it++) {
            cpu->deschedule(*it);
        }
        specWakeEvents.erase(top->seqNum);
        for (int i = 0; i < top->numDestRegs(); i++) {
            auto dst = top->renamedDestIdx(i);
            if (dst->isFixedMapping()) {
                continue;
            }
            earlyScoreboard[dst->flatIndex()] = false;
            for (auto iq : issueQues) {
                for (auto& it : iq->subDepGraph[dst->flatIndex()]) {
                    int srcIdx = it.first;
                    auto& depInst = it.second;
                    if (depInst->readySrcIdx(srcIdx)) {
                        DPRINTF(Schedule, "cancel [sn:%llu], clear src p%d ready\n", depInst->seqNum,
                                depInst->renamedSrcIdx(srcIdx)->flatIndex());
                        depInst->issueQue->cancel(depInst);
                        depInst->clearSrcRegReady(srcIdx);
                        dfs.push(depInst);
                    }
                }
            }
        }
    }

    for (auto iq : issueQues) {
        for (int i = 0; i <= iq->getIssueStages(); i++) {
            int size = iq->inflightIssues[-i].size;
            for (int j = 0; j < size; j++) {
                auto& inst = iq->inflightIssues[-i].insts[j];
                if (inst && inst->canceled()) {
                    inst = nullptr;
                }
            }
        }
    }
}

// 写回唤醒
// 当指令写回时，更新计分板并唤醒依赖指令
void
Scheduler::writebackWakeup(const DynInstPtr& inst)
{
    DPRINTF(Schedule, "[sn:%llu] was writeback\n", inst->seqNum);
    inst->setWriteback();  // 在发射队列中清除
    // 更新性能计数器
    cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtWriteVal);
    // 更新计分板
    for (int i = 0; i < inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping()) {
            continue;
        }
        scoreboard[dst->flatIndex()] = true;  // 标记数据就绪
    }
    // 唤醒所有发射队列中的依赖指令
    for (auto it : issueQues) {
        it->wakeUpDependents(inst, false);
    }
}

// 旁路写回
// 当指令可以通过旁路网络提供数据时调用
void
Scheduler::bypassWriteback(const DynInstPtr& inst)
{
    // 如果是非流水线操作，释放端口
    if (!opPipelined[inst->opClass()] && inst->issueportid >= 0) {
        inst->issueQue->portBusy[inst->issueportid] = 0;
    }
    // 更新性能计数器
    cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtBypassVal);
    DPRINTF(Schedule, "[sn:%llu] bypass write\n", inst->seqNum);
    // 更新旁路计分板
    for (int i = 0; i < inst->numDestRegs(); i++) {
        auto dst = inst->renamedDestIdx(i);
        if (dst->isFixedMapping()) {
            continue;
        }
        bypassScoreboard[dst->flatIndex()] = true;  // 标记旁路数据就绪
        DPRINTF(Schedule, "p%lu in bypassNetwork ready\n", dst->flatIndex());
    }
}

// 获取操作延迟
// 返回指定指令的执行延迟周期数
uint32_t
Scheduler::getOpLatency(const DynInstPtr& inst)
{
    if (inst->opClass() == FloatCvtOp) [[unlikely]] {
        if (inst->destRegIdx(0).isFloatReg()) {
            return 2 + opExecTimeTable[inst->opClass()];
        }
    }
    return opExecTimeTable[inst->opClass()];
}

// 获取修正后的操作延迟
// 返回考虑各种因素后的最终操作延迟
uint32_t
Scheduler::getCorrectedOpLat(const DynInstPtr& inst)
{
    uint32_t oplat = getOpLatency(inst);
    return oplat;
}

// 检查是否有准备就绪的指令
// 遍历所有发射队列，查看是否有指令可以发射
bool
Scheduler::hasReadyInsts()
{
    for (auto it : issueQues) {
        if (!it->idle()) {
            return true;
        }
    }
    return false;
}

// 检查调度器是否已清空
// 判断所有发射队列是否都没有待处理的指令
bool
Scheduler::isDrained()
{
    for (auto it : issueQues) {
        if (!it->instList.empty()) {
            return false;
        }
    }
    return true;
}

// 执行提交操作
// 通知所有发射队列处理指令提交
void
Scheduler::doCommit(const InstSeqNum seqNum)
{
    for (auto it : issueQues) {
        it->doCommit(seqNum);
    }
}

// 执行撤销操作
// 通知所有发射队列执行撤销
void
Scheduler::doSquash(const InstSeqNum seqNum)
{
    DPRINTF(Schedule, "doSquash until seqNum %lu\n", seqNum);
    // 通知所有发射队列执行撤销
    for (auto it : issueQues) {
        it->doSquash(seqNum);
    }
}

// 获取发射队列中的指令总数
// 统计所有发射队列中的指令数量
uint32_t
Scheduler::getIQInsts()
{
    uint32_t total = 0;
    for (auto iq : issueQues) {
        total += iq->instNum;
    }
    return total;
}

// 设置主要就绪检测优化
// 启用或禁用发射队列的主要就绪检测优化
void
Scheduler::setMainRdpOpt(bool enable)
{
    for (auto iq : issueQues) {
        iq->setMainRdpOpt(enable);
    }
}

}
}

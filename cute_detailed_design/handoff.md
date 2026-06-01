# Handoff

## 文档职责

- 这份文件只负责 **交接**。
- 它只写：
  - 当前停在哪个阶段
  - 当前 checkpoint 是什么
  - 哪些结论已成立
  - 剩余风险与下一步建议
- 它**不负责**：
  - 承担长期计划
  - 详细解释核内设计
  - 详细解释 simplified CUTE 接入设计
  - 展开完整验证日志

## 相关文档边界

- `Plan.md`
  - 负责计划与阶段推进
- `cute_detailed_design/matrix_core_design.md`
  - 负责核内设计
- `cute_detailed_design/matrix_diff_vs_rtl.md`
  - 负责当前未对齐项
- `implementation_notes.md`
  - 负责本轮改动摘要
- `verify_report.md`
  - 负责验证与证据

## 当前阶段
- Phase 4 `4.1/4.3/4.4/4.5`
- 当前收口阶段：`4.1/4.3/4.5` 已完成，`4.4` 以 backend/unit checkpoint 形式收口；`4.6` 仍待完成
- 历史的 Phase 2A / Stage E 交接记录保留在下方

## 当前停点
- 已完成：
  - `4.1` 主线 workload / datatype 边界冻结
  - `4.3` `MatrixElemType` 扩展与 datatype tag 透传
  - `4.4` FP16 backend/unit compute checkpoint
  - 当前 int8/int16/int32 详细 backend 单测与 `gem5.opt` 编译
  - `4.5` `gemm_precomp` 已作为 `0x2b` 主口径 workload 跑通 SE smoke 回归
- 仍待完成：
  - active ISA/decode 路径上的 FP MMA workload 闭环
  - BF16/TF32 compute 正确性闭环
  - `4.6` 默认主线路径上的最终回归收口

## 当前 FP 边界
- 已支持：
  - FP16/BF16/TF32 的 datatype tag 透传
  - LSU / memory adapter 按 raw bits 处理 FP16/BF16/TF32
  - request 可以带着 FP datatype 一路传到 backend
  - FP16 MMA 在 detailed backend 单测路径上的结果正确执行
- 仍不支持：
  - active ISA/decode 路径上的 FP MMA workload 闭环
  - BF16/TF32 compute 的结果正确执行

## 当前 checkpoint
- 阶段 D fix-up:
  - `4e6efb0095`
  - `cpu-o3: fix matrix mls replay readiness contract`
- 阶段 E:
  - `f40735a9a2`
  - `cpu-o3: close matrix mls commit proxy boundary`
- 阶段 E docs:
  - `2b1ea9e393`
  - `cpu-o3: record matrix mls stage-e checkpoint`

## 2026-05-13 Phase 4 baseline checkpoint
- 这轮没有形成新的 git commit checkpoint，但已经把 `4.1/4.3/4.5` 的文档边界和 datatype 传递基础收口：
  - [Plans.md](/nfs/home/hujun/GEM5/Plans.md) 已改成 `4.1-4.6`
  - [src/matrix/matrix_types.hh](/nfs/home/hujun/GEM5/src/matrix/matrix_types.hh) 已扩展 FP16/BF16/TF32 tag
  - [src/cpu/o3/rob.cc](/nfs/home/hujun/GEM5/src/cpu/o3/rob.cc) 与 [src/matrix/matrix_memory_adapter_gem5.cc](/nfs/home/hujun/GEM5/src/matrix/matrix_memory_adapter_gem5.cc) 已打通 tag 传递
- 当前状态：
  - 这个 baseline checkpoint 里，`4.1/4.3/4.5` 已经在当时的 task review 后收口为 `cc:完成`
  - 当时 `4.4/4.6` 继续保持待完成
- 当前建议：
  - 当时的下一步是围绕 `gemm_precomp` 继续收主线 datatype / FP16 边界
  - 当时的边界是保持 FP request 在 compute 未闭环前显式 `Unsupported`

## 2026-05-13 Phase 4 FP16 compute / final regression update
- 这轮新增收口：
  1. `matrix_ex.cc` 已支持 `FP16 x FP16 -> raw FP32 accumulator` 的最小 backend compute
  2. `DetailedCuteBackend.Fp16MmaSucceedsWithInt32Accumulator` 已用 CUTE `cute-fpe` 软件 oracle 固定结果
  3. `detailed_cute_backend.test.opt` 全量通过
  4. `gem5.opt` 编译通过
  5. `gemm_precomp` 最终回归继续 `All 8 precomp tests PASSED.`
- 当前必须保留的边界：
  - 这次 `4.4` 是 backend/unit 级闭环，不是 active ISA/decode FP workload 已可达
  - BF16/TF32 compute 仍未闭环
  - 因此 `4.6` 不能按“默认 RTL 主线最终回归完成”来收口

## 2026-05-13 Phase 4A review update
- 已根据代码级差异重新收紧主线时序/建模对齐口径：
  - 不再使用“GEM5 没有 FIFO/没有 compute 分阶段”这种过时表述
  - compute boundary 仍然优先，但 memory/cache/resource 建模已前置或并行；这一步先收简单带宽/时序，不先做细 cache 行为
- 当前 review 后的关键 blocking 差异：
  1. GEM5 `computePathReady()` 曾只看 `ADC`；2026-05-15 已收紧为 `ADC/BDC/CDC` all-ready，但 `ComputeGo / MicroTaskEndValid/Ready` 仍是事件式近似
  2. GEM5 memory path 还是功能访问，没有 `LocalMMU/sourceId/fill-table/FIFO` 资源模型
  3. GEM5 `mregfile` 还没有 `AML/BML/CML/ADC/BDC/CDC` 级别的读写仲裁、每拍带宽和 bank 并行度模型
  4. GEM5 MLS replay/cancel 主要还是 `tlbMiss` 粒度，没有 RTL `cause/carry/safe-writeback` 粒度
  5. `MatrixAmuBuffer` 仍是单线程 shadow carrier，当前阶段不支撑 CommitWidth/SMT/global-oldest timing 结论

## 2026-05-09 `3.16` 当前停点
- 这轮 active 代码只新增了一条 targeted regression：
  - [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc)
  - `DetailedCuteBackend.SecondComputeWaitsOnAdcAvailabilityNotScoreboardComputeBusy`
- 它固定了当前 `3.16` 的最小 issue 边界：
  - 第二条 `mma` 在第一条占用 `ADC` 时，会被 `fifo/downstream` 挡住
  - 它不是被 monolithic compute scoreboard busy 挡住
  - 等第一条离开 `ADC` 后，第二条 `mma` 可以进入前级子单元

## 2026-05-11 `3.18` release gating follow-up
- 这轮新增了一条 release gating regression：
  - `DetailedCuteBackend.ReleaseWaitsForComputeTerminalCompletion`
- 它固定了当前 release 语义的最小闭环：
  - `pendingStoreCount == 0` 还不够
  - 前序 backend drain 还要清空
  - 前序 compute 的 terminal completion 被消费后，release 才能从 FIFO 发射
- release 自己的 token release 仍然只在最终 completion 时可见

## 2026-05-09 语义结论
- 当前建议固定成三层：
  - 入口层：`commit-visible request + FIFO`
  - 控制层：`scoreboard + TaskController-style issue`
  - 执行层：`AML/BML/CML/ADC/BDC/CDC/MTE` 独立微状态机
- `scoreboard`
  - GEM5 当前承担 issue 依赖检查、pending reader 和记账式 staged release
  - 还不是 `ADC/BDC/CDC/MTE` 的完整子单元调度器
  - 但已经应该按发射判定第一公民来理解，而不是普通辅助对象
- `regfile owner`
  - GEM5 当前用它表达“哪个 microtask 仍拥有目标寄存器写回”
  - RTL 上最接近的是 `Scoreboard` 里的 writer / waitFu 语义，不应把它理解成独立 owner 模块
- `pendingStoreCount`
  - 仍是 release gating 的最低约束
  - GEM5 当前还额外要求 backend drain，这比 `TaskController.scala` 里的 `releaseReady = !pendingStore` 更保守
- compute dispatch
  - RTL/CUTE 是 `TaskController -> ADC/BDC/CDC config + MTE.ComputeGo`
  - GEM5 当前用 `computeTasks + ADC/BDC/MTE/CDC` 子单元占用去近似这个流程，重点先对齐 task ownership、占用和完成边界
  - 下一步继续收 `ComputeGo / EndReady` 时，应按“ready 进入、end-valid 完成、确认后释放”的边界来做

## 2026-05-09 review fix
- 本轮已经把 compute 终段从“本地 CDC 写完就退场”收紧为：
  - `CDC` local finish
  - `WriteFinish` event
  - `TerminalCompletion` consume
  - 之后才 retire compute task
- 这意味着 `MTE` 结果准备好与 `CDC/write finish`、`completion`、`resource release` 之间的边界更清楚了
- 另补了一条 regression：
  - `ComputeKeepsCDestBusyUntilWriteFinishAndCompletion`
  - 证明 `MTE` 后 `C dest` 仍 busy，直到 terminal completion 真被消费

## 2026-05-09 store lifecycle follow-up
- 追加了一个 store 边界回归：
  - `StoreSnapshotsRegisterDataAtReadFinish`
  - 证明 `ReadFinish` 之后 `hasCompletion()` 仍为 false
  - `WriteFinish + TerminalCompletion` 才是真正退场点

## 2026-05-09 taskslot terminal boundary follow-up
- 这轮把 `AML/BML/CML/release` 的 `TaskSlot` 退场点也统一到了 terminal completion：
  - `finishTaskSlot()` 后先进入 `TerminalPending`
  - `TerminalCompletion` consume 后才 retire slot
- 这样当前 active backend 的 task 退场规则更统一：
  - compute 与 load/store/zero/release 都不再在本地 finish 点立刻退场

## 2026-05-09 3.16 completion checkpoint
- `Plan.md` 中 `3.16` 已标记为 `cc:完成`
- 当前结论：
  - 3.16 的收口已经完成
  - 仍保留后续继续细化 `ComputeGo / EndReady` 与独立实体化的空间，但不再影响本 task 的完成判定

## 2026-05-11 3.18 review gate
- 当前状态：
  - 3.18 的 release gating 回归已补
  - task-level / phase-level / code style review 均已通过
  - `Plan.md` 中 `3.18` 已更新为 `cc:完成`

## 本轮补的证据
- clean path:
  - `/tmp/gem5-matrix-commit-e2/matrix_lsu_commit_e2.log`
- fault path:
  - `/tmp/gem5-matrix-fault-e1/matrix_lsu_fault_e1.log`
- cancel / squash path:
  - `/tmp/gem5-matrix-cancel-e1/matrix_lsu_cancel_e1.log`
- `mrelease / macquire` 顺序:
  - `/tmp/gem5-matrix-release-e1/matrix_release_acquire_e1.log`
- `MLS replay` 命中:
  - `/tmp/gem5-matrix-mls-replay-st/matrix_mls_replay.log`
- `squash + shadow window` 命中:
  - `/tmp/gem5-matrix-shadow-squash-st/matrix_shadow_squash.log`
- `AMU full/backpressure` 命中（capacity=1）:
  - `/tmp/gem5-matrix-amu-full-st/matrix_amu_full.log`
- `AMU full + deq` 命中（capacity=6）:
  - `/tmp/gem5-matrix-amu-cap6-st/matrix_amu_cap6.log`
- 新增 compute terminal boundary regression：
  - `build/RISCV/matrix/detailed_cute_backend.test.opt`

## 关键结论
- `mls` clean path 上：
  - payload staged
  - ROB writeback
  - ROB committed
  - `toAMU proxy ready/fire`
  - backend-visible
  顺序已经成立
- `mls` fault path 上：
  - `Matrix AMU entry writeback suppressed`
  - 没有 `toAMU proxy fire`
  - 没有 backend-visible
- `mls` cancel path 上：
  - `Matrix AMU entry squash`
  - 没有 `toAMU proxy fire`
  - 没有 backend-visible
- `mrelease / macquire` 上：
  - `mrelease` 先 commit、更新 shadow token、再 `toAMU proxy fire`
  - `macquire` 之后才 execute/commit

## 当前状态判断
- 复审 reviewer 已返回：
  - `Verdict: single-thread 前提下可继续收证，但不能宣称 Phase 2A 已收口`
- 当前实验前提：
  - 只覆盖 `single-thread`
  - 不覆盖 `SMT`
- 当前新增 artifact 解除的问题：
  1. `MLS replay` 新路径已经被真实命中
  2. `squash + shadow window` 已有单线程证据
  3. `AMU full/backpressure/deq` 已有单线程证据
- 当前仍未解除的问题：
  1. `MLS` 仍未对齐 RTL `s2/s3` 中的 PMP/MMIO/rep_info/feedback/non-tlbMiss replay cause
  2. `AMU shadow buffer` 仍是阶段性近似，不应写成“已结构对齐 RTL”
  3. 少量旧设计表述曾保留 `buffered` 近似口径；当前主文档已基本收口，但后续仍要避免回退到旧说法

## 用户已有改动
- 继续保留，不纳入本轮提交：
- `Plan.md`
  - `mls_cpu_path_design.md`

## 剩余风险
- fault probe 虽然成功给出 suppress/no-backend-visible 证据，但程序最终以 illegal instruction panic 结束，说明这条更像 fault artifact，而不是完整 workload。
- 地址 fault / LSQ fault 仍未单独覆盖。
- 非 O3 fallback 路径仍未收口。

## 下一步建议
1. 进入 `3.19`，重点收 `macquire` 的阻塞与 re-exec，不要把 release gating 与 acquire 语义混做一团
2. 在所有阶段判断里继续坚持当前实验前提：
   - 只覆盖 `single-thread`
   - 不覆盖 `SMT`
3. 在补完更细 RTL 证据前，不要把当前状态写成“RTL 级 release gating 已对齐”。

---

## 2026-04-24 Detailed CUTE Track

### 当前阶段
- `Plan.md`
- Phase 2 `2.1/2.3/2.4` 进入 WIP

### 当前改动
- 已引入 `MatrixBackend` 抽象
- `ROB` 新增 `peekReadyMatrixAmuEntry()`
- `Commit` 现在先检查 backend accept，再 dequeue `MatrixAmuEntry`

### 当前验证
- `build/RISCV/arch/riscv/matrix_sync_check.opt`
  - `matrix_sync_check: PASS`

### 当前结论
- backend interface 抽象已成立
- commit 不会在 backend 不可接收时提前消费 AMU entry
- 这一步为后续 detailed FIFO/scoreboard 铺好了最小 contract
- `MatrixCuteTrace` 已经在 O3 matrix LSU clean path 上跑通
- `matrixBackendQueueEntries` 已经从 config 层可配
- `Release` 已经收回统一 backend contract，不再由 commit 侧直接更新 token
- drain / serialize 已经对 pending backend state fail-fast
- `MatrixAmuBuffer` 现在采用 `alloc-time ROB-parallel shadow` 近似
- `MatrixAmuBuffer` 已经独立到：
  - `src/cpu/o3/matrix_amu_buffer.hh`
  - `src/cpu/o3/matrix_amu_buffer.cc`

### 当前阶段判断
- `Plan.md` 中 detailed backend 相关阶段不能直接视为完成；至少还需完成：
  - single-thread 定向验证结果收敛
  - 设计文档剩余旧口径清理
  - 当前实验前提冻结

### 下一步建议
1. 进入 `Plan.md` 的 detailed backend 下一小步：
   - 提炼 CUTE scoreboard 行为并冻结最小 stall reason 集合

## 2026-04-28 Detailed CUTE Phase 3A shell

## 当前停点
- `DetailedCuteBackend` 最小壳已经落地，但仍是 **Phase 3A/3B shell**，不是 Phase 4 timing model
- 新增 backend 选择参数：
  - `BaseO3CPU.matrixBackendMode`
  - `configs/example/matrix_seq_o3.py --matrix-backend-mode`
- 显式选择 `detailed-cute` 时，backend 内部结构为：
  - `DecodedFifo`
  - `MatrixRegFile`
  - `DetailedCuteScoreboard`
  - `MatrixKernels`

## 已完成
- 新增文件：
  - `src/matrix/detailed_cute_backend.hh`
  - `src/matrix/detailed_cute_backend.cc`
  - `src/matrix/detailed_cute_backend.test.cc`
  - `src/matrix/decoded_fifo.hh`
  - `src/matrix/matrix_regfile.hh`
  - `src/matrix/detailed_cute_scoreboard.hh`
- 已通过：
  - `build/RISCV/matrix/detailed_cute_backend.test.opt`
    - `3 tests passed`

## 当前限制
- `DetailedCuteScoreboard`
  - 只有最小 `canIssue/onIssue/onCompletion`
  - 没有真实 source wait/wakeup 生命周期
- `MatrixRegFile`
  - 目前只是 shell，底下直接包 `MatrixState`
  - 没有 bank / port / AML/BML/CML / LocalMMU
- `DecodedFifo`
  - 是 `std::deque + depth guard`
  - issue eligibility 仍由 backend `step()` 组合判断
- 结论继续只覆盖：
  - `single-thread`
  - `no-timing shell`

## 本轮构建/环境情况
- `gem5.opt` 全量构建已越过本轮改动的关键编译点，但本会话未等到最终完成
- `src/matrix/SConscript` 中新增 `DebugFlag('MatrixCuteTrace')`
  - 修复了 matrix 子目录编译看不到 `MatrixCuteTrace` 头文件的问题
- 重新跑旧 functional 单测时遇到环境/构建树问题：
  - `build/RISCV/systemc/tlm_core/2/quantum/SConscript` 缺失

## 推荐下一步
1. 先完成 `gem5.opt` 全量构建闭环
2. 若 `gem5.opt` 成功，补一个最小 SE smoke：
   - `configs/example/matrix_seq_o3.py --matrix-backend-mode=detailed-cute`
3. 再进入下一小步：
   - 给 `DetailedCuteScoreboard` 补更真实的 source wait / pending reader 语义
   - 或开始补 `DetailedCuteBackend` 的最小 trace 点

## 2026-04-28 Phase 3B follow-up

## 当前新增状态
- `DetailedCuteBackend` 现在已经具备最小 3.B 语义：
  - `DecodedFifo` request normalization
  - queue depth=8
  - head-gated issue
  - `issue` / `completion` 分离
  - `release` 受 pending store 阻塞的最小行为

## 已通过新增测试
- `DecodedFifo.DecodeMmaNormalizesReadWriteSets`
- `DetailedCuteBackend.ReleaseWaitsForPendingStoreCompletion`

## 下一步更聚焦建议
1. 进入 3.C 时，不要重写 `DecodedFifo`
   - 直接在现有 queue + head gating 基础上增强 `DetailedCuteScoreboard`
2. 若要继续 3.B 而不是 3.C，可优先补：
   - `fifo_block` / `fifo_deq` trace 点
   - 更明确的 block reason 统计

## 2026-04-28 Phase 3C follow-up

## 当前新增状态
- `DetailedCuteScoreboard` 已升级为更像 CUTE 的内部状态机：
  - reg busy + writer owner
  - pendingReaders
  - per-FU `srcs`
  - `ready / waitFu / readPending`
- 外部接口仍保持简单：
  - `canIssue`
  - `onIssue`
  - `onCompletion`

## 已通过新增测试
- `DetailedCuteScoreboard.LoadReserveBlocksDependentComputeUntilCompletion`
- `DetailedCuteScoreboard.StoreReaderBlocksOverwriteUntilCompletion`

## 当前边界
- 已经比 3.B 更像真实 scoreboard
- 但还没到：
  - 多实例 FU
  - 更细粒度 `compute_read_finish_a/b/write_c` 分步更新
  - trace/stat stall reason 导出

## 推荐下一步
1. 若继续 3.C：
   - 给 `DetailedCuteBackend` / scoreboard 补最小 block reason trace
   - 把 `canIssue` 的阻塞理由显式化
2. 若准备接 3.D / Phase 4：
   - 先别动 FIFO 外部 contract
   - 在现有 scoreboard 状态基础上接 `matrix_regfile` 更真实的数据面

## 2026-04-28 FIFO / scoreboard trace follow-up

## 当前新增状态
- `DetailedCuteBackend::step()` 已有最小 trace 点：
  - `fifo_enq`
  - `fifo_deq`
  - `fifo_block`
  - `scoreboard_block`
  - `backend completion`
- `DetailedCuteScoreboard` 已能给出最小 block reason

## 当前已确认
- 带 trace 依赖的 `detailed_cute_backend.test.opt` 重新编译、重新运行均通过
- 因此：
  - 3.B queue 语义
  - 3.C scoreboard 语义
  - trace 补点
  三者当前没有互相打坏

## 最自然的下一步
1. 跑一个最小 SE smoke：
   - `configs/example/matrix_seq_o3.py --matrix-backend-mode=detailed-cute`
   - 开 `--debug-flags=MatrixCuteTrace`
2. 检查是否能真实看到：
   - `fifo_enq`
   - `fifo_deq`
   - `scoreboard_block`
   - `backend completion`

## 2026-04-28 Phase 3D follow-up

## 当前新增状态
- `MatrixRegFile` 已从 shell 升级为 backend-owned regfile 实体
- `DetailedCuteBackend` 现在通过 `MatrixRegFile` 读写 AB/B/C regs
- `MatrixKernels` 同时保留两套入口：
  - `MatrixState` 给 functional backend
  - `MatrixRegFile` 给 detailed backend

## 已通过新增测试
- `DetailedCuteBackend.MatrixRegFileTracksOwnerAndLastWriter`

## 当前边界
- 已有：
  - AB/B/C namespace
  - `allocated`
  - `owner`
  - `lastWriterKind`
- 仍没有：
  - banked storage
  - per-port arbitration
  - LocalMMU / fill path
  - timing model

## 推荐下一步
1. 先跑最小 SE smoke，确认 3.A/3.B/3.C/3.D 串起来仍能走通
2. 如果 smoke 正常，再决定是：
   - 继续补 trace/stats
   - 还是开始接最小 Phase 4 memory/compute datapath

## 2026-04-28 Phase 3 remaining gaps follow-up

## 当前新增状态
- `DetailedCuteScoreboard` 已补齐更细的生命周期：
  - `load_finish_a/b/c`
  - `compute_read_finish_a/b`
  - `compute_write_finish_c`
  - `store_finish`
- `DetailedCuteBackend` 已有更细的 inflight 分阶段：
  - load/store/arith/release 的 completion 延后
  - compute 的 readA/readB/writeC 分步推进
- `fifo_block / scoreboard_block` 现在都能被 `BlockReason` 分类

## 已通过新增测试
- `DetailedCuteScoreboard.ComputeLifecycleClearsReadersInStages`
- `DetailedCuteBackend.ScoreboardBlockCounterTracksSrcNotReady`

## 当前结论
- 第三阶段里“刚才列出的没对齐项”已经进一步收拢到：
  - per-op lifecycle
  - finer scoreboard updates
  - block reason 分类
- 现在已经有最小 SE smoke 证明这些 trace/行为能在真实 O3 流程里成立，但证据主要集中在 `release` 和 `lsu queue ordering`

## 推荐下一步
1. 若继续补 Phase 3 证据，优先补：
   - `lsu replay`
   - `squash / redirect`
   - `mma end-to-end` fault root cause
2. 若 `mma_end2end` 能跑通，再决定是否进入：
   - 更稳定的 stats
   - 或最小 Phase 4 memory/compute datapath

## 2026-04-30 Phase 3 runtime trace follow-up

## 当前新增状态
- `build/RISCV-clean/gem5.opt` 已产出，可直接运行 `matrix_seq_o3.py`
- `MatrixCuteTrace` 已在真实 `O3 + SE` 路径上跑通以下 probe：
  - `matrix_release_acquire_probe`
  - `matrix_release_double_probe`
  - `matrix_lsu_queue_order_probe`
  - `matrix_release_squash_window_probe`
  - `matrix_lsu_replay_probe`

## 当前关键证据
- `release_acquire_probe`
  - 有 `backend submit -> fifo_enq -> fifo_deq -> backend completion`
- `release_double_probe`
  - 两次 release 都完整通过 backend
- `lsu_queue_order_probe`
  - 第二个 LSU 因 `reason=downstream_not_accepting` 被 `fifo_block`
  - 第三个 LSU 不会越过第二个 LSU，证明 FIFO/head gating 在 runtime 下成立
- `release_squash_window_probe`
  - `[sn:1]` 和 `[sn:19]` 两次 release 都完整完成
- `lsu_replay_probe`
  - 当前只拿到单次 LSU 的最小 backend trace，replay-specific 证据仍偏弱

## 当前 blocker / 未覆盖项
- `matrix_mma_end2end_probe_nogp`
  - 运行时在 tick `107500` 命中 page-table fault
  - 只看到首个 LSU request，没拿到 `mma/store/release` 的后续 detailed trace
- 因此当前 runtime 结论仍然更适合写成：
  - `release + lsu queue/backpressure` 已有 Phase 3 证据
  - `mma end-to-end` 仍待补证据

## 2026-04-30 Phase 3 runtime trace follow-up 2

## 当前新增状态
- `matrix_mma_probe_nostore`
  - 已看到：
    - `lsu`
    - `arith`
    - `mma`
    的 submit/enqueue 进入 detailed backend
  - 说明 `mma` 路径至少已经穿过 backend glue 并进入 FIFO 层
- `matrix_lsu_queue_squash_probe`
  - 当前拿到单个 LSU request 的最小闭环
- `matrix_lsu_replay_squash_probe`
  - 当前也只拿到单个 LSU request 的最小闭环
- `matrix_lsu_replay_cancel_probe`
  - stats 正常，但 `MatrixCuteTrace` 没形成新的 backend 结论

## 当前更精确的口径
- 现在可以写成：
  - `release`
  - `lsu queue ordering`
  - `lsu downstream backpressure`
  - `mma enters FIFO`
  已经有 runtime 证据
- 还不能写成：
  - `mma compute completion`
  - `mma end-to-end`
  - 强 replay-specific backend 语义
  已经对齐

## 2026-04-30 Phase 4.0 / 4.1 doc freeze

## 当前停点
- `Phase 4.0`
  - 已完成文档冻结
- `Phase 4.1`
  - 已完成文档冻结
- 下一阶段入口：
  - `Phase 4.2` 最小 `AML/BML/CML microtask` 壳

## 本轮新增产物
- 新增/更新：
  - AML/BML/CML 与 matrix regfile 设计摘要
  - `Plan.md`
  - `implementation_notes.md`
  - `verify_report.md`

## 当前冻结结论
- `AML/BML/CML` 的组件边界已经收口：
  - 各自读写谁
  - 哪个点算 `read finish`
  - 哪个点算 `write finish`
  - 哪个点才允许形成 `completion_event`
- `CML-store` 已明确拆成两级 finish：
  - `store_read_finish`
  - `store_write_finish`
- `matrix_regfile` 的拍级聚合带宽已经冻结为：
  - `AB = 1024b/cycle`
  - `C = 512b/cycle`

## 当前仍未冻结的点
- 这些宽度只代表：
  - 单 channel beat 宽度
- 这些宽度不代表：
  - 多 channel 同拍并发能力
  - bank conflict
  - 读写仲裁
  - 多请求同拍冲突

## 下一步建议
1. 进入 `4.2` 时，先按单 request / 单 microtask 壳推进，不要一开始就引入多 channel 并发
2. `4.2` 第一版优先只做：
   - issue
   - inflight
   - finish
   三段
3. 在 `4.2` 实现前，继续保持：
   - 不改 CPU core path

## 2026-04-30 Phase 5 functional completion

## 当前停点
- `Phase 5`
  - 已按 `functional-only` 口径收口
  - 本轮没有补 trace

## 当前新增产物
- 新增：
  - compute timing 设计摘要
- 更新：
  - `src/matrix/detailed_cute_backend.hh`
  - `src/matrix/detailed_cute_backend.cc`
  - `src/matrix/detailed_cute_backend.test.cc`
  - `Plan.md`

## 当前 compute 结论
- `mma` 不再走旧 `inflight` 立即执行壳
- 当前统一走：
  - `TaskSlot`
  - `TaskEvent`
  回路
- 最小 compute 状态机：
  - `Accepted`
  - `ComputeReadA`
  - `ComputeReadB`
  - `ComputeExecute`
  - `ComputeWriteC`
  - `TerminalCompletion`
- `A/B` 会先 snapshot，再释放 reader
- `C` 结果只在 `ComputeWriteC` 写回
- completion 只在 terminal event 入队

## 当前 timing / datatype 口径
- 第一版 timing 公式：
  - `m_iters = ceil(mtilem / 4)`
  - `n_iters = ceil(mtilen / 4)`
  - `k_iters = ceil(mtilek / 32)`
  - `compute_execute_latency = max(1, m_iters * n_iters * k_iters)`
- Required:
  - `int8 -> int32`
- 当前明确 `Unsupported`:
  - `isFp == true`
  - 非零 `types1/types2/typed`
- `mzero` 继续留在 AML/CML zero-load family，不属于 compute

## 已做验证
- 构建：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
- 运行：
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt`
- 当前单测已覆盖：
  - compute terminal writeback
  - compute A/B snapshot
  - compute latency monotonicity
  - fp mma unsupported
  - 原有 load/store/release end-to-end

## 剩余未覆盖
- 没有新的 runtime trace 证据
- 还没有把 `Matrix_MN` / `ReduceWidthByte` 接成配置项
- 当前 timing 只保证单调性和功能闭环，不保证 RTL 精确周期

## 2026-05-08 `src/matrix` 收尾前状态

### 当前停点
- `src/matrix` detailed CUTE backend 已进入“可收尾但未完全 RTL 等价”状态

### 当前实现结构
- 当前 active implementation 已拆为：
  - `taskcontrol.cc`
  - `backend_runtime.cc`
  - `mregfile.cc`
  - `memoryload.cc`
  - `matrix_ex.cc`
- `src/matrix/detailed_cute_backend.cc`
  - 当前不参与 build
  - 仍是旧残留实现文件

### 当前已成立
- `TaskController` 风格的 `head -> scoreboard -> ready -> issue`
- `AML/BML/CML` 三路分流
- `ADC/BDC/CDC/MTE` 的阶段映射，并已拆成显式子单元状态与单元级 trace/counter
- `ADC/BDC/CDC/MTE` 已具备最小多 compute in flight / 前级重叠能力
- `scoreboard` 的分阶段释放
- load/store shared `memoryBudget`
- integer mma datatype 扩展到更接近 RTL 的子集
- fp encoding 至少可以到达 execute boundary

### 当前仍未完全对齐 RTL
- 端口级 valid/ready
- 真实 regfile / L2 端口竞争
- `ADC/BDC/CDC/MTE` 的完整独立实体化和多 compute in flight
- fp mma 功能实现

### 当前验证
- `detailed_cute_backend.test.opt`
  - `30 tests passed`

### 2026-05-08 Phase 3C review checkpoint
- Verdict:
  - `APPROVE_WITH_NOTES`
- Evidence:
  - `3.12-3.15` 已完成；当时 `3.16` 仍为 `WIP`，这是历史 checkpoint 口径
  - `detailed_cute_backend.test.opt`: `30 tests passed`
- Blocking Issues:
  - `None`
- Recommended Action:
  1. 当时建议保持 `3.12-3.15=完成`、`3.16=WIP`

## 2026-05-11 3.16 completion review
- Verdict:
  - `APPROVE`
- Evidence:
  - `Plan.md` 中 `3.16` 已标记为 `cc:完成`
  - `detailed_cute_backend.test.opt` 最新回归为 `30 tests passed`
- Notes:
  - 3.16 已完成，但保留后续继续细化的空间，不影响当前完成判定

## 2026-05-11 style follow-up: scoreboard cc split
- 这轮继续把 `DetailedCuteScoreboard` 的实现从头文件下沉到 `.cc`
- 当前主实现入口仍然清晰：
  - `backend_runtime.cc` 负责主控制流
  - `detailed_cute_scoreboard.cc` 负责 scoreboard 策略实现
- 这次修改不改变生命周期语义，只收紧风格

## 2026-05-11 style follow-up: runtime phase split
- 这轮继续把 `backend_runtime.cc` 的主链拆成更直白的阶段函数：
  - `processFifoHead()`
  - `traceActiveComputeTasks()`
  - `serviceActiveComputeUnits()`
  - `dispatchReadyComputeUnits()`
- 目标是让 `step()` 更接近标量 active 路径的“三段式主循环”

## 2026-05-11 style follow-up: test probe shrink
- 这轮继续把 test-only 观察面收口：
  - `matrixRegFile()` 重复入口已去掉
  - 细粒度状态观察改走测试侧 probe
- 目标是让 `DetailedCuteBackend` 的 public API 更接近真正的 backend 可见面
  2. 后续继续推进多 compute in flight 与子单元级 backpressure

### 下一步建议
1. 如果继续向 RTL/CUTE 收敛：
   - 优先把 `ADC/BDC/CDC/MTE` 从“显式子单元状态”继续收成真正独立实体
   - 优先收紧 `ComputeGo / EndReady` 的 ready / end / release 三段边界
2. 如果当前准备收尾：
  - 明确把旧 `detailed_cute_backend.cc` 视为残留
  - 在最终说明里标清 active build 文件与未参与 build 文件

## 2026-05-12 SE gemm runtime unblock stop point

- 当前已确认：
  - 默认 `MatrixRegFile` 槽位从 `4` 扩到 `8`
  - SE `gemm_precomp` 原始 `vector::_M_range_check` 已消失
  - `gemm_precomp` 8 个 precomp case 全部通过
- 当前仍保留：
  - `detailed_cute_backend.test.opt` 里 1 条与 fp encoding 相关的单测失败
  - `Plan.md` 状态不变，仍按 Phase 3 WIP 继续
- 下一步建议：
  - 单独处理那条 fp 单测失败
  - 继续沿 current Phase 3 口径推进，不把这次 SE functional unblock 误写成 RTL/CUTE 收口

## 2026-05-15 Phase 4A MTE timing / all-ready stop point

- 当前已确认：
  - `computePathReady()` 已按 RTL `ADC/BDC/CDC` all-ready issue contract 收紧。
  - MTE timing helper 已使用当前 RTL 主配置 `Tensor_MN=128`, `Tensor_K=64`, `Matrix_MN=8`, `ReduceWidthByte=32`, `ResultWidthByte=4`。
  - 每个 accepted beat 的 A/B/C/D 宽度均为 `2048b = 256B`。
  - Round 1 已按 active RTL controller 事实修正 MTE shape contract：
    - `TaskController.scala` 直接传 `mtilem/mtilen`。
    - A/B controller 对 M 做 `ceil(M / Matrix_MN)`，N 用 `N / Matrix_MN`，K 使用已经除以 `ReduceWidthByte` 后的 groups。
    - CDC 当前 assert `N == Tensor_MN`。
  - 当前 GEM5 拒绝 `mtilen != 128`，支持 `mtilem <= 128`、`mtilen == 128`、`mtilek <= 64` 且 `scaled_mtilek_bytes / 32 > 0` 的 timing contract。
  - `128x128x64` int8 accepted input beats 为 `512`；`4x128x32` int8 accepted input beats 为 `16`。这里的 accepted input beats 表示 MatrixReg read / MTE accepted compute tile-pair 窗口；unique A/B block 各 `32` 只作为数据量解释口径。
  - `totalCompletionCycles` 已定义为 GEM5 compute issue -> terminal completion，并由 `lastMicrotaskLatency` targeted test 覆盖。
- 当前验证：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - 完整结果：`41 tests from 5 test suites passed`
- 当前仍保留：
  - `ComputeGo / EndReady` 仍未做 RTL ready/valid 全形状建模。
  - `FReducePE` 内部 accumulator 没有逐拍仿真；`executeMma()` 仍是最终结果摘要。
  - `types1/types2=0x3` 仍只是 e4-width timing probe，不代表 functional int4 支持。
  - CDC reorder / after-op / memory path / MLS / carrier 不属于本轮覆盖。
- 下一步建议：
  - 如果继续 Phase 4A compute path，优先收 `ComputeGo`、`MicroTaskEndValid/Ready` 与 CDC-visible writeback 的更细事件边界。
  - 进入 review 前不要把本轮写成 RTL timing equivalent，只能写成 active RTL-derived bandwidth / accepted-beat / GEM5 completion-boundary checkpoint。

## 2026-05-16 Humanize Round 3 review blocker stop point

- 当前已修复：
  - FS matrix load/store 由 `TranslatingPortProxy` 处理虚拟地址，避免 base physical offset 跨页错误。
  - O3 `msyncregreset` reset token 从 execute 阶段延后到 commit 成功后生效。
  - backend regfile live state 在 checkpoint/drain 时 fail-fast，避免未序列化的 A/B/C matrix register contents 静默丢失。
- 当前验证：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='MatrixRegFile.StoresTensorAndAllocatedState:DetailedCuteBackend.ReportsCompletedMatrixRegisterStateForCheckpointGuard'`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - `scons -Q build/RISCV/gem5.opt -j8`
- 当前限制：
  - 没有新增完整 matrix backend checkpoint serialization。
  - 没有 FS cross-page matrix workload 运行证据。
  - `matrix_sync_check` 当前不作为绿灯证据，详见 `verify_report.md`。

## 2026-05-18 AGENTS.md SE gemm_precomp stop point

- 当前已修复：
  - RISC-V matrix load/store decode 从旧 `funct7-only` 口径改为 QEMU/toolchain 字段口径。
  - `0x24a48a2b` 正确解释为 `mlce32`：`funct7=0x12`, `width=2`, `md=4`。
  - O3 `MlsUnit` 的 early fault、access size、shape 派生、payload flags/width/elemType 已同步。
  - AGENTS.md 的 SE `gemm_precomp` 命令已恢复到 `All 8 precomp tests PASSED.`
- 当前验证：
  - `scons -Q build/RISCV/gem5.opt -j8`
  - `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-precomp configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
  - `git diff --check -- src/arch/riscv/isa/decoder.isa src/cpu/o3/dyn_inst.cc src/cpu/o3/mls_unit.cc src/arch/riscv/matrix_sync_check.cc cute_detailed_design/matrix_o3_README.md`
- 当前限制：
  - `matrix_sync_check.opt` standalone link 仍因既有目标链接缺 gem5 基础符号失败，只能作为源码覆盖，不作为运行绿灯。
  - 本轮不收 MatrixReg/MTE bank timing 主线；下一步仍应回到 plan.md 中 MatrixReg/MRegFile bank resource helper、ADC/BDC/CDC/CDC writeback timing 的后续项。

## 2026-05-18 O3 MMA opcode metadata passthrough stop point

- 当前已修复：
  - `ROB -> CuteRequest` 的 MMA request 转换现在保留 staged `payload.op`。
  - `mfmacc_s_h` 的 `payload.op = 0x04` 不再被 `CuteRequest::makeMma()` 默认 `0x0c` 覆盖。
  - `MatrixExecPayload -> CuteRequest` 转换已从 `rob.cc` 匿名 helper 提成 `matrixPayloadToCuteRequest()`，便于 focused test 覆盖。
- 当前验证：
  - `scons -Q build/RISCV/cpu/o3/matrix_o3_payload.test.opt --unit-test -j8`
  - `./build/RISCV/cpu/o3/matrix_o3_payload.test.opt`
  - `scons -Q build/RISCV/gem5.opt -j8`
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.Fp16MmaSucceedsWithInt32Accumulator:DetailedCuteBackend.MteTimingReportsPerCycleBandwidthAndAcceptedBeats:MatrixRegResource.*'`
  - `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-precomp-o3-op configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
  - `util/style.py -m --checker SortedIncludes --checker Whitespace --checker LineLength ...`
  - `git diff --check -- ...`
- 当前限制：
  - 本轮只收 O3 backend request opcode metadata，不声明 active ISA FP workload 数值闭环。
  - Planning 文件仍保持交接状态，不因本轮 side issue 刷新结构。
  - 下一步回到 `plan.md` MatrixReg/MTE 主线，优先推进 helper 接入 runtime、ADC/BDC/CDC 并行 read beat、FReduce tail 与 CDC 512 D beat boundary。

## 2026-05-18 Phase 4A compute read frontend stop point

- 当前已完成：
  - `MatrixRegResource` 已接入 compute read frontend。
  - MMA issue 后，ADC/BDC/CDC 在同一 backend step 发起 A/B/C MatrixReg read request。
  - A/B/C read response 按 1-cycle latency 到达；三路 response 到齐后才释放 pending reader 并进入 MTE。
  - C source pending reader 现在由 `onComputeReadFinishC()` 在 CDC read response 阶段释放，C dest busy 仍保留到 write finish / terminal completion。
  - C MatrixReg 同 parity write/read conflict 会 stall/defer compute read frontend。
- 当前验证：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.ComputeReadFrontend*:DetailedCuteBackend.ComputeSubUnitsAreTrackedExplicitly:DetailedCuteBackend.MultipleComputeTasksCanOverlapFrontUnits:DetailedCuteBackend.SecondComputeWaitsOnAdcBdcCdcAvailabilityNotScoreboardComputeBusy:DetailedCuteBackend.ComputePathRequiresAdcBdcCdcReady:DetailedCuteBackend.MteTimingReportsPerCycleBandwidthAndAcceptedBeats:DetailedCuteBackend.MteTimingFollowsActiveRtlControllerShape:DetailedCuteBackend.MteTotalCompletionCyclesMatchesComputeTerminalBoundary:DetailedCuteBackend.ComputeWriteOccursOnlyAtTerminalStage:DetailedCuteBackend.ComputeKeepsCDestBusyUntilWriteFinishAndCompletion:DetailedCuteBackend.ComputeSnapshotsABBeforeWriteback:DetailedCuteBackend.EndToEndLoadMmaStoreReleaseSequence:DetailedCuteScoreboard.ComputeLifecycleClearsReadersInStages:MatrixRegResource.*'`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - 完整结果：`49 tests from 6 test suites passed`
- 当前限制：
  - 还没有实现 CDC `512` D beat writeback timing window。
  - 还没有实现 MLOAD/MSTORE external `64B/cycle` request window、fixed memory latency、fill-table/sourceId/cache backpressure。
  - 还没有实现精确 FReducePE 数值流水；`executeMma()` 仍是最终功能摘要。
- 下一步建议：
  - 继续 `plan.md` Milestone 5/6：先把 FReduce tail 与 CDC D beat/writeback boundary 拆成更明确 runtime state，再收 full int8 `512` D beat timing。
  - 保持“不宣称 RTL timing equivalent”的边界，只报告当前 resource/timing contract checkpoint。

## 2026-05-19 CDC tile-level D beat writeback stop point

- 当前已完成：
  - `MteTiming.cdcWriteCycles` 现在按 `acceptedInputBeats` 建模。
  - full int8 `128x64 * 64x128` 的 CDC D writeback window 是 `512` beats。
  - C/D 地址 tile 数仍是 `256`，不再误用作 CDC terminal 前的 D beat 数。
  - int8 CDC writeback 每个 D beat 执行 CRegFile full-bank tile RMW：
    - read old C tile
    - read response 后计算 updated `8x8xInt32` tile
    - write grant 成功后写回 internal C tensor 并推进 D beat
  - D beat index 映射已按 RTL `CDataController` 口径落地：`addr/kGroup/mTile/nTile`。
  - full int8 all-ones A/B、zero C 时，第 256 个成功 D beat 后 internal C tile `(0,0) == 32`；第 512 个成功 D beat 后为 `64`。
  - 同 parity C MatrixReg write conflict 会 stall CDC D beat，且不更新 C tile。
  - int8 tile-level path terminal 不再再次 full `executeMma()` 覆盖 C；非 int8 path 仍保留旧 fallback/oracle。
  - scoreboard 仍阻止 store/MMA/release 在 write finish / terminal 前消费 partial C。
- 当前验证：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.CdcInt8TileWriteback*:DetailedCuteBackend.CdcWriteback*:DetailedCuteBackend.Cdc*TileRead*:DetailedCuteBackend.Cdc*TileWrite*:DetailedCuteBackend.Dependent*Partial*'`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - `scons -Q build/RISCV/gem5.opt -j8`
  - `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-cdc-tile configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
  - `util/style.py -m --checker SortedIncludes --checker Whitespace --checker LineLength ...`
  - `git diff --check -- src/matrix cute_detailed_design/implementation_notes.md cute_detailed_design/verify_report.md cute_detailed_design/handoff.md plan.md`
  - 完整 detailed backend 结果：`54 tests from 6 test suites passed`
  - SE smoke 结果：`All 8 precomp tests PASSED.`
- 当前限制：
  - MLOAD/MSTORE external `64B/cycle` request window、fixed memory latency、fill/store MatrixReg resource boundary 仍未实现。
  - cache/L2/MSHR/coherence/retry/replay 仍按计划排除。
  - 精确 MTE/FReducePE 数值流水仍不实现。
- 下一步建议：
  - 若继续当前收窄目标，优先做 review/cleanup；LocalMMU/load 建模需另起计划，不应混入本轮 CDC tile-level writeback。

## 2026-05-19 LocalMMU matrix load/store timing stop point

- 当前已完成：
  - 新增 `LocalMmuModel`，支持 fixed latency、每 cycle 一个 `64B` beat、64 source IDs / max outstanding。
  - `DetailedCuteBackend::TimingConfig` 已接入 LocalMMU latency/outstanding 配置，默认构造兼容现有调用。
  - MLOAD/MSTORE 已按 `row * column * elemBytes` 拆成 `64B` LocalMMU beats。
  - load 等待所有 LocalMMU read responses 后执行一次 functional `loadTile()` snapshot。
  - 每个 `64B` read response 需要 `2 x 32B` MatrixReg `MemoryLoader` write chunks 后才让 destination register backend-visible。
  - loader write chunks 经过现有 `MatrixRegResource`，可按 A/B loader write priority stall compute read frontend。
  - store 在 C register snapshot 后 enqueue LocalMMU store beats；`storeTile()` / store completion / release gating 等待所有 store ack。
- 当前验证：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='LocalMmuModel.*'`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='LocalMmuModel.*:DetailedCuteBackend.*LocalMmu*:DetailedCuteBackend.ReadResponseRequiresTwoMatrixRegWriteSlots:DetailedCuteBackend.LoaderWritePriorityCanStallComputeRead:DetailedCuteBackend.ReleaseWaitsForLocalMmuStoreAck'`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - `scons -Q build/RISCV/gem5.opt -j8`
  - `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-localmmu configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
  - `util/style.py -m --checker SortedIncludes --checker Whitespace --checker LineLength ...`
  - `git diff --check -- src/matrix cute_detailed_design/implementation_notes.md cute_detailed_design/verify_report.md cute_detailed_design/handoff.md plan.md`
  - 完整 detailed backend 结果：`65 tests from 7 test suites passed`
  - SE smoke 结果：`All 8 precomp tests PASSED.`
- 当前限制：
  - 本轮没有实现真实 L2 / RequestPort / Packet / recvTimingResp / retry。
  - 本轮没有实现 cache tag / hit-miss / MSHR / coherence / replacement。
  - 本轮没有新增 CDC tile-level functional writeback 行为。
  - LocalMMU 是 fixed-latency timing boundary；功能数据仍由 `MatrixMemoryAdapter` 读写。
- 下一步建议：
  - 后续若继续 memory hierarchy，应另起真实 L2 / cache path 计划，并明确 RequestPort、Packet lifetime、retry/backpressure 和 HBL2 metadata 边界。
  - 若继续 RTL timing 对齐，应拿 RTL trace 或更细 workload evidence 校准 LocalMMU latency 与 response ordering，不要把本轮 fixed-latency model 写成 RTL equivalence。

## 2026-05-19 LocalMMU trace counters stop point

- 当前已补充：
  - `DetailedCuteBackend::TraceCounters` 新增 LocalMMU beat enqueue、issue、read response、store ack 计数。
  - `TraceCounters` 新增 MatrixReg loader-fill chunk queued/granted/stalled 计数。
  - `MatrixCuteTrace` 新增 `local_mmu_enqueue`、`local_mmu_issue`、`local_mmu_response`、`matrix_reg_loader_write_grant`、`matrix_reg_loader_write_stall` 事件。
  - 新增 focused regressions：
    - `DetailedCuteBackend.LocalMmuTraceCountersTrackLoadFill`
    - `DetailedCuteBackend.LocalMmuTraceCountersTrackStoreAck`
- 当前边界：
  - 本轮只补 observability，不改变 LocalMMU fixed-latency 行为。
  - `matrixRegLoaderWriteChunksQueued` 表示 LocalMMU read response 生成的 fill chunk 数；`stalled` 表示 MatrixRegResource grant 失败次数。
  - 仍未接入真实 L2 / `RequestPort` / `Packet` / retry，也未建模 RTL fill FIFO depth/backpressure。

## 2026-05-20 LocalMMU requester/source arbitration stop point

- 当前已补充：
  - `LocalMmuModel` pending requests 已拆为 `AML/BML/CML` 三路队列。
  - issue 选择改为 CUTE `LocalMMU.scala` 风格的 rotating first requester。
  - source ID 分配改为 CUTE `CUTE2TLImp` 当前等效行为：选择最高空闲 ID。
  - 新增 `LocalMmuModel.RotatesRequesterPriorityAcrossClients` 与 `LocalMmuModel.AllocatesHighestAvailableSourceIdLikeCUTE2TL`。
- 当前验证：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='LocalMmuModel.*'`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='LocalMmuModel.*:DetailedCuteBackend.*LocalMmu*:DetailedCuteBackend.ReadResponseRequiresTwoMatrixRegWriteSlots:DetailedCuteBackend.LoaderWritePriorityCanStallComputeRead:DetailedCuteBackend.ReleaseWaitsForLocalMmuStoreAck'`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - 完整 detailed backend 结果：`69 tests from 7 test suites passed`
- 当前限制：
  - 仍没有真实 L2 / `RequestPort` / `Packet` / retry。
  - 仍没有 TL response read-data-vs-ack priority 模型。
  - 仍没有 cache tag / hit-miss / MSHR / coherence / replacement。

## 2026-05-21 CML LocalMMU fill chunk entry/parity stop point

- 当前已补充：
  - LocalMMU read response 产生的 pending MatrixReg loader-fill chunks 现在记录实际 MatrixReg entry。
  - 每个 `64B` response 仍拆成 `2 x 32B` chunks；CML 第二个 chunk 使用下一个 entry/parity。
  - `serviceLsuMatrixRegWriteChunk()` 用 pending chunk entry 进入 `MatrixRegResource`，避免 C odd/even conflict 使用错误 parity。
  - 新增 `DetailedCuteBackend.CmlFillSecondChunkUsesNextMatrixRegEntryParity`。
- 当前验证：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.CmlFillSecondChunkUsesNextMatrixRegEntryParity'`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='MatrixRegResource.*:LocalMmuModel.*:DetailedCuteBackend.*LocalMmu*:DetailedCuteBackend.ReadResponseRequiresTwoMatrixRegWriteSlots:DetailedCuteBackend.LoaderWritePriorityCanStallComputeRead:DetailedCuteBackend.CmlFillSecondChunkUsesNextMatrixRegEntryParity:DetailedCuteBackend.ReleaseWaitsForLocalMmuStoreAck'`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - 完整 detailed backend 结果：`70 tests from 7 test suites passed`
  - `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-cmlfill-20260521 configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
  - SE smoke 结果：`All 8 precomp tests PASSED.`，exit tick `413030889`
- 当前限制：
  - 仍没有 bounded `MReg_Fill_Table` / fill FIFO depth。
  - 仍假设 LocalMMU response always ready，source ID 不被 fill backpressure 持有。
  - 仍没有真实 L2 / TL / `RequestPort` / `Packet` / retry。
- 当前结论：
  - LocalMMU first-stage fixed-latency / MatrixReg fill-resource checkpoint 可以作为进入真实 L2/cache RequestPort 计划的前置状态。
  - 不能把当前 SE smoke 或 fixed-latency model 写成 RTL-equivalent memory timing；fill table/backpressure、TL/LLC ready、真实 cache/MSHR/coherence 仍需后续单独计划。

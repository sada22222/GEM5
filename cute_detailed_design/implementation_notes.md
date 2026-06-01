# Implementation Notes

## 文档职责

- 这份文件只负责 **本轮实现记录**。
- 它只写：
  - 这一次为什么要改
  - 这一次实际改了什么
  - 改动影响到哪些模块
  - 当前代码状态到哪一步
- 它**不负责**：
  - 承担长期计划
  - 充当核内设计文档
  - 充当 simplified CUTE 接入设计文档
  - 完整保存验证证据或 handoff 信息

## 相关文档边界

- `Plan.md`
  - 负责计划与阶段推进
- `cute_detailed_design/matrix_core_design.md`
  - 负责核内设计
- `cute_detailed_design/matrix_diff_vs_rtl.md`
  - 负责当前未对齐项
- `verify_report.md`
  - 负责验证与证据
- `handoff.md`
  - 负责当前交接与下一步

## Stage
- Phase 4 `4.1/4.3/4.4/4.5`
- 当前阶段位置：`4.1/4.3/4.5` 已完成，`4.4` 以 backend/unit checkpoint 形式收口；`4.6` 仍待完成
- 历史的 Phase 2A / Stage E 记录保留在下方
- 历史 checkpoint:
  - 阶段 A: `e65678d462`
  - 阶段 B fix-up: `7bb1033843`
  - 阶段 C fix-up: `698d67ab47`
  - 阶段 D 首版: `e2ad2c38bf`
  - 阶段 D fix-up: `4e6efb0095`
- 阶段 E code checkpoint:
  - `f40735a9a2`
- 阶段 E evidence checkpoint:
  - 待本轮文档提交

## 2026-05-13 Phase 4 baseline update

### Why This Update Exists
- 这轮先不继续扩 Phase 3 的 backend 行为，而是按修订版 `Plans.md` 先把 Phase 4 的顺序和边界钉住：
  - 先冻结旧接口与回归顺序
  - 再把 datatype tag 从 CPU 侧一路透传到 backend request / tensor / memory adapter
  - FP16/BF16/TF32 compute 在没有 oracle 前继续保持 `Unsupported`
- 当前最直接的 correctness 风险不是算子细节，而是：
  - FP16 tag 被误当成 `int16`
  - `MatrixExecPayload -> CuteRequest -> MatrixTensor` 中间丢 datatype

### What Changed
- [Plans.md](/nfs/home/hujun/GEM5/Plans.md)
  - 将 Phase 4 修订为 `4.1` 到 `4.6`
  - 去掉 `4.1 -> 3.20` 这种过重依赖
  - 将 `4.5` 拆成不等待 FP16 compute 闭环的 smoke 回归
  - 将 `4.6` 单列为最终回归
- [src/cpu/exec_context.hh](/nfs/home/hujun/GEM5/src/cpu/exec_context.hh)
  - `MatrixExecPayload` 新增显式 datatype 字段：
    - `elemType`
    - `lhsElemType`
    - `rhsElemType`
    - `dstElemType`
- [src/cpu/o3/rob.cc](/nfs/home/hujun/GEM5/src/cpu/o3/rob.cc)
  - `toCuteRequest()` 不再只靠 `widths` 推断 datatype
  - LSU / MMA / zero 路径现在显式带 `MatrixElemType`
- [src/cpu/o3/mls_unit.cc](/nfs/home/hujun/GEM5/src/cpu/o3/mls_unit.cc)
  - 当前 `mlae8/mlbe8/mlce32/msce32` 主线在 payload build 时补齐 `elemType`
- [src/cpu/o3/cpu.cc](/nfs/home/hujun/GEM5/src/cpu/o3/cpu.cc)
  - commit-side `CuteRequest -> ISA stub request` 也开始回灌 LSU / MMA datatype
- [src/matrix/matrix_types.hh](/nfs/home/hujun/GEM5/src/matrix/matrix_types.hh)
  - `MatrixElemType` 扩到：
    - `Int8/Int16/Int32/Int64`
    - `Fp16/Bf16/Tf32`
  - `AmuMmaDesc` 显式持有 `lhs/rhs/dst` elem type
  - `elemBytes()` 对 FP16/BF16/TF32 给出正确 byte width
- [src/matrix/matrix_memory_adapter_gem5.cc](/nfs/home/hujun/GEM5/src/matrix/matrix_memory_adapter_gem5.cc)
  - `Fp16/Bf16` 作为 16-bit raw payload 读写
  - `Tf32` 作为 32-bit raw payload 读写
- [src/matrix/matrix_ex.cc](/nfs/home/hujun/GEM5/src/matrix/matrix_ex.cc)
  - FP elem type 现在显式返回 `Unsupported`
  - 整数 path 保留 `types1/types2/typed -> int elem type` 的 fallback，不回归现有 int8/int16/int32 测试
- [src/arch/riscv/isa/decoder.isa](/nfs/home/hujun/GEM5/src/arch/riscv/isa/decoder.isa)
  - 当前 active int8/int32 decode/stub path 也补齐 datatype 字段，避免主线与新 tag 体系脱节
- [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc)
  - 新增 `DetailedCuteBackend.Fp16LsuKeepsElemTypeAndRawBits`
  - 将 FP 相关测试改成显式 `Fp16` tag，而不是再借 `Int16` 近似

### Current Status
- 已完成：
  - `4.1` 主线 workload / datatype 边界冻结
  - `4.3` current 主线上的 datatype tag 透传基础
  - `4.4` FP16 backend/unit compute checkpoint
  - backend 对 FP16/BF16/TF32 的显式 `Unsupported` 边界
  - `4.5` `gemm_precomp` 作为 `0x2b` 主口径 workload 的 SE smoke 回归
- 尚未完成：
  - active ISA/decode 路径上的 FP MMA workload 闭环
  - BF16/TF32 compute 结果正确性闭环
  - `4.6` 默认主线路径上的最终回归收口

### 当前 datatype / FP16 边界
- 目前已经支持：
  - FP16/BF16/TF32 的 datatype tag 透传
  - LSU / memory adapter 按 raw bits 处理 FP16/BF16/TF32
  - request 可以带着 FP datatype 一路传到 backend
  - FP16 MMA 在 detailed backend/unit 路径上的结果正确性闭环
- 目前还不支持：
  - active ISA/decode 路径上的 FP MMA workload 闭环
  - BF16/TF32 compute 的结果正确执行
  - 默认 RTL 主线路径上的 FP compute 可用性结论

### 2026-05-13 Phase 4A review 修正
- 这轮对 `Phase 4A` 做了代码级差异复核后，修正了两点：
  1. 不再使用“GEM5 没有 FIFO/没有 compute 分阶段”这类过时口径
  2. 不再把 `3 -> 4 -> 2 -> 1` 当成机械串行实现顺序，而改成：
     - compute boundary 先行
     - memory/cache/resource 建模前置或并行
     - 之后再收 MLS replay/cancel
     - 最后才收 carrier 结构
- 当前新的 blocking 差异已经显式落到：
  - `ADC-only ready` vs RTL `ADC/BDC/CDC all-ready`
  - `LocalMMU/sourceId/fill-table/FIFO` 缺失
  - `mregfile` 读写仲裁 / 每拍带宽 / bank 并行度模型缺失
  - MLS `cause/carry/safe-writeback` 粒度缺失
  - `MatrixAmuBuffer` 仍非 RTL fixed-slot / CommitWidth carrier

### 2026-05-13 Phase 4 baseline task review fix
- 这轮按 review 结论补齐了 `Phase 4` 文档状态自洽：
  - `Plans.md` 中 `4.1/4.3/4.5` 已更新为 `cc:完成`
  - `verify_report.md` 新增本轮 task review gate
  - `matrix_diff_vs_rtl.md` 将 `mregfile` 仲裁/带宽升格为独立重点未对齐项
- 当前统一口径：
  - 这个 baseline checkpoint 只收 `4.1/4.3/4.5`
  - 当时 `4.4/4.6` 继续保持未完成
  - `Phase 4A` 仍是后续时序/建模对齐计划，不与本轮 datatype/smoke checkpoint 混写成“整个 Phase 4 完成”

### 2026-05-13 Phase 4 FP16 compute close-out
- 这轮在不改现行 int8/int32 主线的前提下，只补了 FP16 backend compute 最小闭环：
  - [src/matrix/matrix_ex.cc](/nfs/home/hujun/GEM5/src/matrix/matrix_ex.cc)
    - `executeMma()` 现在对 `FP16 x FP16 -> raw FP32 accumulator` 返回 `Success`
    - 继续显式拒绝：
      - 非 `Int32` accumulator 形状
      - BF16/TF32 compute
  - [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc)
    - `Fp16MmaSucceedsWithInt32Accumulator` 用 CUTE `cute-fpe` 软件 oracle 固定了 2x2 最小结果
- 这轮没有做：
  - active `decoder.isa` 上的 FP MMA decode 接入
  - FP workload 从 ISA 到 backend 的端到端可达
- 当前应固定的边界：
  - `4.4` 的“闭环”是 backend/unit 级闭环
  - 不是 ISA/workload 级 FP 路全通，更不是“已对齐 RTL”
  - 因此这轮不能单独把 `4.6` 最终回归收成完成

## 2026-05-09 Phase 3C `3.16` semantic boundary follow-up

### Why This Update Exists
- 这轮不是继续扩 `release/token` 或 Phase 5，而是按 `Plan.md` 先把 `3.16` 的最小收口边界钉实。
- 重点不是把 RTL 信号形状硬抄进 GEM5，而是先确认当前 active detailed backend 的任务语义到底对应到 CUTE 哪一层。

### What Changed
- 没有继续重写 `src/matrix` active backend 主逻辑。
- 仅在 [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc) 新增：
  - `DetailedCuteBackend.SecondComputeWaitsOnAdcAvailabilityNotScoreboardComputeBusy`
- 这条单测把当前 `3.16` 的关键边界固定下来：
  - 第二条 `mma` 在第一条仍占用 `ADC` 时，会因为 downstream/subunit 不可接收而留在 FIFO
  - 它不是被一个粗粒度 “compute FU busy” 的 scoreboard 谓词挡住
  - 等第一条离开 `ADC` 进入后级子单元后，第二条 `mma` 可以在前级发射

### Current Semantic Conclusions
- 当前收口准则：
  - 不强求 `valid/ready` 形式对齐
  - 优先固定 task 的接收、占用、消费、完成、释放五类语义
- 当前结构准则：
  - 入口层：`commit-visible request + FIFO`
  - 控制层：`scoreboard + TaskController-style issue`
  - 执行层：`AML/BML/CML/ADC/BDC/CDC/MTE` 独立微状态机
- 当前主轴：
  - 不再把 compute/load/store/release 混成一个大流程
  - 而是按 `队首判定 -> scoreboard 查询 -> 单元 ready -> issue -> end` 理解 active backend
- `scoreboard`
  - 在 GEM5 里主要承担 issue 依赖检查、pending reader 计数和 staged release bookkeeping
  - 它不是 `ADC/BDC/CDC/MTE` 的完整子单元调度器
  - 但它已经是当前发射判定的第一公民，而不是单纯辅助记账对象
- `regfile owner`
  - 在 GEM5 里是一个 backend-visible 的“当前写者 provenance”标记
  - 它的 RTL 对应更接近 `Scoreboard` 里的 writer / waitFu 语义，而不是一个独立硬件模块
  - 现在保留它的原因，是让寄存器在 task finish 前能显式表达“谁拥有这个目标写回”
- `pendingStoreCount`
  - 仍是当前 release gating 的最低约束
  - GEM5 当前还额外叠加了 backend drain，语义上比 `TaskController.scala` 里的 `releaseReady = !pendingStore` 更保守
- compute dispatch
  - RTL/CUTE 里，一条 `mma` 在 `TaskController` 通过后，会把任务扇出给 `ADC/BDC/CDC` 配置口，再由 `MTE.ComputeGo` 驱动读/算/写锁步推进
  - GEM5 当前不强求这组 valid/ready 的形状等价，而是用 `computeTasks + ADC/BDC/MTE/CDC` 子单元占用，去近似 task ownership、推进和完成边界
  - 下一步收口重点是把 `ComputeGo / EndReady` 从“事件式近似”继续收成真正边界：
    - 进入单元前看 ready
    - 单元结束后形成 end-valid 风格完成点
    - 对端确认后才释放资源与依赖

### Task Contract View
- `MLS`
  - 接收：`fifo_deq + dispatchTask`
  - 占用：对应 `AML/BML/CML` task slot 从 issue 起占用到 terminal completion
  - 消费：load 在 `loadTile` snapshot 成功后、store 在 `advanceStoreRead()` 快照 C reg 后
  - 完成：`WriteFinish + TerminalCompletion`
  - 释放：`scoreboard.onLoadFinish()` / `scoreboard.onStoreReadFinish()` / `scoreboard.onStoreWriteFinish()`
- `MACC/MMA`
  - 接收：`fifo_deq + computeTasks.emplace_back()`
  - 占用：先占住 compute task，再按 `ADC -> BDC -> MTE -> CDC` 子单元依次占用
  - 消费：`advanceComputeReadA()`、`advanceComputeReadB()`、以及执行阶段读取旧 C 值时
  - 完成：`executeComputeWrite()` 后进入 `TerminalCompletion`
  - 释放：`scoreboard.onComputeReadFinishA/B()` 与 `scoreboard.onComputeWriteFinishC()` 分阶段释放

### 2026-05-09 compute terminal boundary fix
- 这轮补的是 compute 终段的最小边界：
  - `CDC` 不再在本地 `write finish` 一发出就立刻退场
  - `TerminalCompletion` 被消费前，`computeTasks.front()` 仍然保持占用
  - 这样 `C dest` 的释放和 completion 的产生仍然绑定在确认后的终段
- 新增回归：
  - `DetailedCuteBackend.ComputeKeepsCDestBusyUntilWriteFinishAndCompletion`
  - 证明 `MTE` 完成之后，`C dest` 仍然 busy，直到 `WriteFinish + TerminalCompletion` 真被消费
- `AML/BML/CML` 这轮未改语义，因为它们的 completion 已经通过 task event 回路收束，当前 review 里没有发现同样级别的早退问题

### 2026-05-09 store lifecycle follow-up
- 这轮没有改 store 主逻辑，只补了一个更明确的回归断言：
  - `StoreSnapshotsRegisterDataAtReadFinish`
  - 在 `ReadFinish` 与 `WriteFinish/TerminalCompletion` 之间，`hasCompletion()` 仍应为 false
  - 这样能更清楚地区分 store 的消费点和完成点

### 2026-05-09 taskslot terminal boundary follow-up
- 这轮继续把 `AML/BML/CML/release` 的 `TaskSlot` 退场点和 compute 收成一致：
  - `finishTaskSlot()` 不再立刻 `slot.reset()`
  - 改为进入 `TerminalPending`
  - 等 `TerminalCompletion` task event 被消费后，再按 `microTaskKind` retire 对应 slot
- 这样当前 active backend 更明确地区分了：
  - `WriteFinish`：写回/写内存完成与依赖释放
  - `TerminalCompletion`：task 真正退场与单元释放

### Current Status
- `3.16` 已完成。
- 这轮把“多 compute in flight + 子单元级 backpressure”的最小边界锁成了回归证据，同时仍保留“不是完整独立实体模型”的阶段性说明。

## 2026-05-08 Phase 3C Review
- Verdict:
  - `APPROVE_WITH_NOTES`
- Evidence:
  - `3.12-3.15` 已完成；当时 `3.16` 仍为 `WIP`，这是历史 checkpoint 的口径。
- `detailed_cute_backend.test.opt` 当前为 `30 tests passed`。
- Issues:
  - `Minor`: `matrix_diff_vs_rtl.md` 的推荐表述里曾残留旧的 “simplified / functional model” 口径，现已同步到当前 detailed backend 事实。
  - `Recommendation`: `3.15` 仍应继续明确为 “integer Required，fp 只放宽到 execute boundary，结果仍可能 Unsupported”。
- Blocking Issues:
  - `None`
- Recommended Action:
  - 当时建议保持 `3.12-3.15=完成`、`3.16=WIP`，继续推进多 compute in flight 与子单元级 backpressure。

## 2026-05-11 3.16 completion review
- Verdict:
  - `APPROVE`
- Evidence:
  - `Plan.md` 已将 `3.16` 标为 `cc:完成`
  - `detailed_cute_backend.test.opt` 最新回归为 `30 tests passed`
  - `StoreSnapshotsRegisterDataAtReadFinish`、`ComputeKeepsCDestBusyUntilWriteFinishAndCompletion`、`ReleaseWaitsForPendingStoreCompletion` 等回归已覆盖主要生命周期边界
- Notes:
  - 3.16 当前已完成，但仍保留“不是完整独立实体模型”的阶段性说明，作为后续细化空间

## 2026-05-11 style follow-up: scoreboard cc split
- 这轮继续按标量风格收口：
  - 将 `DetailedCuteScoreboard` 的实现从头文件下沉到 `detailed_cute_scoreboard.cc`
  - `detailed_cute_backend.hh` 继续保留单一 canonical `matrixState()` 入口
- 这次修改不改语义，只让头文件更瘦、主控制流更直接

## 2026-05-11 style follow-up: runtime phase split
- 继续按标量风格把 `backend_runtime.cc` 的主链拆成更直白的阶段函数：
  - `processFifoHead()`
  - `traceActiveComputeTasks()`
  - `serviceActiveComputeUnits()`
  - `dispatchReadyComputeUnits()`
- 这次依旧不改生命周期语义，只让 `step()` 的控制流更像 gem5 标量 active 路径

## 2026-05-11 style follow-up: test probe shrink
- 继续收窄详细 backend 的 test-only 入口：
  - `matrixRegFile()` 重复入口已去掉，测试统一走 `matrixState()`
  - `computeTaskCount / backendStep / activeComputeUnit / computeUnitBusy` 等观察点收进测试侧 probe
- 目标是让 `DetailedCuteBackend` 对外看起来更像单一 backend，而不是公开内部状态机

## 2026-05-11 release gating follow-up

### Why This Update Exists
- `3.18` 继续收 release 语义，而不是扩 compute/load/store 主链。
- 这轮重点只盯：
  - `pendingStoreCount`
  - backend drain
  - terminal completion
- 目标是避免 release 过早放行，并把该边界固定成可回归证据。

### What Changed
- 在 [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc) 新增：
  - `DetailedCuteBackend.ReleaseWaitsForComputeTerminalCompletion`
- 这条回归固定了：
  - `mma` 先占住 backend
  - `release` 先留在 FIFO
  - `mma` 的 terminal completion 被消费后，`release` 才能 dequeue
  - `release` 自己的 token release 仍然只在最终 completion 出现时生效

### Current Semantic Conclusion
- 当前 release gating 已明确覆盖：
  - `pendingStoreCount`
  - backend drain
  - terminal completion
- 目前仍不是 RTL 级完整 ready/valid completion gating。

### 2026-05-11 review result
- `task-level code style review`: `APPROVE`
- `phase-level formal review`: `APPROVE`
- `Plan.md` 已把 `3.18` 更新为 `cc:完成`

## 2026-05-11 `3.18` task-level code style review

### Review Target
- Task:
  - Phase 3D `3.18` release gating
- Review type:
  - task-level code style review
- Claimed transition:
  - `WIP -> WIP`

### Required Gates

| Gate | Result | Evidence | Issue if FAIL |
|---|---|---|---|
| Scope matches current Phase / Task | PASS | 本轮改动只新增 [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc) 的 release gating 回归，未扩到 `3.19` 或其它 phase。 | - |
| DoD is observable and reproducible | PASS | 回归测试命令已回写到 [verify_report.md](/nfs/home/hujun/GEM5/cute_detailed_design/verify_report.md)，且新测试直接固定 `mma -> release` 的 FIFO/terminal completion 边界。 | - |
| Depends / Status / DoD are self-consistent | PASS | 当前文档把 `3.18` 描述成 release gating 证据收口，未提前声称 RTL ready/valid 已完成。 | - |
| No completed task depends on TODO without explanation | PASS | 当前 style review 不强化 phase status，只检查本 task 本地改动；`3.19` 仍保留在后续。 | - |
| Claimed alignment does not exceed evidence | PASS | 实现说明明确写了“仍不是 RTL 级完整 ready/valid completion gating”。 | - |
| Functional correctness is separated from resource/timing alignment | PASS | 本轮只固定 release issue/completion 边界，不把它外推成完整性能/时序对齐。 | - |
| Missing evidence is marked as `证据不足` | PASS | 文档仍保留“不是 RTL 级完整 ready/valid completion gating”的限制口径。 | - |
| `verify_report` contains required evidence | N/A | task-level style review 不直接审批 verify coverage。 | - |
| `handoff` reflects current stop point | N/A | task-level style review 不直接审批 handoff 完整性。 | - |
| `matrix_diff_vs_rtl.md` is consistent with `Plan.md` | N/A | task-level style review 不直接审批 phase 文档一致性。 | - |
| Review reports RTL alignment, design complexity, and code style as separate dimensions | PASS | 本节已分开报告。 | - |

### Review Dimensions

| Dimension | Result | Evidence | Issue if FAIL / N/A reason |
|---|---|---|---|
| Design Complexity | PASS | 只补一条最小回归，没有新增 helper、wrapper 或主路径抽象层。 | - |
| Code Style | PASS | 新测试沿用现有 `DetailedCuteBackend` 单测风格：显式准备 `matrixState()`、逐拍 `step()`、断言 `fifoDequeue` 和 completion 边界，定位直接。 | - |
| RTL Alignment | N/A | task-level style review does not approve RTL/CUTE alignment. | - |

### Evidence
- 新增测试：
  - `DetailedCuteBackend.ReleaseWaitsForComputeTerminalCompletion`
- 相关文档：
  - [verify_report.md](/nfs/home/hujun/GEM5/cute_detailed_design/verify_report.md)
  - [handoff.md](/nfs/home/hujun/GEM5/cute_detailed_design/handoff.md)

### Issues
- None

### Blocking Issues
- None

### Required Document Updates
- `verify_report.md`
- `handoff.md`

### Recommended Action
- 已执行：
  - phase-level formal review 已通过
  - `Plan.md` 中 `3.18` 已更新为 `cc:完成`

### Verdict

`APPROVE`

## 2026-05-16 Humanize Round 3 review blockers

### 变更摘要
- FS `Gem5MatrixMemoryAdapter` 不再用 `physBaseAddr + offset` 推导后续 matrix element 物理地址；FS 路径改用 `TranslatingPortProxy(desc.tc)` 按虚拟地址访问，让 gem5 MMU functional translation 处理跨页和 fault。
- O3 `msyncregreset` 不再在 execute/speculative 阶段立即清 shadow token 和 ISA token；`DynInst` 仅 stage reset token，commit 成功后由 `CPU::commitMatrixResetToken()` 更新 shadow/ISA token。
- checkpoint/drain 对 completed backend regfile live state 改为 fail-fast；当前没有 matrix backend regfile serialization 格式，因此拒绝带 live A/B/C matrix register contents 的 checkpoint。

### 影响文件
- [matrix_memory_adapter_gem5.cc](/nfs/home/hujun/GEM5/src/matrix/matrix_memory_adapter_gem5.cc:29)
- [dyn_inst.hh](/nfs/home/hujun/GEM5/src/cpu/o3/dyn_inst.hh:361)
- [commit.cc](/nfs/home/hujun/GEM5/src/cpu/o3/commit.cc:1910)
- [cpu.cc](/nfs/home/hujun/GEM5/src/cpu/o3/cpu.cc:958)
- [matrix_backend.hh](/nfs/home/hujun/GEM5/src/matrix/matrix_backend.hh:40)
- [matrix_regfile.hh](/nfs/home/hujun/GEM5/src/matrix/matrix_regfile.hh:43)

### 边界
- 本轮不实现完整 checkpoint serialization 格式。
- 本轮不实现 LocalMMU/cache backpressure/sourceId/fill-table。
- `matrix_sync_check` 当前不能作为绿灯证据：旧二进制命中既有 `mlae16` 预期失败，重建该小工具在链接阶段缺 gem5 基础符号。

## 2026-05-15 Phase 4A MTE timing and all-ready checkpoint

### Why This Update Exists
- 本轮按 Humanize/RLCR `plan.md` 收窄到 `ADC/BDC/CDC/MTE` compute path。
- 实现前复核了当前代码事实：
  - GEM5: `src/matrix/taskcontrol.cc`, `backend_runtime.cc`, `matrix_ex.cc`, `detailed_cute_backend.hh`, `detailed_cute_backend.test.cc`
  - RTL/CUTE: `TaskController.scala`, `ADataController.scala`, `BDataController.scala`, `CDataController.scala`, `MatrixTE.scala`, `FReducePE.scala`, `CUTEParameters.scala`
- 关键修正是：当前 RTL 主配置来自 `CUTE_8Tops_128SCP`，不是 `baseParams`；因此 `Matrix_MN=8`, `Tensor_MN=128`, `Tensor_K=64`, `ReduceWidthByte=32`, `ResultWidthByte=4`。

### What Changed
- [src/matrix/taskcontrol.cc](/nfs/home/hujun/GEM5/src/matrix/taskcontrol.cc)
  - `computePathReady()` 从 ADC-only 改成 `ADC && BDC && CDC` all-ready。
  - MTE busy 不作为 issue gate，因为 RTL `TaskController` 的 issue ready contract 是 `ADC/BDC/CDC` config ready。
- [src/matrix/detailed_cute_backend.hh](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.hh)
  - 新增 test-visible `MteTiming` contract，用于集中表达 MTE bandwidth、active RTL-derived accepted input beats 与 completion 组成。
- [src/matrix/matrix_ex.cc](/nfs/home/hujun/GEM5/src/matrix/matrix_ex.cc)
  - Round 0 曾按计划文本把小逻辑矩阵统一建模为完整 `128x128x64` physical work；Round 1 review 后已改为 active RTL controller 事实。
  - `TaskController.scala` 直接传 `mtilem/mtilen`；A/B controller 按 `ceil(M / Matrix_MN)` 与 `N / Matrix_MN` 迭代；CDC 当前 assert `N == Tensor_MN`。
  - `computeMteTiming()` 当前拒绝 zero dimension、超过 `Tensor_MN/Tensor_K` 的请求，以及无 partial-N RTL 证据的 `mtilen != 128`。
  - accepted input beats 当前表示不可打断 MTE task 的 MatrixReg read / compute tile-pair 窗口，使用 `ceil(mtilem / 8) * (mtilen / 8) * k_groups`；full-shape int8 为 `16 * 16 * 2 = 512` beats。
  - unique A/B block 各为 `32` 只用于解释数据量复用口径，不能作为 full MMA latency helper。
  - 当 `scaled_mtilek_bytes / 32 == 0` 时返回 unsupported，避免把 `mtilek < ReduceWidthByte` 误建模成有效 MTE compute。
  - `computeExecuteLatency()` 不再使用旧 `Matrix_MN=4` 逻辑 tile 公式，而使用 `acceptedInputBeats + FReducePE pipeline tail` 的 MTE 内部执行窗口。
- [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc)
  - 新增 MTE bandwidth / accepted beats / unsupported shape / runtime terminal-boundary tests。
  - 更新旧 compute latency tests，避免继续固化“逻辑小矩阵更短”或“任意 partial-N 都可 compute”的旧假设。
  - 既有 functional tests 已迁移到当前 supported 的 `N=128` 形状，并只检查左上角结果，避免把功能摘要扩展成完整 TE 仿真。
  - 新增 `ComputePathRequiresAdcBdcCdcReady`，覆盖 BDC/CDC busy 时 FIFO head MMA 不 dequeue。
- [src/arch/riscv/SConscript](/nfs/home/hujun/GEM5/src/arch/riscv/SConscript)
  - 修复阻塞 unit-test build 的 `/if` 语法错误。

### Current Boundary
- GEM5 现在按 active RTL controller 事实建模当前 MTE bandwidth 与 no-stall accepted input beat contract。
- `512` 表示 int8/e8 full physical shape 的 no-stall MatrixReg read / MTE accepted compute beats，不表示完整 issue-to-completion 总周期。
- 对 `mtilem=128,mtilen=128,mtilek=64`，int8/e8 accepted input beats 为 `512`；对 `mtilem=4,mtilen=128,mtilek=32`，accepted input beats 为 `16`。
- `totalCompletionCycles` 当前明确包含 `ADC read + BDC read + MTE accepted beats + FReducePE tail + CDC write + terminal handshake`，并定义为 GEM5 compute issue 到 terminal completion 的 runtime event interval；仍不宣称 RTL cycle equivalence。
- `executeMma()` 仍保持功能级最终结果摘要，不逐拍模拟 FReducePE accumulator。
- `types1/types2=0x3` 仍只作为 timing helper 的 e4-width probe，不代表 `executeMma()` 已支持 functional int4。

## 2026-05-16 Phase 4A MatrixReg resource helper

### Why This Update Exists
- 计划要求先建立 MRegFile / MatrixReg bank resource helper，避免后续 ADC/BDC/CDC 并行 read beat、loader fill、CDC writeback 没有统一资源仲裁口径。
- 本轮只建立 helper 和 targeted tests，不把 helper 接入完整 compute/load/store runtime。

### What Changed
- [src/matrix/matrix_reg_resource.hh](/nfs/home/hujun/GEM5/src/matrix/matrix_reg_resource.hh)
  - 新增 `MatrixRegResource`，固定 `8` banks、`32B` entry、full-bank vector grant 与 `1` cycle read response queue。
  - 提供 `makeRead()` / `makeWrite()` 与 batch `arbitrate()`，用于表达同拍资源请求。
- [src/matrix/matrix_reg_resource.cc](/nfs/home/hujun/GEM5/src/matrix/matrix_reg_resource.cc)
  - A/B MatrixReg：同拍存在 MemoryLoader write 时，DataController read 被 `AbWritePriority` stall。
  - C MatrixReg：同 odd/even SRAM parity 的 read/write 同拍冲突返回 `CReadWriteConflict`；不同 parity read/write 可同拍 grant。
  - partial bank mask 会被拒绝为 `PartialBankMask`，避免部分 bank grant 被误当成 vector 成功。
- [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc)
  - 新增 `MatrixRegResource.*` tests 覆盖 full-bank grant、1-cycle response、A/B write priority、C parity conflict。
- [src/matrix/SConscript](/nfs/home/hujun/GEM5/src/matrix/SConscript)
  - 将 `matrix_reg_resource.cc` 加入 active build 与 gtest。

### Current Boundary
- Helper 已表达本轮所需的 resource contract，但还没有接入 `backend_runtime.cc` 的 compute/load/store 事件推进。
- C MatrixReg conflict 当前选择让同 parity read/write 都 stall，由调用侧在后续 runtime 集成时决定重试/优先级。

## 2026-05-18 Phase 4A compute read frontend integration

### Why This Update Exists
- 上一轮只建立了 `MatrixRegResource` helper；本轮把 helper 接入 compute runtime 的前端读路径，先收 `ADC/BDC/CDC` 同拍 MatrixReg read request 与 1-cycle response 边界。
- 目标是消除旧的 `ADC -> BDC -> MTE` 串行前端，同时保持 `executeMma()` 仍只在 MTE/tail 边界做一次功能摘要。

### What Changed
- [src/matrix/backend_runtime.cc](/nfs/home/hujun/GEM5/src/matrix/backend_runtime.cc)
  - Compute task issue 后同拍向 A/B/C MatrixReg 发起 full-bank read request。
  - 三路 read response 都在下一拍到齐后，才释放 A/B/C pending reader 并进入 MTE。
  - C MatrixReg 同 parity write 与 CDC read 同拍冲突时，compute read 前端 stall/defer；不会静默吞掉 RTL assert 语义。
- [src/matrix/detailed_cute_scoreboard.hh](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_scoreboard.hh) / [src/matrix/detailed_cute_scoreboard.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_scoreboard.cc)
  - 新增 `onComputeReadFinishC()`，把 C source pending reader 的释放从 compute writeback 阶段提前到 CDC read response 阶段。
- [src/matrix/matrix_ex.cc](/nfs/home/hujun/GEM5/src/matrix/matrix_ex.cc)
  - `MteTiming` 记录 `cdcReadCycles=1`。
  - `totalCompletionCycles` 使用并行 read frontend 的最大 read latency，而不是把 ADC/BDC 串行相加。
- [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc)
  - 新增 `ComputeReadFrontendIssuesAdcBdcCdcInParallel`。
  - 新增 `ComputeReadFrontendStallsOnCWriteConflict`。
  - 更新旧子单元测试，区分 CDC read busy 与 CDC writeback busy。

### Current Boundary
- 本轮只接入 compute 前端 read resource 与 C read/write conflict。
- `executeMma()` 仍保持最终功能摘要；没有实现逐拍 FReducePE 数值网络。
- MLOAD/MSTORE 仍未实现 external `64B/cycle` memory request window、fixed memory latency、fill-table/sourceId/cache backpressure。
- CDC D writeback 仍未建成 `512` D beat window；当前只保留既有 writeback/terminal boundary。

## Why This Update Exists
- 上一轮 reviewer 对阶段 E 的 clean path 实现基本认可，但按 `Plan.md` 的最少证据要求仍给出 `REQUEST_CHANGES`。
- 缺的不是主代码骨架，而是两类动态证据：
  1. `mls` 在 `fault / flush / cancel / redirect` 负路径下不会误入 `toAMU / backend-visible`
  2. `mrelease / macquire` 在 CPU→buffer 边界上的顺序证据
- 这次 update 不改主代码，只补证据与文档。

## Stage E Main Code State
- 阶段 E 主代码仍是：
  - `src/arch/riscv/isa/decoder.isa`
  - `src/cpu/o3/mls_unit.cc`
  - `src/cpu/o3/rob.cc`
  - `src/cpu/o3/cpu.cc`
- 这轮没有新的代码逻辑改动，只有补充验证与复审材料。

## Existing Stage E Behavior

### 1. O3 `mls` request owner 已迁到 `DynInst / ROB / commit`
- O3 `mls` execute 不再直接 `isa->recordMatrixLsuRequest(req)`。
- 改为：
  - decoder 先 `stageMatrixExecPayload(payload)`
  - `MlsUnit` 从 staged payload finalize
  - ROB writeback 记录 payload
  - commit 后 `toAMU proxy` 才把 `Lsu` request 变成 backend-visible stub

### 2. commit-side proxy 边界
- `ROB::MatrixAmuEntry` 现在跟踪 `mls`
- `cpu->consumeMatrixAmuProxy()` 的 `Kind::Lsu` 已落地
- clean path 已证明：
  - payload staged
  - ROB writeback
  - ROB committed
  - `toAMU proxy ready/fire`
  - backend-visible

## New Evidence Added In This Round

### A. `mls` 参数 fault 负路径
- 程序：
  - `/tmp/matrix_lsu_fault_probe.S`
  - `/tmp/matrix_lsu_fault_probe`
- 日志：
  - `/tmp/gem5-matrix-fault-e1/matrix_lsu_fault_e1.log`
- 关键现象：
  - `sn:8` 先有 `Matrix AMU entry alloc`
  - `93500`: `Matrix AMU entry writeback suppressed [sn:8] fault=1 payloadValid=0`
  - 对同一 `sn:8`：
    - `Matrix toAMU proxy fire` 次数为 0
    - `Matrix LSU backend-visible after toAMU proxy` 次数为 0
- 说明：
  - faulted `mls` 已被 commit-side suppress
  - 没有误进入 backend-visible 路径
- 备注：
  - 该 probe 最终以 illegal instruction panic 结束，但在 panic 前已经收集到所需边界证据

### B. `mls` redirect / squash 负路径
- 程序：
  - `/tmp/matrix_lsu_cancel_probe_e.S`
  - `/tmp/matrix_lsu_cancel_probe_e`
- 日志：
  - `/tmp/gem5-matrix-cancel-e1/matrix_lsu_cancel_e1.log`
- 关键现象：
  - `sn:121` 先有 `Matrix AMU entry alloc`
  - `509000`: `Matrix AMU entry squash [sn:121] writebacked=0 committed=0`
  - 对同一 `sn:121`：
    - `Matrix toAMU proxy fire` 次数为 0
    - `backend-visible` 次数为 0
- 说明：
  - redirect/squash 下，`mls` AMU entry 能被撤销
  - 不会误入 commit-side proxy

### C. `mrelease -> macquire` 顺序证据
- 程序：
  - `/tmp/matrix_release_acquire_probe.S`
  - `/tmp/matrix_release_acquire_probe`
- 日志：
  - `/tmp/gem5-matrix-release-e1/matrix_release_acquire_e1.log`
- 关键顺序：
  - `92000`: `sn:2` release commit
  - `92000`: `Matrix release temporary token update [sn:2] token=3 value=0x1`
  - `92000`: `Matrix toAMU proxy fire [sn:2] payload=release`
  - `92000`: `Matrix release backend-visible after toAMU proxy [sn:2] token=3`
  - `94000`: `sn:3` macquire execute
  - `95000`: `sn:3` macquire commit
- 说明：
  - release 先经过 commit-side proxy/backend-visible
  - 之后 acquire 才在 CPU 控制路径放行

## Behavioral Notes
- 现在阶段 E 已经同时具备：
  - clean path commit-boundary 证据
  - fault negative-path 证据
  - squash/cancel negative-path 证据
  - `mrelease/macquire` 顺序证据
- 仍然没有补的内容：
  - 非 O3 fallback 收口
  - 更复杂的地址 fault / LSQ fault 归属
  - 更完整 Phase 2.7 exception ownership 报告

## Review Status
- 复审 reviewer subagent 已返回正式结论。
- Verdict:
  - `APPROVE`

### Reviewer Evidence
1. `mls` clean path 已满足：
   - `payload staged -> writeback -> committed -> toAMU proxy ready -> toAMU proxy fire -> backend-visible`
2. `fault` 负路径已满足：
   - `writeback suppressed`
   - 无 `toAMU proxy fire`
   - 无 backend-visible
3. `cancel/squash` 负路径已满足：
   - `Matrix AMU entry squash`
   - 无 `toAMU proxy fire`
   - 无 backend-visible
4. `mrelease/macquire` 已补齐 CPU→buffer 顺序证据：
   - release 先 commit / token update / toAMU fire
   - acquire 之后才 execute / commit

### Reviewer Issues
1. `Severity: recommendation`
   - `Location`: `verify_report.md`, `/tmp/gem5-matrix-fault-e1/matrix_lsu_fault_e1.log`
   - `Problem`: 当前 fault probe 最终以 illegal instruction panic 结束，且还没有独立 address fault / LSQ fault 证据。
   - `Why it matters`: 这影响后续若把评审范围扩到完整 `2.7 exception ownership` 时的覆盖完整度，但不影响本轮 blocker 是否解除。
   - `Suggested follow-up`: 若后续继续扩到完整 `2.7` 收口，再补 dedicated address-fault 与 LSQ-fault probes。

### Reviewer Blocking Issues
- `None`

## Recommended Next Step
1. 允许把本轮证据更新作为阶段 E evidence checkpoint 提交。
2. 若后续继续推进完整 `2.7` exception ownership，再补 address fault / LSQ fault probes。

---

## 2026-05-07 `src/matrix` detailed CUTE 进展

### Why This Update Exists
- 这一轮目标不是再扩 ISA/O3 路径，而是把 `src/matrix` 里的 detailed CUTE backend 往 RTL/CUTE 口径继续推进。
- 重点收了三类东西：
  1. `AML/BML/CML` 与 `ADC/BDC/CDC/MTE` 的职责拆分
  2. `ComputeGo / EndReady` 风格的事件边界
  3. 更接近标量风格的代码组织与 trace 命名

### What Changed
- `DetailedCuteBackend` 现在的实现已拆分到：
  - `src/matrix/taskcontrol.cc`
  - `src/matrix/backend_runtime.cc`
  - `src/matrix/mregfile.cc`
  - `src/matrix/memoryload.cc`
  - `src/matrix/matrix_ex.cc`
- `src/matrix/detailed_cute_backend.cc`
  - 已不再参与 build，属于旧残留实现
- `taskcontrol.cc`
  - 负责 `decoded fifo + scoreboard + headReady + issueHead + dispatchTask`
- `backend_runtime.cc`
  - 负责 inflight/runtime/event/completion 流
- `memoryload.cc`
  - 负责 `AML/BML/CML` 相关 load/store 路径
  - 当前新增共享 `memoryBudget`，开始近似建模 shared memory path 竞争
- `matrix_ex.cc`
  - 负责 `ADC/BDC/CDC/MTE` 对应的 compute 路径
  - 当前 `computeDatatypeSupported()` 已接受更接近 RTL 的整数编码子集
  - `fp mma` 现在至少能进入 execute boundary，再由当前 kernel 返回 `Unsupported`

### Current Alignment Status
- 已明显收敛的部分：
  - `TaskController` 风格的 `head -> scoreboard -> ready -> issue`
  - `AML/BML/CML` 三路分流
  - `ADC/BDC/CDC/MTE` 的阶段映射，且已拆成显式子单元状态与对应 trace/counter
  - `compute` 路径已具备最小多 request 前级重叠，`MTE/CDC` 仍保留单槽 backpressure
  - scoreboard 的 `load_finish / compute_read_finish_a/b / compute_write_finish_c / store_finish`
  - trace 命名风格往标量写法靠拢，活动源文件里不再依赖那批字符串 helper
- 仍然只是阶段性近似的部分：
  - 端口级 valid/ready 真实握手
  - 真实 load/store 带宽与 bank 竞争
  - `ADC/BDC/CDC/MTE` 的完整独立实体化和多 compute in flight
  - fp mma 功能闭环

### 2026-05-08 follow-up: compute subunit entityization

- 这轮继续收了 `ADC/BDC/CDC/MTE` 的独立化，但只收到了“显式子单元状态”这一级。
- 当前实现已做到：
  - MMA 路径不再只依赖一个粗粒度 `computeTask` stage 枚举
  - `ADC / BDC / MTE / CDC` 各自有单独 busy / issue / finish 记录
  - 对应单测可以直接观测子单元推进顺序
- 当前仍未做到：
  - 完整多 compute request 并行实体化
  - RTL 级子单元 ready/valid 背压
  - RTL 级真实并行/重叠关系

---

## 2026-04-24 Detailed CUTE Phase 2 续推

### Why This Update Exists
- `Plan.md` 所对应的 detailed backend 计划进入该阶段后，最先缺的是 backend 抽象层和最小 backpressure 语义。
- 如果 commit 继续“先 pop 再 submit”，后面接 detailed FIFO/scoreboard 时会过早消费 `MatrixAmuEntry`，不符合 `toAMU` 反压方向。

### What Changed
- 新增 `src/matrix/matrix_backend.hh`
  - 抽出 `MatrixBackend` 抽象接口：`canAccept/submit/hasWork/step/hasCompletion/popCompletion`
- 修改 `src/matrix/functional_cute_backend.*`
  - `FunctionalCuteBackend` 现在实现 `MatrixBackend`
  - 新增可选 `maxQueueDepth`
  - `canAccept()` 允许 functional backend 承担最小 queue/backpressure 角色
- 修改 `src/cpu/o3/{cpu,rob,commit}.*`
  - `CPU` 持有 `std::unique_ptr<matrix::MatrixBackend>`
  - 新增 `buildMatrixBackendReq()` 与 `canAcceptMatrixBackendReq()`
  - `ROB` 新增 `peekReadyMatrixAmuEntry()`
  - `Commit::commitInsts()` 改成 `peek -> canAccept -> pop -> consume`

### Current Code State
- 这次改动只完成 Phase 2 的最小骨架：
  - 已有 pluggable backend interface
  - 已有 commit-side accept gating
  - 已有 functional backend queue-depth backpressure
- 还没做：
  - detailed `AMUCtrlBuffer`
  - decoded FIFO
  - scoreboard
  - micro task timing

### 2026-04-25 follow-up
- 新增 `BaseO3CPU.matrixBackendQueueEntries`
  - 当前 functional backend 的 queue depth 终于可从 config 层设置
- 新增 `MatrixCuteTrace`
  - 在 `Commit / CPU / FunctionalCuteBackend` 上补了窄口径 backend trace
- 修改 `configs/example/matrix_seq_o3.py`
  - 新增 `--matrix-backend-queue-entries`
  - 便于直接在 SE smoke 下压 queue depth，而不改默认配置脚本

### 2026-04-25 release path cleanup
- 删除了 `Commit::commitInsts()` 中对 `Release` 的直接 `commitMatrixReleaseToken()` 调用
- `CPU::consumeMatrixAmuProxy()` 的 `Release` 路径现在会：
  - `submitMatrixBackendReq()`
  - 记录 `release submit`
  - 等待 backend completion 后才更新 token
- 这一步把 `Release` 从“commit 侧特殊路径”收回到统一 backend contract

### 2026-04-25 drain / serialize contract
- `CPU::isCpuDrained()` 现在会把以下状态算进 drain 判定：
  - `matrixBackend->hasWork()`
  - `matrixBackend->hasCompletion()`
  - `matrixBackendOwners`
- `CPU::serializeThread()` 现在在 backend 仍有 pending state 时会 fail-fast
- 这一步的目的不是实现 backend checkpoint，而是先阻止“带着未完成 backend state 进入 checkpoint”这种错误状态

### 2026-04-25 AMU shadow approximation
- `MatrixAmuBuffer` 当前不再使用旧 `buffered` 语义
- 当前 carrier 语义改为：
  - `alloc` 时占用 `ROB-parallel shadow` entry
  - `writebacked` 代表 payload ready
  - `committed` 代表已越过 commit 边界
  - `canDeq` 代表已 fire 或不再需要 AMU，可在 front cleanup 时释放
- `matrixAmuBufferEntries`
  - 当前限制的是 `shadow buffer` live entry 数量
- 当前顺序语义：
  - `peekReady/popReady` 在 oldest window 中找 ready entry
  - `cleanupFront()` 释放 head 侧连续 `canDeq` entry
  - 这仍是阶段性近似，不应写成 `AMUCtrlBuffer` 已结构对齐 RTL

### 2026-04-25 split out `matrix_amu_buffer`
- 新增：
  - `src/cpu/o3/matrix_amu_buffer.hh`
  - `src/cpu/o3/matrix_amu_buffer.cc`
- `ROB` 现在通过独立 `MatrixAmuBuffer` 做：
  - allocate
  - writeback note
  - commit note
  - ordered peek/pop
  - squash / cleanup
- 这一步的目的，是把 AMUbuffer 生命周期从 `rob.cc` 杂糅逻辑里拆出来，给后续 `AMUCtrlBuffer` 风格细化打底

## 2026-04-28 Detailed CUTE Phase 3A shell

### Why This Update Exists
- Phase 3 需要先把 `detailed-cute` backend 作为独立实现壳接进 GEM5。
- 当前目标仍是最小可验证切片：
  - 保持 CPU 边界 contract 不变
  - 新增 backend 模式切换
  - 落 `decoded_fifo + matrix_regfile shell + scoreboard shell`
  - 暂不进入真实 microtask timing / LocalMMU / cache hierarchy

### What Changed
- 新增：
  - `src/matrix/detailed_cute_backend.hh`
  - `src/matrix/detailed_cute_backend.cc`
  - `src/matrix/detailed_cute_backend.test.cc`
  - `src/matrix/decoded_fifo.hh`
  - `src/matrix/matrix_regfile.hh`
  - `src/matrix/detailed_cute_scoreboard.hh`
- 修改：
  - `src/matrix/SConscript`
  - `src/cpu/o3/BaseO3CPU.py`
  - `src/cpu/o3/cpu.cc`
  - `configs/example/matrix_seq_o3.py`

### Current Code State
- `DetailedCuteBackend`
  - 外部继续实现 `MatrixBackend`
  - 内部先接：
    - `DecodedFifo`
    - `MatrixRegFile`
    - `DetailedCuteScoreboard`
    - `MatrixKernels`
- `DecodedFifo`
  - 采用 `std::deque<DecodedFifoEntry> + depth guard`
  - 默认 depth 为 `8`
- `MatrixRegFile`
  - 当前是 backend-owned shell
  - 先包 `MatrixState`
  - 暂不做 bank / port / loader / compute datapath
- `DetailedCuteScoreboard`
  - 当前只实现最小 `canIssue/onIssue/onCompletion`
  - 先覆盖：
    - AB/C busy
    - FU busy
    - pending readers 的最小记账
- `BaseO3CPU` 新增 `matrixBackendMode`
  - `functional`
  - `detailed-cute`
- `matrix_seq_o3.py` 新增：
  - `--matrix-backend-mode`

### Important Constraints
- 新 `detailed-cute` 当前是 `no-timing shell`
- 不改：
  - `decode/rename/csr/mset`
  - `L2Cache`
  - O3 PRF / rename scoreboard

### 2026-04-28 follow-up: Phase 3B semantics
- `DetailedCuteBackend` 不再在同一步里 `issue -> completion`
- 新增最小 `inflight` 语义：
  - `step()` 先看队首能否 issue
  - issue 后进入 `inflight`
  - completion 在后续 step 才可见
- `DecodedFifo`
  - 当前不仅是 queue 壳，还承担：
    - request decode normalization
    - 队首顺序保持
    - 与 backend step 分离的 head gating 基础
- `release`
  - 当前最小语义已体现 `pending store` 阻塞
  - 还不是完整 release/store-drain timing

### 2026-04-28 follow-up: Phase 3C scoreboard semantics
- `DetailedCuteScoreboard` 不再只是 `busy + pending counter`
- 新增更接近 CUTE 的内部状态：
  - reg state:
    - `busy`
    - `writer`
    - `pendingReaders`
  - fu state:
    - `busy`
    - `destValid/destBank/destReg`
    - `srcs[3]`
  - src state:
    - `valid`
    - `bank`
    - `reg`
    - `ready`
    - `waitFu`
    - `readPending`
- 仍保持外部接口不变：
  - `canIssue(entry)`
  - `onIssue(entry)`
  - `onCompletion(entry)`
- 当前实现重点：
  - load reserve 会阻塞依赖它的 compute
  - store 作为 reader 会阻塞对同一 `C` reg 的后续覆盖
  - producer release 时会做最小 wakeup

### 2026-04-28 follow-up: FIFO / scoreboard trace
- `DetailedCuteBackend::step()` 新增最小窄口径 trace：
  - `fifo_enq`
  - `fifo_deq`
  - `fifo_block`
  - `scoreboard_block`
  - `backend completion`
- `DetailedCuteScoreboard` 新增：
  - `BlockReason`
  - `blockReason(entry)`
  - `blockReasonName(...)`
- 当前阻塞原因口径先覆盖：
  - `fu_busy`
  - `dest_busy`
  - `dest_pending_readers`
  - `src_not_ready`
  - `src_pending_readers`
  - `release_pending_store`
  - `downstream_not_accepting`
- `src/matrix/SConscript`
  - 两个 GTest target 现已显式带上 `with_tag('gem5 trace')`
  - 这样 matrix backend unit test 可以在启用 DPRINTF 后继续链接通过

### 2026-04-28 follow-up: Phase 3D matrix_regfile
- `MatrixRegFile` 不再只是包一层 `MatrixState`
- 新增 backend-owned AB/B/C reg namespace 与 metadata：
  - `allocated`
  - `owner`
  - `lastWriterKind`
- 新增：
  - `src/matrix/matrix_regfile.cc`
- `MatrixKernels`
  - 现同时支持：
    - `MatrixState`
    - `MatrixRegFile`
  - `DetailedCuteBackend` 改走 `MatrixRegFile`
- `DetailedCuteBackend`
  - issue 时会给写目标 reg 记 `owner`
  - completion 时清掉 `owner`
  - load/mma/arith 写入时会更新 `lastWriterKind`
- 当前 3.D 仍然不做：
  - bank / port / arbitration
  - LocalMMU / memory fill path
  - Phase 4 timing

### 2026-04-28 follow-up: Phase 3 remaining gaps
- `DetailedCuteScoreboard` 已继续补细：
  - `load_finish_a/b/c`
  - `compute_read_finish_a/b`
  - `compute_write_finish_c`
  - `store_finish`
- `DetailedCuteBackend`
  - 现已采用分阶段 inflight 生命周期：
    - load / store / arith / release 的 finish 延后
    - compute 的 readA -> readB -> writeC 分步完成
  - `scoreboard_block` / `fifo_block` 现在可通过 `BlockReason` 分类
- 新增单测覆盖：
  - `DetailedCuteScoreboard.ComputeLifecycleClearsReadersInStages`
  - `DetailedCuteBackend.ScoreboardBlockCounterTracksSrcNotReady`

### 2026-04-30 follow-up: Phase 4.0 / 4.1 doc freeze
- 本轮没有进入新的 backend 代码实现，只做 `Phase 4` 文档冻结。
- `Plan.md`
  - `4.0` 已标记为 `cc:完成`
  - `4.1` 也已从 `cc:WIP` 收口为 `cc:完成`
- 新增/更新：
  - AML/BML/CML 与 matrix regfile 设计摘要
- 当前冻结下来的关键口径：
  - `AML/BML/CML` 的职责、`read finish / write finish / completion_event` 边界已单独成文
  - `matrix_regfile` 的聚合 beat 宽度已冻结为：
    - `AB`: `1024b/cycle`
    - `C`: `512b/cycle`
  - 上述数值只代表单 channel 的拍级口径：
    - 不代表同拍多 channel 并发能力
    - 不代表 bank conflict / arbitration 已建模
- 本轮特意没有做的事：
  - 不改 CPU core path
  - 不提前实现 `4.2` microtask 壳

### 2026-04-30 follow-up: Phase 4.2-5 functional completion
- `DetailedCuteBackend`
  - `load/store/zero/release` 已统一走 `TaskSlot + TaskEvent` 回路
  - `mma` 也已从旧 `inflight` 壳迁入统一 compute task
- 当前 `mma` 最小状态机为：
  - `Accepted`
  - `ComputeReadA`
  - `ComputeReadB`
  - `ComputeExecute`
  - `ComputeWriteC`
  - `TerminalCompletion`
- `ComputeReadA/ReadB`
  - 会先 snapshot `A/B` reg 数据
  - 再分别触发：
    - `scoreboard.onComputeReadFinishA`
    - `scoreboard.onComputeReadFinishB`
- `ComputeWriteC`
  - 才真正把结果写回 `matrix_regfile`
  - 然后通过 `TaskEvent` 触发：
    - `scoreboard.onComputeWriteFinishC`
    - backend completion queue enqueue
- 当前第一版 compute timing 公式：
  - `Matrix_MN = 4`
  - `ReduceWidthByte = 32`
  - `m_iters = ceil(mtilem / Matrix_MN)`
  - `n_iters = ceil(mtilen / Matrix_MN)`
  - `k_iters = ceil(mtilek / ReduceWidthByte)`
  - `compute_execute_latency = max(1, m_iters * n_iters * k_iters)`
- 当前 datatype 策略：
  - Required:
    - `int8 -> int32`
  - 当前一律返回 `Unsupported`:
    - `isFp == true`
    - 非零 `types1/types2/typed`
- `mzero`
  - 继续保留在 AML/CML zero-load family
  - 不进入 compute family
- 本轮按用户要求以 `functional-only` 收口：
  - 不补 trace
  - 不宣称 RTL cycle 等价

## 2026-05-12 SE gemm runtime unblock

### 变更摘要
- 把 `MatrixRegFile` 的默认 `AB/C` 寄存器槽位从 `4` 提到 `8`，对齐当前 ISA 里 `md/ms1/ms2/ms` 的 `& 0x7` 编码范围。
- 新增回归测试 `DetailedCuteBackend.DefaultBackendAcceptsArchitecturalRegIndexFour`，覆盖默认 backend 处理 `md=4` 的路径。

### 影响文件
- [src/matrix/matrix_regfile.hh](/nfs/home/hujun/GEM5/src/matrix/matrix_regfile.hh:57)
- [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc:278)

## 2026-05-12 code style review

### Review Target
- Task:
  - Phase 3 WIP follow-up: SE `gemm_precomp` runtime unblock
- Review type:
  - task-level code style review
- Claimed transition:
  - `WIP -> WIP`

### Required Gates

| Gate | Result | Evidence | Issue if FAIL |
|---|---|---|---|
| Scope matches current task fix | PASS | 只改默认寄存器槽位和一条回归测试，没有扩到 backend 结构重排。 | - |
| Local style evidence is concrete | PASS | [src/matrix/matrix_regfile.hh](/nfs/home/hujun/GEM5/src/matrix/matrix_regfile.hh:57) 只改两个常量；[src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc:278) 只补最小回归。 | - |
| No unnecessary abstraction / wrapper growth | PASS | 本轮没有新增 helper、class 或转发层。 | - |
| Active build file remains the main entry | PASS | 主实现入口仍是 active build 的 `taskcontrol.cc/backend_runtime.cc/mregfile.cc/memoryload.cc/matrix_ex.cc`，本轮未改入口关系。 | - |

### Review Dimensions

| Dimension | Result | Evidence | Issue if FAIL / N/A reason |
|---|---|---|---|
| Design Complexity | PASS | 变更面只在默认槽位和单测回归，符合“小步、可验证、可回退”。 | - |
| Code Style | PASS | 命名直接、修改局部，没有为修一个越界引入额外状态或包装层。 | - |
| RTL Alignment | N/A | Task-level style review does not approve RTL/CUTE alignment. | - |

### Evidence
- [src/matrix/matrix_regfile.hh](/nfs/home/hujun/GEM5/src/matrix/matrix_regfile.hh:57)
- [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc:278)

### Issues
- None

### Blocking Issues
- None

### Required Document Updates
- `cute_detailed_design/verify_report.md`
- `cute_detailed_design/handoff.md`

### Recommended Action
- 保持 `Plan.md` 状态不变，只把这次 runtime unblock 的证据和停点回写到实现/验证/handoff 文档。

### Verdict

`APPROVE`

## 2026-05-18 RISC-V matrix load/store encoding unblock

### 变更摘要
- 修正 RISC-V matrix load/store decode 与 O3 MLS payload 解释口径：
  - `funct7=0x02/0x0a/0x12/0x13` 分别表示 A load、B load、C load、C store 类。
  - raw `RD[4:3]` 表示 width。
  - raw `RD[2:0]` 表示 matrix reg index。
- `0x24a48a2b` 现在按 QEMU/toolchain 口径解为 `mlce32 acc0,(s1),a0`，不再被旧 GEM5 路径误判成 `mlae16`。
- O3 `MlsUnit` 的 early fault、access size、shape 派生、payload flags/width/elemType 同步改为同一字段解释。
- `matrix_sync_check.cc` 增加 workload 实际机器码覆盖；standalone checker 目标仍受既有链接缺依赖限制，不能作为本轮绿灯。

### 影响文件
- [decoder.isa](/nfs/home/hujun/GEM5/src/arch/riscv/isa/decoder.isa)
- [dyn_inst.cc](/nfs/home/hujun/GEM5/src/cpu/o3/dyn_inst.cc)
- [mls_unit.cc](/nfs/home/hujun/GEM5/src/cpu/o3/mls_unit.cc)
- [matrix_sync_check.cc](/nfs/home/hujun/GEM5/src/arch/riscv/matrix_sync_check.cc)

### 边界
- 本轮是 RISC-V/O3 matrix ISA decode 与 SE functional unblock。
- 不声明 MatrixReg bank timing、MTE accepted beat、CDC writeback timing 或 RTL timing equivalence 已收口。

## 2026-05-18 O3 MMA opcode metadata passthrough

### 变更摘要
- 修复 O3 `ROB -> CuteRequest` 路径的 MMA opcode metadata 透传缺口。
- `decoder.isa` 已经把 `mfmacc_s_h` 的 staged `payload.op` 设为 `0x04`，但旧 `ROB::toCuteRequest()` 通过 `CuteRequest::makeMma()` 构造 request 后没有覆盖默认 `req.op = 0x0c`。
- 新增 `src/cpu/o3/matrix_payload.hh/.cc`，把原本藏在 `rob.cc` 匿名 namespace 的 `MatrixExecPayload -> CuteRequest` 转换提成可测试 helper。
- MMA 分支现在明确执行 `out.op = payload.op`，因此 `mfmacc_s_h` 的真实 O3/AMU backend request 不再被 `makeMma()` 的 int MMA 默认 opcode 覆盖。
- 新增 `src/cpu/o3/matrix_o3_payload.test.cc`，focused 覆盖 FP MMA payload `0x04` 到 `CuteRequest.op` 的转换。

### 影响文件
- [matrix_payload.hh](/nfs/home/hujun/GEM5/src/cpu/o3/matrix_payload.hh)
- [matrix_payload.cc](/nfs/home/hujun/GEM5/src/cpu/o3/matrix_payload.cc)
- [matrix_o3_payload.test.cc](/nfs/home/hujun/GEM5/src/cpu/o3/matrix_o3_payload.test.cc)
- [rob.cc](/nfs/home/hujun/GEM5/src/cpu/o3/rob.cc)
- [SConscript](/nfs/home/hujun/GEM5/src/cpu/o3/SConscript)

### 边界
- 本轮只修 RISC-V/O3 matrix backend request metadata 透传。
- 不恢复 `xmxrm/xmfrm/xmsaten` renamed carriers，不引入 `XMCSR` composite carrier。
- 不推进 MatrixReg/MTE bank timing 主线；该主线仍按 `plan.md` 后续 Milestone 执行。

## 2026-05-19 CDC tile-level D beat writeback

### 变更摘要
- `MteTiming.cdcWriteCycles` 仍按 `acceptedInputBeats` 建模，full int8 `128x64 * 64x128` 是 `512` 个 CDC D writeback beats；`256` 只表示 C/D 地址 tile 数。
- int8 path 的 CDC writeback 从“窗口结束后一次性 `executeMma()` 覆盖 C”改为 tile-level RMW：
  - 每个 D beat 先对 CRegFile 发一次 full-bank C tile read。
  - read response 到达后，在 backend 内部计算一个 `8x8xInt32` updated C tile。
  - 再发一次 full-bank C tile write；write grant 成功后才写回 internal C tensor 并推进 `done/remaining`。
- D beat index 现在按 RTL `CDataController` 口径映射：
  - `addr = dBeatIndex % (mTiles * nTiles)`
  - `kGroup = dBeatIndex / (mTiles * nTiles)`
  - `mTile = addr / nTiles`
  - `nTile = addr % nTiles`
- full int8 all-ones、zero C 时，第 256 个成功 D beat 后，internal C tile `(0,0)` 已可见 partial `32`；第 512 个成功 D beat 后为 final `64`。
- int8 tile-level terminal 不再再次调用 full `executeMma()` 覆盖 C；非 int8 path 继续保留既有 `executeMma()` fallback/oracle。
- scoreboard 仍阻止 store/MMA/release 在 compute write finish / terminal 前消费 partial C。

### 影响文件
- [backend_runtime.cc](/nfs/home/hujun/GEM5/src/matrix/backend_runtime.cc)
- [detailed_cute_backend.hh](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.hh)
- [detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc)

### 边界
- 覆盖 int8 CDC tile-level functional writeback、CRegFile read-modify-write resource boundary、C MatrixReg write grant/stall、partial C scoreboard 外部可见性边界。
- 不覆盖 MLOAD/MSTORE `64B/cycle` external memory window。
- 不声明 RTL timing equivalence。

## 2026-05-19 LocalMMU matrix load/store timing

### 变更摘要
- 新增 `LocalMmuModel`，作为 `src/matrix` 内部的固定延迟 LocalMMU timing model：
  - 每个 backend cycle 最多 issue 一个 request beat。
  - 每个 beat 最大 `64B`。
  - source ID / issued outstanding 上限可配置，默认 `64`。
  - fixed latency 后产生 read response 或 store ack。
- `DetailedCuteBackend` 新增 `TimingConfig`，默认构造保持兼容；测试可配置 `localMmuLatencyCycles` 和 `localMmuMaxOutstanding`。
- MLOAD/MSTORE 现在按 `row * column * elemBytes` 拆成 `64B` LocalMMU beats。
- load completion 边界拆成两层：
  - 所有 LocalMMU read responses 到达后，执行一次 `MatrixMemoryAdapter::loadTile()` 形成功能 snapshot。
  - 每个 `64B` response 再消耗 `2 x 32B` MatrixReg `MemoryLoader` write chunks，全部 grant 后才让目标 matrix register backend-visible。
- store path 在 C register snapshot 后 enqueue LocalMMU store beats；`MatrixMemoryAdapter::storeTile()` 和 store completion 等待所有 store acks。
- release gating 继续等待 backend drain / pending store；`pendingStoreCount` 只在 store ack 后的 write finish 阶段递减。
- loader write chunks 接入现有 `MatrixRegResource`，因此 A/B loader write priority 可 stall compute read frontend。

### 影响文件
- [local_mmu_model.hh](/nfs/home/hujun/GEM5/src/matrix/local_mmu_model.hh)
- [local_mmu_model.cc](/nfs/home/hujun/GEM5/src/matrix/local_mmu_model.cc)
- [SConscript](/nfs/home/hujun/GEM5/src/matrix/SConscript)
- [detailed_cute_backend.hh](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.hh)
- [taskcontrol.cc](/nfs/home/hujun/GEM5/src/matrix/taskcontrol.cc)
- [backend_runtime.cc](/nfs/home/hujun/GEM5/src/matrix/backend_runtime.cc)
- [memoryload.cc](/nfs/home/hujun/GEM5/src/matrix/memoryload.cc)
- [mregfile.cc](/nfs/home/hujun/GEM5/src/matrix/mregfile.cc)
- [detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc)

### 边界
- 本轮只实现第一阶段 LocalMMU / matrix load-store timing。
- 不实现 CDC tile-level functional writeback 的新增内容。
- 不新增真实 L2 / `RequestPort` / `Packet` / `recvTimingResp()` / retry。
- 不实现 cache tag、hit/miss、MSHR、coherence 或 replacement。
- `MatrixMemoryAdapter` 仍负责功能数据读写；LocalMMU 只控制 request issue、response/ack、completion 和 MatrixReg resource timing boundary。

## 2026-05-19 LocalMMU trace counters

### 变更摘要
- 在 `DetailedCuteBackend::TraceCounters` 中补充 LocalMMU / loader-fill 观测项：
  - `localMmuLoadBeatsEnqueued`
  - `localMmuStoreBeatsEnqueued`
  - `localMmuBeatsIssued`
  - `localMmuReadResponses`
  - `localMmuStoreAcks`
  - `matrixRegLoaderWriteChunksQueued`
  - `matrixRegLoaderWriteChunksGranted`
  - `matrixRegLoaderWriteChunksStalled`
- `MatrixCuteTrace` 新增 LocalMMU/load-fill 时间线事件：
  - `local_mmu_enqueue`
  - `local_mmu_issue`
  - `local_mmu_response`
  - `matrix_reg_loader_write_grant`
  - `matrix_reg_loader_write_stall`
- 新增 focused tests 覆盖 load fill 与 store ack 计数，确保 trace counters 不只是字段声明。

### 影响文件
- [detailed_cute_backend.hh](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.hh)
- [backend_runtime.cc](/nfs/home/hujun/GEM5/src/matrix/backend_runtime.cc)
- [memoryload.cc](/nfs/home/hujun/GEM5/src/matrix/memoryload.cc)
- [detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc)

### 边界
- 本轮只补观测点，不改变 LocalMMU fixed-latency 行为。
- 不接入真实 L2 / RequestPort / Packet / retry。
- 不建模 RTL `MReg_Fill_Table` / per-bank fill FIFO depth 与 backpressure。

## 2026-05-20 LocalMMU requester/source arbitration

### 变更摘要
- `LocalMmuModel` pending requests 从单个 FIFO 改为 `AML/BML/CML` 三路队列。
- 每个 backend cycle 按 CUTE `LocalMMU.scala` 口径使用 rotating first requester：
  - 当前 priority 顺序从 `firstRequestIndex` 开始扫描 `AML/BML/CML`。
  - cycle 末 `firstRequestIndex` 自增。
  - 三路同时 valid 时呈现 `AML -> BML -> CML` 轮询。
- source ID 分配从最低空闲改为最高空闲，匹配 CUTE `CUTE2TLImp` 中 `for(i <- 0 until LLCSourceMaxNum)` 后写覆盖后的有效行为。
- 新增 focused tests 固定 requester rotation 与最高空闲 source ID 口径。

### 影响文件
- [local_mmu_model.hh](/nfs/home/hujun/GEM5/src/matrix/local_mmu_model.hh)
- [local_mmu_model.cc](/nfs/home/hujun/GEM5/src/matrix/local_mmu_model.cc)
- [detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc)

### 边界
- 只对齐 LocalMMU requester/source arbitration。
- 仍不实现真实 L2 / `RequestPort` / `Packet` / retry。
- 仍不建模 TL response arbitration 里的 read-data-over-ack priority、cache hit/miss/MSHR/coherence。

## 2026-05-21 CML LocalMMU fill chunk entry/parity

### 变更摘要
- 修正 LocalMMU read response 生成 MatrixReg loader-fill chunk 的 metadata：
  - 每个 `64B` response 仍拆成 `2 x 32B` chunks。
  - 每个 pending chunk 现在记录对应 MatrixReg entry。
  - CML fill 的第二个 `32B` chunk 使用下一个 entry/parity，而不是继续复用第一个 chunk 的 entry。
- `serviceLsuMatrixRegWriteChunk()` 现在用 pending chunk entry 构造 `MatrixRegResource::makeWrite()`，让已有 C odd/even read/write conflict 模型看到正确 parity。
- 新增 focused regression：
  - `DetailedCuteBackend.CmlFillSecondChunkUsesNextMatrixRegEntryParity`

### 影响文件
- [detailed_cute_backend.hh](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.hh)
- [backend_runtime.cc](/nfs/home/hujun/GEM5/src/matrix/backend_runtime.cc)
- [memoryload.cc](/nfs/home/hujun/GEM5/src/matrix/memoryload.cc)
- [detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc)

### 边界
- 这不是 bounded `MReg_Fill_Table` / fill FIFO 实现。
- 仍假设 LocalMMU response always ready，source ID 不被 fill backpressure 持有。
- 仍不接入真实 L2 / TL / `RequestPort` / `Packet` / retry。

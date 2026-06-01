# GEM5 Matrix LocalMMU Timing Plan

## Goal Description / 目标描述

本计划恢复并执行第一阶段 CUTE LocalMMU / matrix load-store timing 建模：在 GEM5 matrix detailed backend 中增加 `64B` MLOAD/MSTORE request beat、AML/BML/CML 共享一个 LocalMMU 发射口、64 个 source ID、固定/可配置 memory latency、read response / store ack，以及 MatrixReg fill/store 与现有 `MatrixRegResource` 的资源冲突边界。

本阶段完全限制在 `src/matrix` 内，不新增 GEM5 timing `RequestPort`，不连接真实 L2，不实现 cache tag / hit-miss / MSHR / coherence / retry / replay / replacement。`MatrixMemoryAdapter` 继续负责功能数据正确性；新增 LocalMMU timing model 只控制 request issue、response/ack 到达、load/store completion 时刻和 MatrixReg 资源冲突。

当前 CDC tile-level per-D-beat functional writeback 暂不作为本轮执行对象；本轮目标是恢复之前的 LocalMMU/load plan 并开始执行它。

## Current Position / 当前进度

- 已完成的前置 checkpoint：
  - `MatrixRegResource` 已存在，表达 `8` banks、`32B` entry、full-bank grant、`1` cycle read response、A/B loader write priority、C odd/even read/write conflict。
  - compute read frontend 已接入现有 `matrixRegResource`：ADC/BDC/CDC 同拍 request，三路 response 到齐后才进入 MTE。
  - MTE full int8 accepted beats 与 CDC D beat timing window 已建模为 `512`。
  - O3 `ROB -> CuteRequest` MMA opcode metadata 已保留 `payload.op`。
- 当前已完成的 LocalMMU/load-store checkpoint：
  - `src/matrix/local_mmu_model.hh/cc` 已接入 active build。
  - `DetailedCuteBackend::TimingConfig` 已提供 LocalMMU latency / max outstanding 配置，默认构造保持兼容。
  - MLOAD/MSTORE 已按 `64B` LocalMMU beats enqueue。
  - load completion 等待所有 LocalMMU read responses，并通过 `2 x 32B` MatrixReg `MemoryLoader` chunks 后才让 destination register backend-visible。
  - CML fill chunk 已记录 MatrixReg entry/parity，避免 `64B -> 2 x 32B` 时第二个 chunk 复用错误 C parity。
  - store completion / release gating 已等待 LocalMMU store ack。
- 当前仍未完成 / 不在本阶段范围：
  - bounded `MReg_Fill_Table` / fill FIFO depth。
  - `response.ready = false` backpressure 与 source ID 被 fill backpressure 持有。
  - TL/LLC ready、真实 L2 / `RequestPort` / `Packet` / retry。
  - cache tag / hit-miss / MSHR / coherence / replacement。
- 当前 worktree 有未提交 planning/doc/code 改动；不要为了干净回滚。若发现本轮实现与已有改动重叠，先读 diff，沿现有改动继续。

## Context Files / 上下文文件

实现前必须读取：

- `task_plan.md`
- `findings.md`
- `progress.md`
- `AGENTS.md`
- `cute_detailed_design/implementation_notes.md`
- `cute_detailed_design/verify_report.md`
- `cute_detailed_design/handoff.md`
- `src/matrix/SConscript`
- `src/matrix/detailed_cute_backend.hh`
- `src/matrix/taskcontrol.cc`
- `src/matrix/backend_runtime.cc`
- `src/matrix/memoryload.cc`
- `src/matrix/mregfile.cc`
- `src/matrix/matrix_reg_resource.hh`
- `src/matrix/matrix_reg_resource.cc`
- `src/matrix/detailed_cute_backend.test.cc`

详细任务草稿来源：

- `/nfs/home/hujun/GEM5/.codex-worktrees/matrix-src-only-pr-v2/docs/superpowers/plans/2026-05-19-matrix-localmmu-timing.md`

按需读取 RTL/CUTE 证据：

- `/nfs/home/hujun/workspace/xsai/xsai-env/XSAI/CUTE/src/main/scala/AMemoryLoader.scala`
- `/nfs/home/hujun/workspace/xsai/xsai-env/XSAI/CUTE/src/main/scala/BMemoryLoader.scala`
- `/nfs/home/hujun/workspace/xsai/xsai-env/XSAI/CUTE/src/main/scala/CMemoryLoader.scala`
- `/nfs/home/hujun/workspace/xsai/xsai-env/XSAI/CUTE/src/main/scala/ABMatrixReg.scala`
- `/nfs/home/hujun/workspace/xsai/xsai-env/XSAI/CUTE/src/main/scala/CMatrixReg.scala`
- `/nfs/home/hujun/workspace/xsai/xsai-env/XSAI/CUTE/src/main/scala/CUTEParameters.scala`

## Current Code Facts / 当前代码事实

- RTL external memory interface 口径是 `outsideDataWidth = 512b = 64B/cycle`。
- A/B MatrixReg 与 C MatrixReg 内部仍是 `8` banks、每 bank `32B`，内部 full-bank vector 峰值是 `256B/cycle`。
- external LocalMMU beat 是 `64B`，read response 写 MatrixReg 时要拆成两个 `32B` chunks。
- AML/BML/CML 共享一个 LocalMMU 发射口；本阶段每个 backend cycle 最多 issue 一个 `64B` beat。
- LocalMMU 第一阶段使用固定/可配置 latency，不根据 cache hit/miss 变化。
- source ID / outstanding 上限按 `64` 建模。
- compute read frontend 已经过现有 `matrixRegResource`，所以本轮不重复实现 compute read arbitration；本轮要把 loader writes 接入同一个 resource，并补与 compute read 的冲突测试。
- `MatrixMemoryAdapter` 继续负责实际 functional tensor load/store；LocalMMU 只决定何时允许 functional result 变为 backend-visible。

## Acceptance Criteria / 验收标准

- AC-1: 本轮范围只覆盖第一阶段 LocalMMU/load-store timing，不做真实 L2。
  - Positive Tests (expected to PASS) / 正向测试（预期通过）:
    - 新增 LocalMMU model 仅在 `src/matrix` 内使用。
    - detailed backend 仍通过 `MatrixMemoryAdapter` 保持功能数据正确。
    - 文档明确不实现 timing RequestPort、L2 packet、HBL2 matrix metadata、cache hit/miss。
  - Negative Tests (expected to FAIL) / 反向测试（预期失败或被拒绝）:
    - 新增 O3 `matrix_mem_port`。
    - 接入真实 `recvTimingResp()` / retry。
    - 引入 cache tag、MSHR、coherence 或 replacement 行为。

- AC-2: `LocalMmuModel` 独立表达 fixed-latency、单发射口、64 source IDs。
  - Positive Tests (expected to PASS) / 正向测试（预期通过）:
    - `LocalMmuModel.IssuesOnlyOneBeatPerCycleWithFixedLatency`：两个 pending beats 分两个 cycle issue，并按固定 latency 返回。
    - `LocalMmuModel.SourceIdsLimitOutstandingIssuedBeats`：最多 64 个 issued outstanding，第 65 个保持 pending。
    - invalid `byteSize == 0` 或 `byteSize > 64` enqueue 被拒绝。
  - Negative Tests (expected to FAIL) / 反向测试（预期失败或被拒绝）:
    - 同一 cycle issue 多个 beats。
    - outstanding 超过 64。
    - response latency 依赖外部 cache 状态。

- AC-3: `DetailedCuteBackend` 支持 LocalMMU timing config 和状态。
  - Positive Tests (expected to PASS) / 正向测试（预期通过）:
    - `DetailedCuteBackend.AcceptsLocalMmuTimingConfig` 能设置 `localMmuLatencyCycles` 和 `localMmuMaxOutstanding`。
    - test probe 可以观察 pending/outstanding/issued 等最小状态。
    - constructor 默认配置保持现有调用兼容。
  - Negative Tests (expected to FAIL) / 反向测试（预期失败或被拒绝）:
    - 破坏现有 `DetailedCuteBackend(...)` 默认构造。
    - 新增 timing config 后旧 tests 需要无关修改。

- AC-4: matrix load/store 入队 `64B` LocalMMU beats。
  - Positive Tests (expected to PASS) / 正向测试（预期通过）:
    - `DetailedCuteBackend.LoadEnqueuesOneLocalMmuBeatPer64Bytes`：`2 x 64B` A load 入队 2 个 beats。
    - A/B full load `8192B` 产生 `128` beats。
    - C full load `65536B` 产生 `1024` beats。
    - store 在 C tensor snapshot 后入队对应 `64B` store beats。
  - Negative Tests (expected to FAIL) / 反向测试（预期失败或被拒绝）:
    - load/store 仍在功能级 tile 操作当拍完成。
    - external request window 使用内部 MatrixReg `256B/cycle`。

- AC-5: LocalMMU responses / store acks 驱动 load/store completion。
  - Positive Tests (expected to PASS) / 正向测试（预期通过）:
    - `DetailedCuteBackend.LoadCompletionWaitsForLocalMmuResponses`：read responses 全部到达前没有 load completion。
    - store write finish 等待所有 store acks。
    - `memory->loadTile()` / `memory->storeTile()` 只在 timing 条件满足后执行一次。
  - Negative Tests (expected to FAIL) / 反向测试（预期失败或被拒绝）:
    - load 在 responses 到达前产生 completion。
    - store ack 到达前 memory adapter 已重复写。

- AC-6: 每个 `64B` read response 拆成两个 `32B` MatrixReg memory-loader writes。
  - Positive Tests (expected to PASS) / 正向测试（预期通过）:
    - `DetailedCuteBackend.ReadResponseRequiresTwoMatrixRegWriteSlots`：一个 `64B` response 需要两个 MatrixReg write chunks 后才能完成 load register writeback。
    - loader write 使用现有 `matrixRegResource` 的 `MemoryLoader` client。
    - loader write priority 能 stall 同 bank A/B compute read。
  - Negative Tests (expected to FAIL) / 反向测试（预期失败或被拒绝）:
    - `64B` response 到达当拍直接完成 load。
    - loader write 绕过 `matrixRegResource`。
    - loader write 与 compute read conflict 时 compute accepted beat 仍推进。

- AC-7: store ack 与 release gating 对齐。
  - Positive Tests (expected to PASS) / 正向测试（预期通过）:
    - `DetailedCuteBackend.ReleaseWaitsForLocalMmuStoreAck`：store ack 前没有 store completion，release 也不能完成。
    - `pendingStoreCount` 只在 CML store write finish / ack 后递减。
    - 既有 release gating tests 继续通过。
  - Negative Tests (expected to FAIL) / 反向测试（预期失败或被拒绝）:
    - store read snapshot 后立即释放 pending store。
    - release 在 LocalMMU store ack 前 completion。

- AC-8: 回归、风格和文档边界清楚。
  - Positive Tests (expected to PASS) / 正向测试（预期通过）:
    - `LocalMmuModel.*` focused tests 通过。
    - LocalMMU backend focused tests 通过。
    - full `detailed_cute_backend.test.opt` 通过。
    - AGENTS.md SE `gemm_precomp` 继续输出 `All 8 precomp tests PASSED.`
    - `implementation_notes.md`、`verify_report.md`、`handoff.md` 只追加已验证边界。
  - Negative Tests (expected to FAIL) / 反向测试（预期失败或被拒绝）:
    - 因 unit tests 通过宣称 RTL timing equivalence。
    - 顺手修改 configs、真实 L2、O3 carrier 或无关 markdown。

## Path Boundaries / 范围边界

### Upper Bound (Maximum Scope) / 最大范围

- 可以新增：
  - `src/matrix/local_mmu_model.hh`
  - `src/matrix/local_mmu_model.cc`
- 可以修改：
  - `src/matrix/SConscript`
  - `src/matrix/detailed_cute_backend.hh`
  - `src/matrix/taskcontrol.cc`
  - `src/matrix/backend_runtime.cc`
  - `src/matrix/memoryload.cc`
  - `src/matrix/mregfile.cc`
  - `src/matrix/detailed_cute_backend.test.cc`
- 可以在阶段完成后追加更新：
  - `cute_detailed_design/implementation_notes.md`
  - `cute_detailed_design/verify_report.md`
  - `cute_detailed_design/handoff.md`

### Lower Bound (Minimum Scope) / 最小范围

- 必须先用 TDD 建 `LocalMmuModel` 红灯测试。
- 必须证明每 cycle 只 issue 一个 `64B` beat。
- 必须证明 source ID / outstanding 上限为 `64`。
- 必须证明 load completion 等待 LocalMMU responses 和 MatrixReg write chunks。
- 必须证明 store/release 等待 LocalMMU store ack。

### Allowed Choices / 允许和禁止的选择

- Can use / 可以使用:
  - `DetailedCuteBackendTestProbe`
  - 现有 gtest
  - `MatrixRegResource`
  - `MatrixMemoryAdapter`
  - 固定/可配置 LocalMMU latency
- Cannot use / 不允许:
  - GEM5 timing `RequestPort`
  - 真实 L2 packet / retry / recvTimingResp
  - cache tag / hit-miss / MSHR / coherence / replacement
  - 顺手恢复非 tile CSR renamed carriers
  - 为了实现回滚用户或既有未提交改动

## Feasibility Hints / 可行性提示

- `LocalMmuModel` 应只管理 timing metadata，不读写 tensor 内容。
- response 与 store ack 可以共用 `Response` 结构，通过 `isStore` 区分。
- `DetailedCuteBackend::step()` 中应先推进 LocalMMU 与 MatrixRegResource cycle，再处理 FIFO/task 事件，具体顺序以现有 task event 语义为准，测试锁定即可。
- load 的 functional `memory->loadTile()` 可以在所有 LocalMMU responses 到达后执行一次；MatrixReg register visible writeback 仍要等待 response 对应的 `2 x 32B` write chunks。
- store 的 functional `memory->storeTile()` 只执行一次，并且 write finish / release gating 绑定到 store ack 完成后。
- compute read arbitration 已经存在；不要重新实现，只把 loader writes 接入同一个 `matrixRegResource`。
- 先实现 no-stall fixed latency，再用 targeted tests 加冲突场景。

## Dependencies and Sequence / 依赖与执行顺序

### Milestones / 里程碑

1. Milestone 1 / 里程碑 1: LocalMMU model TDD
   - Phase A / 阶段 A: 写 `LocalMmuModel.IssuesOnlyOneBeatPerCycleWithFixedLatency` 和 `LocalMmuModel.SourceIdsLimitOutstandingIssuedBeats`。
   - Phase B / 阶段 B: 运行 build，确认缺少 `matrix/local_mmu_model.hh` 红灯。
   - Phase C / 阶段 C: 新增 `local_mmu_model.hh/cc`。
   - Phase D / 阶段 D: 接入 `src/matrix/SConscript`。
   - Verify / 验证:
     - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
     - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='LocalMmuModel.*'`

2. Milestone 2 / 里程碑 2: Backend timing config and LocalMMU state
   - Phase A / 阶段 A: 写 `DetailedCuteBackend.AcceptsLocalMmuTimingConfig`。
   - Phase B / 阶段 B: 增加 `DetailedCuteBackend::TimingConfig`。
   - Phase C / 阶段 C: 增加 `LocalMmuModel localMmu` 成员和 test probe。
   - Phase D / 阶段 D: 保持 constructor 默认参数兼容。
   - Verify / 验证:
     - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.AcceptsLocalMmuTimingConfig'`

3. Milestone 3 / 里程碑 3: Enqueue matrix LSU as `64B` beats
   - Phase A / 阶段 A: 写 `DetailedCuteBackend.LoadEnqueuesOneLocalMmuBeatPer64Bytes`。
   - Phase B / 阶段 B: 给 `TaskSlot` 增加 LSU timing counters。
   - Phase C / 阶段 C: 实现 `lsuPayloadBytes()` / `lsuBeatCount()` / `localMmuClient()` / `enqueueLocalMmuBeats()`。
   - Phase D / 阶段 D: load 在 MemReq/FillPending 阶段 enqueue；store 在 RegRead snapshot 后 enqueue。
   - Verify / 验证:
     - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.LoadEnqueuesOneLocalMmuBeatPer64Bytes'`

4. Milestone 4 / 里程碑 4: LocalMMU responses drive completion
   - Phase A / 阶段 A: 写 `DetailedCuteBackend.LoadCompletionWaitsForLocalMmuResponses`。
   - Phase B / 阶段 B: 每个 backend cycle 调用 `localMmu.step()` 和 `serviceLocalMmuResponses()`。
   - Phase C / 阶段 C: load 等待全部 responses 后执行一次 `memory->loadTile()`。
   - Phase D / 阶段 D: store 等待全部 acks 后执行/确认一次 `memory->storeTile()`。
   - Verify / 验证:
     - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.LoadCompletionWaitsForLocalMmuResponses'`

5. Milestone 5 / 里程碑 5: Read response MatrixReg fill resource
   - Phase A / 阶段 A: 写 `DetailedCuteBackend.ReadResponseRequiresTwoMatrixRegWriteSlots`。
   - Phase B / 阶段 B: 每个 non-store response 增加两个 pending MatrixReg write chunks。
   - Phase C / 阶段 C: 用现有 `matrixRegResource` 仲裁 MemoryLoader writes。
   - Phase D / 阶段 D: 补 `DetailedCuteBackend.LoaderWritePriorityCanStallComputeRead` 或等价冲突测试。
   - Verify / 验证:
     - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.ReadResponseRequiresTwoMatrixRegWriteSlots:DetailedCuteBackend.LoaderWritePriorityCanStallComputeRead:MatrixRegResource.*'`

6. Milestone 6 / 里程碑 6: Store ack and release gating
   - Phase A / 阶段 A: 写 `DetailedCuteBackend.ReleaseWaitsForLocalMmuStoreAck`。
   - Phase B / 阶段 B: 确认 `pendingStoreCount` 只在 store ack / write finish 后递减。
   - Phase C / 阶段 C: 保持 release wait backend drain / pending store 语义。
   - Verify / 验证:
     - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.ReleaseWaitsForLocalMmuStoreAck:DetailedCuteBackend.ReleaseWaitsForPendingStoreCompletion:DetailedCuteBackend.ReleaseWaitsForBackendDrainBeyondPendingStoreCount'`

7. Milestone 7 / 里程碑 7: Focused and full verification
   - Phase A / 阶段 A: 运行 LocalMMU focused tests。
   - Phase B / 阶段 B: 运行 full detailed backend tests。
   - Phase C / 阶段 C: 构建 `gem5.opt`。
   - Phase D / 阶段 D: 运行 SE `gemm_precomp` smoke。
   - Phase E / 阶段 E: 追加更新 implementation notes / verify report / handoff。
   - Verify / 验证:
     - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
     - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='LocalMmuModel.*:DetailedCuteBackend.*LocalMmu*:DetailedCuteBackend.ReadResponseRequiresTwoMatrixRegWriteSlots:DetailedCuteBackend.LoaderWritePriorityCanStallComputeRead'`
     - `./build/RISCV/matrix/detailed_cute_backend.test.opt`
     - `scons -Q build/RISCV/gem5.opt -j8`
     - `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-localmmu configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
     - `util/style.py -m --checker SortedIncludes --checker Whitespace --checker LineLength src/matrix/local_mmu_model.hh src/matrix/local_mmu_model.cc src/matrix/SConscript src/matrix/detailed_cute_backend.hh src/matrix/taskcontrol.cc src/matrix/backend_runtime.cc src/matrix/memoryload.cc src/matrix/mregfile.cc src/matrix/detailed_cute_backend.test.cc`
     - `git diff --check -- src/matrix cute_detailed_design/implementation_notes.md cute_detailed_design/verify_report.md cute_detailed_design/handoff.md plan.md`

## Implementation Notes / 实现注意事项

- 代码里不要出现 `AC-`、`Milestone`、`Phase` 这类计划术语。
- 本轮只做 LocalMMU/load-store timing，不做 CDC tile-level functional writeback。
- 不实现真实 L2、RequestPort、retry/replay、cache hit/miss。
- 不恢复非 tile CSR renamed carriers，只保留 `mtilem`、`mtilen`、`mtilek`。
- `LocalMmuModel` 不应知道 tensor 内容。
- `MatrixMemoryAdapter` 继续负责功能数据读写；LocalMMU 控制 timing boundary。
- loader write 与 compute read/write 必须通过同一个 active `matrixRegResource` 仲裁。
- 文档更新只在阶段完成后做必要追加，不重写 `task_plan.md`、`findings.md`、`progress.md` 结构。

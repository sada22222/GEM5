# Verify Report

## 文档职责

- 这份文件只负责 **验证与证据**。
- 它只写：
  - 跑了哪些命令
  - 命令结果是什么
  - 日志/trace 证明了什么
  - 覆盖边界与未覆盖项
- 它**不负责**：
  - 定义长期计划
  - 解释完整设计分层
  - 记录本轮代码改动摘要
  - 充当 handoff 文档

## 相关文档边界

- `Plan.md`
  - 负责阶段目标与 DoD
- `cute_detailed_design/matrix_core_design.md`
  - 负责核内设计
- `cute_detailed_design/matrix_diff_vs_rtl.md`
  - 负责当前未对齐项
- `implementation_notes.md`
  - 负责本轮改动摘要
- `handoff.md`
  - 负责交接状态

## Stage
- Phase 4 `4.1/4.3/4.4/4.5`
- 当前验证对象：`4.1/4.3/4.5` task-level checkpoint、`4.4` FP16 backend/unit compute checkpoint，以及 `0x2b / gemm_precomp` 主线 smoke/final code-side regression
- 历史的 Phase 2A / Stage E 记录保留在下方

## Verification Performed

### Phase 4 datatype build
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j4`
  - Result: PASS

### Phase 4 full build
- `scons build/RISCV/gem5.opt --linker=gold -j64 --rvv-impl=simple`
  - Result: PASS
  - Note:
    - 仅有环境相关 warning：
      - `png.h` 缺失
      - HDF5 缺失
      - back trace implementation 缺失
    - 未出现本轮 matrix datatype 改动引入的编译错误

### Phase 4 mainline SE regression
- `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-precomp configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
  - Result: PASS
  - Summary:
    - `AME GEMM precomp test: 8 cases`
    - `All 8 precomp tests PASSED.`
    - `Exiting @ tick 403685910 because m5_exit instruction encountered`

### Phase 4 targeted datatype tests
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.UnsupportedFpMmaReturnsUnsupportedCompletion:DetailedCuteBackend.Fp16MmaSucceedsWithInt32Accumulator:DetailedCuteBackend.Fp16LsuKeepsElemTypeAndRawBits'`
  - Result: PASS
  - Summary:
    - `3 tests from DetailedCuteBackend passed`

### Phase 4 detailed backend unit suite
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS
  - Summary:
    - `36 tests from 5 test suites passed`

### Release gating build
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j4`
  - Result: PASS

### Build
- `scons -Q build/RISCV/gem5.opt -j4`
  - Result: PASS

### Release gating unit tests
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.ReleaseCompletionOnlyAppearsAtTerminalStage:DetailedCuteBackend.ReleaseWaitsForPendingStoreCompletion:DetailedCuteBackend.ReleaseWaitsForBackendDrainBeyondPendingStoreCount:DetailedCuteBackend.ReleaseWaitsForComputeTerminalCompletion'`
  - Result: PASS
  - Summary:
    - `4 tests from DetailedCuteBackend passed`

### Detailed backend unit suite
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS
  - Summary:
    - `31 tests from 3 test suites passed`

### Clean path
- `/tmp/gem5-matrix-phase1a-e.opt --outdir=/tmp/gem5-matrix-commit-e2 --debug-flags=IEW,ROB,Commit --debug-file=matrix_lsu_commit_e2.log configs/example/matrix_seq_o3.py --binary /tmp/matrix_lsu_commit_probe_nogp`
  - Result: PASS

### Fault path
- `riscv64-linux-gnu-gcc -nostdlib -static -o /tmp/matrix_lsu_fault_probe /tmp/matrix_lsu_fault_probe.S`
  - Result: PASS
- `/tmp/gem5-matrix-phase1a-e.opt --outdir=/tmp/gem5-matrix-fault-e1 --debug-flags=IEW,ROB,Commit --debug-file=matrix_lsu_fault_e1.log configs/example/matrix_seq_o3.py --binary /tmp/matrix_lsu_fault_probe`
  - Result: expected panic
  - Note:
    - 程序最终以 illegal instruction panic 结束
    - 但 panic 前已经收集到 `writeback suppressed` 与“无 `toAMU`”证据

### Cancel / squash path
- `riscv64-linux-gnu-gcc -nostdlib -static -o /tmp/matrix_lsu_cancel_probe_e /tmp/matrix_lsu_cancel_probe_e.S`
  - Result: PASS
- `/tmp/gem5-matrix-phase1a-e.opt --outdir=/tmp/gem5-matrix-cancel-e1 --debug-flags=IEW,ROB,Commit --debug-file=matrix_lsu_cancel_e1.log configs/example/matrix_seq_o3.py --binary /tmp/matrix_lsu_cancel_probe_e`
  - Result: PASS

### `mrelease / macquire` order
- `riscv64-linux-gnu-gcc -nostdlib -static -o /tmp/matrix_release_acquire_probe /tmp/matrix_release_acquire_probe.S`
  - Result: PASS
- `/tmp/gem5-matrix-phase1a-e.opt --outdir=/tmp/gem5-matrix-release-e1 --debug-flags=IEW,ROB,Commit --debug-file=matrix_release_acquire_e1.log configs/example/matrix_seq_o3.py --binary /tmp/matrix_release_acquire_probe`
  - Result: PASS

## Evidence

### 0. Phase 4 计划修订与 datatype tag 透传基础已建立
- 代码：
  - [Plans.md](/nfs/home/hujun/GEM5/Plans.md)
  - [src/cpu/exec_context.hh](/nfs/home/hujun/GEM5/src/cpu/exec_context.hh)
  - [src/cpu/o3/rob.cc](/nfs/home/hujun/GEM5/src/cpu/o3/rob.cc)
  - [src/cpu/o3/mls_unit.cc](/nfs/home/hujun/GEM5/src/cpu/o3/mls_unit.cc)
  - [src/matrix/matrix_types.hh](/nfs/home/hujun/GEM5/src/matrix/matrix_types.hh)
  - [src/matrix/matrix_memory_adapter_gem5.cc](/nfs/home/hujun/GEM5/src/matrix/matrix_memory_adapter_gem5.cc)
  - [src/matrix/matrix_ex.cc](/nfs/home/hujun/GEM5/src/matrix/matrix_ex.cc)
- 关键断言：
  - `MatrixElemType` 现在能显式区分 `Fp16/Bf16/Tf32`
  - LSU path 会把 `elemType` 从 request 带到 `MatrixTensor`
  - 未完成的 FP compute 不会落到整数 happy-path 冒充成功
  - int8/int16/int32 现有单测仍保持通过

### 0.1 `Fp16` LSU tag 不再被当成 `Int16`
- 测试：
  - `DetailedCuteBackend.Fp16LsuKeepsElemTypeAndRawBits`
- 关键断言：
  - `load_a.lsu.elemType = MatrixElemType::Fp16`
  - backend 完成后，`matrixState().read(MatrixBankKind::A, 0).elemType == Fp16`
  - raw bits `0x3c00/0x4000/0x4200/0x4400` 原样保留

### 0.2 未闭环 FP path 仍明确 `Unsupported`
- 测试：
  - `DetailedCuteBackend.UnsupportedFpMmaReturnsUnsupportedCompletion`
- 关键断言：
  - 不满足当前闭环形状的 FP request 仍会返回 `Unsupported`
  - `C` tensor 不被整数路径误写

### 0.3 int16 integer MMA fallback 仍成立
- 测试：
  - `DetailedCuteBackend.Int16IntegerMmaEncodingSucceeds`
- 关键断言：
  - 即使当前 call site 仍主要依赖 `types1/types2/typed`，整数 `Int16 -> Int32` path 仍保持通过
  - 证明这轮 datatype tag 扩展没有破坏已经闭环的整数主线

### 0.4 `gemm_precomp` 已作为 `0x2b` 主口径 workload 跑通
- 运行输出：
  - `/tmp/gem5-se-gemm-precomp`
- 关键证据：
  - `zeros / ones / max / min / rand0 / rand1 / rand2 / rand3` 全部 `PASS`
  - 终止原因为正常 `m5_exit`
- 结论：
  - 当前 GEM5 对 `0x2b` 主口径 workload 已具备可复现的 SE 主线回归

### 0.5 当前 FP datatype / compute 结论
- 已验证支持：
  - FP16/BF16/TF32 的 datatype tag 透传
  - LSU / memory adapter 按 raw bits 处理 FP16/BF16/TF32
  - request 可以带着 FP datatype 一路传到 backend
  - FP16 MMA 在 detailed backend 单测路径上的结果正确性闭环
- 已验证仍不支持：
  - active ISA/decode 路径上的 FP MMA workload 闭环
  - BF16/TF32 compute 的结果正确执行

### 0.6 FP16 backend compute 与 CUTE oracle 一致
- 测试：
  - `DetailedCuteBackend.Fp16MmaSucceedsWithInt32Accumulator`
- 关键断言：
  - A/B 输入按 `FP16` raw bits 写入
  - C accumulator 按 `Int32` raw `FP32` bits 保存
  - 2x2 case 的结果固定为：
    - `0x40e00000`
    - `0x41200000`
    - `0x41700000`
    - `0x41b00000`
- oracle 来源：
  - `/nfs/home/hujun/workspace/xsai/xsai-env/XSAI/CUTE/cute-fpe/ccode/FloatDecode.h`
  - `/nfs/home/hujun/workspace/xsai/xsai-env/XSAI/CUTE/cute-fpe/ccode/fmac.h`
- 结论：
  - 当前 GEM5 已具备与 CUTE `cute-fpe` 软件参考一致的最小 FP16 backend compute 闭环
  - 这条证据只覆盖 detailed backend/unit 路径，不证明 active ISA/decode FP workload 已可达

## 2026-05-13 Phase 4 baseline task review

注：
- 这是 `4.1/4.3/4.5` baseline checkpoint 当时的 review 结论。
- 同日后续 `4.4/4.6` 的收口见下方 `Phase 4 FP16 compute / final regression task review`。

### Review Target
- Task:
  - Phase 4 `4.1` 主线 workload / datatype 边界冻结
  - Phase 4 `4.3` FP16/BF16/TF32 datatype 透传
  - Phase 4 `4.5` `gemm_precomp` 主线 smoke 回归
- Review type:
  - task review
- Claimed transition:
  - `4.1: cc:WIP -> cc:完成`
  - `4.3: cc:TODO -> cc:完成`
  - `4.5: cc:WIP -> cc:完成`

### Required Gates

| Gate | Result | Evidence | Issue if FAIL |
|---|---|---|---|
| Scope matches current Phase / Task | PASS | 本轮只收 `4.1/4.3/4.5`，没有把 `4.4` FP compute 闭环或 `4.6` 最终回归混进来。 | - |
| DoD is observable and reproducible | PASS | `gem5.opt` build、datatype targeted tests、full unit suite、`gemm_precomp` smoke 回归都已记录且可重现。 | - |
| Depends / Status / DoD are self-consistent | PASS | `4.1/4.3/4.5` 已有对应证据，`4.4/4.6` 继续保留 `TODO`，没有把未完成的 FP compute 闭环提前写成完成。 | - |
| No completed task depends on TODO without explanation | PASS | `4.1/4.3/4.5` 的依赖都已满足；`4.6` 仍显式依赖 `4.4`。 | - |
| Claimed alignment does not exceed evidence | PASS | 本轮只声称 datatype tag 透传、FP path 显式 `Unsupported`、以及 `gemm_precomp` smoke 回归成立，没有外推成 FP compute 已闭环或 RTL timing 已对齐。 | - |
| Functional correctness is separated from resource/timing alignment | PASS | 本节明确区分了 datatype / smoke 功能闭环，与 `Phase 4A` 的 memory/resource/timing 未对齐项。 | - |
| Missing evidence is marked as `证据不足` | PASS | `4.4`、`4.6` 仍保持未完成；`matrix_diff_vs_rtl.md` 继续保留 `mregfile`、memory path、compute overlap 等未对齐项。 | - |
| `verify_report` contains required evidence | PASS | 本文件已记录 build、targeted datatype tests、full suite、`gemm_precomp` 回归和本轮 review 结论。 | - |
| `handoff` reflects current stop point | PASS | 本轮会同步 handoff，明确 `4.1/4.3/4.5` 是 task-level checkpoint，`4.4/4.6` 待完成。 | - |
| `matrix_diff_vs_rtl.md` is consistent with `Plan.md` | PASS | 本轮已把 `mregfile` 读写仲裁/带宽/bank 并行度升级为独立重点未对齐项，和 `Phase 4A` 计划一致。 | - |
| Review reports RTL alignment, design complexity, and code style as separate dimensions | PASS | 见下方 Review Dimensions。 | - |

### Review Dimensions

| Dimension | Result | Evidence | Issue if FAIL / N/A reason |
|---|---|---|---|
| RTL Alignment | PASS | 当前只把 `4.1/4.3/4.5` 收成可验证 checkpoint，没有越过 `matrix_diff_vs_rtl.md` 的未对齐边界。 | - |
| Design Complexity | PASS | 本轮没有新增后端实现，只是把已有最小 patch 的状态、证据和差异真源收口。 | - |
| Code Style | PASS | 本轮只回写文档，没有引入新的实现复杂度；代码侧仍沿用现有 active 路径。 | - |

### Evidence
- [Plans.md](/nfs/home/hujun/GEM5/Plans.md)
- [cute_detailed_design/matrix_diff_vs_rtl.md](/nfs/home/hujun/GEM5/cute_detailed_design/matrix_diff_vs_rtl.md)
- `scons build/RISCV/gem5.opt --linker=gold -j64 --rvv-impl=simple`
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.UnsupportedFpMmaReturnsUnsupportedCompletion:DetailedCuteBackend.Fp16MmaSucceedsWithInt32Accumulator:DetailedCuteBackend.Fp16LsuKeepsElemTypeAndRawBits'`
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
- `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-precomp configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`

### Issues
- None

### Blocking Issues
- None

### Required Document Updates
- `Plans.md`
- `cute_detailed_design/matrix_diff_vs_rtl.md`
- `cute_detailed_design/implementation_notes.md`
- `cute_detailed_design/handoff.md`

### Recommended Action
- 将 `Plans.md` 中 `4.1/4.3/4.5` 更新为 `cc:完成`
- 保持 `4.4/4.6` 未完成
- 继续把 `Phase 4A` 作为时序/建模对齐 WIP 计划，不与本轮 datatype/smoke checkpoint 混写

### Verdict

`APPROVE`

## 2026-05-13 Phase 4 FP16 compute checkpoint review

### Review Target
- Task:
  - Phase 4 `4.4` FP16 backend/unit compute checkpoint
- Review type:
  - task review
- Claimed transition:
  - `4.4: cc:TODO -> cc:完成`

### Required Gates

| Gate | Result | Evidence | Issue if FAIL |
|---|---|---|---|
| Scope matches current Phase / Task | PASS | 本轮只补 `matrix_ex.cc` 的 FP16 backend/unit compute 和相应回归，没有扩到 `Phase 4A` 时序建模或 ISA-level FP decode。 | - |
| DoD is observable and reproducible | PASS | targeted FP tests、full unit suite、`gem5.opt` build、`gemm_precomp` 最终回归都可复现。 | - |
| Depends / Status / DoD are self-consistent | PASS | `4.3` 已完成，本轮只收 `4.4`；`4.6` 继续保持未完成。 | - |
| No completed task depends on TODO without explanation | PASS | 当前没有把依赖 active FP workload 可达性的 `4.6` 错误推进为完成。 | - |
| Claimed alignment does not exceed evidence | PASS | 文档明确把 `4.4` 写成 backend/unit 级 FP16 compute checkpoint，没有外推成 ISA/workload 级 FP 路全通或 RTL 对齐。 | - |
| Functional correctness is separated from resource/timing alignment | PASS | 本轮只证明 FP16 backend compute 正确性和主线回归稳定，不把它混写成 `Phase 4A` 的资源/时序对齐。 | - |
| Missing evidence is marked as `证据不足` | PASS | active ISA/decode FP workload、BF16/TF32 compute 仍明确标为未完成。 | - |
| `verify_report` contains required evidence | PASS | 本文件已记录 oracle、targeted FP tests、full unit suite、build 和 `gemm_precomp` code-side regression。 | - |
| `handoff` reflects current stop point | PASS | 本轮会同步 handoff，明确 Phase 4 task-level 收口与残留边界。 | - |
| `matrix_diff_vs_rtl.md` is consistent with `Plan.md` | PASS | 本轮只收 datatype / compute task，不改变 `Phase 4A` 的 RTL 差异主口径。 | - |
| Review reports RTL alignment, design complexity, and code style as separate dimensions | PASS | 见下方 Review Dimensions。 | - |

### Review Dimensions

| Dimension | Result | Evidence | Issue if FAIL / N/A reason |
|---|---|---|---|
| RTL Alignment | PASS | 当前只把 FP16 compute 收成 CUTE oracle 支撑的 backend/unit checkpoint，没有越过 `matrix_diff_vs_rtl.md` 的未对齐边界。 | - |
| Design Complexity | PASS | 只补 `matrix_ex.cc` 的最小 FP16 path 和一条 oracle 回归，没有引入新 request kind 或平行 backend。 | - |
| Code Style | PASS | 改动集中在 active build 文件和既有测试文件，控制流保持直接。 | - |

### Evidence
- [src/matrix/matrix_ex.cc](/nfs/home/hujun/GEM5/src/matrix/matrix_ex.cc)
- [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc)
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j4`
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.UnsupportedFpMmaReturnsUnsupportedCompletion:DetailedCuteBackend.Fp16MmaSucceedsWithInt32Accumulator:DetailedCuteBackend.Fp16LsuKeepsElemTypeAndRawBits'`
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
- `scons build/RISCV/gem5.opt --linker=gold -j64 --rvv-impl=simple`
- `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-precomp-fp16-final configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`

### Issues
- None

### Blocking Issues
- None

### Required Document Updates
- `Plans.md`
- `cute_detailed_design/implementation_notes.md`
- `cute_detailed_design/handoff.md`

### Recommended Action
- 将 `Plans.md` 中 `4.4` 更新为 `cc:完成`
- 保持 `4.6` 未完成
- 保持 active ISA/decode FP MMA workload、BF16/TF32 compute 继续作为后续未完成边界

### Verdict

`APPROVE`

### 1. release gating: release 需要同时等待 pending store、backend drain 和 compute terminal completion
- 测试：
  - `DetailedCuteBackend.ReleaseCompletionOnlyAppearsAtTerminalStage`
  - `DetailedCuteBackend.ReleaseWaitsForPendingStoreCompletion`
  - `DetailedCuteBackend.ReleaseWaitsForBackendDrainBeyondPendingStoreCount`
  - `DetailedCuteBackend.ReleaseWaitsForComputeTerminalCompletion`
- 关键断言：
  - store 在 `pendingStoreCount != 0` 时阻塞 release
  - 即使 `pendingStoreCount == 0`，只要前序 backend task 还没 drain 完，release 仍不能 dequeue
  - 新增 compute 回归里，`fifoDequeue` 会一直停在 `1`，直到前序 `mma` 的 terminal completion 被消费后，release 才能从 FIFO 发射
  - token release 只在 release 自己的最终 completion 时才可见
- 结论：
  - 当前 detailed backend 已有最小 release gating 闭环
  - 但仍不能据此写成 RTL 级 ready/valid completion gating 已对齐

### 2. clean path: commit 前 backend 不可见，commit 后才可见
- 日志：
  - `/tmp/gem5-matrix-commit-e2/matrix_lsu_commit_e2.log`
- `sn:8` 关键顺序：
  - `103500`: `Matrix payload staged`
  - `104000`: `Matrix AMU entry writeback [sn:8] payload=lsu`
  - `104500`: `Matrix AMU entry committed [sn:8]`
  - `104500`: `Matrix AMU entry toAMU proxy ready [sn:8]`
  - `104500`: `Matrix toAMU proxy fire [sn:8] payload=lsu`
  - `104500`: `Matrix LSU backend-visible after toAMU proxy [sn:8]`

### 3. fault path: writeback suppressed，且没有 backend-visible
- 日志：
  - `/tmp/gem5-matrix-fault-e1/matrix_lsu_fault_e1.log`
- `sn:8` 关键顺序：
  - `90000`: `Matrix AMU entry alloc [sn:8] class=lsu route=lsu`
  - `93500`: `Matrix AMU entry writeback suppressed [sn:8] fault=1 payloadValid=0`
- 同一 `sn:8` 聚合结果：
  - `Matrix toAMU proxy fire`: 0
  - `Matrix LSU backend-visible after toAMU proxy`: 0
- 结论：
  - faulted `mls` 没有误入 backend-visible 路径

### 4. cancel / squash path: AMU entry 被 squash，且没有 backend-visible
- 日志：
  - `/tmp/gem5-matrix-cancel-e1/matrix_lsu_cancel_e1.log`
- `sn:121` 关键顺序：
  - `499500`: `Matrix AMU entry alloc [sn:121] class=lsu route=lsu`
  - `509000`: `Matrix AMU entry squash [sn:121] writebacked=0 committed=0`
- 同一 `sn:121` 聚合结果：
  - `Matrix toAMU proxy fire`: 0
  - `Matrix LSU backend-visible after toAMU proxy`: 0
- 结论：
  - redirect/squash 下 `mls` 不会误入 commit-side proxy

### 5. `mrelease -> macquire` 顺序证据
- 日志：
  - `/tmp/gem5-matrix-release-e1/matrix_release_acquire_e1.log`
- 关键顺序：
  - `92000`: `sn:2` release commit
  - `92000`: `Matrix release temporary token update [sn:2] token=3 value=0x1`
  - `92000`: `Matrix toAMU proxy fire [sn:2] payload=release`
  - `92000`: `Matrix release backend-visible after toAMU proxy [sn:2] token=3`
  - `94000`: `sn:3` macquire execute
  - `95000`: `sn:3` macquire commit
- 结论：
  - release 先经过 commit-side proxy/backend-visible
  - acquire 后续才在 CPU 控制路径放行

## 2026-05-11 `3.18` task review

### Review Target
- Task:
  - Phase 3D `3.18` release gating
- Review type:
  - task review
- Claimed transition:
  - `WIP -> 完成`

### Required Gates

| Gate | Result | Evidence | Issue if FAIL |
|---|---|---|---|
| Scope matches current Phase / Task | PASS | 本轮只新增 release gating 回归与文档回写，范围局限在 `3.18`。 | - |
| DoD is observable and reproducible | PASS | `detailed_cute_backend.test.opt` 的 filter / full run 都 PASS，命令已回写。 | - |
| Depends / Status / DoD are self-consistent | PASS | `3.19` 仍保留 TODO，且 release gating 只被表述为最小闭环。 | - |
| No completed task depends on TODO without explanation | PASS | `3.19` 依赖 `3.18`，但当前没有把 `3.19` 错误推进为完成。 | - |
| Claimed alignment does not exceed evidence | PASS | 文档明确保留“不是 RTL 级完整 ready/valid completion gating”。 | - |
| Functional correctness is separated from resource/timing alignment | PASS | 只证明 release 不会过早放行，没有把它外推成完整 timing 对齐。 | - |
| Missing evidence is marked as `证据不足` | PASS | 仍保留未对齐边界与后续风险说明。 | - |
| `verify_report` contains required evidence | PASS | 本文件已记录 release gating build、filter run 与全量 unit suite。 | - |
| `handoff` reflects current stop point | PASS | [handoff.md](/nfs/home/hujun/GEM5/cute_detailed_design/handoff.md) 已写明当前处于 `3.18` review gate。 | - |
| `matrix_diff_vs_rtl.md` is consistent with `Plan.md` | PASS | release 仍被描述为阶段性保守约束，与 `Plan.md` 的 3.18 目标一致。 | - |
| Review reports RTL alignment, design complexity, and code style as separate dimensions | PASS | 见下方 Review Dimensions。 | - |

### Review Dimensions

| Dimension | Result | Evidence | Issue if FAIL / N/A reason |
|---|---|---|---|
| RTL Alignment | PASS | `mma -> release` 被实证限制在 `pendingStoreCount + backend drain + terminal completion` 之后，但不宣称 RTL ready/valid 已对齐。 | - |
| Design Complexity | PASS | 只补一条最小回归，没有新增状态机或抽象层。 | - |
| Code Style | PASS | 新测试与现有单测风格一致，且 style review 已在 [implementation_notes.md](/nfs/home/hujun/GEM5/cute_detailed_design/implementation_notes.md) 中单独给出。 | - |

### Evidence
- [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc)
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j4`
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.ReleaseCompletionOnlyAppearsAtTerminalStage:DetailedCuteBackend.ReleaseWaitsForPendingStoreCompletion:DetailedCuteBackend.ReleaseWaitsForBackendDrainBeyondPendingStoreCount:DetailedCuteBackend.ReleaseWaitsForComputeTerminalCompletion'`
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`

### Issues
- None

### Blocking Issues
- None

### Required Document Updates
- `Plan.md`
- `handoff.md`

### Recommended Action
- 已执行：
  - `Plan.md` 中 `3.18` 已更新为 `cc:完成`
  - `handoff.md` 已同步当前阶段停点

### Verdict

`APPROVE`

## Coverage Limits
- 这次直接覆盖：
  - clean path commit boundary
  - `fault/no-payload` suppress
  - redirect/squash 不误入 proxy
  - `mrelease/macquire` 顺序证据
- 这次还没有直接覆盖：
  - 地址 fault / LSQ fault
  - 非 O3 fallback
  - 更完整的 exception ownership 总结

## 2026-05-11 `3.18` phase-level formal review

### Review Target
- Phase/Task:
  - Phase 3D `3.18` release gating
- Review type:
  - phase-level formal review
- Claimed transition:
  - `WIP -> 完成`

### Required Gates

| Gate | Result | Evidence | Issue if FAIL |
|---|---|---|---|
| Scope matches current Phase / Task | PASS | 当前 review 只看 `3.18` release gating，不把 `3.19` 混进来。 | - |
| DoD is observable and reproducible | PASS | 全量 `detailed_cute_backend.test.opt` 与 release gating filter 均可重复运行并已 PASS。 | - |
| Depends / Status / DoD are self-consistent | PASS | `3.18` 的描述仍然是“最小 release gating 闭环”，与 `3.19` 的依赖关系一致。 | - |
| No completed task depends on TODO without explanation | PASS | `3.19` 仍是 TODO，且文档没有把它误写成已完成。 | - |
| Claimed alignment does not exceed evidence | PASS | 只证明 release 已受 `pendingStoreCount / backend drain / terminal completion` 限制，没有超出证据宣称 RTL 对齐。 | - |
| Functional correctness is separated from resource/timing alignment | PASS | `mma -> release` 的 gating 只证明 release 时序边界，不代表完整资源/带宽对齐。 | - |
| Missing evidence is marked as `证据不足` | PASS | 文档保留“不是 RTL 级完整 ready/valid completion gating”的限制口径。 | - |
| `verify_report` contains required evidence | PASS | 本文件新增了 release gating build、filter run、full suite 以及 review 结论。 | - |
| `handoff` reflects current stop point | PASS | [handoff.md](/nfs/home/hujun/GEM5/cute_detailed_design/handoff.md) 已写到 `3.18 review gate`。 | - |
| `matrix_diff_vs_rtl.md` is consistent with `Plan.md` | PASS | release 差异口径仍是阶段性保守表达，没有和 `Plan.md` 冲突。 | - |
| Review reports RTL alignment, design complexity, and code style as separate dimensions | PASS | 见下方 Review Dimensions。 | - |

### Review Dimensions

| Dimension | Result | Evidence | Issue if FAIL / N/A reason |
|---|---|---|---|
| RTL Alignment | PASS | `ReleaseWaitsForComputeTerminalCompletion` 把 release 严格卡在前序 compute 的 terminal completion 之后，同时未把它升格成 RTL ready/valid 等价。 | - |
| Design Complexity | PASS | 这轮没有引入新 class / 新 control layer，只补了一个目标明确的回归。 | - |
| Code Style | PASS | 新测试保持了 gem5 matrix 单测的直白写法；更细的 style 结论已在 [implementation_notes.md](/nfs/home/hujun/GEM5/cute_detailed_design/implementation_notes.md) 单独给出。 | - |

### Evidence
- [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc)
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j4`
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.ReleaseCompletionOnlyAppearsAtTerminalStage:DetailedCuteBackend.ReleaseWaitsForPendingStoreCompletion:DetailedCuteBackend.ReleaseWaitsForBackendDrainBeyondPendingStoreCount:DetailedCuteBackend.ReleaseWaitsForComputeTerminalCompletion'`
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`

### Issues
- None

### Blocking Issues
- None

### Required Document Updates
- `Plan.md`
- `handoff.md`

### Recommended Action
- 已执行：
  - `Plan.md` 中 `3.18` 已更新为 `cc:完成`
  - `handoff.md` 已同步当前阶段停点

### Verdict

`APPROVE`

## Independent Review
- Status:
  - reviewer subagent 已返回正式复审结论
- Verdict:
  - `APPROVE`
- Evidence:
  1. `mls` clean path 的 commit-boundary 证据已齐。
  2. `fault` 负路径已证明 `writeback suppressed` 且无 `toAMU / backend-visible`。
  3. `cancel/squash` 负路径已证明 `Matrix AMU entry squash` 且无 `toAMU / backend-visible`。
  4. `mrelease/macquire` 已有 CPU→buffer 顺序证据。
- Issues:
  1. `Severity: recommendation`
     - `Location`: `verify_report.md`, `/tmp/gem5-matrix-fault-e1/matrix_lsu_fault_e1.log`
     - `Problem`: fault probe 最终以 illegal instruction panic 结束，且还未单独覆盖 address fault / LSQ fault。
     - `Why it matters`: 影响后续若把 review 范围扩到完整 `2.7 exception ownership` 时的覆盖完整度。
     - `Suggested follow-up`: 后续补 dedicated address-fault 与 LSQ-fault probes。
- Blocking Issues:
  - `None`
- Recommended Action:
  1. 阶段 E 可以从上一轮 `REQUEST_CHANGES` 更新为 `APPROVE`。
  2. 如果后续扩到完整 `2.7` exception ownership，再补地址/LSQ fault 证据。

---

## 2026-04-24 Detailed CUTE Phase 2 验证

### What This Proves
- `CPU/ROB/Commit` 的 `peek -> canAccept -> pop` 改动没有破坏现有 token / matrix sync 基线。

### Coverage Limits
- 这次没有验证：
  - detailed queue occupancy 统计
  - `AMUCtrlBuffer` 多 entry / 多 fire 时序
  - decoded FIFO / scoreboard 行为

### Runtime smoke
- `build/RISCV/gem5.opt --outdir=/tmp/gem5-matrix-cute-trace-lsu2 -d /tmp/gem5-matrix-cute-trace-lsu2 --debug-flags=MatrixCuteTrace,Commit,ROB --debug-file=matrix_cute_trace.log configs/example/matrix_seq_o3.py --binary /tmp/matrix_lsu_commit_probe_nogp --matrix-backend-queue-entries=1`
  - Result: PASS
- 关键 trace：
  - `106500: system.cpu: backend submit [tid:0] [sn:8] kind=lsu`
  - `106500: global: functional enqueue [sn:8] kind=lsu queued=1`
  - `106500: global: functional issue [sn:8] kind=lsu remaining=0`
  - `106500: system.cpu: backend completion [tid:0] [sn:8] kind=0 status=0 tokenRelease=0 token=0`
- 说明：
  - `MatrixCuteTrace` 已经在 O3 matrix LSU clean path 上产出 submit/issue/completion 证据

### Release runtime smoke
- `build/RISCV/gem5.opt --outdir=/tmp/gem5-matrix-cute-trace-release -d /tmp/gem5-matrix-cute-trace-release --debug-flags=MatrixCuteTrace,Commit,ROB --debug-file=matrix_cute_trace.log configs/example/matrix_seq_o3.py --binary /tmp/matrix_release_acquire_probe --matrix-backend-queue-entries=1`
  - Result: PASS
- 关键 trace：
  - `92000: system.cpu: backend submit [tid:0] [sn:2] kind=release`
  - `92000: system.cpu: release submit [tid:0] [sn:2] token=3`
  - `92000: system.cpu: backend completion [tid:0] [sn:2] kind=3 status=0 tokenRelease=1 token=3`
  - `92000: system.cpu: Matrix release temporary token update [tid:0] [sn:2] token=3 value=0x1`
- 说明：
  - `Release` 已经与其它 matrix backend request 走同一 submit/completion contract
  - token 更新当前发生在 backend completion 之后，而不是 commit 直接更新

### Double-release runtime smoke
- `build/RISCV/gem5.opt --outdir=/tmp/gem5-matrix-cute-trace-release2 -d /tmp/gem5-matrix-cute-trace-release2 --debug-flags=MatrixCuteTrace,Commit,ROB,Drain --debug-file=matrix_cute_trace.log configs/example/matrix_seq_o3.py --binary /tmp/matrix_release_double_probe --matrix-backend-queue-entries=1`
  - Result: PASS
- 关键 trace：
- `backend submit [tid:0] [sn:1] kind=release`
- `backend completion [tid:0] [sn:1] ... token=3`
- `backend submit [tid:0] [sn:2] kind=release`
- `backend completion [tid:0] [sn:2] ... token=4`
- 说明：
  - 双 release 序列能稳定完成
  - CPU 正常退出，未在 backend pending state 下错误结束

---

## 2026-05-07 `src/matrix` detailed CUTE backend 验证

### Build
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS

### Run
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS
  - Summary:
    - `30 tests passed`

### 2026-05-09 Phase 3C 3.16 targeted regression
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS
  - Summary:
    - `30 tests passed`
- New direct evidence:
  - `DetailedCuteBackend.SecondComputeWaitsOnAdcAvailabilityNotScoreboardComputeBusy`
  - `DetailedCuteBackend.MultipleComputeTasksCanOverlapFrontUnits`
- What it proves:
  - 第二条 `mma` 在第一条仍占用 `ADC` 时，先表现为 `fifo_block / downstream_not_accepting`
  - `scoreboardBlock` 没有被 monolithic compute busy 触发
  - 3.16 当前更准确地表现为“子单元可用性 + task ownership”驱动，而不是把 compute 当成单一 busy 实体
- How to read this evidence:
  - 这组证据主要证明控制层与执行层边界更清楚了
  - 它证明的是 `TaskController-style issue` 语义，而不是 `valid/ready` 外形等价
  - 当前仍不能据此声称 `ComputeGo / EndReady` 已完全按 RTL 握手闭环

### 2026-05-09 compute completion/release review follow-up
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS
  - Summary:
    - `30 tests passed`
- New direct evidence:
  - `DetailedCuteBackend.ComputeSubUnitsAreTrackedExplicitly`
- What changed:
  - `CDC` 本地 `write finish` 后，不再立即把 compute task 从 `computeTasks` 中摘掉
  - 现在要等 `TerminalCompletion` task event 真正被消费后，compute task 才退场
- What it proves:
  - `ADC/BDC` 的释放点仍然是 `ComputeReadAFinish/ComputeReadBFinish`
  - `MTE` 完成仍只表示结果准备好
  - `CDC/write finish` 之后，`C dest` 和 completion 的边界不再早于 task retire

### 2026-05-09 compute dest-hold regression
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.ComputeKeepsCDestBusyUntilWriteFinishAndCompletion:DetailedCuteBackend.ComputeSubUnitsAreTrackedExplicitly:DetailedCuteBackend.ReleaseWaitsForPendingStoreCompletion'`
  - Result: PASS
- New direct evidence:
  - `DetailedCuteBackend.ComputeKeepsCDestBusyUntilWriteFinishAndCompletion`
- What it proves:
  - `MTE` 结果准备好后，`C dest` 仍然 busy
  - 直到 `CDC/write finish` 与 `TerminalCompletion` 真被消费，后继写同一个 `C` 的 task 才能 issue
  - 这条边界与 `TaskController + Scoreboard + DataController + MatrixTE` 的语义更贴近

### 2026-05-09 store lifecycle follow-up
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.StoreSnapshotsRegisterDataAtReadFinish:DetailedCuteBackend.ReleaseWaitsForPendingStoreCompletion:DetailedCuteBackend.ZeroLoadDefersVisibleRegisterWriteUntilTaskFinish'`
  - Result: PASS
- What it proves:
  - store 的 `ReadFinish` 之后仍不应立刻产生 completion
  - `hasCompletion()` 只能在 `WriteFinish + TerminalCompletion` 真处理后变为 true
  - 与 `pendingStoreCount` / release gating 的边界一致

### 2026-05-09 taskslot terminal boundary follow-up
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS
  - Summary:
    - `30 tests passed`
- What changed:
  - `AML/BML/CML/release` 的 `TaskSlot` 现在和 compute 一样，要等 `TerminalCompletion` event 被消费后才 retire
- What it proves:
  - `WriteFinish` 不再等价于“任务已经完全退场”
  - 当前 active backend 对 load/store/zero/release 与 compute 的 terminal 边界更一致

### 2026-05-11 style follow-up: scoreboard cc split
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS
  - Summary:
    - `30 tests passed`
- What changed:
  - `DetailedCuteScoreboard` 实现下沉到 `detailed_cute_scoreboard.cc`
  - 头文件只保留声明和少量直接可读的接口
- What it proves:
  - 这次风格收口没有破坏现有 task 生命周期或 scoreboard 释放边界

### 2026-05-11 style follow-up: runtime phase split
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS
  - Summary:
    - `30 tests passed`
- What changed:
  - `backend_runtime.cc` 主链拆成更直白的阶段函数，`step()` 现在更像“头部判定 -> 活跃任务推进 -> 事件消费”的三段式
- What it proves:
  - 风格收口没有破坏现有生命周期边界

### 2026-05-11 style follow-up: test probe shrink
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS
  - Summary:
    - `30 tests passed`
- What changed:
  - detailed backend 的测试观察口被收敛到测试侧 probe，不再把重复状态入口暴露成 public API
- What it proves:
  - 这次 API 收口没有破坏现有测试覆盖或生命周期断言

### 2026-05-09 3.16 completion checkpoint
- `Plan.md` 中 `3.16` 已更新为 `cc:完成`
- 当前总结：
  - `AML/BML/CML/release` 和 `ADC/BDC/MTE/CDC` 都已经按 `Read/Write/TerminalCompletion` 的边界收束
  - `TaskSlot` 与 `ComputeTaskState` 现在都以 terminal completion 作为真正退场点
  - 仍保留“不是完整独立实体模型”的阶段性说明，但不再阻塞 3.16 完成判定

### 2026-05-08 follow-up
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS
  - Summary:
    - `30 tests passed`

### 2026-05-08 Phase 3C Review
- Status:
  - reviewer subagent 已返回正式复审结论
- Verdict:
  - `APPROVE_WITH_NOTES`
- Evidence:
  1. `3.12-3.15` 已完成；当时 `3.16` 仍为 `WIP`，这是历史 checkpoint 口径。
  2. `detailed_cute_backend.test.opt` 当前为 `30 tests passed`。
- Issues:
  1. `Minor`
     - `Location`: `cute_detailed_design/matrix_diff_vs_rtl.md`
     - `Problem`: 推荐表述里曾残留旧的 “simplified / functional model” 总结，现已同步。
  2. `Recommendation`
     - `Location`: Phase 3C datatype 口径
     - `Problem`: `3.15` 仍需继续强调 fp 只放宽到 execute boundary，不能解读成 datatype 已基本对齐 RTL。
- Blocking Issues:
  - `None`
- Recommended Action:
  1. 当时建议保持 `3.12-3.15=完成`、`3.16=WIP`。
  2. 后续继续把 `3.16` 往多 compute in flight 与子单元级 ready/valid/backpressure 推进。

### New Evidence
- `DetailedCuteBackend.UnsupportedLoadStillReachesTerminalCompletion`
  - 证明 `loadTile()` 失败时现在也能走到 `Unsupported completion`，不会卡在 `FillPending`
- `DetailedCuteBackend.MultipleComputeTasksCanOverlapFrontUnits`
  - 证明第二条 `mma` 已可在第一条尚未 completion 时进入前级子单元，但 `MTE/CDC` 仍保持单槽 backpressure
- `DetailedCuteBackend.LoadFillUsesSharedMemoryBudget`
  - 证明 `AML/BML/CML` 路径现在至少存在一个 shared memoryBudget，load fill 不再无限并行
- `DetailedCuteBackend.Fp16MmaSucceedsWithInt32Accumulator`
  - 证明当前 `FP16 x FP16 -> raw FP32 accumulator` 已具备最小成功闭环
- `DetailedCuteBackend.ComputeWriteOccursOnlyAtTerminalStage`
  - 证明 compute 写回与 terminal completion 现在已经拆分成更接近 `WriteFinish -> TerminalCompletion` 的边界
- `DetailedCuteBackend.ComputeSubUnitsAreTrackedExplicitly`
  - 证明 `ADC -> BDC -> MTE -> CDC` 现在已经具备显式子单元状态与单元级 issue/finish 观测点

### Coverage Limits
- 当前验证仍然没有覆盖：
  - 真实端口级 valid/ready 竞争
  - `ADC/BDC/CDC/MTE` 作为独立实体的完整并行/背压关系
  - fp mma 的真实功能结果

---

## 附录 A：当前停点 / 实现摘要 / trace 口径

这部分吸收：
- `implementation_notes.md`
- `handoff.md`
- 先前分散维护的 trace/oracle 规格说明

### 当前停点

- `src/matrix` detailed CUTE backend 已进入“可收尾但未完全 RTL 等价”状态
- active implementation 当前为：
  - `taskcontrol.cc`
  - `backend_runtime.cc`
  - `mregfile.cc`
  - `memoryload.cc`
  - `matrix_ex.cc`

### 当前实现摘要

- 已成立：
  - `TaskController` 风格的 `head -> scoreboard -> ready -> issue`
  - `AML/BML/CML` 三路分流
  - `ADC/BDC/CDC/MTE` 阶段映射
  - scoreboard 分阶段释放
  - shared `memoryBudget`
  - integer mma datatype 扩展
  - fp encoding 到 execute boundary
- 仍未完全对齐：
  - 端口级 valid/ready
  - 真正 regfile / L2 端口竞争
  - `ADC/BDC/CDC/MTE` 的独立实体化
  - fp mma 功能实现

### 当前 trace 口径

- 当前 `MatrixCuteTrace` 主要覆盖：
  - `fifo_enq/deq/block`
  - `microtask_issue/occupy/finish`
  - `task_event`
  - `backend completion`
- 当前 trace 设计原则：
  - 尽量向标量 gem5 风格靠
  - 活动实现文件中，尽量直接打印枚举值，不再依赖大量字符串 helper

### 下一步建议

1. 如果继续向 RTL/CUTE 收敛：
   - 优先做 `ADC/BDC/CDC/MTE` 的进一步独立单元化
2. 如果当前准备收尾：
   - 主仓只保留主线 md
   - 其余细碎 md 后续合并后可归档或删除

### AMU shadow runtime smoke（旧 `buffered` 口径已废弃）
- 2026-04-25 之前的 `buffered [used=1/1]` artifact 只适用于旧实现
- 当前实现已改为 `alloc-time ROB-parallel shadow` 语义，不应再用旧 `buffered` trace 解释当前代码
- 当前 AMU shadow 的有效证据，以本文件 `2026-04-26 Phase 2A Single-thread 定向验证` 中的：
  - `MLS replay`
  - `squash + shadow window`
  - `AMU full/backpressure/deq`
 这三组为准

---

## 2026-04-26 Phase 2A Single-thread 定向验证

### 前提
- 本轮新增验证只覆盖 `single-thread`
- 不覆盖 `SMT`
- 不把 `AMUbuffer` 进一步 RTL 结构对齐作为本轮验证目标

### 1. MLS replay 命中
- 命令：
  - `./build/RISCV/gem5.opt --outdir=/tmp/gem5-matrix-mls-replay-st -d /tmp/gem5-matrix-mls-replay-st --debug-flags=IEW,Commit,ROB,MatrixCuteTrace --debug-file=matrix_mls_replay.log configs/example/matrix_seq_o3.py --binary /tmp/matrix_lsu_commit_probe_nogp --matrix-backend-queue-entries=1`
- Result: PASS
- 关键 trace：
  - `94000: MlsUnit S3 replay request [sn:8] cause=tlb-miss`
  - `94000: MlsReplayQueue alloc [sn:8] slot=0 ready=1`
  - `104000: MlsReplayQueue schedule [sn:8]`
  - `105500: MlsUnit S0 replay-restore [sn:8] vaddr=0x12000 stride=0x40 tile0=0x8 tile1=0x18`
  - `105500: MlsUnit S3 payload [sn:8] load=1 ... base=0x12000 stride=0x40 row=0x8 column=0x18`
  - `106500: Matrix toAMU proxy fire [sn:8] payload=lsu`
  - `106500: backend submit [sn:8] kind=lsu`
- 说明：
  - 首次 issue 命中 `tlb-miss replay`
  - replay 时已走 `replay queue -> S0 replay-restore`
  - `payload` 在 replay 成功后的 `mls S3` 生成并进入后续 commit / toAMU 路径

### 2. squash + shadow window 命中
- 新增 probe：
  - `/tmp/matrix_release_squash_window_probe.S`
  - `/tmp/matrix_release_squash_window_probe`
- 命令：
  - `./build/RISCV/gem5.opt --outdir=/tmp/gem5-matrix-shadow-squash-st -d /tmp/gem5-matrix-shadow-squash-st --debug-flags=IEW,ROB,Commit,MatrixCuteTrace --debug-file=matrix_shadow_squash.log configs/example/matrix_seq_o3.py --binary /tmp/matrix_release_squash_window_probe --matrix-backend-queue-entries=1`
- Result: PASS
- 关键 trace：
  - `89500: Matrix AMU entry alloc [sn:1] class=sync route=release`
  - `92000: Matrix toAMU proxy fire [sn:1] payload=release`
  - `497500: Matrix AMU entry alloc [sn:115] class=sync route=release`
  - `497500: Matrix AMU entry alloc [sn:116] class=sync route=release`
  - `503500: Execute: Branch mispredict detected`
  - `504500: Matrix AMU entry squash [sn:116] writebacked=0 committed=0`
  - `505000: Matrix AMU entry squash [sn:115] writebacked=1 committed=0`
  - `562500: Matrix AMU entry alloc [sn:135] class=sync route=release`
  - `565000: Matrix toAMU proxy fire [sn:135] payload=release`
- 说明：
  - older `release`、wrong-path `release`、以及 target-path younger `release` 都命中了
  - wrong-path entry 在 commit 前被 squash，未误入 backend
  - younger correct-path `release` 后续仍可进入 shadow buffer 并正常 fire

### 3. AMU full / backpressure / deq 命中

#### 3A. full + backpressure（capacity=1）
- 命令：
  - `./build/RISCV/gem5.opt --outdir=/tmp/gem5-matrix-amu-full-st -d /tmp/gem5-matrix-amu-full-st --debug-flags=Commit,ROB,MatrixCuteTrace --debug-file=matrix_amu_full.log configs/example/matrix_seq_o3.py --binary /tmp/matrix_release_double_probe --matrix-backend-queue-entries=1 --matrix-amu-buffer-entries=1`
- Result: FAIL
  - `panic: Commit stage is stucked for more than 40,000 cycles!`
- 关键 trace：
  - 后段持续出现：
    - `matrixAmuEntryBlock 1`
    - `shadowFree 1`
    - `shadowDemand 6`
    - `renameBuffered 6`
- 说明：
  - 这证明当前实现已经命中 `shadow capacity full + backpressure`
  - 同时暴露出：在 `matrix-amu-buffer-entries=1` 下，当前 shadow-admission 语义会把整条管线卡死

#### 3B. full + deq（capacity=6）
- 命令：
  - `./build/RISCV/gem5.opt --outdir=/tmp/gem5-matrix-amu-cap6-st -d /tmp/gem5-matrix-amu-cap6-st --debug-flags=Commit,ROB,MatrixCuteTrace --debug-file=matrix_amu_cap6.log configs/example/matrix_seq_o3.py --binary /tmp/matrix_release_double_probe --matrix-backend-queue-entries=1 --matrix-amu-buffer-entries=6`
- Result: PASS
- 关键 trace：
  - `89500: active 1 shadowFree 6 shadowDemand 6`
  - `89500: Matrix AMU entry alloc [sn:1] ...`
  - `89500: Matrix AMU entry alloc [sn:2] ...`
  - `90000~92000: shadowFree 0`
  - `92000: Matrix AMU entry cleanup [sn:1] ...`
  - `92000: Matrix toAMU proxy fire [sn:1] payload=release`
  - `92500: shadowFree 1`
  - `95000: Matrix AMU entry cleanup [sn:2] ...`
  - `95000: Matrix toAMU proxy fire [sn:2] payload=release`
- 说明：
  - `capacity=6` 时，首批 shadow entry 已打满
  - 后续通过 `cleanup/deq` 释放槽位，`shadowFree` 从 `0 -> 1 -> 2`
  - 这证明当前实现已命中 `full -> deq -> free slot` 的单线程路径

### 本轮结论
- 这轮新增证据足以证明：
  - `MLS replay` 新路径已被实际命中
  - `squash + shadow window` 已被实际命中
  - `AMU full/backpressure/deq` 已被实际命中
- 这轮新增证据仍不足以证明：
  - `MLS` 已对齐 RTL `s2/s3`
  - `AMU shadow buffer` 已结构对齐 RTL
  - 可以无条件宣布进入 Phase 3

## 2026-04-28 Detailed CUTE Phase 3A shell

### 本轮验证目标
- 验证新 `DetailedCuteBackend` 最小壳可编译、可运行
- 验证默认 `functional` fallback 没有被 backend 选择开关直接破坏
- 不在本轮声称：
  - `gem5.opt` 全量构建已完成
  - `detailed-cute` 已具备真实 timing
  - `scoreboard` 已达到当前阶段最小冻结语义

### 1. Detailed backend unit test
- 命令：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - `build/RISCV/matrix/detailed_cute_backend.test.opt`
- Result: PASS
- 覆盖：
  - `QueueDepthDefaultsToEight`
  - `MaintainsRequestOrder`
  - `EndToEndLoadMmaStoreReleaseSequence`
- 说明：
  - 新 backend 壳已经具备：
    - queue depth=8 默认口径
    - in-order request issue
    - `load -> zero -> mma -> store -> release` 的最小功能闭环

### 2. gem5.opt 全量构建状态
- 命令：
  - `scons -Q build/RISCV/gem5.opt -j8`
- 当前状态：
  - 构建已明确越过：
    - `src/matrix/detailed_cute_backend.cc`
    - `src/matrix/functional_cute_backend.cc`
    - `debug/MatrixCuteTrace`
  - 本轮会话内未等到全量构建完成
- 本轮顺手修复：
  - `src/matrix/SConscript` 增加 `DebugFlag('MatrixCuteTrace')`
  - 解决 `src/matrix/*` 编译时找不到 `debug/MatrixCuteTrace.hh` 的问题

### 3. 本轮未完成/受环境影响项
- `scons -Q build/RISCV/matrix/functional_cute_backend.test.opt --unit-test -j8`
  - 命中环境/构建树问题：
    - `build/RISCV/systemc/tlm_core/2/quantum/SConscript` 缺失
- `scons -Q build/RISCV/arch/riscv/matrix_sync_check.opt -j8`
  - 当前 scons 直接 file target 口径下未识别该目标路径

### 本轮结论
- 可以确认：
  - `DetailedCuteBackend` 最小壳已落地
  - `matrixBackendMode=functional|detailed-cute` 已接入 CPU/config
  - `detailed-cute` 至少有独立单元测试闭环
- 不能确认：
  - `gem5.opt` 全量构建最终成功
  - `functional` 旧单测已在当前环境下重新完整跑过

### 2026-04-28 Phase 3B follow-up

#### 新增验证
- 命令：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - `build/RISCV/matrix/detailed_cute_backend.test.opt`
- Result: PASS
- 当前测试数：
  - `5 tests from 2 test suites`
- 新增覆盖：
  - `DecodedFifo.DecodeMmaNormalizesReadWriteSets`
  - `DetailedCuteBackend.ReleaseWaitsForPendingStoreCompletion`

#### 说明
- 当前 `DecodedFifo` 已验证：
  - request -> decoded entry 的最小规范化
- 当前 `DetailedCuteBackend` 已验证：
  - queue depth
  - request in-order
  - `release` 不会在 pending store 尚未完成时立刻通过
  - `issue` 与 `completion` 已分离到不同 step

### 2026-04-28 Phase 3C follow-up

#### 新增验证
- 命令：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - `build/RISCV/matrix/detailed_cute_backend.test.opt`
- Result: PASS
- 当前测试数：
  - `7 tests from 3 test suites`
- 新增覆盖：
  - `DetailedCuteScoreboard.LoadReserveBlocksDependentComputeUntilCompletion`
  - `DetailedCuteScoreboard.StoreReaderBlocksOverwriteUntilCompletion`

#### 说明
- 当前 `DetailedCuteScoreboard` 已验证：
  - load destination reserve 后，依赖该 source 的 compute 不能 issue
  - 对应 load completion 后，compute 可以重新通过 `canIssue`
  - store 作为 `C` reg reader 时，会通过 `pendingReaders` 阻塞后续 writer
  - store completion 后，对同一 `C` reg 的覆盖重新允许

### 2026-04-28 FIFO / scoreboard trace follow-up

#### 新增验证
- 命令：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - `build/RISCV/matrix/detailed_cute_backend.test.opt`
- Result: PASS
- 说明：
  - 本轮是在 **启用 `with_tag('gem5 trace')`** 后重新编译、重新运行
  - 证明新增的 `MatrixCuteTrace` 依赖没有破坏现有 7 个单测

#### 当前可观测 trace 点
- `fifo_enq`
- `fifo_deq`
- `fifo_block`
- `scoreboard_block`
- `backend completion`

#### 当前限制
- 这轮只证明 trace 代码可编译、带 trace 的单测仍通过
- 还没有跑 O3/SE smoke 去抓实际 `matrix_cute_trace.log`

### 2026-04-28 Phase 3D follow-up

#### 新增验证
- 命令：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - `build/RISCV/matrix/detailed_cute_backend.test.opt`
- Result: PASS
- 当前测试数：
  - `8 tests from 3 test suites`
- 新增覆盖：
  - `DetailedCuteBackend.MatrixRegFileTracksOwnerAndLastWriter`

#### 说明
- 当前 `MatrixRegFile` 已验证：
  - load issue 后，目标 reg:
    - `allocated = true`
    - `owner = AML`
    - `lastWriterKind = Load`
  - 对应 completion 后：
    - `owner` 会清回 `None`

#### 当前结论
- 可以确认：
  - `DetailedCuteBackend` 已不再直接依赖 `MatrixState` 作为内部 reg namespace
  - `MatrixRegFile` 已成为 detailed backend 的 backend-owned regfile 实体
- 不能确认：
  - Phase 4 数据面 / bank / timing 已成立

### 2026-04-28 Phase 3 remaining gaps follow-up

#### 新增验证
- 命令：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - `build/RISCV/matrix/detailed_cute_backend.test.opt`
- Result: PASS
- 当前测试数：
  - `10 tests from 3 test suites`
- 新增覆盖：
  - `DetailedCuteScoreboard.ComputeLifecycleClearsReadersInStages`
  - `DetailedCuteBackend.ScoreboardBlockCounterTracksSrcNotReady`

#### 说明
- 当前已验证：
  - compute 的 A/B/C source 生命周期可按阶段解除
  - scoreboard block 计数可区分 `SrcNotReady`
  - FIFO / scoreboard 组合已经能稳定产出更细的 Phase 3 行为

#### 当前限制
- `fifo / scoreboard / regfile` 仍未进入 Phase 4 timing model

### 2026-04-30 Phase 3 runtime trace follow-up

#### 新增验证
- 构建：
  - `scons -Q build/RISCV-clean/gem5.opt TARGET_ISA=riscv PROTOCOL=MI_example --linker=gold -j64 --rvv-impl=simple`
- 运行：
  - `./build/RISCV-clean/gem5.opt -d /tmp/gem5-matrix-trace --debug-flags=MatrixCuteTrace --debug-file=matrix_cute_trace.log ./configs/example/matrix_seq_o3.py --binary /tmp/matrix_release_acquire_probe`
  - `./build/RISCV-clean/gem5.opt -d /tmp/gem5-matrix-release-double --debug-flags=MatrixCuteTrace --debug-file=matrix_cute_trace.log ./configs/example/matrix_seq_o3.py --binary /tmp/matrix_release_double_probe`
  - `./build/RISCV-clean/gem5.opt -d /tmp/gem5-matrix-lsu-order --debug-flags=MatrixCuteTrace --debug-file=matrix_cute_trace.log ./configs/example/matrix_seq_o3.py --binary /tmp/matrix_lsu_queue_order_probe`
  - `./build/RISCV-clean/gem5.opt -d /tmp/gem5-matrix-release-squash --debug-flags=MatrixCuteTrace --debug-file=matrix_cute_trace.log ./configs/example/matrix_seq_o3.py --binary /tmp/matrix_release_squash_window_probe`
  - `./build/RISCV-clean/gem5.opt -d /tmp/gem5-matrix-lsu-replay --debug-flags=MatrixCuteTrace --debug-file=matrix_cute_trace.log ./configs/example/matrix_seq_o3.py --binary /tmp/matrix_lsu_replay_probe`
  - `./build/RISCV-clean/gem5.opt -d /tmp/gem5-matrix-mma-end2end --debug-flags=MatrixCuteTrace --debug-file=matrix_cute_trace.log ./configs/example/matrix_seq_o3.py --binary /tmp/matrix_mma_end2end_probe_nogp`

#### 关键 trace 证据
- `release_acquire_probe`
  - `backend submit -> fifo_enq -> fifo_deq -> backend completion` 已在真实 O3/SE 路径上出现
  - 证据文件：
    - `/tmp/gem5-matrix-trace/matrix_cute_trace.log`
- `release_double_probe`
  - 两次独立 `release` 都形成完整链路：
    - `[sn:1] submit/enq/deq/completion`
    - `[sn:2] submit/enq/deq/completion`
  - 说明 repeated release 在当前 detailed backend 下可稳定通过
- `lsu_queue_order_probe`
  - `[sn:44]` 先发射
  - `[sn:45]` 因 `reason=downstream_not_accepting` 被 `fifo_block`
  - `[sn:46]` 入队后仍等待 `[sn:45]`，直到后者完成才 `fifo_deq`
  - 说明当前 `decoded_fifo` 已有真实 runtime 的 in-order / head gating / downstream backpressure 证据
- `release_squash_window_probe`
  - trace 中可见两个相距较远的 `release` request 都完成：
    - `[sn:1]`
    - `[sn:19]`
  - 当前 probe 至少证明 squash window 场景没有把后续 `release` 链路打坏
- `lsu_replay_probe`
  - 当前只看到单次 LSU request 的 `submit/enq/deq/completion`
  - 还没有在 `MatrixCuteTrace` 中直接看到更强的 replay-specific backend 事件

#### 未通过 / 证据不足
- `matrix_mma_end2end_probe_nogp`
  - 只看到首个 LSU request:
    - `[sn:12] submit -> fifo_enq -> fifo_deq`
  - 随后在 tick `107500` 命中 page-table fault，未拿到 `mma/store/release` 的 backend trace
  - 因此它不能作为 `mma detailed backend end-to-end` 已收敛的证据

### 2026-04-30 Phase 3 runtime trace follow-up 2

#### 新增验证
- 运行：
  - `./build/RISCV-clean/gem5.opt -d /tmp/gem5-matrix-mma-nostore --debug-flags=MatrixCuteTrace --debug-file=matrix_cute_trace.log ./configs/example/matrix_seq_o3.py --binary /tmp/matrix_mma_probe_nostore`
  - `./build/RISCV-clean/gem5.opt -d /tmp/gem5-matrix-lsu-queue-squash --debug-flags=MatrixCuteTrace --debug-file=matrix_cute_trace.log ./configs/example/matrix_seq_o3.py --binary /tmp/matrix_lsu_queue_squash_probe`
  - `./build/RISCV-clean/gem5.opt -d /tmp/gem5-matrix-lsu-replay-squash --debug-flags=MatrixCuteTrace --debug-file=matrix_cute_trace.log ./configs/example/matrix_seq_o3.py --binary /tmp/matrix_lsu_replay_squash_probe`
  - `./build/RISCV-clean/gem5.opt -d /tmp/gem5-matrix-lsu-replay-cancel --debug-flags=MatrixCuteTrace --debug-file=matrix_cute_trace.log ./configs/example/matrix_seq_o3.py --binary /tmp/matrix_lsu_replay_cancel_probe`

#### 新增有效证据
- `matrix_mma_probe_nostore`
  - 相比 `matrix_mma_end2end_probe_nogp`，当前 probe 已不再在首个 LSU 后立即 fault
  - trace 已看到：
    - `[sn:10] lsu submit/enq/deq/completion`
    - `[sn:11] lsu submit + downstream_not_accepting`
    - `[sn:12] arith submit/enq`
    - `[sn:13] mma submit/enq`
  - 这至少证明：
    - `arith`
    - `mma`
    已经能进入 detailed backend 的 FIFO 层
  - 但本轮 trace 还没继续走到 `mma fifo_deq / compute completion`
- `matrix_lsu_queue_squash_probe`
  - 当前只看到单个 LSU request 的最小闭环：
    - `submit -> fifo_enq -> fifo_deq -> completion`
  - 说明 squash 场景没有直接破坏最小 LSU backend contract
- `matrix_lsu_replay_squash_probe`
  - 当前也只看到单个 LSU request 的最小闭环
  - 还不足以单独证明更强的 replay-specific backend 行为

#### 仍未补强的点
- `matrix_lsu_replay_cancel_probe`
  - stats 正常产出，但 `MatrixCuteTrace` 中没有看到可用于 backend issue/dependency/stall 结论的高价值事件
- `matrix_mma_probe_nostore`
  - 虽然已看到 `mma submit/enq`
  - 但还没有看到：
    - `mma fifo_deq`
    - `compute completion`
  - 所以它还不能作为 `mma end-to-end runtime` 证据

#### 当前结论
- 现在可以确认：
  - `DetailedCuteBackend` 已在真实 `O3 + SE + matrix_seq_o3.py` 路径上跑通最小 runtime 闭环
  - `release` 类 request 的 submit/queue/complete contract 已有 runtime 证据
  - `decoded_fifo` 的队首阻塞与 downstream backpressure 已有 runtime 证据
- 现在还不能确认：
  - `mma` detailed backend end-to-end 已跑通
  - replay-specific backend 语义已经有足够强的 runtime 证据

---

## 2026-04-30 Phase 4.0 / 4.1 文档冻结验证

### Verification Performed

#### Static document/code cross-check
- 读取并交叉核对：
  - 当时分散维护的 AML/BML/CML 与 matrix regfile 设计说明
  - `Plan.md`
  - `CUTE/src/main/scala/CUTEParameters.scala`
  - `CUTE/src/main/scala/AMemoryLoader.scala`
  - `CUTE/src/main/scala/BMemoryLoader.scala`
  - `CUTE/src/main/scala/CMemoryLoader.scala`
  - `CUTE/src/main/scala/ADataController.scala`
  - `CUTE/src/main/scala/BDataController.scala`
  - `CUTE/src/main/scala/CDataController.scala`
  - `CUTE/src/main/scala/MatrixTE.scala`

### What This Proves

- `Phase 4.0`
  - `AML/BML/CML` 的职责边界、`read finish / write finish / completion_event` 语义已经冻结成文
  - `CML-store` 已明确拆分为：
    - `store_read_finish`
    - `store_write_finish`
- `Phase 4.1`
  - `matrix_regfile` 的聚合 beat 宽度已经冻结成文：
    - `AB = 1024b/cycle`
    - `C = 512b/cycle`
  - 这些数值与 CUTE 参数、DataController/MatrixTE 接口宽度一致

### Coverage Limits

- 本轮没有新增：
  - build
  - unit test
  - runtime trace
- 因此本轮验证只证明：
  - 文档口径与当前本地代码/RTL 参考一致
- 本轮不能证明：
  - `4.2` microtask 壳已经正确实现
  - `4.3` completion_event 已经跑通
  - `AB/C` 多 channel 同拍并发能力已经建模

---

## 2026-04-30 Phase 5 functional verification

### Verification Performed

#### Build
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`

#### Unit test
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`

### What This Proves

- `mma` 已不再沿用旧 `inflight` 的 issue-time execute 壳
- `DetailedCuteBackend` 当前已具备最小 compute microtask 生命周期：
  - `issue`
  - `readA`
  - `readB`
  - `compute`
  - `writeC`
  - `finish`
- `A/B` source 会在 read finish 前被 snapshot
- `C` 结果只会在 terminal `writeC` 阶段回写
- compute latency 会随 tile shape 增大而单调增加
- 当前 datatype 策略已收紧：
  - `int8 -> int32` 为 Required
  - fp/non-default type encoding 当前返回 `Unsupported`
- `mzero` 继续留在 load-like zero family，没有混入 compute path

### Unit Tests Added / Covered

- `ComputeWriteOccursOnlyAtTerminalStage`
- `ComputeSnapshotsABBeforeWriteback`
- `ComputeLatencyScalesWithTileShape`
- `UnsupportedFpMmaReturnsUnsupportedCompletion`
- 以及原有：
  - `EndToEndLoadMmaStoreReleaseSequence`
  - `MicroTaskLifecycleCountersTrackIssueOccupyFinish`
  - `ReleaseWaitsForPendingStoreCompletion`

### Coverage Limits

- 本轮按用户要求没有补 trace
- 因此本轮不能证明：
  - `compute_issue -> readA/readB/writeC` 的 runtime event 已与 CUTE trace 对齐
  - 当前 timing 公式与 RTL cycle 精确一致
  - `Matrix_MN / ReduceWidthByte` 已通过配置项可调

## 2026-05-12 SE `gemm_precomp` runtime unblock verification

### Verification Performed

#### Build
- `scons build/RISCV/matrix/detailed_cute_backend.test.opt -j1`
  - Result: PASS
- `scons build/RISCV/gem5.opt --linker=gold -j64 --rvv-impl=simple`
  - Result: PASS

#### Targeted regression
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.DefaultBackendAcceptsArchitecturalRegIndexFour'`
  - Result: PASS

#### Backend unit suite snapshot
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: 34 PASS, 1 FAIL
  - Note:
    - 当时仍有 1 条 fp compute 相关 case 未闭环
    - 这条历史失败已在 2026-05-13 后续 `4.4` 收口中解决

#### SE runtime
- `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-precomp configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
  - Result: PASS
  - Summary:
    - 原始 `IndexError: vector::_M_range_check` 不再出现
    - `All 8 precomp tests PASSED.`

### What This Proves

- 当前 detailed backend 的默认 matrix regfile / scoreboard 容量已不再和 ISA 的 `0..7` 寄存器编码冲突。
- SE 模式下 `gemm_precomp` 的 integer GEMM happy-path 已从运行期越界恢复到完整 functional pass。
- 这次验证只证明：
  - 功能闭环恢复
  - 运行期 reg-index 越界被修掉
- 这次验证不证明：
  - Phase 3 已完成
  - datatype / fp 覆盖已对齐
  - 资源/时序已与 RTL/CUTE 对齐

## 2026-05-12 task review

### Review Target
- Task:
  - Phase 3 WIP follow-up: SE `gemm_precomp` runtime unblock
- Review type:
  - task review
- Claimed transition:
  - `WIP -> WIP`

### Required Gates

| Gate | Result | Evidence | Issue if FAIL |
|---|---|---|---|
| Scope matches current Phase / Task | PASS | 只处理 SE 模式 `gemm_precomp` 的运行期越界，没有扩到 Phase 3 之外。 | - |
| DoD is observable and reproducible | PASS | `gemm_precomp` 原始命令已复跑，最终输出 `All 8 precomp tests PASSED.` | - |
| Depends / Status / DoD are self-consistent | PASS | 本轮没有把 `Plan.md` 中任何 `WIP` task 强行改成完成。 | - |
| No completed task depends on TODO without explanation | PASS | 本轮只做 WIP bugfix checkpoint，没有新增状态强化。 | - |
| Claimed alignment does not exceed evidence | PASS | 只声称 SE functional pass 恢复，不声称 RTL/CUTE 对齐。 | - |
| Functional correctness is separated from resource/timing alignment | PASS | 本节明确把功能恢复与资源/时序结论分开。 | - |
| Missing evidence is marked as `证据不足` | PASS | 仍保留 fp/datatype/时序未证明项。 | - |
| `verify_report` contains required evidence | PASS | 本节已记录 build、单测、SE runtime 命令和结果。 | - |
| `handoff` reflects current stop point | PASS | 本轮会同步 handoff，写明“SE gemm 已过，但 `Plan.md` 不变”。 | - |
| `matrix_diff_vs_rtl.md` is consistent with `Plan.md` | PASS | 本轮没有强化 `matrix_diff_vs_rtl.md` 和 `Plan.md` 的对齐口径。 | - |
| Review reports RTL alignment, design complexity, and code style as separate dimensions | PASS | 见下方 Review Dimensions。 | - |

### Review Dimensions

| Dimension | Result | Evidence | Issue if FAIL / N/A reason |
|---|---|---|---|
| RTL Alignment | PASS | 结论只限于 SE functional pass 恢复，没有越过 `matrix_diff_vs_rtl.md` 的未对齐边界。 | - |
| Design Complexity | PASS | 修复只改默认寄存器槽位和一条回归测试。 | - |
| Code Style | PASS | 代码风格已在 [implementation_notes.md](/nfs/home/hujun/GEM5/cute_detailed_design/implementation_notes.md:772) 单独复核。 | - |

### Evidence
- [src/matrix/matrix_regfile.hh](/nfs/home/hujun/GEM5/src/matrix/matrix_regfile.hh:57)
- [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc:278)
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.DefaultBackendAcceptsArchitecturalRegIndexFour'`
- `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-precomp configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`

### Issues
- 当时仍有 1 条 fp compute 相关 case 未闭环，但它不属于当次 integer SE gemm runtime unblock 范围；该问题已在 2026-05-13 后续 `4.4` 收口中解决。

### Blocking Issues
- None

### Required Document Updates
- `cute_detailed_design/implementation_notes.md`
- `cute_detailed_design/handoff.md`

### Recommended Action
- 保持 `Plan.md` 状态不变，把本轮视为 WIP checkpoint，而不是 task completion。

### Verdict

`APPROVE_WITH_NOTES`

## 2026-05-18 RISC-V matrix load/store encoding verification

### Verification Performed
- `scons -Q build/RISCV/gem5.opt -j8`
  - Result: PASS
  - Notes:
    - 仅有环境 warning：`png.h`、HDF5、back trace implementation 缺失。
- `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-precomp configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
  - Result: PASS
  - Summary: `All 8 precomp tests PASSED.`
- `scons -Q build/RISCV/arch/riscv/matrix_sync_check.opt -j8`
  - Result: PARTIAL / known link issue
  - Summary: `matrix_sync_check.cc` compiled, standalone link still fails with missing gem5 base symbols such as `Serializable`, `Logger`, `SimObject`.
- `git diff --check -- src/arch/riscv/isa/decoder.isa src/cpu/o3/dyn_inst.cc src/cpu/o3/mls_unit.cc src/arch/riscv/matrix_sync_check.cc cute_detailed_design/matrix_o3_README.md`
  - Result: PASS

### Evidence
- Before fix, SE `gemm_precomp` failed at `0x24a48a2b` with `mlae16 parameter check failed`.
- QEMU/toolchain decode facts:
  - `RD[2:0]` is `%md`.
  - `RD[4:3]` is matrix load/store width.
  - `0x24a48a2b` has `funct7=0x12`, `width=2`, `md=4`, so it is `mlce32`.
- After fix, the same AGENTS.md SE command reaches all eight workload cases:
  - `zeros`, `ones`, `max`, `min`, `rand0`, `rand1`, `rand2`, `rand3`
  - final line: `All 8 precomp tests PASSED.`

### Coverage Boundary
- 覆盖：RISC-V/O3 matrix load/store decode metadata、MLS early fault/access size/payload field interpretation、SE `gemm_precomp` functional smoke。
- 不覆盖：standalone `matrix_sync_check.opt` runtime、MatrixReg bank timing、MTE accepted-beat timing、CDC 512 D beat writeback、cache/L2 behavior。

## 2026-05-16 Humanize Round 3 review blocker verification

### Verification Performed
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS after implementation.
  - Red step: checkpoint guard tests initially failed to compile because `MatrixRegFile::hasAllocatedState()` and `DetailedCuteBackend::hasArchitecturalState()` did not exist.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='MatrixRegFile.StoresTensorAndAllocatedState:DetailedCuteBackend.ReportsCompletedMatrixRegisterStateForCheckpointGuard'`
  - Result: PASS
  - Summary: `2 tests from 2 test suites ran` / `2 passed`.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS
  - Summary: `47 tests from 6 test suites ran` / `47 passed`.
- `scons -Q build/RISCV/gem5.opt -j8`
  - Result: PASS
  - Notes: only existing environment warnings for missing `png.h`, HDF5, and backtrace implementation.

### Negative / Limited Verification
- `build/RISCV/arch/riscv/matrix_sync_check.opt`
  - Result: FAIL
  - Failure: `matrix_sync_check: mlae16 request width/elem type should be e16/fp16`.
  - Classification: existing checker expectation failure, not used as Round 3 pass evidence.
- `scons -Q build/RISCV/arch/riscv/matrix_sync_check.opt -j8`
  - Result: FAIL
  - Failure: link errors with missing gem5 base symbols such as `Serializable`, `Logger`, and `SimObject`.
  - Classification: standalone checker link issue; `build/RISCV/gem5.opt` still builds successfully.

### Coverage Boundary
- Covered: checkpoint live-state predicate and backend exposure, full detailed backend regression, O3/matrix code compilation in `gem5.opt`.
- Covered by code-path inspection plus build: FS matrix memory uses page-aware `TranslatingPortProxy`; O3 `msyncregreset` reset is staged on `DynInst` and consumed from commit.
- Not covered by runtime workload: full-system cross-page matrix load/store fault behavior and speculative squash replay of `msyncregreset`.

## 2026-05-15 Phase 4A MTE timing / all-ready verification (Round 0, superseded)

Round 1 已修正本节的 small-shape contract。下面记录保留为历史 Round 0 验证，不再作为当前 MTE shape 结论；当前结论见下一节 `Round 1 active RTL MTE timing correction verification`。

### Verification Performed
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS
  - Notes:
    - 仅有环境 warning：`png.h`、HDF5、back trace implementation 缺失。
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.MteTimingReportsPerCycleBandwidthAndAcceptedBeats:DetailedCuteBackend.MteTimingFollowsActiveRtlControllerShape:DetailedCuteBackend.MteRejectsMatricesLargerThanFixedShape:DetailedCuteBackend.ComputeLatencyPadsSmallTilesToFixedShape:DetailedCuteBackend.ComputeLatencyScalesWithDatatypeWidth'`
  - Result: PASS
  - Summary: `5 tests from DetailedCuteBackend passed`
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.ComputePathRequiresAdcBdcCdcReady:DetailedCuteBackend.MultipleComputeTasksCanOverlapFrontUnits:DetailedCuteBackend.SecondComputeWaitsOnAdcBdcCdcAvailabilityNotScoreboardComputeBusy'`
  - Result: PASS
  - Summary: `3 tests from DetailedCuteBackend passed`
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS
  - Summary: `40 tests from 5 test suites passed`

### Evidence
- `MteTimingReportsPerCycleBandwidthAndAcceptedBeats`
  - A/B/C/D per accepted beat 均为 `256B = 2048b`。
  - 常量口径：`Tensor_MN=128`, `Tensor_K=64`, `Matrix_MN=8`, `ReduceWidthByte=32`, `ResultWidthByte=4`。
- `MteTimingFollowsActiveRtlControllerShape`
  - `128x128x64` int8 accepted input beats: `512`
  - `4x128x32` int8 accepted input beats: `16`
  - e16/e32/e4 full shape accepted input beats: `1024/2048/256`
  - Round 1/2 结论：`4x4x32` / `64x64x32` 这类 partial-N timing contract 已不再成立；当前 active RTL-derived contract 拒绝 `mtilen != 128`。accepted input beats 按 RTL controller tile-pair 迭代数建模，unique A/B block `32` 只作为数据量解释口径。
- `MteRejectsMatricesLargerThanFixedShape`
  - `129x128x64`, `128x129x64`, `128x128x65` 均 unsupported。
- `ComputePathRequiresAdcBdcCdcReady`
  - 第二条 MMA 在 BDC busy 时不会 dequeue。
  - 第一条进入 MTE 后，因 ADC/BDC/CDC all-ready 已满足，第二条可以 dequeue；MTE busy 不属于 issue gate。

### Coverage Boundary
- 覆盖：`ADC/BDC/CDC` issue gate、MTE bandwidth、fixed physical shape、accepted input beats、unsupported shape、compute writeback/completion 边界不回归。
- 不覆盖：逐拍 FReducePE accumulator 数值、完整 CDC reorder/after-op、memory path、MLS replay/cancel、carrier/AMU buffer 结构。
- 结论：这是 resource contract / timing-boundary checkpoint，不是 RTL timing equivalence approval。

## 2026-05-15 Round 1 active RTL MTE timing correction verification

### Verification Performed
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS
  - Notes:
    - 仅有环境 warning：`png.h`、HDF5、back trace implementation 缺失。
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.MteTimingReportsPerCycleBandwidthAndAcceptedBeats:DetailedCuteBackend.MteTimingFollowsActiveRtlControllerShape:DetailedCuteBackend.MteRejectsMatricesLargerThanFixedShape:DetailedCuteBackend.MteTotalCompletionCyclesMatchesComputeTerminalBoundary:DetailedCuteBackend.EndToEndLoadMmaStoreReleaseSequence'`
  - Result: PASS
  - Summary: `5 tests from DetailedCuteBackend passed`
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS
  - Summary: `41 tests from 5 test suites passed`

### Evidence
- `MteTimingFollowsActiveRtlControllerShape`
  - `128x128x64` int8 accepted input beats: `512`
  - `4x128x32` int8 accepted input beats: `16`
  - `64x64x32` 当前 unsupported，因为 active `CDataController.scala` assert `N == Tensor_MN`，暂无 partial-N compute 证据。
- `MteTimingReportsPerCycleBandwidthAndAcceptedBeats`
  - `MteTiming` 显式拆出 `adcReadCycles`、`bdcReadCycles`、`mteAcceptedInputBeats`、`fReduceTailCycles`、`cdcWriteCycles`、`terminalHandshakeCycles`。
- `MteTotalCompletionCyclesMatchesComputeTerminalBoundary`
  - `totalCompletionCycles` 已定义为 GEM5 compute issue -> terminal completion。
  - targeted test 比较 `traceCounters().lastMicrotaskLatency == computeMteTiming(desc).totalCompletionCycles`。
  - 同一 test 覆盖 C register 在 CDC write 前不可见、terminal completion 前不可见 completion。
- `EndToEndLoadMmaStoreReleaseSequence`
  - 端到端 load/mma/store/release 测试已迁移到当前 supported 的 `2x128x32` compute shape，并只检查左上角 `2x2` functional 摘要结果。

### Coverage Boundary
- 覆盖：active RTL-derived MTE accepted input beat contract、partial-N unsupported、GEM5 runtime completion boundary、既有 detailed backend suite 不回归。
- 不覆盖：RTL cycle-accurate `ComputeGo` 波形、CDC reorder/after-op、MatrixReg bank arbitration、functional int4、memory path/MLS/carrier。
- 结论：AC-4/AC-5 的 GEM5 helper 与 runtime 边界已有可执行证据；仍不能写成 RTL timing equivalent。

## 2026-05-16 Round 2 restore 512 MTE accepted beats

### Verification Performed
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS
  - Notes:
    - 仅有环境 warning：`png.h`、HDF5、back trace implementation 缺失。
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.MteTimingReportsPerCycleBandwidthAndAcceptedBeats:DetailedCuteBackend.MteTimingFollowsActiveRtlControllerShape:DetailedCuteBackend.ComputeLatencyFollowsActiveRtlKGroups:DetailedCuteBackend.ComputeLatencyScalesWithDatatypeWidth'`
  - Result: PASS
  - Summary: `4 tests from DetailedCuteBackend passed`

### Evidence
- TDD 红灯曾确认旧实现返回 `32/64/128/16`，与当前 full-shape RTL tile-pair beat 口径不符。
- `computeMteTiming()` 已恢复为 `ceil(mtilem / 8) * (mtilen / 8) * k_groups`。
- `128x128x64` int8 accepted input beats 为 `512`；e16/e32/e4 full shape accepted input beats 为 `1024/2048/256`。
- `4x128x32` int8 accepted input beats 为 `16`；`mtilen != 128` 仍 unsupported。

### Coverage Boundary
- 覆盖：MTE timing helper、datatype K scaling、partial-M active shape、targeted runtime latency scaling。
- 不覆盖：MatrixReg bank arbitration、ADC/BDC/CDC 并行 read beat runtime 重构、CDC 512 D beat writeback boundary、MLOAD/MSTORE bandwidth window。

## 2026-05-16 MatrixRegResource helper verification

### Verification Performed
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: expected FAIL before helper implementation
  - Summary: `fatal error: matrix/matrix_reg_resource.hh: No such file or directory`
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS after helper implementation
  - Notes:
    - 仅有环境 warning：`png.h`、HDF5、back trace implementation 缺失。
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='MatrixRegResource.*'`
  - Result: PASS
  - Summary: `5 tests from MatrixRegResource passed`

### Evidence
- `GrantsFullBankReadWithOneCycleResponse`
  - full-bank read 当拍 grant，下一拍才有 read response。
- `RejectsPartialBankVectorAccess`
  - partial bank mask 不被视为成功 vector access。
- `AbLoaderWritePriorityStallsDataControllerRead`
  - A/B MemoryLoader write 同拍优先，DataController read stall 且无 read response。
- `CReadWriteSameParityConflictStalls`
  - C MatrixReg 同 parity read/write 不被静默并行。
- `CReadWriteOppositeParityCanShareCycle`
  - C MatrixReg 不同 parity read/write 可同拍 grant，read response 下一拍出现。

### Coverage Boundary
- 覆盖：helper-level 资源仲裁语义。
- 不覆盖：helper 接入 runtime 后的 compute/load/store backpressure、CDC D beat writeback、MLOAD/MSTORE 64B/cycle memory request window。

## 2026-05-18 O3 MMA opcode metadata passthrough verification

### Verification Performed
- `scons -Q build/RISCV/cpu/o3/matrix_o3_payload.test.opt --unit-test -j8`
  - Result: PASS
  - Red step before helper implementation: expected compile failure, `fatal error: cpu/o3/matrix_payload.hh: No such file or directory`.
  - Notes: only existing environment warnings for missing `png.h`, HDF5, and back trace implementation.
- `./build/RISCV/cpu/o3/matrix_o3_payload.test.opt`
  - Result: PASS
  - Summary: `1 test from MatrixO3Payload passed`.
- `scons -Q build/RISCV/gem5.opt -j8`
  - Result: PASS
  - Notes: only existing environment warnings for missing `png.h`, HDF5, and back trace implementation.
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS
  - Summary: target was up to date after the focused O3 fix.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.Fp16MmaSucceedsWithInt32Accumulator:DetailedCuteBackend.MteTimingReportsPerCycleBandwidthAndAcceptedBeats:MatrixRegResource.*'`
  - Result: PASS
  - Summary: `7 tests from 2 test suites passed`.
- `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-precomp-o3-op configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
  - Result: PASS
  - Summary: `All 8 precomp tests PASSED.`
- `util/style.py -m --checker SortedIncludes --checker Whitespace --checker LineLength src/cpu/o3/SConscript src/cpu/o3/rob.cc src/cpu/o3/matrix_payload.hh src/cpu/o3/matrix_payload.cc src/cpu/o3/matrix_o3_payload.test.cc`
  - Result: PASS
- `git diff --check -- src/cpu/o3/SConscript src/cpu/o3/rob.cc src/cpu/o3/matrix_payload.hh src/cpu/o3/matrix_payload.cc src/cpu/o3/matrix_o3_payload.test.cc`
  - Result: PASS

### Evidence
- `MatrixO3Payload.MmaConversionPreservesDecodedOpcode` constructs an FP MMA `MatrixExecPayload` with `payload.op = 0x04`, converts it through the same helper used by `ROB::noteMatrixAmuWriteback()`, and asserts:
  - `req.kind == CuteRequestKind::Mma`
  - `req.op == payload.op`
  - `req.op != 0x0c`
  - FP/type metadata remains present.
- `rob.cc` now uses `matrixPayloadToCuteRequest()` instead of a private anonymous conversion function, so the focused test covers the ROB conversion logic used before AMU backend submission.

### Coverage Boundary
- 覆盖：O3 `MatrixExecPayload -> CuteRequest` conversion metadata, `gem5.opt` compilation, existing detailed backend focused regressions, and SE `gemm_precomp` no-regression smoke.
- 不覆盖：standalone `matrix_sync_check.opt` link issue, active ISA FP workload end-to-end numerical execution, MatrixReg bank runtime integration, CDC 512 D beat writeback, MLOAD/MSTORE bandwidth timing.

## 2026-05-18 Phase 4A compute read frontend verification

### Verification Performed
- Red step:
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.ComputeReadFrontendIssuesAdcBdcCdcInParallel'`
  - Result: expected FAIL before runtime change.
  - Failure showed old runtime issued only `ADC` on the first step, left `BDC/CDC` not busy, and kept B/C pending readers.
- Red step:
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.ComputeReadFrontendStallsOnCWriteConflict'`
  - Result: expected FAIL before conflict integration.
  - Failure showed CDC read advanced despite same-parity C write conflict.
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS.
  - Notes: only existing environment warnings for missing `png.h`, HDF5, and back trace implementation.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.ComputeReadFrontend*:DetailedCuteBackend.ComputeSubUnitsAreTrackedExplicitly:DetailedCuteBackend.MultipleComputeTasksCanOverlapFrontUnits:DetailedCuteBackend.SecondComputeWaitsOnAdcBdcCdcAvailabilityNotScoreboardComputeBusy:DetailedCuteBackend.ComputePathRequiresAdcBdcCdcReady:DetailedCuteBackend.MteTimingReportsPerCycleBandwidthAndAcceptedBeats:DetailedCuteBackend.MteTimingFollowsActiveRtlControllerShape:DetailedCuteBackend.MteTotalCompletionCyclesMatchesComputeTerminalBoundary:DetailedCuteBackend.ComputeWriteOccursOnlyAtTerminalStage:DetailedCuteBackend.ComputeKeepsCDestBusyUntilWriteFinishAndCompletion:DetailedCuteBackend.ComputeSnapshotsABBeforeWriteback:DetailedCuteBackend.EndToEndLoadMmaStoreReleaseSequence:DetailedCuteScoreboard.ComputeLifecycleClearsReadersInStages:MatrixRegResource.*'`
  - Result: PASS.
  - Summary: `19 tests from 3 test suites passed`.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS.
  - Summary: `49 tests from 6 test suites passed`.

### Evidence
- `ComputeReadFrontendIssuesAdcBdcCdcInParallel`
  - After MMA issue, `ADC/BDC/CDC` are busy in the same backend step.
  - A/B/C pending readers remain until the next step read response.
  - On the next step, all three pending readers are released and MTE becomes busy.
- `ComputeReadFrontendStallsOnCWriteConflict`
  - Same-parity C MatrixReg write and CDC read in one step prevents compute read issue.
  - Pending C reader is not released until the conflict clears and the next 1-cycle response arrives.
- `MteTotalCompletionCyclesMatchesComputeTerminalBoundary`
  - `totalCompletionCycles` now uses the parallel read frontend latency, with `cdcReadCycles=1`.

### Coverage Boundary
- 覆盖：ADC/BDC/CDC 同拍 read request、1-cycle response、C source pending reader release、C same-parity read/write conflict stall、既有 detailed backend suite 不回归。
- 不覆盖：CDC `512` D beat writeback window、MLOAD/MSTORE external `64B/cycle` bandwidth and fixed latency、cache/L2/MSHR/coherence/retry/replay、精确 FReducePE 数值流水。

## 2026-05-19 CDC tile-level D beat writeback verification

### Commands
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS.
  - Notes: only existing environment warnings for missing `png.h`, HDF5, and back trace implementation.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.CdcInt8TileWriteback*:DetailedCuteBackend.CdcWriteback*:DetailedCuteBackend.Cdc*TileRead*:DetailedCuteBackend.Cdc*TileWrite*:DetailedCuteBackend.Dependent*Partial*'`
  - Result: PASS.
  - Summary: `4 tests from DetailedCuteBackend passed`.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS.
  - Summary: `54 tests from 6 test suites passed`.
- `scons -Q build/RISCV/gem5.opt -j8`
  - Result: PASS.
  - Notes: only existing environment warnings for missing `png.h`, HDF5, and back trace implementation, plus existing generated Python invalid escape warnings.
- `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-cdc-tile configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
  - Result: PASS.
  - Summary: `All 8 precomp tests PASSED.`
- `util/style.py -m --checker SortedIncludes --checker Whitespace --checker LineLength src/matrix/backend_runtime.cc src/matrix/detailed_cute_backend.hh src/matrix/matrix_ex.cc src/matrix/matrix_regfile.hh src/matrix/matrix_regfile.cc src/matrix/mregfile.cc src/matrix/detailed_cute_backend.test.cc`
  - Result: PASS.
- `git diff --check -- src/matrix cute_detailed_design/implementation_notes.md cute_detailed_design/verify_report.md cute_detailed_design/handoff.md plan.md`
  - Result: PASS.
- `scons -Q build/RISCV/gem5.opt -j8`
  - Result: PASS.
  - Note: only existing environment warnings for PNG, HDF5, and back trace support.
- `build/RISCV/gem5.opt --debug-flags=MatrixCuteTrace --debug-file=matrix_cute_trace.log --outdir=/tmp/gem5-se-gemm-localmmu-trace-20260520-rerun configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
  - Result: PASS, `All 8 precomp tests PASSED.`
  - Trace file: `/tmp/gem5-se-gemm-localmmu-trace-20260520-rerun/matrix_cute_trace.log`.

### Evidence
- `CdcInt8TileWritebackShowsPartialAfterFirstKGroup`
  - full int8 all-ones A/B and zero C show internal C tile `(0,0) == 32` after 256 successful D beats.
  - `backend.hasCompletion()` remains false at that partial point.
- `CdcFullInt8WritebackUses512DBeatWindow`
  - `acceptedInputBeats == 512` and `cdcWriteCycles == 512`.
  - internal C tile `(0,0)` is partial `32` after first K group and final `64` after second K group.
  - terminal completion follows the CDC writeback boundary.
- `CdcWritebackStallsOnSameParityCWriteConflict`
  - same-parity C MatrixReg write conflict prevents D beat progress and leaves C tile unchanged for that stalled step.
- `DependentStoreCannotReadPartialCWhileMmaBusy`
  - store queued behind busy C cannot read partial C; memory remains unwritten until MMA completion.
- `DependentMmaCannotUsePartialCDestBeforeWriteFinish`
  - a dependent MMA that reads/writes the same C dest does not dequeue while the first MMA exposes only partial internal C.

### Coverage Boundary
- 覆盖：int8 CDC tile-level D beat writeback、CRegFile read-modify-write resource boundary、partial/final internal C visibility、same-parity C write conflict stall、scoreboard 阻止 partial C 被 store/MMA 外部消费、existing detailed backend regressions、SE `gemm_precomp` smoke。
- 不覆盖：MLOAD/MSTORE external `64B/cycle` bandwidth/fixed-latency window、cache/L2/MSHR/coherence/retry/replay、精确 FReducePE 数值流水。

## 2026-05-19 LocalMMU matrix load/store timing

### Verification Performed
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS.
  - Note: only existing environment warnings for PNG, HDF5, and back trace support.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='LocalMmuModel.*'`
  - Result: PASS, `2 tests`.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='LocalMmuModel.*:DetailedCuteBackend.*LocalMmu*:DetailedCuteBackend.ReadResponseRequiresTwoMatrixRegWriteSlots:DetailedCuteBackend.LoaderWritePriorityCanStallComputeRead:DetailedCuteBackend.ReleaseWaitsForLocalMmuStoreAck'`
  - Result: PASS, `8 tests`.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS, `65 tests from 7 test suites`.
- `scons -Q build/RISCV/gem5.opt -j8`
  - Result: PASS.
  - Note: only existing environment/generated warning noise.
- `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-localmmu configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
  - Result: PASS.
  - Summary: `All 8 precomp tests PASSED.`
- `util/style.py -m --checker SortedIncludes --checker Whitespace --checker LineLength src/matrix/local_mmu_model.hh src/matrix/local_mmu_model.cc src/matrix/SConscript src/matrix/detailed_cute_backend.hh src/matrix/taskcontrol.cc src/matrix/backend_runtime.cc src/matrix/memoryload.cc src/matrix/mregfile.cc src/matrix/detailed_cute_backend.test.cc`
  - Result: PASS.
- `git diff --check -- src/matrix cute_detailed_design/implementation_notes.md cute_detailed_design/verify_report.md cute_detailed_design/handoff.md plan.md`
  - Result: PASS.

### Evidence
- `LocalMmuModel.IssuesOnlyOneBeatPerCycleWithFixedLatency`
  - Two enqueued beats issue in separate cycles and return after the configured fixed latency.
- `LocalMmuModel.SourceIdsLimitOutstandingIssuedBeats`
  - With latency `100` and `maxOutstanding = 64`, only 64 of 65 pending beats issue; the 65th remains pending.
- `DetailedCuteBackend.AcceptsLocalMmuTimingConfig`
  - Backend preserves configurable LocalMMU latency and max outstanding while old constructor remains compatible.
- `DetailedCuteBackend.LoadEnqueuesOneLocalMmuBeatPer64Bytes`
  - A `128B` load enqueues two `64B` LocalMMU beats and does not complete or allocate the destination register on issue.
- `DetailedCuteBackend.LoadCompletionWaitsForLocalMmuResponses`
  - Load completion and register visibility wait until LocalMMU read responses arrive.
- `DetailedCuteBackend.LoadSnapshotsMemoryAfterResponsesBeforeRegWrite`
  - Functional memory snapshot occurs after LocalMMU responses and before MatrixReg write chunks finish.
- `DetailedCuteBackend.ReadResponseRequiresTwoMatrixRegWriteSlots`
  - A single `64B` read response requires two `32B` MatrixReg MemoryLoader write grants before register visibility.
- `DetailedCuteBackend.LoaderWritePriorityCanStallComputeRead`
  - Loader write chunks share `MatrixRegResource` with compute reads and can stall A/B compute read frontend.
- `DetailedCuteBackend.ReleaseWaitsForLocalMmuStoreAck`
  - Store memory write and release completion wait for LocalMMU store ack.

### Coverage Boundary
- 覆盖：LocalMMU fixed-latency single-issue model、64 source ID / outstanding limit、MLOAD/MSTORE `64B` beat enqueue、read response driven load completion、`64B -> 2 x 32B` MatrixReg loader writes、loader write vs compute read resource conflict、store ack / release gating、existing detailed backend regressions、SE `gemm_precomp` smoke。
- 不覆盖：真实 L2 / `RequestPort` / `Packet` / retry / `recvTimingResp()`、cache tag / hit-miss / MSHR / coherence / replacement、HBL2 matrix metadata、CDC tile-level functional writeback 新增行为、RTL timing equivalence。

## 2026-05-19 LocalMMU trace counters

### Verification Performed
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS.
  - Note: only existing environment warnings for PNG, HDF5, and back trace support.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.LocalMmuTraceCountersTrack*:LocalMmuModel.*:DetailedCuteBackend.*LocalMmu*:DetailedCuteBackend.ReadResponseRequiresTwoMatrixRegWriteSlots:DetailedCuteBackend.LoaderWritePriorityCanStallComputeRead'`
  - Result: PASS, `10 tests`.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS, `67 tests from 7 test suites`.
- `util/style.py -m --checker SortedIncludes --checker Whitespace --checker LineLength src/matrix/detailed_cute_backend.hh src/matrix/backend_runtime.cc src/matrix/memoryload.cc src/matrix/detailed_cute_backend.test.cc`
  - Result: PASS.
- `git diff --check -- src/matrix cute_detailed_design/implementation_notes.md cute_detailed_design/verify_report.md cute_detailed_design/handoff.md plan.md`
  - Result: PASS.

### Evidence
- `DetailedCuteBackend.LocalMmuTraceCountersTrackLoadFill`
  - A one-beat `64B` load increments load enqueue, issue, read response, and two MatrixReg loader write grant counters.
- `DetailedCuteBackend.LocalMmuTraceCountersTrackStoreAck`
  - A one-beat store increments store enqueue, issue, and store ack counters.
- SE `gemm_precomp` trace:
  - `local_mmu_enqueue`: `18432` events (`10240` load beats, `8192` store beats).
  - `local_mmu_issue`: `18432` events.
  - `local_mmu_response`: `18432` events (`10240` read responses, `8192` store acks).
  - `matrix_reg_loader_write_grant`: `20480` events, matching `2 * 10240` read responses.
  - `matrix_reg_loader_write_stall`: `0` events in this workload.

### Coverage Boundary
- 覆盖：LocalMMU beat enqueue/issue/read response/store ack counters、MatrixReg loader write chunk queued/granted/stalled counters、focused gtest 与完整 detailed backend regression。
- 不覆盖：真实 L2/cache timing、RTL fill FIFO depth/backpressure、RTL-equivalent latency calibration。

## 2026-05-20 LocalMMU requester/source arbitration

### Verification Performed
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS.
  - Note: only existing environment warnings for PNG, HDF5, and back trace support.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='LocalMmuModel.*'`
  - Result: PASS, `4 tests`.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='LocalMmuModel.*:DetailedCuteBackend.*LocalMmu*:DetailedCuteBackend.ReadResponseRequiresTwoMatrixRegWriteSlots:DetailedCuteBackend.LoaderWritePriorityCanStallComputeRead:DetailedCuteBackend.ReleaseWaitsForLocalMmuStoreAck'`
  - Result: PASS, `12 tests`.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS, `69 tests from 7 test suites`.
- `util/style.py -m --checker SortedIncludes --checker Whitespace --checker LineLength src/matrix/local_mmu_model.hh src/matrix/local_mmu_model.cc src/matrix/detailed_cute_backend.test.cc`
  - Result: PASS.
- `git diff --check -- src/matrix cute_detailed_design/implementation_notes.md cute_detailed_design/verify_report.md cute_detailed_design/handoff.md plan.md`
  - Result: PASS.
- `scons -Q build/RISCV/gem5.opt -j8`
  - Result: PASS.
  - Note: only existing environment warnings for PNG, HDF5, and back trace support.
- `build/RISCV/gem5.opt --debug-flags=MatrixCuteTrace --debug-file=matrix_cute_trace.log --outdir=/tmp/gem5-se-gemm-localmmu-rr-20260520 configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
  - Result: PASS, `All 8 precomp tests PASSED.`
  - Trace file: `/tmp/gem5-se-gemm-localmmu-rr-20260520/matrix_cute_trace.log`.

### Evidence
- Red test observed before implementation:
  - `LocalMmuModel.IssuesOnlyOneBeatPerCycleWithFixedLatency` expected high source IDs but old model returned `0/1`.
  - `LocalMmuModel.RotatesRequesterPriorityAcrossClients` expected `AML/BML/CML`, but old model returned CML FIFO requests first.
  - `LocalMmuModel.AllocatesHighestAvailableSourceIdLikeCUTE2TL` expected `3/2/1`, but old model returned `0/1/2`.
- Green tests after implementation:
  - `LocalMmuModel.RotatesRequesterPriorityAcrossClients` proves a large CML pending queue no longer starves concurrently valid AML/BML requests.
  - `LocalMmuModel.AllocatesHighestAvailableSourceIdLikeCUTE2TL` proves source ID selection uses highest available ID.
- SE `gemm_precomp` trace:
  - `local_mmu_enqueue`: `18432` events (`10240` load beats, `8192` store beats).
  - `local_mmu_issue`: `18432` events.
  - `local_mmu_response`: `18432` events.
  - First responses show high source ID reuse and A/B insertion into a large CML load: CML `source=63/62`, then AML `source=62`, BML `source=63`.
  - `matrix_reg_loader_write_grant`: `20480` events, matching `2 * 10240` read responses.
  - `matrix_reg_loader_write_stall`: `0` events in this workload.

### Coverage Boundary
- 覆盖：LocalMMU `AML/BML/CML` rotating requester priority、highest-free source ID selection、existing LocalMMU/backend regressions。
- 不覆盖：真实 L2/TL packet behavior、TL response read-data-vs-ack priority、cache/MSHR/coherence、RTL-equivalent latency calibration。

## 2026-05-21 CML LocalMMU fill chunk entry/parity

### Verification Performed
- `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8`
  - Result: PASS.
  - Note: only existing environment warnings for PNG, HDF5, and back trace support.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.CmlFillSecondChunkUsesNextMatrixRegEntryParity'`
  - Red before fix: FAIL, second CML fill chunk still used the first chunk parity and stalled a non-conflicting C read.
  - Green after fix: PASS, `1 test`.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='MatrixRegResource.*:LocalMmuModel.*:DetailedCuteBackend.*LocalMmu*:DetailedCuteBackend.ReadResponseRequiresTwoMatrixRegWriteSlots:DetailedCuteBackend.LoaderWritePriorityCanStallComputeRead:DetailedCuteBackend.CmlFillSecondChunkUsesNextMatrixRegEntryParity:DetailedCuteBackend.ReleaseWaitsForLocalMmuStoreAck'`
  - Result: PASS, `20 tests`.
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - Result: PASS, `70 tests from 7 test suites`.
- `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-cmlfill-20260521 configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`
  - Result: PASS, `All 8 precomp tests PASSED.`
  - Cases: `zeros`, `ones`, `max`, `min`, `rand0`, `rand1`, `rand2`, `rand3`.
  - Exit tick: `413030889`.

### Evidence
- `DetailedCuteBackend.CmlFillSecondChunkUsesNextMatrixRegEntryParity`
  - A CML `64B` read response first drains one `32B` fill chunk to odd C entry.
  - The second chunk now uses the next C entry/parity, so a same-cycle MMA read from a non-conflicting odd C entry can issue.
- Existing `MatrixRegResource.CReadWriteSameParityConflictStalls` and `MatrixRegResource.CReadWriteOppositeParityCanShareCycle` continue to pass.
- SE `gemm_precomp` after the CML entry/parity fix still passes all 8 integer precomp smoke cases.

### Coverage Boundary
- 覆盖：CML LocalMMU response 的 `2 x 32B` MatrixReg loader-fill chunk entry/parity metadata、与现有 C odd/even conflict 模型的交互。
- 不覆盖：bounded `MReg_Fill_Table` / fill FIFO depth、`response.ready = false` backpressure、source ID 被 fill backpressure 持有、真实 TL/L2 response timing。

## 2026-05-21 LocalMMU closeout consistency review

### Review Target
- Phase/Task:
  - LocalMMU / matrix load-store timing first-stage closeout
- Review type:
  - phase-level consistency review
- Claimed transition:
  - `WIP -> WIP`

### Required Gates

| Gate | Result | Evidence | Issue if FAIL |
|---|---|---|---|
| Scope matches current Phase / Task | PASS | `plan.md` 当前范围只覆盖 LocalMMU/load-store timing；本轮新增证据只记录 CML fill parity 与 SE smoke。 | - |
| DoD is observable and reproducible | PASS | 本节记录了 focused gtest、full detailed backend test 与 SE `gemm_precomp` 命令。 | - |
| Depends / Status / DoD are self-consistent | PASS | 当前仍保持 `WIP -> WIP`，没有把 fixed-latency model 强化成真实 L2/TL。 | - |
| No completed task depends on TODO without explanation | PASS | bounded fill table、response backpressure、TL/LLC ready 和 real L2 均保留为未完成边界。 | - |
| Claimed alignment does not exceed evidence | PASS | `matrix_diff_vs_rtl.md` 明确记录 LocalMMU response always ready / infinite fill-buffer 近似。 | - |
| Functional correctness is separated from resource/timing alignment | PASS | SE smoke 只作为功能闭环证据；MatrixReg parity gtest 作为资源冲突 metadata 证据。 | - |
| Missing evidence is marked as `证据不足` | PASS | 未宣称 RTL-equivalent latency、TL response priority、cache/MSHR/coherence 或 fill-table backpressure。 | - |
| `verify_report` contains required evidence | PASS | 本节包含 CML parity focused test、20-test focused suite、70-test full suite、SE smoke。 | - |
| `handoff` reflects current stop point | PASS | `handoff.md` 记录 CML entry/parity stop point 和仍未实现的 fill/backpressure/L2 边界。 | - |
| `matrix_diff_vs_rtl.md` is consistent with `Plan.md` | PASS | 两者都把 real L2/TL/cache 与 bounded fill backpressure 留在后续阶段。 | - |
| Review reports RTL alignment, design complexity, and code style as separate dimensions | PASS | 见下方 Review Dimensions。 | - |

### Review Dimensions

| Dimension | Result | Evidence | Issue if FAIL / N/A reason |
|---|---|---|---|
| RTL Alignment | PASS | Requester rotation/source ID/CML fill parity 更接近 RTL/CUTE 事实；bounded fill table、response ready/backpressure、TL/LLC ready 未对齐且已记录。 | - |
| Design Complexity | PASS | CML parity 修复只增加 pending chunk entry metadata，未引入真实 L2 或新 cache 抽象。 | - |
| Code Style | PASS | Focused style check 已通过 touched `src/matrix` 文件；实现仍沿用 `DetailedCuteBackend` 与 `MatrixRegResource` 现有路径。 | - |

### Evidence
- `DetailedCuteBackend.CmlFillSecondChunkUsesNextMatrixRegEntryParity`
- `MatrixRegResource.CReadWriteSameParityConflictStalls`
- `MatrixRegResource.CReadWriteOppositeParityCanShareCycle`
- `./build/RISCV/matrix/detailed_cute_backend.test.opt`: `70 tests from 7 test suites`
- SE `gemm_precomp`: `All 8 precomp tests PASSED.`
- `cute_detailed_design/matrix_diff_vs_rtl.md`: LocalMMU response backpressure delta

### Issues
- None for this closeout boundary.

### Blocking Issues
- None.

### Required Document Updates
- `cute_detailed_design/verify_report.md`
- `cute_detailed_design/handoff.md`

### Recommended Action
- 当前 LocalMMU first-stage closeout 可作为进入真实 L2/cache RequestPort 计划的前置证据，但不能作为 RTL-equivalent memory timing 结论。

### Verdict

`APPROVE_WITH_NOTES`

## 2026-05-12 phase review

### Review Target
- Phase/Task:
  - Phase 3 checkpoint after SE `gemm_precomp` runtime unblock
- Review type:
  - phase-level formal review
- Claimed transition:
  - `WIP -> WIP`

### Required Gates

| Gate | Result | Evidence | Issue if FAIL |
|---|---|---|---|
| Scope matches current Phase / Task | PASS | 本轮只修复 Phase 3 detailed backend runtime 越界。 | - |
| DoD is observable and reproducible | PASS | SE runtime 命令可复现 8 个 case 全通过。 | - |
| Depends / Status / DoD are self-consistent | PASS | `Plan.md` 仍把相关 Phase 3 条目标为 `WIP`，没有被这次 bugfix 误强化。 | - |
| No completed task depends on TODO without explanation | PASS | 本轮没有新增“完成”状态。 | - |
| Claimed alignment does not exceed evidence | PASS | 只确认 SE functional pass，不宣称 Phase 3 收口或 RTL 对齐。 | - |
| Functional correctness is separated from resource/timing alignment | PASS | 文档明确区分功能恢复与未完成的 timing/resource 对齐。 | - |
| Missing evidence is marked as `证据不足` | PASS | fp/datatype、完整 unit suite、资源/时序仍保留限制说明。 | - |
| `verify_report` contains required evidence | PASS | 本节已补 build、单测、SE runtime 和 review 结论。 | - |
| `handoff` reflects current stop point | PASS | 本轮会同步 handoff 当前停点。 | - |
| `matrix_diff_vs_rtl.md` is consistent with `Plan.md` | PASS | 本轮没有改差异口径，也没有把“部分对齐”写成“已对齐”。 | - |
| Review reports RTL alignment, design complexity, and code style as separate dimensions | PASS | 见下方 Review Dimensions。 | - |

### Review Dimensions

| Dimension | Result | Evidence | Issue if FAIL / N/A reason |
|---|---|---|---|
| RTL Alignment | PASS | 当前只证明 reg-index runtime bug 已修和 SE integer gemm 跑通；`matrix_diff_vs_rtl.md` 中的未对齐项没有被越权收口。 | - |
| Design Complexity | PASS | 修复仍是“小步、可验证、可回退”的局部调整。 | - |
| Code Style | PASS | 任务级 style review 已单独给出 `APPROVE`，且没有新增复杂包装层。 | - |

### Evidence
- [src/matrix/matrix_regfile.hh](/nfs/home/hujun/GEM5/src/matrix/matrix_regfile.hh:57)
- [src/matrix/detailed_cute_backend.test.cc](/nfs/home/hujun/GEM5/src/matrix/detailed_cute_backend.test.cc:278)
- [implementation_notes.md](/nfs/home/hujun/GEM5/cute_detailed_design/implementation_notes.md:772)
- 本节记录的 build / unit / SE runtime 命令

### Issues
- 当前 `detailed_cute_backend.test.opt` 仍有 1 条与 fp encoding 相关的非本轮失败，因此不适合据此强化更大的 Phase 3 状态。

### Blocking Issues
- None

### Required Document Updates
- `cute_detailed_design/handoff.md`

### Recommended Action
- 不更新 `Plan.md` 状态；把当前 stop point 记成“SE gemm 已跑通，继续留在 Phase 3 WIP，后续再单独处理 fp/unit-suite 问题”。

### Verdict

`APPROVE_WITH_NOTES`

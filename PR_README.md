# Matrix ISA / O3 / Detailed CUTE Backend Mainline

## PR 摘要

本 PR 是 GEM5 matrix 主线的一次阶段性集成，覆盖从 RISC-V matrix ISA、O3 commit 后可见性边界，到 `src/matrix` detailed CUTE backend 的主要路径。

当前分支相对 `matrix-o3-squash` 的主要增量包括：

- RISC-V matrix decode / tile CSR carrier / load-store 字段解释修正。
- O3 `DynInst / ROB / Commit / CPU` 中 matrix payload、AMU carrier、commit 后 backend-visible 边界。
- `MatrixBackend` 抽象与 `DetailedCuteBackend` 主链。
- `DecodedFifo + Scoreboard + TaskController-style issue + AML/BML/CML/ADC/BDC/CDC/MTE` 的 event-driven backend。
- datatype tag 透传与 FP16 backend/unit compute checkpoint。
- MatrixReg resource 仲裁模型。
- MTE timing / CDC D beat / LocalMMU load-store timing 的第一阶段建模。
- SE `gemm_precomp` 主线 smoke 回归与详细 gtest 回归。
- `cute_detailed_design/`、`reviews/`、planning 文档和验证报告整理。

当前结论应表述为：

- 已具备 matrix ISA/O3/backend 的可运行主线闭环。
- `gemm_precomp` 在 SE 下通过，能作为当前 `0x2b` 主口径 workload 的 functional smoke 证据。
- detailed backend 已具备一批资源/时序边界模型，但仍是阶段性 detailed 近似。
- 不能描述为 RTL/CUTE cycle-equivalent 或完整 memory hierarchy 对齐。

## 代码范围

### ISA / Decode / Architectural State

涉及路径：

- `src/arch/riscv/isa/decoder.isa`
- `src/arch/riscv/isa/formats/matrix_conf.isa`
- `src/arch/riscv/isa.hh`
- `src/arch/riscv/regs/matrix.hh`
- `src/arch/riscv/regs/renameable_misc.hh`
- `src/arch/riscv/matrix_sync_check.cc`

主要变化：

- 接入当前 matrix 指令子集的 decode 和最小参数检查。
- 保留 renamed carrier：`mtilem / mtilen / mtilek`。
- 不再把 `xmxrm / xmfrm / xmsaten / XMCSR composite` 作为 renamed carrier。
- `XMCSR / XMXRM / XMFRM / XMSATEN` 回到普通 CSR sideband 读取路径。
- 修正 matrix load/store 字段解释：
  - `funct7` 区分 A/B/C load-store 类。
  - `RD[4:3]` 解释为 width。
  - `RD[2:0]` 解释为 matrix register index。
- 修复 `0x24a48a2b` 实际应按 `mlce32 acc0,(s1),a0` 解释，而不是旧口径的 `mlae16`。
- MMA payload 保留 `payload.op`，避免 FP MMA opcode metadata 被默认 `0x0c` 覆盖。

当前边界：

- decode 覆盖当前 workload 与验证所需子集，不代表完整 IME/matrix ISA 全覆盖。
- `matrix_sync_check.opt` standalone link 仍有既有链接范围问题，因此主要作为源码覆盖参考，不作为最终绿灯。

### O3 Matrix Path

涉及路径：

- `src/cpu/exec_context.hh`
- `src/cpu/o3/dyn_inst.hh`
- `src/cpu/o3/dyn_inst.cc`
- `src/cpu/o3/iew.cc`
- `src/cpu/o3/lsq.cc`
- `src/cpu/o3/lsq.hh`
- `src/cpu/o3/lsq_unit.cc`
- `src/cpu/o3/lsq_unit.hh`
- `src/cpu/o3/mls_unit.cc`
- `src/cpu/o3/mls_unit.hh`
- `src/cpu/o3/mls_replay_queue.cc`
- `src/cpu/o3/mls_replay_queue.hh`
- `src/cpu/o3/matrix_payload.cc`
- `src/cpu/o3/matrix_payload.hh`
- `src/cpu/o3/matrix_amu_buffer.cc`
- `src/cpu/o3/matrix_amu_buffer.hh`
- `src/cpu/o3/rob.cc`
- `src/cpu/o3/rob.hh`
- `src/cpu/o3/commit.cc`
- `src/cpu/o3/cpu.cc`
- `src/cpu/o3/cpu.hh`

主要变化：

- 新增/整理 `MatrixExecPayload` 与 O3 matrix payload helper。
- `DynInst` 记录 matrix 指令类别、payload、state read/write 摘要。
- matrix mem 指令经 `MlsUnit` 形成最终 LSU payload。
- `MlsUnit` 负责基址、stride、tile 维度、地址翻译、payload finalize。
- `ROB::MatrixAmuEntry` 记录 commit 后才允许提交给 backend 的 matrix request。
- `Commit / CPU` 在 backend 可接受时才消费 ready matrix AMU entry。
- wrong-path、squash、faulted request 不应 backend-visible。
- `msyncregreset / mrelease` 的 token reset ordering 已保留，晚到旧 release completion 会被过滤。

当前边界：

- `MatrixAmuEntry / MatrixAmuBuffer` 是 `AMUCtrlBuffer` 的阶段性近似，不是固定槽位 RTL 结构等价。
- 当前结论主要覆盖 single-thread；不支撑 SMT/global-oldest/CommitWidth timing 结论。
- MLS replay/cancel/fault cause 仍偏粗，主要是 `tlbMiss` 粒度，不等价于 RTL `cause/carry/safe-writeback`。

### Detailed Matrix Backend

涉及路径：

- `src/matrix/matrix_backend.hh`
- `src/matrix/detailed_cute_backend.hh`
- `src/matrix/decoded_fifo.hh`
- `src/matrix/detailed_cute_scoreboard.hh`
- `src/matrix/detailed_cute_scoreboard.cc`
- `src/matrix/taskcontrol.cc`
- `src/matrix/backend_runtime.cc`
- `src/matrix/mregfile.cc`
- `src/matrix/memoryload.cc`
- `src/matrix/matrix_ex.cc`
- `src/matrix/matrix_regfile.hh`
- `src/matrix/matrix_regfile.cc`
- `src/matrix/matrix_types.hh`
- `src/matrix/matrix_memory_adapter.hh`
- `src/matrix/matrix_memory_adapter_gem5.cc`
- `src/matrix/SConscript`

主要变化：

- 从原 functional backend 收敛为 active build 的 `DetailedCuteBackend`。
- 主链为：
  - `commit-visible request`
  - `DecodedFifo`
  - `Scoreboard`
  - `TaskController-style issue`
  - micro task
  - completion
- request 类型分为：
  - `Lsu`
  - `Mma`
  - `Arith`
  - `Release`
- load/store/release/compute 的退场点统一到 task event / terminal completion 边界。
- scoreboard 分阶段释放 source/dest 依赖：
  - load finish
  - compute read A/B finish
  - compute write C finish
  - store read/write finish
- release gating 等待 pending store 和 backend drain。

当前边界：

- backend 是 event-driven detailed 近似，不是 RTL ready/valid 全形状。
- completion 由 CPU tick/service 路径轮询，不是 RTL 原始完成通路。

### Datatype / FP Path

涉及路径：

- `src/cpu/exec_context.hh`
- `src/cpu/o3/rob.cc`
- `src/cpu/o3/mls_unit.cc`
- `src/matrix/matrix_types.hh`
- `src/matrix/matrix_memory_adapter_gem5.cc`
- `src/matrix/matrix_ex.cc`
- `src/matrix/detailed_cute_backend_fp_end_to_end.test.cc`

主要变化：

- `MatrixElemType` 扩展到：
  - `Int8 / Int16 / Int32 / Int64`
  - `Fp16 / Bf16 / Tf32`
- LSU / MMA request 显式携带 datatype tag。
- FP16/BF16/TF32 LSU 按 raw bits 读写。
- FP16 MMA 在 detailed backend/unit 路径形成最小 correctness checkpoint。
- FP16 backend compute 使用 CUTE `cute-fpe` 软件 oracle 固定结果。

当前边界：

- FP16 目前是 backend/unit checkpoint，不是 active ISA/decode FP workload 全链路闭环。
- BF16/TF32 compute 仍未闭环。
- 不能写成 matrix datatype 已完整对齐 RTL。

### Compute Timing / MatrixReg Resource

涉及路径：

- `src/matrix/matrix_ex.cc`
- `src/matrix/backend_runtime.cc`
- `src/matrix/matrix_reg_resource.hh`
- `src/matrix/matrix_reg_resource.cc`
- `src/matrix/detailed_cute_backend_mte.test.cc`
- `src/matrix/matrix_reg_resource.test.cc`

主要变化：

- `ADC/BDC/CDC/MTE` 拆成显式子单元状态。
- compute issue gate 已收紧为 `ADC && BDC && CDC` all-ready。
- MTE timing 按当前 active RTL controller shape 建模。
- full int8 MTE accepted beats 修正为 `512`。
- CDC D beat timing window 建模为 `512`。
- 新增 `MatrixRegResource`：
  - `8` banks。
  - `32B` entry。
  - full-bank grant。
  - `1` cycle read response。
  - A/B loader write priority。
  - C odd/even read/write conflict。
- compute read frontend 已接入 `MatrixRegResource`。

当前边界：

- `ComputeGo / MicroTaskEndValid / MicroTaskEndReady` 仍是软件事件近似。
- `FReducePE` 内部行为仍由功能级摘要表示。
- CDC reorder / after-op / 完整 TE 内部流水仍未建模。

### LocalMMU / Matrix Load-Store Timing

涉及路径：

- `src/matrix/local_mmu_model.hh`
- `src/matrix/local_mmu_model.cc`
- `src/matrix/memoryload.cc`
- `src/matrix/backend_runtime.cc`
- `src/matrix/detailed_cute_backend.hh`
- `src/matrix/detailed_cute_backend.test.cc`

主要变化：

- 新增 `LocalMmuModel`。
- MLOAD/MSTORE 按 `64B` LocalMMU beat 入队。
- 每个 backend cycle 最多 issue 一个 `64B` beat。
- `64` source IDs / max outstanding。
- fixed configurable latency。
- AML/BML/CML pending requests 拆成三路队列。
- requester rotation 避免 CML 大队列饿死 AML/BML。
- source ID 选择与当前 CUTE2TL-style highest-free 行为对齐。
- load completion 等待全部 LocalMMU read responses。
- store completion / release gating 等待全部 store acks。
- 每个 `64B` read response 拆成 `2 x 32B` MatrixReg MemoryLoader fill chunks。
- CML fill chunk 记录实际 MatrixReg entry/parity，避免第二个 chunk 复用错误 C parity。
- 新增 LocalMMU / MatrixReg loader-fill trace counters。

当前边界：

- LocalMMU 是 fixed-latency、response-always-ready 近似。
- 当前等价于 infinite fill buffer。
- 不建模 bounded `MReg_Fill_Table` / fill FIFO depth。
- 不建模 response ready/backpressure。
- 不建模 TL/LLC ready、真实 L2/cache/MSHR/coherence/retry。

## 文档与 Review

新增/整理文档：

- `cute_detailed_design/Plans.md`
- `cute_detailed_design/matrix_core_design.md`
- `cute_detailed_design/matrix_diff_vs_rtl.md`
- `cute_detailed_design/matrix_o3_README.md`
- `cute_detailed_design/implementation_notes.md`
- `cute_detailed_design/verify_report.md`
- `cute_detailed_design/handoff.md`
- `task_plan.md`
- `findings.md`
- `progress.md`
- `plan.md`
- `reviews/matrix_review_standard.md`
- `reviews/matrix_review_prompt_template.md`
- `reviews/matrix_task_style_review_prompt_template.md`

文档口径：

- `cute_detailed_design/Plans.md`：阶段计划真源。
- `cute_detailed_design/matrix_core_design.md`：核内路径与 backend glue 契约。
- `cute_detailed_design/matrix_diff_vs_rtl.md`：当前未对齐项真源。
- `cute_detailed_design/implementation_notes.md`：实现摘要。
- `cute_detailed_design/verify_report.md`：验证证据。
- `cute_detailed_design/handoff.md`：当前停点和下一步。

## 已验证内容

### 构建

```bash
scons -Q build/RISCV/gem5.opt -j8
```

结果：PASS。

历史记录中也通过过：

```bash
scons build/RISCV/gem5.opt --linker=gold -j64 --rvv-impl=simple
```

结果：PASS。

说明：

- 只观察到既有环境 warning：
  - 缺少 `png.h`
  - 缺少 HDF5
  - 缺少 backtrace support

### Detailed Backend Unit Tests

```bash
scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8
```

结果：PASS。

```bash
./build/RISCV/matrix/detailed_cute_backend.test.opt
```

最新记录：PASS，`70 tests from 7 test suites`。

覆盖方向包括：

- MatrixRegFile state。
- MatrixRegResource arbitration。
- DetailedCuteScoreboard lifecycle。
- load/store/release lifecycle。
- compute frontend / MTE timing。
- FP16 backend path。
- CDC D beat writeback。
- LocalMMU load-store timing。

### LocalMMU Focused Tests

```bash
./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='LocalMmuModel.*'
```

结果：PASS，`4 tests`。

```bash
./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='MatrixRegResource.*:LocalMmuModel.*:DetailedCuteBackend.*LocalMmu*:DetailedCuteBackend.ReadResponseRequiresTwoMatrixRegWriteSlots:DetailedCuteBackend.LoaderWritePriorityCanStallComputeRead:DetailedCuteBackend.CmlFillSecondChunkUsesNextMatrixRegEntryParity:DetailedCuteBackend.ReleaseWaitsForLocalMmuStoreAck'
```

结果：PASS，`20 tests`。

关键断言：

- `LocalMmuModel.IssuesOnlyOneBeatPerCycleWithFixedLatency`
- `LocalMmuModel.SourceIdsLimitOutstandingIssuedBeats`
- `LocalMmuModel.RotatesRequesterPriorityAcrossClients`
- `LocalMmuModel.AllocatesHighestAvailableSourceIdLikeCUTE2TL`
- `DetailedCuteBackend.LoadEnqueuesOneLocalMmuBeatPer64Bytes`
- `DetailedCuteBackend.LoadCompletionWaitsForLocalMmuResponses`
- `DetailedCuteBackend.ReadResponseRequiresTwoMatrixRegWriteSlots`
- `DetailedCuteBackend.LoaderWritePriorityCanStallComputeRead`
- `DetailedCuteBackend.CmlFillSecondChunkUsesNextMatrixRegEntryParity`
- `DetailedCuteBackend.ReleaseWaitsForLocalMmuStoreAck`

### Compute / MatrixReg / FP Focused Tests

已记录通过的代表性命令：

```bash
./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.MteTimingReportsPerCycleBandwidthAndAcceptedBeats:DetailedCuteBackend.MteTimingFollowsActiveRtlControllerShape:DetailedCuteBackend.MteTotalCompletionCyclesMatchesComputeTerminalBoundary'
```

```bash
./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='MatrixRegResource.*'
```

```bash
./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.UnsupportedFpMmaReturnsUnsupportedCompletion:DetailedCuteBackend.Fp16MmaSucceedsWithInt32Accumulator:DetailedCuteBackend.Fp16LsuKeepsElemTypeAndRawBits'
```

结果：PASS。

### O3 Payload Tests

```bash
scons -Q build/RISCV/cpu/o3/matrix_o3_payload.test.opt --unit-test -j8
```

```bash
./build/RISCV/cpu/o3/matrix_o3_payload.test.opt
```

结果：PASS。

覆盖：

- O3 matrix payload metadata。
- MMA opcode metadata 透传。

### SE Workload

#### `gemm_precomp`

```bash
./build/RISCV/gem5.opt \
  --outdir=/tmp/gem5-se-gemm-cmlfill-20260521 \
  configs/example/se.py \
  -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp \
  --enable-riscv-vector \
  --no-pf
```

结果：PASS。

摘要：

- `All 8 precomp tests PASSED.`
- Cases:
  - `zeros`
  - `ones`
  - `max`
  - `min`
  - `rand0`
  - `rand1`
  - `rand2`
  - `rand3`
- Exit tick: `413030889`。

#### `hello_xsai`

历史记录命令：

```bash
build/RISCV/gem5.opt --outdir=/tmp/gem5-se-hello-xsai-rerun \
  configs/example/se.py \
  -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/hello_xsai/build/hello_xsai \
  --enable-riscv-vector --no-pf
```

结果摘要：

- `Hello, XiangShan AI!`
- `hello_xsai is running from XSAI init flow.`
- `mem_test`: `16 passed, 0 failed`

### MatrixCuteTrace 观察

SE `gemm_precomp` 配合 `MatrixCuteTrace` 已记录：

- `local_mmu_enqueue`: `18432` events。
- `local_mmu_issue`: `18432` events。
- `local_mmu_response`: `18432` events。
- Load beats: `10240`。
- Store beats / acks: `8192`。
- MatrixReg loader write grants: `20480`，匹配 `2 * 10240` read responses。
- 当前 workload 中 `matrix_reg_loader_write_stall`: `0` events。

### Style / Diff Checks

已记录通过：

```bash
util/style.py -m --checker SortedIncludes --checker Whitespace --checker LineLength ...
```

已记录通过：

```bash
git diff --check -- src/matrix cute_detailed_design/implementation_notes.md cute_detailed_design/verify_report.md cute_detailed_design/handoff.md plan.md
```

以及若干针对 `src/arch/riscv`、`src/cpu/o3`、`src/matrix` touched files 的 `git diff --check`。

## 当前未对齐项

以下内容是已知限制，不应在 PR 描述或 review 回复中写成“已对齐 RTL”。

### ISA / Decode

- 指令覆盖范围仍是当前 workload 和测试需要的子集。
- decode 参数检查是最小功能语义，不是完整 RTL frontend gating。
- active FP ISA workload 仍未完整闭环。

### Rename / Matrix State

- 当前复用通用 RMiscReg rename。
- 没有独立 matrix rename resource / rename stall / rename credit 模型。
- 只有 `mtilem/mtilen/mtilek` 保持 renamed carrier；其它 matrix CSR 走 sideband。

### O3 / AMU Carrier

- `MatrixAmuBuffer` 不是 RTL `AMUCtrlBuffer` 的固定槽位实现。
- 不支持 SMT/global-oldest/CommitWidth timing 结论。
- backend completion 是 CPU tick/service 路径轮询，不是 RTL 原始完成路径。

### MLS Replay / Fault / Cancel

- GEM5 当前 replay/cancel cause 粒度仍比 RTL 粗。
- 主要覆盖 single-thread 证据。
- 未完整对齐 RTL `MlsUnit` 的 `cause/carry/safe-writeback`。

### Detailed Backend

- `TaskController` 已有 `head -> scoreboard -> ready -> issue` 主链，但仍不是 RTL ready/valid 完整形态。
- `ComputeGo / MicroTaskEndValid / MicroTaskEndReady` 仍是事件式近似。
- CDC reorder / after-op / TE 内部流水仍未建模。
- `FReducePE` 内部行为仍是功能级摘要。

### MatrixReg / Fill Path

- 已有 `MatrixRegResource` 的 bank/resource conflict 模型。
- 但 `MatrixRegFile` 仍主要负责 backend-owned functional state。
- 还没有完整 AML/BML/CML/ADC/BDC/CDC 端口级读写仲裁和真实带宽模型。
- 没有 bounded `MReg_Fill_Table` / fill FIFO depth。

### Memory / LocalMMU / L2

- LocalMMU 当前是 fixed-latency、response-always-ready 近似。
- 当前等价于 infinite fill buffer。
- source ID 在 fixed latency response 被服务时释放。
- 未建模 response backpressure 导致 source ID 继续 busy。
- 未接真实 L2 / TL / `RequestPort` / `Packet` / retry / `recvTimingResp()`。
- 未建模 cache tag / hit-miss / MSHR / coherence / replacement。
- 未做 RTL-equivalent latency calibration。

### Datatype / FP

- FP16 backend/unit checkpoint 已成立。
- BF16/TF32 compute 仍未闭环。
- active ISA/decode FP workload 仍未形成主线证据。
- 不能写成 datatype 全覆盖或 FP path 完全对齐。

## Review 建议口径

推荐描述：

- “当前 GEM5 matrix 已具备 ISA/O3/backend 的可运行主线闭环。”
- “commit 前不可 backend-visible、commit 后进入 backend 的边界已建立。”
- “`gemm_precomp` 在 SE 下通过，可作为当前 integer mainline functional smoke 证据。”
- “Detailed backend 已引入 event-driven CUTE 近似、MatrixReg resource、MTE timing、CDC D beat、LocalMMU fixed-latency load/store timing。”
- “当前仍不是 RTL/CUTE cycle-equivalent；memory hierarchy、fill backpressure、TL/L2/cache、完整 ready/valid 与 SMT/CommitWidth carrier 仍需后续 PR。”

不建议描述：

- “GEM5 CUTE 已与 RTL 对齐。”
- “LocalMMU timing 已与 RTL 等价。”
- “通过 `gemm_precomp` 证明资源/时序正确。”
- “MatrixAmuBuffer 等价于 AMUCtrlBuffer。”
- “datatype / FP path 已完整支持。”

## 后续工作

- 接入真实 L2/TL memory path：
  - `RequestPort`
  - `Packet`
  - retry/backpressure
  - `recvTimingResp()`
- 加 bounded `MReg_Fill_Table` / fill FIFO。
- response backpressure 时保持 source ID busy。
- 建模 TL read-data/store-ack response arbitration。
- 建模 cache hit/miss、MSHR、coherence、replacement。
- 继续细化 `ComputeGo / EndReady` 和 CDC reorder/after-op。
- 扩展 BF16/TF32 compute 与 active FP workload。
- 细化 MLS replay/cancel/fault cause。
- 若要做 timing claim，需要 RTL trace / oracle 做 latency 与 response ordering 校准。


# Matrix RTL/CUTE 差异文档

创建日期: 2026-04-23
更新日期: 2026-05-21

## 文档职责

- 这份文件只负责整理 **当前 GEM5 matrix 路径与 RTL/CUTE 的未对齐项**。
- 它回答的问题是：
  - 哪些边界已经对齐
  - 哪些点只是阶段性简化
  - 哪些点仍明确未对齐
  - 哪些点目前证据不足，不能下强结论
- 它不负责：
  - 替代计划文档
  - 替代 core 内设计文档
  - 替代 backend 接入设计文档
  - 记录某一轮具体 patch 或运行日志

## 1. 当前已对齐或基本对齐的边界

以下边界当前可以写成“已对齐到当前阶段目标”，但不能外推成 RTL/CUTE 等价：

| 边界 | 当前 GEM5 状态 | 说明 |
|------|----------------|------|
| commit 前不可 backend-visible | 已对齐 | `MatrixAmuEntry` 只在 commit 后通过 `consumeMatrixAmuProxy()` 进入 backend |
| wrong-path / faulted request 不进入 backend | 基本对齐 | `noteMatrixAmuWriteback()` 在 fault/payload invalid 时会抑制 `needAMU` |
| request 大类划分 | 基本对齐 | GEM5 与 CUTE 都以 `Lsu / Mma / Arith / Release` 为 backend request 大类 |
| `msettile* / mtile* / xmcsr` 等 arch-side 语义 | 已对齐到当前阶段 | 这部分主要属于 `src/arch/riscv/*`，不是 detailed CUTE 差异中心 |
| SE 模式下 matrix load/store 的逐地址 guest virtual access | 基本对齐 | `Gem5MatrixMemoryAdapter` 的 SE 路径通过 `SETranslatingPortProxy` 按 guest 地址逐元素访问 |
| `hello_xsai` / `gemm_precomp` 功能闭环 | 已验证 functional 对齐 | 其中 `gemm_precomp` 当前可作为 `0x2b` 主口径 workload 的主线 functional 证据；仍不证明资源/时序对齐 |

## 2. 已确认差异

以下差异已经足够明确，应直接写成“当前未对齐”。

| 类别 | 当前 GEM5 | RTL/CUTE 参考 | 影响 | 计划承接位置 |
|------|-----------|---------------|------|--------------|
| commit 后 carrier | `MatrixAmuBuffer` 采用 `alloc-time ROB-parallel shadow` 近似，当前状态为 `valid/needAMU/writebacked/committed/canDeq` | `AMUCtrlBuffer` | `single-thread` 下已更接近 RTL 的 alloc/cleanup/fire contract，但仍不是固定槽位实现，且仍缺 `canEnqueue`、显式 ready/valid、SMT/global-oldest 语义 | `cute_detailed_design/matrix_core_design.md`，Phase 2 |
| request carrier | `MatrixExecPayload -> CuteRequest` | `AmuCtrlIO + AmuMmaIO/AmuLsuIO/AmuReleaseIO2CUTE/AmuArithIO` | 缺少 `pc/coreid`、difftest 风格 AMU event 载体 | Phase 0-1 trace/oracle，Phase 2 |
| backend issue 模型 | 已有 `DecodedFifo + Scoreboard + task/event` 主链；队首 issue、stall reason、分阶段 release 已接入，但 unit ready / accept / overlap 仍是近似 | `DecodedAmuCtrlFIFO + Scoreboard + micro task issue` | 当前 issue 有主链但还不是 RTL 级 ready/valid / FU 资源模型，仍会影响 stall 分布与吞吐解释 | Phase 3 / Phase 4A |
| load/store 路径 | 已有 `AML/BML/CML` task slot、`WriteFinish/TerminalCompletion`、SE guest-address 访问，以及第一阶段 fixed-latency `LocalMmuModel` / `64B` beat / 64 source ID / requester rotation；仍未建模 bounded fill table、response ready/backpressure、source ID 被 fill backpressure 持有、TL/LLC ready、response FIFO、cache backpressure | AML/BML/CML + LocalMMU + memory request/source 管理 + TL/LLC ready/valid | 当前 memory path 已有 fixed-latency timing boundary，但 response side 仍假设 always-ready / infinite fill buffer，不足以解释 fill table 满、source ID pressure、真实 cache/backpressure 行为 | Phase 4A / 后续 L2/TL 计划 |
| `mregfile` 读写仲裁/带宽 | `MatrixRegFile` 当前仍主要提供功能性 register 存取，没有按 `AML/BML/CML/ADC/BDC/CDC` 建立 bank 级读写仲裁、每拍读写宽度和 bank 并行度模型 | `MatrixReg` 端口 + A/B/C DataController | 当前无法解释 reg 侧 backpressure、bank 冲突，以及 load/fill/compute/writeback 的吞吐差异 | Phase 4A |
| compute 路径 | 已有 `ADC/BDC/CDC/MTE` 子状态、`ReadA/ReadB/Execute/WriteC/TerminalCompletion` 事件；`computePathReady()` 已收紧为 `ADC && BDC && CDC` all-ready；MTE helper 已按 active RTL controller 事实建模 accepted beats 与 GEM5 runtime completion 边界。仍缺 `ComputeGo` 逐拍锁步、CDC reorder/after-op、真实 TE 内部行为 | ADC/BDC/CDC + MatrixTE + compute completion 流程 | 当前 issue gate、beat bandwidth 和 GEM5 event 边界部分对齐，但资源/时序模型仍偏抽象，不能宣称 RTL cycle 等价 | Phase 4A |
| release/token | `Release` 已统一走 backend submit/completion，但 `execRelease()` 仍然是立即 functional release | release 受 pending store / backend drain / terminal completion 约束 | 当前 owner 已收口，但 release timing 仍过于理想化，可能过早放行 `macquire` | Phase 6 |
| backend 参数 | 基本无 CUTE 资源参数 | `DecodedAmuCtrlFIFODepth`、AB/C reg 数、`Matrix_MN`、`ReduceWidthByte`、`LLCSourceMaxNum` 等 | 当前无法解释资源和性能变化 | Phase 7 |
| trace/oracle | 依赖零散 `DPRINTF` 和 ISA stub request | ChiselDB `AMUCtrl_table`、`CUTELoadEvent`、`CUTEComputeEvent`、`CUTEStoreEvent`、`CUTEReleaseEvent` | 当前缺统一 diff 口径 | `verify_report.md` 与 `Plan.md`，Phase 0-1 |
| datatype / FP compute 覆盖 | tag 透传已扩到 `int8/int16/int32/int64/Fp16/Bf16/Tf32`；FP16 compute 已在 detailed backend/unit 路径形成最小 checkpoint，但 active ISA/decode 路径还不能稳定产生 FP MMA workload，BF16/TF32 compute 仍未闭环 | CUTE/RTL 支持更多 datatype 配置且可进入闭环 | 现在可以说“tag 已透传 + FP16 backend/unit checkpoint 已成立”，但不能把它外推成默认 RTL 主线或 ISA/workload 级 FP 已对齐 | Phase 4-5 |
| FS memory path | `physBaseAddr + offset` 简化 | 更接近 LocalMMU / TL / L2 路径 | 当前 FS timing/功能证据不足 | Phase 4 |

### 2026-05-15 compute path delta

- `ADC/BDC/CDC` issue gate:
  - 当前 GEM5 已把 `computePathReady()` 从 ADC-only 收紧为 `ADC && BDC && CDC` all-ready。
  - 该点从“已确认差异”降为“当前 all-ready issue contract 已覆盖到 unit-test 证据”。
- MTE bandwidth / accepted input beats:
  - GEM5 现在以当前 RTL `CUTE_8Tops_128SCP` 参数建模：`Tensor_MN=128`, `Tensor_K=64`, `Matrix_MN=8`, `ReduceWidthByte=32`, `ResultWidthByte=4`。
  - 每个 accepted beat: A/B/C/D 均为 `2048b = 256B`。
  - Round 1 按 active RTL controller 事实修正：`TaskController.scala` 直接传 `mtilem/mtilen`，A/B controller 对 M 做 `ceil(M/Matrix_MN)`，K 使用 `scaled_mtilek_bytes / ReduceWidthByte`，CDC 当前 assert `N == Tensor_MN`。
  - 因此当前 GEM5 timing contract 拒绝 `mtilen != 128`，接受 `mtilem <= 128`、`mtilen == 128`、`mtilek <= 64` 且 `k_groups > 0` 的请求；不再把任意小 `mtilen` 请求写成“补零到 full shape 后可 compute”。
- 仍未对齐:
  - `ComputeGo` 仍是事件式近似，不是 RTL ready/valid 全形状。
  - `totalCompletionCycles` 已绑定为 GEM5 compute issue -> terminal completion，并由 `lastMicrotaskLatency` targeted test 覆盖；仍不宣称 RTL cycle-accurate。
  - `FReducePE` 内部 accumulator 仍由 `executeMma()` 功能级摘要表示。

### 2026-05-21 LocalMMU/load-store response backpressure delta

- 当前 GEM5 已补到第一阶段 LocalMMU timing boundary：
  - MLOAD/MSTORE 按 `64B` LocalMMU beat 入队。
  - `LocalMmuModel` 支持 fixed latency、每 cycle 最多 issue 一个 beat、64 source IDs / max outstanding。
  - AML/BML/CML pending requests 已拆成三路队列，并按 requester rotation issue。
  - 每个 `64B` read response 在 backend 中拆成 `2 x 32B` MatrixReg `MemoryLoader` write chunks。
- 当前明确保留的未对齐块：
  - 不建模 bounded `MReg_Fill_Table` / fill FIFO depth。
  - 不建模 `response.ready = false` 时 response 留在 LocalMMU/TL response side。
  - 不建模 fill table 满或 MatrixReg fill drain 堵塞时 source ID 继续 busy。
  - 不建模 TL/LLC ready、TL D read-data/store-ack arbitration、真实 cache hit/miss/MSHR/coherence/retry。
- 当前推荐表述：
  - “LocalMMU 已是 fixed-latency / response-always-ready 近似。”
  - “当前等价于 infinite fill buffer；source ID 在 fixed latency response 被服务时释放。”
  - “不能据此声称 source ID pressure、fill table backpressure 或 TL/LLC timing 已对齐 RTL。”

### 2.1 当前必须盯住的重点未对齐项

下面这些点是当前 matrix detailed backend 收尾和后续性能口径里最关键的未对齐项，后续讨论必须优先引用：

| 点 | 当前 GEM5 状态 | RTL/CUTE 参考 | 当前判断 | 为什么重要 |
|----|----------------|---------------|----------|------------|
| TaskController 发射门控 | 当前已经有 `head -> scoreboard -> ready -> issue` 主链，但 `ready` 仍主要由 slot 占用和软件谓词近似；release path 也比 RTL `releaseReady = !pendingStore` 更保守 | `decodedFifo.head -> scoreboard.query -> unit ready -> deq.fire -> issue` | 部分对齐 | 会直接影响 issue stall 分布、release 放行时刻与吞吐解释 |
| `AML/BML/CML` 三路分流 | 已分成 A/B/C 三条逻辑路径，也已有 task slot、event、fixed-latency LocalMMU、source ID 上限和 requester rotation；但没有 bounded fill table、response ready/backpressure、source ID hold、TL/LLC ready 或真实 cache/resource backpressure | `AML/BML/CML` 独立路径 + LocalMMU/TL response backpressure | 部分对齐 | 会影响 load/store 资源竞争、source ID pressure、带宽解释和完成时刻 |
| `mregfile` 读写仲裁 / bank 带宽 | `MatrixRegFile` 已是 backend-owned regfile，但当前没有 `AML/BML/CML` 与 `ADC/BDC/CDC` 共享时的每拍读/写带宽、bank 并行度、仲裁和回填冲突模型 | `MatrixReg` 端口 | 未对齐 | 会直接影响 source/read/writeback/fill 的资源冲突与 timing 解释 |
| `ADC/BDC/CDC/MTE` compute 路径 | 已拆成显式 `ADC/BDC/MTE/CDC` 子单元状态，并保留 `ReadA/ReadB/Execute/WriteC` 对应边界；`computePathReady()` 已要求 `ADC/BDC/CDC` 同时 ready；MTE accepted beats 已按 active RTL controller 事实修正，`totalCompletionCycles` 已绑定 GEM5 runtime terminal boundary；但 `ComputeGo` 锁步、CDC reorder/after-op 和真实 TE 内部行为仍未建模 | `ADC + BDC + CDC + MTE` 独立子单元 | 部分对齐 | 会直接影响 compute overlap、stall 分布、重叠关系与 timing 解释 |
| `ComputeGo / EndReady` 完成边界 | 当前已用 `TaskEvent` 拆开 read/write/terminal completion | `MicroTaskEndValid && MicroTaskEndReady` | 方向对，但仍是软件事件近似 | 会影响 completion 边界和依赖释放时刻 |
| scoreboard 分阶段释放 | 已有 `load_finish / compute_read_finish_a/b / compute_write_finish_c / store_finish` 分阶段释放 | issue reserve + 分阶段 release | 部分对齐 | 会直接影响后继依赖解除时刻，进而影响性能判断 |
| datatype 编码 | `MatrixExecPayload -> CuteRequest -> MatrixTensor -> memory adapter` 的 tag 传递已显式，FP16/BF16/TF32 不再被整数宽度吞掉；FP16 compute 已在 backend/unit 路径形成最小 checkpoint，但 BF16/TF32 compute 仍未闭环，active decode 也还没形成 FP workload 主线 | RTL `mmaDataType` 覆盖更广整数/fp 编码 | 目前能说“tag 没丢 + FP16 backend/unit checkpoint 成立”，但还不能说“FP workload 已对齐” | 会造成功能口径与性能口径双重不一致 |

这些点里，前六项主要影响：
- backend stall reason
- 依赖解除时刻
- 子单元重叠关系
- 吞吐和性能趋势解释

最后一项 `datatype` 主要影响：
- 功能覆盖范围
- workload 可达路径
- datatype 相关 timing 口径

当前建议固定的口径是：
- 已支持：
  - FP16/BF16/TF32 的 datatype tag 透传
  - LSU / memory adapter 按 raw bits 处理 FP16/BF16/TF32
  - request 可以带着 FP datatype 一路传到 backend
  - FP16 MMA 在 detailed backend/unit 路径上的结果正确性闭环
- 仍不支持：
  - active ISA/decode 路径上的 FP MMA workload 闭环
  - BF16/TF32 compute 的结果正确执行

## 3. 部分对齐 / 阶段性简化

以下点当前可以视为“阶段性近似”，但必须明确写出限制。

### 3.1 `MatrixAmuBuffer` 与 `AMUCtrlBuffer`

- 当前 `valid / needAMU / writebacked / committed / canDeq / amuReqValid` 这组状态语义接近 RTL
- 当前 commit 侧已经补上 `peekReadyMatrixAmuEntry() + canAcceptMatrixBackendReq()`，不会在 backend 不可接收时提前消费 ready entry
- 当前 `MatrixAmuBuffer` 已改成：
  - `alloc` 时占用 `ROB-parallel shadow` entry
  - `cleanup` 时释放 entry
  - `single-thread` 下可以观察到 `shadowFree/full/deq` 行为
- 但实现上仍缺：
  - 固定槽位而非 `std::list + erase` 的 carrier 结构
  - `CommitWidth` window 的严格 RTL 等价
  - `canEnqueue / canEnqueueForDispatch`
  - `pc/coreid`
  - 显式 backend ready/backpressure
  - `SMT` 下 global-oldest ready 仲裁

### 3.2 request shape

- `AmuLsuDesc / AmuMmaDesc / AmuArithDesc / AmuReleaseDesc` 已能承载当前 functional 闭环
- 但仍只是 CUTE request 的最小子集，不是完整 `AmuCtrlIO` 行为等价

### 3.3 token 语义

- `msyncregreset / mrelease / macquire` 已有 functional 语义闭环
- `mrelease` 的 owner 已收口到 backend completion
- 但当前 CPU shadow token、ISA token、backend token 的整体 serialize/恢复策略仍未冻结

### 3.4 workload functional pass

- `hello_xsai`、`mem_test`、`gemm_precomp` 通过，说明当前功能闭环可用
- 新增 `single-thread` 定向验证已命中：
  - `MLS replay`
  - `squash + shadow window`
  - `AMU full/backpressure/deq`
- 但不能据此推断：
  - queue/backpressure 对齐
  - scoreboard 对齐
  - release gating 对齐
  - memory path 对齐
  - `MLS s2/s3` 已对齐

### 3.5 当前实验前提

- 当前阶段结论只覆盖 `single-thread`
- 当前阶段不覆盖 `SMT`

### 3.6 当前对“部分对齐”的推荐口径

对下列点，当前推荐统一使用“部分对齐”而不是“已对齐”：

- `TaskController` 发射门控
- `AML/BML/CML` 三路分流
- `ADC/BDC/CDC/MTE` compute 路径
- `ComputeGo / EndReady` 完成边界
- scoreboard 分阶段释放

原因不是这些点完全没做，而是：
- 当前已有结构映射或事件边界
- 但还没达到 RTL 级真实并行、端口竞争、ready/valid 背压或 datatype 全覆盖

## 4. 证据不足

以下点目前不能写成强结论。

| 点 | 当前证据状态 | 当前结论 |
|----|--------------|----------|
| FS 模式 matrix memory access | 只有简化实现，没有与 RTL/CUTE memory path 的对照证据 | 只能写“当前为简化实现” |
| CUTE event 对照 | 还没有 GEM5/CUTE 统一 trace diff | 不能写“backend 事件已对齐” |
| detailed release/acquire 时序 | 还没有 store -> release -> acquire 的 detailed oracle | 不能写“token 时序已接近 RTL” |
| datatype 全覆盖 | 当前 functional kernel 主要覆盖 int8/int32 | 不能写“matrix datatype 与 RTL 对齐” |
| performance/stall 解释 | 当前没有 detailed CUTE stats | 不能写“性能趋势与 RTL 一致” |

## 5. 推荐分层表述

推荐使用以下表述：

- “当前 GEM5 已具备 matrix O3 到 functional backend 的最小闭环。”
- “commit 前不可见、commit 后 backend-visible 的边界已经建立。”
- “`MatrixAmuEntry` 是 `AMUCtrlBuffer` 的阶段性近似。”
- “当前 backend 已进入 event-driven detailed CUTE 阶段，但仍未实现 RTL 级端口竞争、完整 ready/valid 背压与多 compute 并行实体。”
- “SE 模式下 matrix 访存功能已打通；FS 模式仍为简化路径。”

## 6. 避免过强表述

以下说法当前都过强，不应直接使用：

- “GEM5 的 CUTE 已与 RTL 对齐”
- “Matrix backend 已经详细建模完成”
- “`MatrixAmuEntry` 等价于 `AMUCtrlBuffer`”
- “release/acquire 时序已经与 CUTE 一致”
- “通过 `hello_xsai` / `gemm_precomp` 就说明 backend 资源模型正确”
- “FS 路径已经可用于 RTL 风格 memory timing 结论”

## 相关文档

- `Plan.md`
  - 计划真源
- `cute_detailed_design/matrix_core_design.md`
  - 核内路径设计
- `verify_report.md`
  - GEM5/CUTE trace、oracle 与验证证据摘要
- `implementation_notes.md`
  - 本轮实际改动
- `verify_report.md`
  - 验证与证据
- `handoff.md`
  - 交接状态

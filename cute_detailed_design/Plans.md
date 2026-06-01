# GEM5 Matrix Plan

创建日期: 2026-04-12  
最近更新: 2026-05-13

## 文档职责

- 这份文件是 matrix 工作的唯一计划真源。
- 它只负责：
  - 阶段划分
  - task / DoD / depends / status
  - 推进边界
  - 当前停点与下一步
- 它不负责：
  - 展开核内设计细节
  - 保存完整验证日志
  - 记录逐轮代码改动
  - 充当交接摘要

## 相关文档边界

- `cute_detailed_design/matrix_core_design.md`
  - 负责核内路径、regfile、AML/BML/CML、backend glue 最小契约
- `cute_detailed_design/matrix_diff_vs_rtl.md`
  - 负责当前未对齐项清单
- `cute_detailed_design/implementation_notes.md`
  - 负责本轮实际改动
- `cute_detailed_design/verify_report.md`
  - 负责验证与证据
- `cute_detailed_design/handoff.md`
  - 负责当前停点与交接

## 总目标

- 让 GEM5 能识别并正确执行当前 RTL 已实现的 matrix 指令子集。
- 让 GEM5 的 matrix 配置、tile、token 与 RTL 的可观察行为一致。
- 把当前 simplified backend 逐步收敛成更接近 RTL/CUTE 的 detailed backend。
- 明确区分：
  - 功能正确
  - 资源约束对齐
  - 时序/性能近似对齐

## 执行约定

- 运行与评审规则见 `AGENTS.md`。
- 本文件只保留阶段、task、DoD、depends、status、阶段边界与当前停点。
- 每个 task 在从 `TODO/WIP` 推进到下一状态前，至少要有一条可复现的最小回归测试；优先复用当前 matrix 回归阶梯，不用无关测试替代。

## 当前事实基线

- 当前 active `src/matrix` detailed backend 已拆成：
  - `taskcontrol.cc`
  - `backend_runtime.cc`
  - `mregfile.cc`
  - `memoryload.cc`
  - `matrix_ex.cc`
- 当前已部分收敛：
  - `TaskController` 风格发射门控
  - `AML/BML/CML` 三路分流
  - `ADC/BDC/CDC/MTE` 分阶段 compute 路径
  - 事件式 `ComputeGo / EndReady` 边界
  - scoreboard 分阶段释放
- 当前仍未完全对齐：
  - 端口级 valid/ready
  - 真正 load/store 带宽与 bank 竞争
  - `ADC/BDC/CDC/MTE` 独立实体化
  - fp mma 功能闭环
- 当前重点未对齐项以：
  - `cute_detailed_design/matrix_diff_vs_rtl.md`
  为唯一差异清单来源；后续讨论 `TaskController` 门控、`AML/BML/CML`、`ADC/BDC/CDC/MTE`、`ComputeGo/EndReady`、scoreboard 边界、datatype 编码时，都应先回到该文档口径。

## 阶段总览

- Phase 0: 目标冻结与证据基线
- Phase 1: ISA 语义阶段
- Phase 2: O3 CPU 通路与 commit 后边界
- Phase 3: detailed CUTE backend
- Phase 4: 配置、统计与性能校准入口
- Phase 5: 回归、RTL/CUTE 对照与收敛

---

## Phase 4A: 主线时序/建模对齐计划

Purpose: 以 `0x2b / gemm_precomp` 为主线 workload，把当前“功能可跑”继续收成“时序/建模更接近 RTL/CUTE”的可执行计划。当前优先级原则是：

1. 先梳理 compute 边界差异
2. memory/resource/bandwidth/timing 建模前置或并行推进
3. 再细化 MLS replay/cancel/fault cause
4. 最后收 AMU buffer / carrier 结构

### 对齐思路总览

- **第一优先：compute 边界梳理**
  - 先把 `ADC/BDC/CDC/MTE` 的 issue / occupy / finish / completion contract 说清楚
  - 当前最关键的 blocking 差异不是“有没有子状态”，而是：
    - GEM5 issue gate 已在 2026-05-15 收紧为 `ADC/BDC/CDC` all-ready，后续仍需继续收 `ComputeGo / MicroTaskEndValid/Ready`
    - RTL `TaskController` 要求 `ADC/BDC/CDC` 同时 ready 才能 `issueMma`
    - `ComputeGo` / `MicroTaskEndValid/Ready` 锁步 contract 在 GEM5 还只是事件近似
- **第二优先：memory/resource/bandwidth/timing 建模**
  - 这一步不能等 compute timing 完成后再做
  - 因为 `gemm_precomp` 的主线性能/时序解释很大程度上受 memory path 限制
  - 当前 GEM5 还是逐元素 SE 访问，而 RTL/CUTE 已经显式有 `LocalMMU/sourceId/fill-table/FIFO`
  - 另外当前还缺 `mregfile` 读写仲裁与每拍读写带宽模型
  - 所以这一步先收紧 memory path 的 source/response/backpressure，以及 `mregfile` 的简单带宽/时序模型；细 cache 行为先不优先建模
- **第三优先：MLS replay/cancel/fault cause**
  - 在 compute 与 memory 主 contract 收紧后，再把 MLS replay/cancel 的来源、恢复和 safe-writeback 边界收细
- **第四优先：AMU buffer / carrier**
  - carrier 结构留到最后
  - 前提是先声明：当前阶段不会支撑 CommitWidth/SMT/global-oldest 的 timing 结论

### 当前代码级差异摘要

- `3. compute backend`
  - GEM5 当前：
    - `backend_runtime.cc` 里 `computeTasks.front()` 逐阶段推进
    - `executeMma()` 仍然是一次性矩阵乘加循环，没有 RTL 级 `TE` 内部行为
    - `computeExecuteLatency()` 只用 `Matrix_MN / ReduceWidthByte` 做宏观延迟近似
  - RTL/CUTE 当前：
    - `TaskController.scala` 要求 `ADC/BDC/CDC` 同时 ready 才允许 `issueMma`
    - `BDataController.scala` / `CDataController.scala` 用 `ComputeGo` 锁步发射、hold 数据、等待 `MicroTaskEndReady`
    - `CDC` 同时承担旧 C 读出、结果写回、transpose/reorder/after-op 等收尾
  - 直接差异：
    - GEM5 还没有 `ComputeGo` 等价握手
    - GEM5 没有 `CDC` 级别的数据重排/写回 contract
    - GEM5 的 `MTE` 只是抽象执行延迟，不是 RTL 级运算单元

- `4. memory path`
  - GEM5 当前：
    - `memoryload.cc` 里 `executeLsu()` 直接 `loadTile/storeTile`
    - `matrix_memory_adapter_gem5.cc` 在 SE 下逐元素按 guest 地址访问
    - 没有 sourceId、response FIFO、fill-table、多拍回填和 coherent/noncoherent 区分
    - 没有 `mregfile` 读写仲裁与每拍读/写带宽模型
  - RTL/CUTE 当前：
    - `TaskController.scala` 会为 `AML/BML/CML` 下发 tensor datatype、stride、row/col、transpose、source scoreboard 更新
    - `CMemoryLoader.scala` 明确有 `LocalMMUIO.Request/Response`
    - 有 `sourceId -> bank/addr` 搜索表、fill FIFO、按 bank 回填、`MicroTaskEndValid/Ready`
    - `A/B/C DataController` 与 `AMemoryLoader/BMemoryLoader/CMemoryLoader` 都通过 `MatrixReg` 端口协作，天然受 bank 和拍级读写能力约束
  - 直接差异：
    - GEM5 当前 memory path 还是功能访问，不是 loader/source/response 模型
    - GEM5 没有 RTL/CUTE 的请求-响应-回填-完成时序链
    - GEM5 也还没把 `mregfile` 读写仲裁、bank 并行度、每拍读写带宽组织成 AML/BML/CML/ADC/BDC/CDC 级别的资源模型

- `2. MLS replay/cancel/fault cause`
  - GEM5 当前：
    - `cpu/o3/mls_unit.cc` 只识别一个核心 replay cause：`tlbMiss`
    - replay state 只保存 `vaddr/paddr/stride/tile0/tile1/mode/asid`
    - squash/cancel 主要靠 O3 redirect 和 replay queue 生命周期近似
  - RTL 当前：
    - `XSAI/src/.../MlsUnit.scala` 明确有 `rep_carry`、`cause` 向量、`need_rep`
    - 注释和逻辑里区分 tlb miss、forward fail、redirect kill、cache replay 等来源
    - replay ready / cancel / kill 在各级流水里有不同触发点
  - 直接差异：
    - GEM5 replay/cancel 原因过粗
    - 缺少 replay cause 粒度，没法对齐 RTL 的 replay/cancel 分布和恢复边界

- `1. AMU buffer / carrier`
  - GEM5 当前：
    - `matrix_amu_buffer.cc` 是 `std::list<Entry>`，动态扫描 `findReadyToFire()`
    - 只保留 `valid/needAMU/writebacked/committed/canDeq/backendReq`
    - `fireWidth` 只是队首窗口扫描，不是固定槽位和显式 ready/valid
  - RTL 当前：
    - `AMUCtrlBuffer.scala` 有固定 `size = 32`
    - `valid/needAMU/writebacked/committed/canDeq/amuReqValid` 是显式寄存状态
    - `toAMU` 是 `Vec(CommitWidth, DecoupledIO(new AmuCtrlIO))`
    - 直接挂在 ROB commit 侧
  - 直接差异：
    - GEM5 carrier 结构不是固定槽位
    - GEM5 没有 `CommitWidth` 级别的显式 `toAMU` 端口语义
    - 单线程 contract 已有，但结构/时序上还不是 RTL 形态

### 主线差异锚点

- compute backend：
  - GEM5：
    - `src/matrix/backend_runtime.cc`
    - `src/matrix/taskcontrol.cc`
    - `src/matrix/matrix_ex.cc`
  - RTL/CUTE：
    - `XSAI/CUTE/src/main/scala/TaskController.scala`
    - `XSAI/CUTE/src/main/scala/BDataController.scala`
    - `XSAI/CUTE/src/main/scala/CDataController.scala`
- memory path：
  - GEM5：
    - `src/matrix/memoryload.cc`
    - `src/matrix/matrix_memory_adapter_gem5.cc`
  - RTL/CUTE：
    - `XSAI/CUTE/src/main/scala/CMemoryLoader.scala`
    - `XSAI/CUTE/src/main/scala/TaskController.scala`
- MLS replay/cancel：
  - GEM5：
    - `src/cpu/o3/mls_unit.cc`
    - `src/cpu/o3/mls_replay_queue.hh`
    - `src/cpu/o3/mls_virtual_queue.hh`
  - RTL：
    - `XSAI/src/main/scala/xiangshan/mem/pipeline/MlsUnit.scala`
- AMU buffer：
  - GEM5：
    - `src/cpu/o3/matrix_amu_buffer.hh`
    - `src/cpu/o3/matrix_amu_buffer.cc`
    - `src/cpu/o3/rob.cc`
  - RTL/CUTE：
    - `XSAI/CUTE/src/main/scala/TaskController.scala`
    - `XSAI/src/main/scala/xiangshan/XSTile.scala`

### 4A.1 compute boundary 先行

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 4A.1 | 梳理 `ADC/BDC/CDC/MTE` 边界差异 | 已明确 GEM5 `backend_runtime/matrix_ex` 与 CUTE `TaskController/BDataController/CDataController` 的阶段差异；issue gate 已在 2026-05-15 收紧为 `ADC/BDC/CDC` all-ready。剩余 gap 聚焦 `ComputeGo / EndReady`、CDC reorder/after-op、MatrixReg arbitration 与 TE 内部行为 | 4.5 | cc:WIP |
| 4A.2 | 收紧 `ComputeGo/EndReady` 对应关系 | 让 GEM5 的 compute read/execute/write/terminal 边界能映射到 `ComputeGo` 与 `MicroTaskEndValid/Ready` | 4A.1 | cc:TODO |
| 4A.3 | 收紧 compute overlap / unit-ready contract | all-ready issue gate、second compute 前级 overlap、子单元占用和 terminal boundary 已有 targeted evidence；后续只保留完整 RTL ready/valid、独立实体化、CDC reorder/after-op 与 MatrixReg arbitration 等未完成范围 | 4A.2 | cc:WIP |

### 4A.2 memory/resource/bandwidth/timing 建模前置

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 4A.4 | 梳理 `AML/BML/CML + LocalMMU` 主 contract 差异 | 写清 request sourceId、response、fill-table、store path、完成边界与当前 GEM5 对应关系 | 4A.1 | cc:TODO |
| 4A.5 | 冻结 `mregfile` 读写仲裁与带宽模型 | 明确 A/B/C 路径与 `ADC/BDC/CDC` 对 `MatrixReg` 的读写仲裁、bank 并行度、每拍读写带宽口径 | 4A.4 | cc:TODO |
| 4A.6 | 明确带宽/资源参数差异 | 把 `outsideDataWidthByte`、AB/C bank、fill FIFO depth、`LLCSourceMaxNum`、store transpose/reorder 等列成差异表 | 4A.5 | cc:TODO |
| 4A.7 | 收紧 SE memory path 的简单带宽/时序模型 | 至少把 `mla/mlb/mlc/msc` 的 source/backpressure/FIFO/response 边界，以及 `mregfile` 简单仲裁/带宽模型收成稳定模型；细 cache 行为先不做 | 4A.6 | cc:TODO |
| 4A.8 | 建主线 memory path 观察点 | 输出能解释 load/store latency、backpressure、response path、`mregfile` 争用的最小统计/trace 证据 | 4A.7 | cc:TODO |

### 4A.3 compute timing 数值口径

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 4A.9 | 建主线 compute timing 口径 | 基于 `Matrix_MN / ReduceWidthByte / mtilem/n/k` 建立可解释的主线 compute timing 口径，并明确与 RTL 仍未对齐的子单元延迟项 | 4A.3, 4A.8 | cc:TODO |

### 4A.4 MLS replay/cancel/fault cause 细化

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 4A.10 | 梳理 MLS replay/cancel 原因矩阵 | 把 tlb miss、forward fail、redirect cancel、fault、replacement/backpressure、LSQ/feedback 等原因整理成对照表 | 4A.8 | cc:TODO |
| 4A.11 | 收紧 GEM5 MLS replay/cancel contract | 让 replay/cancel/fault 的触发与恢复边界更接近 RTL `MlsUnit` 的 cause/carry 与 safe-writeback 语义 | 4A.10 | cc:TODO |
| 4A.12 | 固定 MLS replay/cancel 定向回归 | 用最小 workload 固定 replay/cancel/fault cause 的可复现证据 | 4A.11 | cc:TODO |

### 4A.5 AMU buffer / carrier 结构对齐

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 4A.13 | 梳理 `MatrixAmuBuffer` 与 RTL carrier 差异 | 明确固定槽位、ready/valid、enqueue/dequeue、CommitWidth、global-oldest、SMT 差异边界 | 4A.12 | cc:TODO |
| 4A.14 | 收紧单线程 carrier contract | 在不扩到 SMT 的前提下，把当前 shadow 近似收成更像 `AMUCtrlBuffer` 的单线程 contract | 4A.13 | cc:TODO |
| 4A.15 | 冻结 carrier 与主线建模边界 | 明确哪些继续是近似，哪些已作为主线 contract 收口；声明当前阶段不支撑 CommitWidth/SMT/global-oldest timing 结论 | 4A.14 | cc:TODO |

### 4A 验收标准

- 已把过时的“没有 FIFO/没有 compute 分阶段”旧口径清理掉，当前计划建立在双方现有代码事实上。
- compute boundary 已作为第一优先，但 memory/resource/bandwidth/timing 建模已前置或并行，不再被迫等 compute timing 完成后才开始。
- compute backend、memory path、MLS replay/cancel、AMU buffer 四块都各自有：
  - RTL/CUTE 参考锚点
  - GEM5 对应入口
  - 可观测 DoD
- 文档中明确区分：
  - 功能闭环
  - 时序/建模对齐
  - 证据不足

---

## Phase 0: 目标冻结与证据基线

Purpose: 冻结范围、边界、oracle 与验证口径，避免目标漂移。

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 0.1 | 冻结 matrix 指令范围 | 覆盖 `minit`、`msettile*`、`msyncregreset`、`mrelease`、`macquire`、`csrr mtile*`、`mzero`、默认 `mma/mls` | - | cc:完成 |
| 0.2 | 建 RTL ↔ GEM5 差距矩阵 | 明确每类指令的 RTL owner、GEM5 owner、当前缺口 | 0.1 | cc:完成 |
| 0.3 | 冻结验证基线 | 明确 unit test / smoke / trace / diff 的最小阶梯 | 0.2 | cc:完成 |
| 0.4 | 冻结 detailed backend 层级 | 明确哪些先做资源 timing、哪些只做统计近似 | 0.2 | cc:完成 |

### Phase 0 验收标准

- 指令范围、对齐方向、验证阶梯、detailed backend 层级已冻结成文。
- `Plan.md`、`matrix_diff_vs_rtl.md`、`matrix_core_design.md` 的职责边界不冲突。
- 不再存在“这个阶段到底要不要做”的范围歧义。
- reviewer 能仅靠文档判断后续各阶段要证明什么。

---

## Phase 1: ISA 语义阶段

Purpose: 只处理 `src/arch/riscv/*` 的 matrix ISA、CSR、tile、token 与最小功能语义。

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 1.1 | 提取 RTL matrix ISA/state 模型 | 覆盖 `mx/mtile*`、`xmcsr`、token、AMU 请求类型 | 0.2 | cc:完成 |
| 1.2 | 设计 GEM5 matrix architectural state | 冻结 `mtilem/mtilen/mtilek`、`xmxrm/xmfrm/xmsat/xmsaten`、token 状态 | 1.1 | cc:WIP |
| 1.3 | 接入 matrix decode / inst class | GEM5 能 decode 第一批 matrix 控制/同步/算术/访存指令 | 1.2 | cc:WIP |
| 1.4 | 对齐 tile / CSR / token 语义 | 完成 `minit`、`msettile*`、`csrr mtile*`、`msyncregreset`、`macquire`、`mrelease` | 1.3 | cc:WIP |
| 1.5 | 建 checker / trace / 最小验证 | 至少覆盖 `msettile* -> csrr mtile* -> mls -> mma -> mrelease/macquire` | 1.4 | cc:WIP |

### Phase 1 验收标准

- `src/arch/riscv/*` 已覆盖当前目标 matrix ISA 子集的 decode 与最小 architectural state。
- `tile / CSR / token` 语义至少有一组可复现最小验证。
- `mzero / mma / mls / mrelease / macquire` 的前端 request-shape 或最小功能语义已建立。
- 仍未覆盖的 ISA 子集已明确列为未完成，而不是隐含省略。

---

## Phase 2: O3 CPU 通路与 Commit 后边界

Purpose: 把 `decode -> rename -> issue -> execute -> LSQ -> commit -> matrix buffer/backend` 这条 CPU 通路接完整。

### Phase 2 当前实验前提

- 只覆盖 `single-thread`
- 不覆盖 `SMT`
- `AMUCtrlBuffer` 的进一步结构对齐不作为当前 blocker

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 2.1 | 梳理 RTL CPU 通路职责映射 | 明确各类 matrix 指令是否进 rename / issue / LSU / commit 后 AMU | 1.5 | cc:TODO |
| 2.2 | 接入 O3 decode 摘要 | O3 能稳定看到 matrix 指令类别与关键摘要 | 2.1 | cc:完成 |
| 2.3 | 完成 rename / ROB 承载 | matrix 指令的 rename / ROB / carrier 关系成立 | 2.2 | cc:完成 |
| 2.4 | 完成 dispatch / issue / wakeup | 接入正确 scheduler / IQ / wakeup / bypass | 2.3 | cc:完成 |
| 2.5 | 完成非 LSU m 类执行路径 | `mzero/marith/mma/release` 的 CPU 内路径可工作 | 2.4 | cc:完成 |
| 2.6 | 完成非 LSU producer 的 commit 边界 | `mma/marith/mrelease` commit 后才 backend-visible | 2.5 | cc:WIP |
| 2.7 | 完成 `mla/msa/mls` 全路径 | `MLS` 并入既有 commit/buffer 边界 | 2.6 | cc:TODO |
| 2.8 | 补 single-thread 定向证据 | 命中 replay / squash / backpressure / deq 等关键证据 | 2.7 | cc:完成 |
| 2.9 | 冻结当前实验前提 | 文档中明确 single-thread only，不夸大为 Phase 2 收口 | 2.8 | cc:完成 |

### Phase 2 当前结论

- 已成立：
  - commit 前不可 backend-visible
  - wrong-path / faulted request 不误入 backend
  - `toAMU proxy -> backend submit/completion` contract 已收口
- 还不能写成：
  - `MLS` 已对齐 RTL
  - `AMUCtrlBuffer` 已结构对齐 RTL
  - `Phase 2` 已完成

### Phase 2 当前评审结论（2026-04-26）

- 当前阶段的实验前提收敛为：
  - 只覆盖 `single-thread`
  - 不覆盖 `SMT`
  - 不以 `AMUbuffer` 进一步结构改造作为本轮 blocker
- 在这个前提下，`AMUbuffer` 相关实现先不继续改；当前只把它视为阶段性近似，不继续追求固定槽位、SMT 全局 oldest、全局共享容量语义。
- 当前 `MLS` 路径已朝 RTL 方向收敛，但仍未达到“已对齐 RTL”的结论。
- 当前最多只能说：
  - `single-thread happy-path` 更接近 RTL
- 当前不能写成：
  - `MLS` 已对齐 RTL

### Phase 2 -> Phase 3 进入门槛

- 进入 Phase 3 前，至少应满足：
  - `MLS replay` 命中
  - `squash + shadow window` 命中
  - `AMU full/backpressure/deq` 命中
  - 当前实验前提已冻结为 `single-thread only`
  - 旧 `buffered` 语义已从主文档中清掉

### Phase 2 验收标准

- commit 前不可 backend-visible、commit 后才进入 backend 的边界有直接证据。
- wrong-path / faulted / squash request 不误入 backend 有直接证据。
- `single-thread` 前提已明确冻结，文档中不再混入 `SMT` 结论。
- `MLS replay`、`squash + shadow window`、`AMU full/backpressure/deq` 至少各有一条验证证据。
- 当前阶段若仍是阶段性近似，文档必须明确写成 `部分对齐` 或 `证据不足`，不能写成 `已对齐 RTL`。

---

## Phase 3: Detailed CUTE Backend

Purpose: 把当前 simplified backend 逐步收敛成更接近 RTL/CUTE 的 detailed backend。

### Phase 3A: request / issue / dependency

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 3.1 | 提炼 backend request / issue 协议 | 已冻结当前 `submit -> fifo -> headReady -> issue -> task/event -> completion` 主链语义，并明确其与 RTL `TaskController` 的部分对齐边界 | 2.9 | cc:WIP |
| 3.2 | 建 decoded FIFO 模型 | 已有深度可配 FIFO，默认深度 8；已覆盖 enqueue/full/deq/headReady 与 read/write set normalization | 3.1 | cc:完成 |
| 3.3 | 建 scoreboard 抽象 | 已支持 reg busy、pending readers、FU busy、src ready、dest busy，并具备 load/store/compute/arith 分阶段 release 回调 | 3.2 | cc:完成 |
| 3.4 | 接入 issue stall reason | 已至少区分 fifo full、FU busy、dest busy、src not ready、pending reader、release blocked，并已有 `fifo_block / scoreboard_block` trace | 3.3 | cc:完成 |
| 3.5 | 所有 request 走 FIFO + scoreboard | 当前 `load/store/mzero/mma/release` 已走 FIFO + scoreboard 主链，且 release 也受 FIFO/ready/block 约束；但 `TaskController` 的 unit ready/path-level backpressure 仍只做到部分对齐 | 3.3, 3.4 | cc:WIP |

### Phase 3B: AML/BML/CML load/store 数据面

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 3.6 | 冻结 AML/BML/CML 设计方案 | 明确职责、read/write set、finish 边界、completion_event 接口 | 3.5 | cc:完成 |
| 3.7 | 冻结 MatrixReg 拍级位宽口径 | 写清一拍读/写多少 bit；先不补 bank/arbitration | 3.6 | cc:完成 |
| 3.8 | 建 load/store/zero task dispatch | `AML/BML/CML-load/CML-store/zero/release` 已进入各自 task path 和内部状态推进，但 path-level ready/backpressure 仍未完全对齐 RTL | 3.6, 3.7 | cc:WIP |
| 3.9 | 建 loader/store 最小阶段机 | 当前已具备 `Accepted -> MemReq/RegRead -> Fill/StorePending -> RegWrite` 主体阶段，但 `Done` 仍主要通过软件事件/slot 收尾近似 | 3.8 | cc:WIP |
| 3.10 | 建 completion_event 回路 | 当前已具备 `WriteFinish / TerminalCompletion` 事件回路，且 release 已观察 pending store；但 `ComputeGo / EndReady` 仍只做到软件事件近似 | 3.9 | cc:WIP |
| 3.11 | 对齐 LSU 数据功能与最小 memory path | 当前已实现非 issue 同拍完成、load/store snapshot、shared memoryBudget 等最小路径；但完整 LSU 数据功能与 memory path 仍未收口 | 3.9, 3.10 | cc:WIP |

### Phase 3C: ADC/BDC/CDC/MTE compute 与 datatype

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 3.12 | 冻结 MatrixTE timing 公式 | 说明 `Matrix_MN`、`ReduceWidthByte`、`mtilem/n/k` 如何影响 cycle | 3.7 | cc:完成 |
| 3.13 | 建 compute microtask 状态机 | 至少有 `ReadA/ReadB/Execute/WriteC/TerminalCompletion` | 3.8, 3.9, 3.12 | cc:完成 |
| 3.14 | 收紧 `mzero` / compute 边界 | `mzero` 不混入 compute family | 3.13 | cc:完成 |
| 3.15 | 扩展 datatype 策略 | `int8->int32` Required；fp/bf16/tf32 先标 Recommended | 3.13 | cc:完成 |
| 3.16 | 继续收 ADC/BDC/CDC/MTE 独立化 | 已拆成显式 `ADC/BDC/CDC/MTE` 子单元状态，并具备最小多 compute in flight / 子单元级 backpressure；当前 second compute issue 要求 `ADC/BDC/CDC` all-ready，MTE busy 不属于 `TaskController` issue-ready contract；但仍未形成完整 RTL ready/valid 与独立实体模型 | 3.13 | cc:完成 |

### Phase 3D: release / acquire / token / completion

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 3.17 | 梳理 token owner | 明确 CPU shadow token、ISA token、backend token 的 owner | 2.9 | cc:TODO |
| 3.18 | 建 release gating 模型 | 当前 release 已至少受 pending store、backend drain 与 terminal completion 约束；但仍未展开成 RTL 级完整 ready/valid completion gating | 3.10 | cc:完成 |
| 3.19 | 对齐 macquire 阻塞与 re-exec | token 未 ready 时顺序不被异步 completion 破坏 | 3.18 | cc:WIP |
| 3.20 | 明确 serialize/checkpoint 策略 | 支持则实现；不支持则 fail-fast | 3.19 | cc:TODO |

### Phase 3 当前状态快照（2026-05-08）

- 当前重点未对齐项的口径与 `cute_detailed_design/matrix_diff_vs_rtl.md` 保持一致：
  - `TaskController` 发射门控：部分对齐
  - `AML/BML/CML` 三路分流：部分对齐
  - `ADC/BDC/CDC/MTE` compute 路径：已拆成显式子单元状态，并具备最小前级重叠；当前 second compute 的 issue 由子单元可用性驱动，而不是 monolithic compute busy；但仍未达到完整独立实体与 RTL 级并行关系
  - `ComputeGo / EndReady` 完成边界：方向对，但仍是软件事件近似
  - scoreboard 分阶段释放：部分对齐
  - datatype 编码：明确未对齐
- 上述未对齐项主要影响：
  - backend stall reason
  - 依赖解除时刻
  - 子单元重叠关系
  - 吞吐和性能趋势解释

### 关键替代路径

- 保守路径：
  - 先实现 event-driven abstract CUTE model
  - 不做精确 memory/TL timing
  - 优点是低风险、快验证
  - 缺点是性能结论偏弱
- 激进路径：
  - 直接按 CUTE 模块拆 `AML/BML/CML/ADC/BDC/CDC/MTE/LocalMMU`
  - 优点是结构最接近 RTL
  - 缺点是实现量大、验证难
- 推荐路径：
  - 先 event-driven
  - 再按 trace mismatch 和 stats 需求逐层细化
  - 不再继续维护 functional fallback

### 第一轮最小 DoD

- `Plan.md` 的 Phase 0-1 产物落地
- GEM5 能输出稳定 AMU/CUTE trace
- 至少一个 CUTE oracle trace 口径被确认可用，或明确标注当前不可用原因
- 当前差异清单已更新 detailed CUTE 相关差异分类

### Phase 3 验收标准

- `matrix_diff_vs_rtl.md` 中列出的重点未对齐项，已逐条映射到 Phase 3 任务。
- `Depends / Status / DoD` 三者自洽，不再出现“completed 依赖 TODO”但又没说明是设计冻结的情况。
- `TaskController` 发射门控、`AML/BML/CML`、`ADC/BDC/CDC/MTE`、`ComputeGo/EndReady`、scoreboard 边界、datatype 编码，以及 `release / acquire / token / serialize` 口径，必须分别标明：
  - 本阶段收掉
  - 本阶段只做到部分对齐
  - 延后到后续阶段
- 每个已完成子阶段都至少有一个可观测 completion gate，而不只是结构描述。
- 任何“部分对齐”的点，都必须能说明还差什么，不能只写“已部分完成”。

---

## Phase 4: 主线 datatype 与 FP16 分阶段接入

Purpose: 以当前 RTL 主口径 workload 为准，在不破坏当前 int8/int32 主链的前提下，先把 FP16/BF16/TF32 datatype 透传到 backend request / tensor / memory adapter，再只在 oracle 可用时收敛 FP16 compute 正确性，并用 `gemm_precomp` 等 `0x2b` workload 固定主回归。

### 4.1 主线 workload 与 datatype 边界冻结

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 4.1 | 冻结主线 workload 与 datatype 边界 | 明确以 `0x2b` / RTL 当前主口径 workload 为准，datatype 目标覆盖 `FP16/BF16/TF32`，compute 未闭环前显式 `Unsupported` | 3.18 | cc:完成 |

### 4.3 FP16/BF16/TF32 datatype 透传

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 4.3 | 打通 FP16/BF16/TF32 datatype 透传 | 让 decode 到 `MatrixExecPayload`、`CuteRequest`、`MatrixTensor`、memory adapter 的 datatype 字段不丢失；FP16 先可达执行边界，未实现前保持 `Unsupported` | 4.1 | cc:完成 |

### 4.4 FP16 compute 闭环

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 4.4 | 建 FP16 backend/unit compute checkpoint | 在有 oracle 后完成 `detailed backend/unit` 路径上的 FP16 compute 最小结果正确性闭环；这一步不外推成 active ISA/workload 或 RTL 主线已闭环 | 4.3 | cc:完成 |

### 4.5 gemm_precomp 主线 smoke 回归

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 4.5 | 固定 gemm_precomp 主线 smoke 回归 | 把 `gemm_precomp` 作为 `0x2b` 主口径 smoke 固定回归，不等 FP16 闭环完成 | 4.3 | cc:完成 |

### 4.6 主线最终回归

| Task | 内容 | DoD | Depends | Status |
|------|------|-----|---------|--------|
| 4.6 | 固定主线最终回归 | 在 active 主线路径上完成可达 workload、datatype 透传与 compute 闭环后，确认 `gemm_precomp` 等 `0x2b` workload 成为稳定最终回归；仅 backend/unit 级 FP16 checkpoint 不足以单独完成本项 | 4.3, 4.4, 4.5 | cc:TODO |

### Phase 4 验证命令

#### gem5 编译
```bash
scons build/RISCV/gem5.opt --linker=gold -j64 --rvv-impl=simple
```

#### gemm_precomp 主线回归
```bash
./build/RISCV/gem5.opt \
  --outdir=/tmp/gem5-se-gemm-precomp \
  configs/example/se.py \
  -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp \
  --enable-riscv-vector --no-pf
```

### Phase 4 验收标准

- FP16/BF16/TF32 的 datatype 信息可以从 decode 一路透传到 backend request / tensor / memory adapter。
- FP16 compute 的结果正确性与未实现状态明确分离，不能混用 int8/int32 路径。
- `gemm_precomp` 等 `0x2b` 主口径 workload 先作为 smoke 固定回归，再在 FP16 闭环后成为最终回归之一，且 build / run 命令固定可复现。

---

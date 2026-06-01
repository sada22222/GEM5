# Matrix Core Design

创建日期: 2026-04-23

## 文档职责

- 这份文件是 **matrix 核内设计文档**。
- 它只负责说明 GEM5 CPU/core 内部路径的设计与边界。
- 它回答的问题是：
  - matrix 指令在 core 内如何流动
  - 各模块各自负责什么
  - 哪些语义属于 decode / DynInst / IEW / LSQ / ROB / commit
  - commit 前后可见性边界在哪里
- 它**不负责**：
  - 记录某一轮具体代码改动
  - 保存验证命令与日志
  - 记录 handoff 状态
  - 详细描述 simplified CUTE/backend 内部实现

## 设计范围

- 范围内：
  - `src/arch/riscv/*`
  - `src/cpu/o3/*`
  - matrix 指令在 CPU/core 内部从 decode 到 commit 的路径
  - `MatrixAmuEntry` / matrix buffer analog 在 core 内的 owner 与 contract
- 范围外：
  - simplified CUTE/backend 的内部执行细节
  - detailed timing CUTE 设计
  - workload / probe / trace artifact

## 建议章节

### 1. 背景与目标
- 当前阶段目标
- 对齐的 RTL 事实
- 不在本阶段解决的内容

### 2. Core 内总数据流
- `decode -> rename -> issue -> execute -> LSQ -> writeback -> ROB -> commit -> matrix buffer`
- 哪些指令只在 core 内完成
- 哪些指令在 commit 后继续进入 backend

### 3. 模块职责
- `decoder / inst class`
- `DynInst / payload staging`
- `IEW / execute`
- `MlsUnit / LSQ`
- `ROB / MatrixAmuEntry`
- `Commit / CPU`

### 4. 可见性与完成边界
- commit 前不可 backend-visible
- commit 后才能进入 `toAMU proxy` / matrix buffer analog
- completion / token update 各自归属

### 5. 异常与撤销契约
- fault owner
- squash owner
- replay owner
- redirect 下哪些状态允许保留，哪些必须撤销

### 6. 当前实现与 RTL 差异
- 只记录“核内路径相关”的差异
- 不扩展到 simplified CUTE 内部差异

### 7. 未决问题
- 当前仍待确认的边界
- 需要回到 RTL/CUTE 再确认的问题

## 相关文档

---

## 附录 A：matrix regfile / AML-BML-CML / integration 合并摘要

这部分吸收了此前分散维护的 matrix regfile、AML/BML/CML、integration 和 backend glue 专题说明。

### matrix regfile 最小角色

- `AB reg`
  - 承接 A/B tile 数据
  - 主要被 `AML/BML` 写入，被 `ADC/BDC` 读取
- `C reg`
  - 承接 accumulator / store source / compute destination
  - 被 `CML`、`CDC`、`MTE` 共同相关

### AML / BML / CML 最小职责

- `AML`
  - A 路 load / zeroTr 路径
- `BML`
  - B 路 load 路径
- `CML`
  - C 路 load/store / zeroAcc 路径

### 最小 finish 边界

- `AML/BML/CML load`
  - `write_finish` 才算对后继 compute 可见
- `CML store`
  - `read_finish` 与 `write_finish` 分离
  - `release` 至少要观察 `store write_finish`

### 当前 GEM5 ↔ CUTE integration 口径

- GEM5 当前 detailed backend 是 CUTE 的阶段性近似，不是 cycle-equal RTL
- 当前主线数据流：
  - `commit -> matrix backend -> decoded fifo -> scoreboard -> micro task -> completion`
- 当前允许的阶段性近似：
  - event-driven 代替 RTL 真正的信号握手
  - 资源竞争先按预算/占用近似

### backend glue 最小契约

- CPU 可见边界仍固定在：
  - `commit -> MatrixAmuBuffer -> consumeMatrixAmuProxy() -> MatrixBackend`
- 当前外部 request / completion 契约仍固定为：
  - `CuteRequestKind::{Lsu, Mma, Arith, Release}`
  - `CuteCompletion`
- `commit` 前 matrix request 不得 backend-visible
- backend 背压当前只通过：
  - `canAccept(req)`
- completion 回到 CPU 后，当前唯一真实 side effect 仍是：
  - `Release -> token update`

- `Plan.md`
  - 计划真源
- `implementation_notes.md`
  - 本轮实际代码改动
- `verify_report.md`
  - 本轮验证与证据
- `handoff.md`
  - 当前交接状态

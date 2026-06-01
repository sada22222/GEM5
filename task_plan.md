# GEM5 Matrix 文件规划任务

## 目标
把现有 `Plans.md`、`reviews/`、`cute_detailed_design/` 中的阶段、发现、验证与交接信息整理成可恢复的工作记忆，并生成可供 Humanize/RLCR 使用的 `plan.md`。

## 当前阶段
阶段 8：新会话上下文保存

## 各阶段

### 阶段 1：恢复上下文与输入边界
- [x] 读取 `AGENTS.md`、`README.md` 与 `planning-with-files-zh` / Humanize 规则
- [x] 确认项目根目录当前没有 `task_plan.md`、`findings.md`、`progress.md`、`plan.md`
- [x] 确认输入来源限定为 `Plans.md`、`reviews/`、`cute_detailed_design/`
- **状态：** complete

### 阶段 2：抽取当前阶段与状态
- [x] 从 `Plans.md` 抽取 Phase 0-4、Phase 4A 的状态
- [x] 从 `handoff.md` 抽取当前停点
- [x] 明确当前主停点为 Phase 4：`4.1/4.3/4.4/4.5` 已完成，`4.6` 未完成，Phase 4A 为后续建模对齐计划
- **状态：** complete

### 阶段 3：抽取发现、风险与验证证据
- [x] 从 `matrix_diff_vs_rtl.md` 抽取当前未对齐项
- [x] 从 `verify_report.md` 抽取已通过验证、覆盖范围和未覆盖风险
- [x] 从 `reviews/` 抽取 review gate 与状态强化规则
- **状态：** complete

### 阶段 4：生成持久化规划文件
- [x] 创建 `task_plan.md`
- [x] 创建 `findings.md`
- [x] 创建 `progress.md`
- [x] 创建 `plan.md`
- **状态：** complete

### 阶段 5：交付与后续执行准备
- [x] 检查四个文件是否存在且内容可读
- [x] 汇报新增文件、信息来源、验证方式与剩余风险
- **状态：** complete

### 阶段 6：Humanize Gen Plan 重生成
- [x] 按 `humanize-gen-plan` 读取并校验草稿输入
- [x] 以 `findings.md`、`Plans.md`、`cute_detailed_design/`、`reviews/` 为依据重新生成 `plan.md`
- [x] 保留 `Plans.md` 为 matrix 计划真源，明确 `plan.md` 只作为 Humanize/RLCR 执行计划
- [x] 验证 `plan.md` 结构完整并更新 `progress.md`
- **状态：** complete

### 阶段 7：新会话实现交接准备
- [x] 根据最新 RTL/GEM5 证据修正 `plan.md` 中 CDC D beat、MRegFile 仲裁、MLOAD/MSTORE cache 边界
- [x] 将新会话实现入口和关键约束保存到 `findings.md`、`progress.md`
- [x] 为下一会话准备可直接复制的实现提示词
- **状态：** complete

### 阶段 8：新会话上下文保存
- [x] 记录本轮已提交的 RISC-V/O3 matrix side issue commits
- [x] 记录 O3 `mfmacc_s_h` MMA op metadata 透传待修 review
- [x] 记录新会话从该 review 待修项开始，再回到 `plan.md` MatrixReg/MTE 主线
- **状态：** complete

## 当前项目阶段摘要

| 阶段 | 当前状态 | 依据 |
|------|----------|------|
| Phase 0 | `cc:完成` | 指令范围、差距矩阵、验证基线、backend 层级已冻结 |
| Phase 1 | `cc:WIP` 为主 | ISA architectural state、decode、tile/CSR/token、checker 仍在 WIP |
| Phase 2 | 阶段性 single-thread 证据成立，但不宣称完成 | `2.2-2.5/2.8/2.9` 完成，`2.1/2.6/2.7` 仍有 TODO/WIP；不覆盖 SMT |
| Phase 3 | 多个 detailed backend 子任务完成或 WIP，仍不宣称 RTL 等价 | FIFO/scoreboard/compute/release gating 有证据；token、macquire、serialize、memory path 仍有缺口 |
| Phase 4 | `4.1/4.3/4.4/4.5` 完成，`4.6` TODO | datatype tag 透传、FP16 backend/unit checkpoint、`gemm_precomp` smoke 已成立；默认主线最终回归未收口 |
| Phase 4A | TODO | 后续主线时序/建模对齐计划：compute boundary、memory/resource/bandwidth、MLS cause、carrier |

## 关键问题
1. `4.6` 的“默认主线最终回归”需要哪些 active ISA/decode FP workload 或 `0x2b` 回归证据才足够收口？
2. Phase 4A 是否先按 `4A.1 -> 4A.3` 收紧 compute ready/overlap，还是先并行建立 `mregfile` / memory path 观察点？
3. `Plans.md` 与 `AGENTS.md` 中提到的 `Plan.md` 命名不一致，后续应统一引用当前实际存在的 `Plans.md`，避免把 Humanize 的 `plan.md` 误认为仓库计划真源。

## 已做决策
| 决策 | 理由 |
|------|------|
| `task_plan.md` 只记录 planning-with-files-zh 的阶段和状态，不替代 `Plans.md` | 避免与 repo 的 matrix 计划真源冲突 |
| `findings.md` 记录从现有文档抽取的事实、风险和 review 规则 | 发现需要长期复用，但不适合反复注入 `task_plan.md` |
| `progress.md` 记录本次初始化动作、命令和验证结果 | 方便 `/clear` 后恢复“我做了什么” |
| `plan.md` 采用 Humanize/RLCR 结构 | 后续可直接作为 RLCR 执行计划输入 |
| `plan.md` 是派生执行计划，不是 matrix 计划真源 | `Plans.md` 仍是阶段/status/DoD 真源 |
| `humanize-gen-plan` 使用 `findings.md` 作为主草稿输入 | `findings.md` 已聚合需求、事实、风险、验证和 review gate，适合作为生成计划的稳定草稿 |
| 先用 `/tmp/gem5-humanize-phase4a-plan.md` 通过 IO 校验，再更新现有 `plan.md` | Humanize 校验脚本要求输出文件不存在，而项目根目录已有用户指定的 `plan.md` |
| 下一新会话从 `plan.md` 执行，但先恢复 planning 文件上下文 | 避免 Humanize/RLCR 实现时把旧 `32/256` 口径或 cache 范围误带入代码 |
| 新会话先修 O3 MMA `payload.op -> CuteRequest.op` 透传，再回到 MatrixReg/MTE 主线 | 避免 `mfmacc_s_h` 在真实 O3/AMU backend request 中被 `makeMma()` 默认 `0x0c` 覆盖 |

## 遇到的错误
| 错误 | 尝试次数 | 解决方案 |
|------|---------|----------|
| 未发现脚本级错误 | 0 | 当前仅做文档抽取与文件创建 |
| `validate-gen-plan-io.sh --output plan.md` 会因输出已存在而不适合直接使用 | 1 | 改用 `/tmp/gem5-humanize-phase4a-plan.md` 做 IO 校验，随后按用户要求更新现有 `plan.md` |

## 备注
- 任何 task/phase 状态强化仍必须回写 `Plans.md`、`verify_report.md`、`handoff.md`，并满足 `reviews/matrix_review_standard.md`。
- 不能把 functional pass 外推成资源/时序对齐，也不能把 backend/unit FP16 checkpoint 外推成 active ISA/workload FP 主线闭环。
- 后续进入 Phase 4A 前，建议先重新读取 `task_plan.md`、`findings.md`、`progress.md`、`Plans.md` 和 `cute_detailed_design/handoff.md`。

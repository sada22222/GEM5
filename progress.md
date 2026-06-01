# GEM5 Matrix 进度日志

## 会话：2026-05-14

### 阶段 1：恢复上下文与规则
- **状态：** complete
- **开始时间：** 2026-05-14
- 执行的操作：
  - 读取 `planning-with-files-zh` 技能说明，确认需要在项目根目录创建 `task_plan.md`、`findings.md`、`progress.md`。
  - 读取 Humanize 技能说明，确认 `plan.md` 应包含 Goal、Acceptance Criteria、Path Boundaries、Milestones 等结构。
  - 读取 `/nfs/home/hujun/GEM5/AGENTS.md`，确认 matrix 文档真源、review gate 和验证汇报要求。
  - 检查项目根目录，确认本次开始前不存在目标四文件。
- 创建/修改的文件：
  - 无

### 阶段 2：读取现有输入材料
- **状态：** complete
- 执行的操作：
  - 读取 `Plans.md`，抽取 Phase 0-4、Phase 4A 的 task/status/DoD。
  - 读取 `cute_detailed_design/handoff.md`，抽取当前停点与后续建议。
  - 读取 `cute_detailed_design/matrix_diff_vs_rtl.md`，抽取已对齐、部分对齐、未对齐和证据不足项。
  - 读取 `cute_detailed_design/implementation_notes.md`，抽取 Phase 4 改动边界。
  - 读取 `cute_detailed_design/verify_report.md`，抽取验证命令、结果、review 结论与覆盖限制。
  - 读取 `reviews/matrix_review_standard.md` 与 prompt templates，抽取状态强化和 review 格式要求。
- 创建/修改的文件：
  - 无

### 阶段 3：整理信息分流
- **状态：** complete
- 执行的操作：
  - 将阶段/status/下一步放入 `task_plan.md`。
  - 将长期事实、风险、未对齐项、review 规则放入 `findings.md`。
  - 将本次读取、抽取、文件创建过程放入 `progress.md`。
  - 将后续可执行计划按 Humanize/RLCR 结构放入 `plan.md`。
- 创建/修改的文件：
  - `task_plan.md`
  - `findings.md`
  - `progress.md`
  - `plan.md`

### 阶段 4：交付前检查
- **状态：** complete
- 执行的操作：
  - 执行 `ls -l task_plan.md findings.md progress.md plan.md`，确认四个文件均已创建。
  - 执行 `sed -n` 抽样读取四个文件开头，确认结构与用途匹配。
  - 执行 `git status --short task_plan.md findings.md progress.md plan.md`，确认本次新增文件为四个未跟踪规划文件。
- 创建/修改的文件：
  - `task_plan.md`
  - `findings.md`
  - `progress.md`
  - `plan.md`

## 测试结果
| 测试 | 输入 | 预期结果 | 实际结果 | 状态 |
|------|------|----------|----------|------|
| 目标文件存在性检查 | `ls -l task_plan.md findings.md progress.md plan.md` | 四个文件均存在 | 四个文件均存在 | PASS |
| 目标文件内容抽样 | `sed -n '1,80p' ...` | 四个文件开头内容可读，结构符合用途 | 抽样内容可读，结构符合用途 | PASS |
| 本次新增文件检查 | `git status --short task_plan.md findings.md progress.md plan.md` | 仅显示四个目标文件为新增/未跟踪 | 四个文件均为 `??` | PASS |

## 错误日志
| 时间戳 | 错误 | 尝试次数 | 解决方案 |
|--------|------|----------|----------|
| 2026-05-14 | 未发生脚本级错误 | 0 | 不适用 |

## 当前 worktree 观察
- `git status --short` 显示本次开始前已有大量未提交/迁移状态，包括：
  - `Plans.md` 已修改
  - 多个根目录旧文档显示删除，`cute_detailed_design/` 下对应文档显示新增
  - `src/arch/riscv/*`、`src/cpu/o3/*` 等已有源码修改
  - `reviews/` 为未跟踪目录
- 本次只新增四个规划文件，不回滚、不整理、不重写既有改动。

## 五问重启检查
| 问题 | 答案 |
|------|------|
| 我在哪里？ | planning-with-files 初始化与交付检查均已完成 |
| 我要去哪里？ | 用四个文件支撑后续 Phase 4A / 4.6 的 RLCR 执行 |
| 目标是什么？ | 把现有 matrix 计划、发现、验证、review 规则转成可恢复的文件工作记忆和 Humanize 执行计划 |
| 我学到了什么？ | 见 `findings.md` |
| 我做了什么？ | 创建并填充 `task_plan.md`、`findings.md`、`progress.md`、`plan.md` |

---
*每个阶段完成后或遇到错误时更新此文件*

## 会话：2026-05-14 Humanize Gen Plan

### 阶段 1：草稿校验与上下文恢复
- **状态：** complete
- 执行的操作：
  - 读取 `task_plan.md`、`progress.md`、`findings.md`，恢复 planning-with-files 上下文。
  - 读取 `humanize-gen-plan` 技能说明，确认输出结构要求。
  - 读取现有 `plan.md`，确认已有草案需要精炼而不是另起无关计划。
  - 执行 `/nfs/home/hujun/.codex/skills/humanize/scripts/validate-gen-plan-io.sh --input findings.md --output /tmp/gem5-humanize-phase4a-plan.md`。
  - 执行 `python3 /nfs/home/hujun/.agents/skills/planning-with-files-zh/scripts/session-catchup.py /nfs/home/hujun/GEM5`，未输出未同步上下文。
- 创建/修改的文件：
  - 无

### 阶段 2：计划重生成
- **状态：** complete
- 执行的操作：
  - 以 `findings.md` 为主草稿，结合 `Plans.md`、`handoff.md`、`matrix_diff_vs_rtl.md`、`verify_report.md` 和 `reviews/matrix_review_standard.md` 重新生成 `plan.md`。
  - 新 `plan.md` 补强了：
    - AC-1 到 AC-9
    - Path Boundaries
    - Feasibility Hints
    - Dependencies and Sequence / Milestones
    - Implementation Notes
  - 明确 `plan.md` 只作为 Humanize/RLCR 执行计划，`Plans.md` 仍是 matrix task/status 真源。
- 创建/修改的文件：
  - `plan.md`
  - `task_plan.md`
  - `progress.md`

### 阶段 3：结构验证
- **状态：** complete
- 执行的操作：
  - 执行 `rg` 检查 `plan.md`，确认包含 Humanize 要求的核心章节：Goal、Acceptance Criteria、Path Boundaries、Feasibility Hints、Dependencies and Sequence / Milestones、Implementation Notes。
  - 执行 `rg` 检查 `task_plan.md` 和 `progress.md`，确认本轮 Humanize Gen Plan 重生成已记录。
  - 执行 `git status --short task_plan.md findings.md progress.md plan.md`，确认本轮仍只涉及四个 planning 文件。
- 创建/修改的文件：
  - `plan.md`
  - `task_plan.md`
  - `progress.md`

## Humanize Gen Plan 测试结果
| 测试 | 输入 | 预期结果 | 实际结果 | 状态 |
|------|------|----------|----------|------|
| Humanize IO 校验 | `validate-gen-plan-io.sh --input findings.md --output /tmp/gem5-humanize-phase4a-plan.md` | 校验通过 | `VALIDATION_SUCCESS` | PASS |
| session catch-up | `session-catchup.py /nfs/home/hujun/GEM5` | 无未同步上下文或输出可处理 | 无输出，未发现需要额外同步项 | PASS |
| `plan.md` 结构检查 | `rg` 核心章节 | 包含 Goal、AC、Path Boundaries、Feasibility、Milestones、Implementation Notes | 已检出核心章节与 AC-1 到 AC-9 | PASS |
| planning 文件同步检查 | `rg` task/progress 记录 | `task_plan.md` 和 `progress.md` 记录本轮重生成 | 已检出阶段 6 与 Humanize Gen Plan 会话记录 | PASS |
| 本轮文件范围检查 | `git status --short task_plan.md findings.md progress.md plan.md` | 仅四个 planning 文件显示变化 | 四个文件显示为未跟踪 planning 文件 | PASS |

## Humanize Gen Plan 错误日志
| 时间戳 | 错误 | 尝试次数 | 解决方案 |
|--------|------|----------|----------|
| 2026-05-14 | 输出目标 `plan.md` 已存在，不适合直接作为 `validate-gen-plan-io.sh` 的 output 参数 | 1 | 使用 `/tmp/gem5-humanize-phase4a-plan.md` 做 IO 校验，再按用户要求更新现有 `plan.md` |
| 2026-05-14 | 一条最终 `rg` 验证命令因为 shell 反引号转义错误报 `unexpected EOF while looking for matching \\`\\`` | 1 | 改用单引号包裹的简单 pattern 复跑，验证通过 |

## 会话：2026-05-16 新会话实现交接

### 阶段 1：恢复 planning-with-files 上下文
- **状态：** complete
- 执行的操作：
  - 读取 `task_plan.md`、`findings.md`、`progress.md`。
  - 执行 `python3 /nfs/home/hujun/.agents/skills/planning-with-files-zh/scripts/session-catchup.py /nfs/home/hujun/GEM5`，未输出需要同步的上下文。
  - 读取当前 `plan.md` 并确认用户要求：下一新会话开始实现；保存上下文；提供可复制提示词。

### 阶段 2：保存新会话实现上下文
- **状态：** complete
- 执行的操作：
  - 更新 `task_plan.md`，新增阶段 7：新会话实现交接准备。
  - 更新 `findings.md`，保存最新关键技术决策：`512` compute beats、CDC `512` D beat timing、MRegFile A/B 与 C 仲裁差异、MLOAD/MSTORE 不做 cache/L2。
  - 准备新会话实现提示词，要求先恢复 planning 文件，再按 `plan.md` Milestone 1/2 走 TDD。
- 创建/修改的文件：
  - `task_plan.md`
  - `findings.md`
  - `progress.md`

### 新会话提示词摘要
下一会话应使用 `planning-with-files-zh` 与 `humanize-rlcr`，先读 planning 文件和 `plan.md`，再从 Milestone 1/2 开始实现。首要目标是修正 `src/matrix/matrix_ex.cc` 和 `src/matrix/detailed_cute_backend.test.cc` 中任何残留的 `32` unique A/B timing helper 口径，恢复 full int8 `512` accepted compute beats；随后推进 MRegFile bank/resource helper 与 CDC `512` D beat timing。禁止本轮实现 cache/L2。

## 新会话交接验证
| 测试 | 输入 | 预期结果 | 实际结果 | 状态 |
|------|------|----------|----------|------|
| planning 文件恢复 | `sed -n` 读取三文件 | 能恢复当前目标、发现和过程 | 已恢复 | PASS |
| session catch-up | `session-catchup.py /nfs/home/hujun/GEM5` | 无未同步上下文或输出可处理 | 无输出 | PASS |
| 新约束保存 | 更新 `task_plan.md`、`findings.md`、`progress.md` | 新会话可从文件恢复关键口径 | 已写入 | PASS |

## 会话：2026-05-16 实现 Milestone 1/2

### 阶段 1：恢复上下文与核对代码事实
- **状态：** complete
- 执行的操作：
  - 读取 `task_plan.md`、`findings.md`、`progress.md`、`plan.md`、`AGENTS.md`。
  - 执行 `session-catchup.py /nfs/home/hujun/GEM5`，无未同步输出。
  - 读取 `plan.md` Context Files 中的 GEM5/RTL 关键文件，确认 RTL A/B/C controller 的 `Max_Caculate_Iter` 为 `M * N * K`，full int8 为 `16 * 16 * 2 = 512`。
  - 确认当前 dirty diff 中 `src/matrix/matrix_ex.cc` 和 `src/matrix/detailed_cute_backend.test.cc` 存在残留 `32` unique-window timing 口径。

### 阶段 2：TDD 修正 MTE accepted beats
- **状态：** complete
- 执行的操作：
  - 先将 targeted tests 恢复为 full int8 `512`、e16/e32/e4 `1024/2048/256`、partial-M `4x128x32 = 16`。
  - 运行 targeted tests 得到预期红灯：旧实现返回 `32/64/128/16`。
  - 修正 `computeMteTiming()` 为 `ceil(mtilem / 8) * (mtilen / 8) * k_groups`，撤销 `max(m_iters*k, n_iters*k)` 口径。
  - 更新 `implementation_notes.md`、`verify_report.md`、`handoff.md` 中当前结论段落，明确 unique A/B block `32` 不能作为 full MMA latency。
- 创建/修改的文件：
  - `src/matrix/matrix_ex.cc`
  - `src/matrix/detailed_cute_backend.test.cc`
  - `cute_detailed_design/implementation_notes.md`
  - `cute_detailed_design/verify_report.md`
  - `cute_detailed_design/handoff.md`

### Milestone 1/2 验证
| 测试 | 输入 | 预期结果 | 实际结果 | 状态 |
|------|------|----------|----------|------|
| TDD 红灯 | targeted gtest before implementation fix | `512` 预期失败，旧实现暴露 `32` | 2 tests failed as expected | PASS |
| unit-test build | `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8` | 编译通过 | PASS，仅环境 warning | PASS |
| targeted 绿灯 | `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.MteTimingReportsPerCycleBandwidthAndAcceptedBeats:DetailedCuteBackend.MteTimingFollowsActiveRtlControllerShape:DetailedCuteBackend.ComputeLatencyFollowsActiveRtlKGroups:DetailedCuteBackend.ComputeLatencyScalesWithDatatypeWidth'` | 4 tests pass | 4 tests passed | PASS |

### 剩余风险
- MatrixReg bank resource helper、ADC/BDC/CDC 并行 read beat runtime、FReduce tail 之后的 CDC `512` D beat writeback boundary 尚未实现。
- Humanize RLCR setup 当前仍受 worktree dirty 状态约束，需要在本轮可提交状态后启动或继续 hook 管理。

### 阶段 3：MatrixReg bank resource helper
- **状态：** complete
- 执行的操作：
  - 先新增 `MatrixRegResource.*` targeted tests，构建因缺少 `matrix/matrix_reg_resource.hh` 失败，形成 TDD 红灯。
  - 新增 `src/matrix/matrix_reg_resource.hh` / `.cc`，表达 8-bank、32B entry、full-bank vector grant、1-cycle read response。
  - 建模 A/B loader write priority：同拍 loader write 会 stall ADC/BDC DataController read。
  - 建模 C MatrixReg odd/even SRAM read/write conflict：同 parity read/write 同拍冲突会 stall，不静默并行；不同 parity 可同拍 grant。
  - 更新 `src/matrix/SConscript`，让 helper 参与 library 与 gtest build。
- 创建/修改的文件：
  - `src/matrix/matrix_reg_resource.hh`
  - `src/matrix/matrix_reg_resource.cc`
  - `src/matrix/SConscript`
  - `src/matrix/detailed_cute_backend.test.cc`

### Milestone 3 验证
| 测试 | 输入 | 预期结果 | 实际结果 | 状态 |
|------|------|----------|----------|------|
| TDD 红灯 | `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8` before helper | 缺少 helper header 导致失败 | `fatal error: matrix/matrix_reg_resource.hh` | PASS |
| helper build | `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8` | 编译通过 | PASS，仅环境 warning | PASS |
| MatrixRegResource targeted | `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='MatrixRegResource.*'` | 5 tests pass | 5 tests passed | PASS |

## 会话：2026-05-18 RISC-V matrix CSR rename 范围收窄

### 阶段 1：删除非 tile CSR renamed carrier
- **状态：** complete
- 执行的操作：
  - 复核当前源码和 Humanize Round 5/6/7 记录，确认本轮只保留 `mtilem/mtilen/mtilek` renamed carrier。
  - 从 `RMiscRegClass` 和 `MatrixRenamed*` 定义中删除 `xmxrm/xmfrm/xmsaten` carrier。
  - 删除 `XMCSR` composite carrier 模板和 decode 路由，让 `XMCSR/XMXRM/XMFRM/XMSATEN` 回到普通 CSR 路径。
  - 将 `mfmacc_s_h` / `mmacc_w_b` 的 matrix renamed source 收窄为 `mtilem/mtilen/mtilek`，`rm/frm/sat` 改为执行时读取当前 CSR sideband。

### 阶段 2：RISC-V matrix decode metadata 与 token 顺序复核
- **状态：** complete
- 执行的操作：
  - 修正 `mfmacc_s_h` payload metadata：`payload.op = 0x04`，不再复用 int MMA 的 `0x0c`。
  - O3 `DynInst` metadata 现在识别 `funct7=0x04` 和 `0x0c` 为 MMA，并只记录 tile state reads。
  - 复核 Round 6 `msyncregreset/mrelease`：当前 `commitMatrixResetToken()` 记录 per-token reset seq，晚到的旧 release completion 在 `commitMatrixReleaseToken()` 中被忽略，仍需要保留。
- 创建/修改的文件：
  - `src/arch/riscv/regs/renameable_misc.hh`
  - `src/arch/riscv/regs/matrix.hh`
  - `src/arch/riscv/isa/formats/matrix_conf.isa`
  - `src/arch/riscv/isa/decoder.isa`
  - `src/cpu/o3/dyn_inst.hh`
  - `src/cpu/o3/dyn_inst.cc`
  - `src/arch/riscv/matrix_sync_check.cc`

### 本轮验证
| 测试 | 输入 | 预期结果 | 实际结果 | 状态 |
|------|------|----------|----------|------|
| RISC-V build | `scons -Q build/RISCV/gem5.opt -j8` | 编译链接通过 | PASS，仅环境 warning | PASS |
| detailed backend build | `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j8` | 编译通过 | PASS，仅环境 warning | PASS |
| detailed backend tests | `./build/RISCV/matrix/detailed_cute_backend.test.opt` | 全部 gtest 通过 | 47 tests passed | PASS |
| style/diff check | `git diff --check` + `util/style.py -m --checker SortedIncludes --checker Whitespace --checker LineLength ...` | 无 whitespace/style 报错 | 无输出 | PASS |
| residual carrier grep | `rg MatrixComposite/MatrixRenamedX/...` | 无源码残留 | 无匹配 | PASS |
| matrix_sync_check build | `scons -Q build/RISCV/arch/riscv/matrix_sync_check.opt -j8` | 至少编译 checker 源文件 | 源文件编译通过，但 standalone link 仍因既有 gem5 符号缺失失败 | PARTIAL |

### 剩余风险
- `cute_detailed_design/matrix_o3_README.md` 仍有旧口径文字：`mmacc_w_b` 读取 `M/N/K + xmxrm/xmfrm/xmsaten`。源码已改为只 renamed-read `M/N/K`，该文档后续应单独清理。
- `matrix_sync_check.opt` standalone 链接问题仍是既有目标链接范围问题；本轮使用 `gem5.opt` 构建和 gtest 作为主要验证闭环。

## 会话：2026-05-18 AGENTS.md SE gemm_precomp unblock

### 阶段 1：复现与定位
- **状态：** complete
- 执行的操作：
  - 按 AGENTS.md 命令运行 SE `gemm_precomp`，复现 `Illegal instruction 0x24a48a2b ... mlae16 parameter check failed`。
  - 对照 QEMU/toolchain `insn32.decode`，确认 `0x24a48a2b` 是 `mlce32 acc0,(s1),a0`，其中 raw `RD=20` 拆成 `width=2, md=4`。
  - 确认问题根因不是 matrix reg index 越界，而是 GEM5 仍按旧 `funct7-only` load/store 编码口径解码。

### 阶段 2：修复 RISC-V matrix load/store 编码口径
- **状态：** complete
- 执行的操作：
  - 在 `matrix_sync_check.cc` 增加 workload 实际机器码 `0x24a48a2b` 的回归覆盖，并把旧测试编码更新到 `funct7 + RD[4:3]/RD[2:0]` 口径。
  - 修正 `decoder.isa`：
    - A/B/C load/store 类由 `funct7=0x02/0x0a/0x12/0x13` 区分。
    - width 来自 `RD[4:3]`。
    - matrix reg index 来自 `RD[2:0]`。
  - 修正 `DynInst::initMatrixInstInfo()` 中 LSU route/state reads，去掉旧 `0x1a/0x22` load/store 口径。
  - 修正 `MlsUnit` 的 early fault、access size、shape 派生和 payload build，避免把 `0x12,width=2` 当成 A e16 load。
- 创建/修改的文件：
  - `src/arch/riscv/isa/decoder.isa`
  - `src/arch/riscv/matrix_sync_check.cc`
  - `src/cpu/o3/dyn_inst.cc`
  - `src/cpu/o3/mls_unit.cc`

### 本轮验证
| 测试 | 输入 | 预期结果 | 实际结果 | 状态 |
|------|------|----------|----------|------|
| RISC-V full build | `scons -Q build/RISCV/gem5.opt -j8` | 编译链接通过 | PASS，仅环境 warning | PASS |
| SE gemm_precomp | `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-precomp configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf` | 不再在 `0x24a48a2b` illegal instruction 崩溃；8 cases PASS | `All 8 precomp tests PASSED.` | PASS |
| standalone matrix_sync_check | `scons -Q build/RISCV/arch/riscv/matrix_sync_check.opt -j8` | 至少覆盖新增源码测试 | 目标仍因既有 standalone link 缺 gem5 基础符号失败 | PARTIAL |
| whitespace check | `git diff --check -- ...` | 无 whitespace 报错 | 无输出 | PASS |

### 剩余风险
- `matrix_sync_check.opt` 仍不能作为独立绿灯，原因是目标链接缺 `Serializable/Logger/SimObject` 等 gem5 基础符号；本轮主验证用 `gem5.opt` full build 和 SE workload。
- 本轮只收 RISC-V/O3 matrix load/store 编码与 SE functional unblock，不收 MatrixReg/MTE bank timing 主线。

## 会话：2026-05-18 新会话前上下文保存

### 阶段 1：提交结果落盘
- **状态：** complete
- 执行的操作：
  - 记录已创建两个提交：
    - `9b25fa3473 arch-riscv,cpu-o3: Limit matrix renamed carriers to tile CSRs`
    - `d7c00d5a30 arch-riscv,cpu-o3: Decode matrix loads with width fields`
  - 记录提交后验证：
    - `scons -Q build/RISCV/gem5.opt -j8` PASS。
    - AGENTS.md SE `gemm_precomp` PASS，输出 `All 8 precomp tests PASSED.`
    - `git diff HEAD~2..HEAD --check` PASS。
    - `git diff --check` PASS。

### 阶段 2：review 待修项落盘
- **状态：** complete
- 执行的操作：
  - 核对并记录 O3 `ROB -> CuteRequest` MMA op metadata 透传缺口：
    - `toCuteRequest()` MMA 分支使用 `CuteRequest::makeMma()`。
    - `makeMma()` 默认 `req.op = 0x0c`。
    - `mfmacc_s_h` 的 `payload.op = 0x04` 未复制到 `CuteRequest.op`。
  - 记录新会话建议顺序：先修 `out.op = payload.op` side issue，再回到 `plan.md` MatrixReg/MTE bank timing 主线。
- 创建/修改的文件：
  - `task_plan.md`
  - `findings.md`
  - `progress.md`

### 当前停点
- worktree 仍有 planning 文件改动用于新会话恢复上下文。
- 代码提交已完成；新会话开始时先读取 planning 文件，再处理 O3 MMA op 透传 review。

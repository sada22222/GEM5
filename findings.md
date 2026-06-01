# GEM5 Matrix 发现与决策

## 需求
- 用户已有 `Plans.md`、`reviews/`、`cute_detailed_design/`，希望把合适内容整理进：
  - `task_plan.md`：planning-with-files-zh 使用，记录阶段和状态
  - `findings.md`：planning-with-files-zh 使用，记录发现
  - `progress.md`：planning-with-files-zh 使用，记录过程、错误、验证
  - `plan.md`：Humanize 使用，作为 RLCR 执行计划
- 本次目标是整理现有上下文，不修改 matrix 代码、不强化已有 task 状态。

## 研究发现

### 文档真源与边界
- `Plans.md` 是当前实际存在的 matrix 计划真源，负责阶段划分、task / DoD / depends / status、推进边界与当前停点。
- `cute_detailed_design/matrix_core_design.md` 负责核内路径、regfile、`AML/BML/CML`、backend glue 最小契约。
- `cute_detailed_design/matrix_diff_vs_rtl.md` 是当前未对齐项清单真源。
- `cute_detailed_design/implementation_notes.md` 记录本轮实际改动。
- `cute_detailed_design/verify_report.md` 记录验证与证据。
- `cute_detailed_design/handoff.md` 记录当前停点与交接。
- `plan.md` 是本次新建的 Humanize/RLCR 执行计划，不能替代 `Plans.md`。

### 当前阶段状态
- Phase 0 已完成：目标、范围、验证基线和 detailed backend 层级已冻结。
- Phase 1 仍以 WIP 为主：architectural state、decode、tile/CSR/token、checker 仍未完全收口。
- Phase 2 只在 `single-thread` 前提下有阶段性证据；不能写成 `MLS` 已对齐 RTL，也不能写成 `AMUCtrlBuffer` 结构已对齐 RTL。
- Phase 3 detailed backend 已形成 event-driven 主链：`commit-visible request -> FIFO -> scoreboard -> micro task -> completion`，但多处仍是部分对齐或阶段性近似。
- Phase 4 当前状态是 `4.1/4.3/4.4/4.5` 已完成，`4.6` 仍为 TODO。
- Phase 4A 是下一步主线时序/建模对齐计划，当前所有 task 仍是 TODO。

### Phase 4 已成立的证据
- `4.1`：主线 workload 与 datatype 边界已冻结，以 `0x2b / gemm_precomp` 为当前主口径 workload。
- `4.3`：FP16/BF16/TF32 datatype tag 已能从 CPU/request/tensor/memory adapter 透传。
- `4.4`：FP16 MMA 在 detailed backend/unit 路径上已用 CUTE `cute-fpe` 软件 oracle 形成最小结果正确性 checkpoint。
- `4.5`：`gemm_precomp` 已作为 `0x2b` 主口径 workload 跑通 SE smoke 回归，输出 `All 8 precomp tests PASSED.`
- 已通过的核心验证包括：
  - `scons -Q build/RISCV/matrix/detailed_cute_backend.test.opt --unit-test -j4`
  - `scons build/RISCV/gem5.opt --linker=gold -j64 --rvv-impl=simple`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt`
  - `./build/RISCV/matrix/detailed_cute_backend.test.opt --gtest_filter='DetailedCuteBackend.UnsupportedFpMmaReturnsUnsupportedCompletion:DetailedCuteBackend.Fp16MmaSucceedsWithInt32Accumulator:DetailedCuteBackend.Fp16LsuKeepsElemTypeAndRawBits'`
  - `./build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-precomp configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf`

### 当前重点未对齐项
- `TaskController` 发射门控：GEM5 已有 `head -> scoreboard -> ready -> issue` 主链，但 `ready` 仍主要由 slot 占用和软件谓词近似；RTL/CUTE 需要更明确的 unit ready / deq fire / issue contract。
- `ADC/BDC/CDC/MTE` compute 路径：GEM5 已有显式子单元状态，但 `computePathReady()` 当前只看 `ADC`，而 RTL `TaskController` 要求 `ADC/BDC/CDC` 同时 ready。
- `ComputeGo / EndReady`：GEM5 方向对，但仍是软件事件近似，不是 RTL 级 `MicroTaskEndValid/Ready`。
- `AML/BML/CML + LocalMMU`：GEM5 有三路逻辑路径和 task slot，但缺 `sourceId`、fill-table、response FIFO、cache/backpressure 模型。
- `mregfile` 仲裁/带宽：当前没有 `AML/BML/CML/ADC/BDC/CDC` 级别的每拍读写宽度、bank 并行度和仲裁模型。
- MLS replay/cancel/fault cause：GEM5 仍主要是 `tlbMiss` 粒度，缺 RTL `cause/carry/safe-writeback` 粒度。
- `MatrixAmuBuffer` / carrier：当前是 single-thread shadow carrier，不支撑 CommitWidth/SMT/global-oldest timing 结论。
- datatype：tag 透传和 FP16 backend/unit checkpoint 已成立，但 active ISA/decode FP workload、BF16/TF32 compute 仍未闭环。

### Review 与状态强化规则
- 所有 matrix review 必须遵循 `reviews/matrix_review_standard.md`。
- 缺少证据就是 blocking issue。
- 任一 Required Gate 为 `FAIL` 时，Verdict 必须是 `REQUEST_CHANGES` 或 `REJECT`。
- 没有有效 review verdict，不得强化 task/phase status。
- 不允许声称“RTL aligned”，除非 `Plans.md`、`matrix_diff_vs_rtl.md`、`verify_report.md` 三者都支持。
- review 必须分别报告 RTL Alignment、Design Complexity、Code Style。
- task-level style review 不等于 phase-level RTL/CUTE alignment approval。

## 技术决策
| 决策 | 理由 |
|------|------|
| 后续优先把 Phase 4A 作为 RLCR 执行对象 | `4.6` 和 Phase 4A 是当前最明确的后续入口 |
| `plan.md` 的验收标准按 AC 拆分功能、资源/时序、证据与文档回写 | Humanize/RLCR 需要可检查的接受条件，且 repo 明确要求区分功能正确、资源约束对齐、时序/性能近似 |
| 保守采用 event-driven 逐层细化路径 | 现有文档推荐先 event-driven，再按 trace mismatch 和 stats 需求逐层细化，风险低且可验证 |
| 不把 `gemm_precomp` functional pass 当成 timing/resource 对齐证据 | `matrix_diff_vs_rtl.md` 明确禁止这种外推 |
| full int8 MTE accepted compute beats 按 `16 * 16 * 2 = 512` 建模 | RTL A/B/C controllers 按 `M_IteratorMax * N_IteratorMax * K_IteratorMax` 推进；unique A/B block `32` 不能当 latency |
| full int8 CDC D writeback timing 也按当前 RTL 的 `512` 个 D beat 建模 | `CDataController` 的 `DVectorCount` 结束条件跟随 `Max_Caculate_Iter`；`256` 只表示 C/D 地址 tile 数 |
| MRegFile bank/resource helper 要区分 A/B 与 C 的仲裁语义 | `ABMatrixReg` 是 loader write 优先导致 read stall；`CMatrixReg` 的同 odd/even SRAM read/write 冲突应 stall/defer，避免 RTL assert |
| MLOAD/MSTORE 本轮只做带宽/固定延时窗口，不实现 cache/L2 行为 | 用户明确后续要进行 L2 cache 建模；本轮只为 L2 保留接口，不建 hit/miss、MSHR、coherence |
| RISC-V matrix load/store 解码按 QEMU/toolchain 新字段口径处理 | `funct7` 区分 A/B/C/load-store 类，`RD[4:3]` 是 width，`RD[2:0]` 是 matrix reg index；`0x24a48a2b` 是 `mlce32 acc0,(s1),a0`，不是旧 `mlae16` |

## 2026-05-18 SE `gemm_precomp` 解码问题发现
- AGENTS.md 中 SE `gemm_precomp` 原始失败为：
  - `panic: Illegal instruction 0x24a48a2b ... mlae16 parameter check failed`
- 本地对照 QEMU/toolchain `insn32.decode` 后确认：
  - `%md = bits[9:7]`
  - width = bits[11:10]
  - raw `RD=20` 表示 `width=2, md=4`，不是 matrix reg `20`
  - `funct7=0x12, width=2, md=4` 对应 `mlce32`
- 修复方向：
  - 不能通过放宽 raw `RD` 合法性修复。
  - 应让 GEM5 decode/O3 MLS 按 `funct7 + RD width/md` 解释 matrix load/store。
- 验证：
  - `scons -Q build/RISCV/gem5.opt -j8` PASS
  - AGENTS.md SE `gemm_precomp` PASS，输出 `All 8 precomp tests PASSED.`
- 边界：
  - 这是 RISC-V/O3 matrix ISA decode/MLS functional unblock，不代表 MatrixReg bank timing 或 MTE resource/timing 收口。

## 2026-05-18 新会话前提交与待修 review
- 已提交 side issue：
  - `9b25fa3473 arch-riscv,cpu-o3: Limit matrix renamed carriers to tile CSRs`
  - `d7c00d5a30 arch-riscv,cpu-o3: Decode matrix loads with width fields`
- 提交后 fresh 验证：
  - `scons -Q build/RISCV/gem5.opt -j8` PASS，仅环境 warning。
  - `build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-precomp configs/example/se.py -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp --enable-riscv-vector --no-pf` PASS，输出 `All 8 precomp tests PASSED.`
  - `git diff HEAD~2..HEAD --check` PASS。
  - `git diff --check` PASS。
- 已确认但未修的 review 项：
  - `src/cpu/o3/rob.cc` 的 `toCuteRequest()` 在 MMA 分支通过 `CuteRequest::makeMma()` 构造 request。
  - `CuteRequest::makeMma()` 默认 `req.op = 0x0c`。
  - `decoder.isa` 已将 `mfmacc_s_h` 的 `payload.op` 改为 `0x04`，但 O3 `ROB -> CuteRequest` 路径没有把 `payload.op` 复制到 `out.op`。
  - 因此真实 O3/AMU backend request 的 `mfmacc_s_h` opcode metadata 仍可能被记录成 `0x0c`。这更准确地说是 O3 backend request opcode metadata 丢失；当前 detailed backend 数值路径主要看 `isFp/types/elemType`，不一定已受功能影响。
- 建议新会话第一步：
  - 先用 focused check 或 TDD 修 `toCuteRequest()` 的 MMA 分支，令 `out.op = payload.op`。
  - 再回到 `plan.md` MatrixReg/MTE 主线。

## 下一新会话实现入口（2026-05-16）
- 先读取：`task_plan.md`、`findings.md`、`progress.md`、`plan.md`、`GEM5/AGENTS.md`。
- 再读取 `plan.md` 的 Context Files 中列出的 GEM5/RTL 文件，尤其是 `CDataController.scala`、`ABMatrixReg.scala`、`CMatrixReg.scala`、`MatrixTE.scala`、`FReducePE.scala`、`src/matrix/matrix_ex.cc`、`src/matrix/backend_runtime.cc`、`src/matrix/detailed_cute_backend.test.cc`。
- 首个实现阶段建议从 `plan.md` Milestone 1/2 开始：先把中间错误的 `32` timing helper 和测试期望修回 `512` tile-pair beat 口径。
- 之后推进 MRegFile helper：8 banks、32B entry、1-cycle read response、A/B write-priority stall、C read/write conflict stall。
- 不要在本轮实现 cache/L2 hit/miss、MSHR、coherence、retry/replay 或 cache line replacement。

## 遇到的问题
| 问题 | 解决方案 |
|------|----------|
| `AGENTS.md` 提到 `Plan.md`，但项目根目录当前实际文件是 `Plans.md` | 本次统一以用户指定和实际存在的 `Plans.md` 为 matrix 计划真源；新建 `plan.md` 明确标记为 Humanize 派生执行计划 |
| worktree 已有大量未提交/迁移状态 | 本次只新增四个规划文件，不修改现有源码或既有文档 |
| Phase 2/3/4 中存在“功能已通过但对齐未完成”的混合状态 | 在发现和计划中显式分开 functional、resource/timing、RTL/CUTE alignment |

## 资源
- `Plans.md`
- `AGENTS.md`
- `README.md`
- `reviews/matrix_review_standard.md`
- `reviews/matrix_task_style_review_prompt_template.md`
- `reviews/matrix_review_prompt_template.md`
- `cute_detailed_design/handoff.md`
- `cute_detailed_design/implementation_notes.md`
- `cute_detailed_design/matrix_core_design.md`
- `cute_detailed_design/matrix_diff_vs_rtl.md`
- `cute_detailed_design/verify_report.md`

## 视觉/浏览器发现
- 本次未使用浏览器、截图或多模态输入。

---
*每执行2次查看/浏览器/搜索操作后更新此文件*
*防止视觉信息丢失*

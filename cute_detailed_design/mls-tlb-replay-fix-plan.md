# MLS TLB Replay Fix Plan / MLS TLB Replay 修复计划

## Goal Description / 目标描述

本计划针对 `/nfs/home/hujun/GEM5` 中 `src/cpu/o3/mls_unit.cc` 的 MatrixMemOp TLB/replay 逻辑进行修复，目标是消除 `kmhv3.py` FS/raw-cpt baremetal 路径下 `mlce32` 因错误进入 MLS replay 而永久卡死的问题，并让 MLS 的 translation/replay 语义尽量向标量 LSU/TLB/PTW 的事件驱动模型靠拢。

当前已确认的问题是：

- `MlsUnit::issue()` 先用 `probeTlbState()` 做同步 `dtb->lookup(...)` 预探测。
- 只要预探测 miss，就强制 `needReplay = true` 并创建 `MlsReplayQueue` entry。
- replay 恢复依赖 `refreshReady() -> replayReady() -> dtb->lookup(...)` 轮询。
- 在 FS/raw-cpt baremetal direct/physical path 下，`runStage1()` 内部 `translateAtomic()` 可以成功得到 `paddr`，但 DTLB cache entry 不一定出现，于是 replay entry 永远 `ready=0`，导致 `mlce32` 卡死在 ROB head。

本计划的目标不是重写 matrix backend，也不是复制整套 scalar dcache/LQ/SQ 实现，而是：

1. 明确 MLS 该从标量语义中复用哪一部分 TLB/PTW/replay 机制。
2. 分阶段修正 MLS 的 replay 进入条件与恢复条件。
3. 保持 matrix payload/handoff/backend-visible 边界不被无关改动破坏。

## Acceptance Criteria / 验收标准

- AC-1: 现有 bare/direct path 不再因错误 TLB replay 进入条件卡死。
  - Positive Tests (expected to PASS) / 正向测试（预期通过）:
    - `kmhv3.py` FS/raw-cpt baremetal 的首条 `mlce32` 不再出现 “首次翻译成功但 replay entry 永远 ready=0”。
    - 调试窗口内 `sn:1697357` 至少能继续走到非 replay 完成路径，或进入新的事件驱动 translation 恢复路径，而不是只停留在一次 `MlsReplayQueue alloc ready=0`。
    - `IEW`/`Commit` trace 中不再持续报 `can't commit inst [sn:1697357] mlce32` 直至 stuck panic。
  - Negative Tests (expected to FAIL) / 反向测试（预期失败或被拒绝）:
    - 仍然保留 “`translateAtomic()` 成功但仅因 pre-lookup miss 强制 replay” 的行为。
    - 通过关闭 stuck check、跳过 commit、或绕过 matrix mem 执行来“掩盖”问题。

- AC-2: MLS replay 进入条件必须从“外部 lookup 预探测”转向“翻译子系统的真实状态”。
  - Positive Tests (expected to PASS) / 正向测试（预期通过）:
    - bare/direct path 下，若 request 已确定走 `Request::PHYSICAL` 或等价 direct path，则不会仅因 `probeTlbState()` miss 进入 replay。
    - 若后续实现事件驱动 translation，则 replay/defer 的进入依据是 translation delayed / miss request 发起，而不是外部 `dtb->lookup()` 预判。
  - Negative Tests (expected to FAIL) / 反向测试（预期失败或被拒绝）:
    - 继续把 `probeTlbState()` 作为功能正确性的主判据。
    - 继续要求 replay ready 只能靠 DTLB entry 轮询命中。

- AC-3: 如果采用“按标量语义”重构，恢复条件必须是事件驱动，而不是外部轮询 ready。
  - Positive Tests (expected to PASS) / 正向测试（预期通过）:
    - 设计上明确 matrix mem translation request 的发起、defer、translation complete、retry、squash/cancel 生命周期。
    - 恢复执行依据为 translation 结果返回后的事件，等价于 “translationCompleted / resp / bypass 可用”，而非独立 replay ready 轮询。
    - 如继续保留 `MlsReplayQueue`，则其职责被明确缩减，不再承担“猜测 TLB 何时 ready”的角色。
  - Negative Tests (expected to FAIL) / 反向测试（预期失败或被拒绝）:
    - 用额外轮询/定时器/人工插 entry 的方式替代 translation result 事件。
    - 将 `MlsReplayQueue` 继续作为主恢复机制，却不绑定任何真实 translation 结果。

- AC-4: 不破坏现有 matrix payload/handoff/commit 语义边界。
  - Positive Tests (expected to PASS) / 正向测试（预期通过）:
    - `state.needReplay == false` 的路径仍然正常经过 `runStage2()/runStage3()/runStage4()`。
    - `stageMatrixExecPayload()`、`virtualQueue->markFinished()`、`IEW::readyToFinish()`、`inst->setExecuted()` 的调用关系在成功路径上保持一致或更清晰。
    - 对 matrix arithmetic / sync / non-mem 指令无行为回归。
  - Negative Tests (expected to FAIL) / 反向测试（预期失败或被拒绝）:
    - 为修 TLB replay 顺手改动 matrix payload 内容、backend handoff 语义或 commit boundary。
    - 通过提前 `setExecuted()` 掩盖 translation/replay 生命周期错误。

- AC-5: squash/cancel/flush 生命周期必须正确。
  - Positive Tests (expected to PASS) / 正向测试（预期通过）:
    - matrix mem inst 在 translation 或 replay 期间被 squash/redirect/flush 时，不会在晚到结果返回后重新 resurrect。
    - 若复用 IQ `deferMemInst()/retryMem()` 机制，应明确和 `req_kill` / `flush_pipe` / redirect 的对应关系。
  - Negative Tests (expected to FAIL) / 反向测试（预期失败或被拒绝）:
    - translation 结果晚到后仍让已 squashed 指令继续执行。
    - 通过忽略 squash/cancel 场景来换取短期“跑通”。

- AC-6: 验证范围必须聚焦本问题，不用不相关通过代替。
  - Positive Tests (expected to PASS) / 正向测试（预期通过）:
    - 使用 `--debug-start=471562299 --debug-end=471695499` 窗口验证 `mlce32` 路径变化。
    - 对比修复前后的 `mlce32` trace：首次 execute、是否 replay、是否 schedule/retry、是否 readyToFinish、是否 commit。
    - 复跑 baremetal/raw-cpt workload 确认不再卡在首条 `mlce32`。
  - Negative Tests (expected to FAIL) / 反向测试（预期失败或被拒绝）:
    - 只跑 SE `gemm_precomp` 就宣称修复 FS/raw-cpt 问题。
    - 只看 build 成功，不看 trace 与 commit 行为。

## Path Boundaries / 范围边界

### Upper Bound (Maximum Scope) / 最大范围

- 允许修改：
  - `src/cpu/o3/mls_unit.cc`
  - `src/cpu/o3/mls_unit.hh`
  - 如确有必要，最小范围触达：
    - `src/cpu/o3/iew.cc`
    - `src/cpu/o3/inst_queue.cc`
    - `src/cpu/o3/lsq.cc`
    - `src/cpu/o3/lsq_unit.cc`
- 允许新增极小型 matrix translation helper / state object，只要范围仍局限在 `src/cpu/o3`。
- 允许追加更新：
  - `cute_detailed_design/implementation_notes.md`
  - `cute_detailed_design/verify_report.md`
  - `cute_detailed_design/handoff.md`

### Lower Bound (Minimum Scope) / 最小范围

- 至少必须修掉 baremetal/raw-cpt 下 `mlce32` 因错误 MLS TLB replay 进入条件导致的永久卡死。
- 至少必须给出一条代码上自洽的解释：为什么修复后的 replay 进入/退出条件比当前更接近标量语义。
- 至少必须通过 trace 证明：
  - 修复前：`alloc ready=0` 后无 schedule/retry
  - 修复后：`mlce32` 不再困死在该状态

### Allowed Choices / 允许和禁止的选择

- Can use / 可以使用:
  - 复用 `DynInst::translationStarted/translationCompleted`
  - 复用 `IEW::deferMemInst()` / `InstructionQueue::retryMem()`
  - 参考 XiangShan `TLB.scala` 中 `handle_nonblock` / `handle_block` / `ptw_resp_bypass` 的语义
  - 将 `probeTlbState()` 降级为 debug / fast-path hint，而非主判据

- Cannot use / 不允许:
  - 复制整套 scalar `LSQ::SingleDataRequest` / dcache / miss queue / store-load violation 逻辑到 MLS
  - 顺手修改 `src/matrix/*` backend 行为
  - 顺手修改 `configs/example/kmhv3.py`
  - 通过回滚用户已有改动换取“干净工作树”
  - 通过关闭 stuck check / 忽略 commit 卡死来伪造修复

## Feasibility Hints / 可行性提示

- 当前问题的最小根因是：`probeTlbState()` 产生的 pre-lookup miss 被当成了 replay 真相，而 `runStage1()` 成功翻译的结果没有优先级更高地覆盖它。
- 第一阶段可以先把 direct/physical path 从错误 replay 中解出来，作为行为修正和风险收敛点。
- 如果继续向“标量语义”推进，建议复制的是 translation request 的事件驱动协议，而不是 scalar dcache 请求生命周期。
- 真正值得参考的标量代码不是 `LoadPipe` 的 dcache replay，而是 `xiangshan/cache/mmu/TLB.scala` 中：
  - `missVec`
  - `ptw.req`
  - `ptw.resp`
  - `ptw_resp_bypass`
  - `tlbreplay`
- 若保留 `MlsReplayQueue`，可考虑让它只承载 matrix-specific retry bookkeeping，而不再轮询 DTLB cache entry。
- 若引入新的 matrix translation helper，需优先定义清楚：
  - 谁拥有 request 生命周期
  - translation 完成后由谁回写 `physEffAddr/memReqFlags`
  - squash/cancel 晚到结果如何丢弃

## Dependencies and Sequence / 依赖与执行顺序

### Milestones / 里程碑

1. Milestone 1 / 里程碑 1: 建立标量 TLB/PTW 语义到 MLS 的映射
   - Phase A / 阶段 A: 梳理 `MlsUnit::issue()` 当前状态机与 `TLB.scala` 的 `handle_nonblock/handle_block/ptw_resp_bypass` 差异。
   - Phase B / 阶段 B: 选定本轮实现策略：
     - 最小 direct-path 修复先行
     - 或最小事件驱动 translation helper 先行
   - Verify / 验证:
     - 书面说明当前 replay 进入/退出条件与标量参考实现的差异点

2. Milestone 2 / 里程碑 2: 实现 bare/direct path 正确行为
   - Phase A / 阶段 A: 修改 MLS replay 进入条件，避免 direct/physical path 被错误送入 replay。
   - Phase B / 阶段 B: 确保成功路径仍然走 `runStage2/S3/S4 -> readyToFinish -> commit`。
   - Verify / 验证:
     - 使用 debug window 复跑，确认 `sn:1697357` 不再只有一次 issue 且只留下 `alloc ready=0`

3. Milestone 3 / 里程碑 3: 视本轮范围决定是否推进最小事件驱动 translation
   - Phase A / 阶段 A: 若 direct-path 修复后仍需更贴近标量语义，则新增最小 matrix translation request/helper。
   - Phase B / 阶段 B: 将 defer/retry 从 `lookup` 轮询改为 translation 结果事件驱动。
   - Verify / 验证:
     - trace 中能看到“进入 defer/等待结果/重新 retry”的闭环，而不是 replay ready 轮询

4. Milestone 4 / 里程碑 4: 验证与文档回写
   - Phase A / 阶段 A: 对比修复前后 trace，整理证据。
   - Phase B / 阶段 B: 更新 `implementation_notes.md`、`verify_report.md`、`handoff.md`。
   - Verify / 验证:
     - 明确列出通过项、未覆盖风险和下一步建议

## Implementation Notes / 实现注意事项

- 代码中不要引入 `AC-`、`Milestone`、`Phase` 等计划术语。
- 优先保持行为边界清晰：matrix mem 修复只应改变 translation/replay 生命周期，不应改变 payload 内容或 backend 完成语义。
- 如果本轮最终只做 direct-path 修复，必须在文档里明确：这是为当前 baremetal 卡死问题做的最小行为修正，不等于 MLS 已经完全对齐标量 TLB/PTW 模型。

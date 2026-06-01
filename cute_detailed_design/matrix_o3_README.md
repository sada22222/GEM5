# Matrix O3 README

创建日期: 2026-04-24

## 1. 文档定位

这份文档只说明当前 GEM5 主仓里 matrix O3 路径的**实现思路**、**阶段职责**、**未对齐项**，以及已经做过的**测试命令和结果**。

它不是：

- RTL 设计文档
- simplified CUTE 详细设计文档
- 长期计划文档
- 逐轮 patch changelog

相关文档：

- `cute_detailed_design/matrix_core_design.md`
- `cute_detailed_design/matrix_diff_vs_rtl.md`
- `implementation_notes.md`
- `verify_report.md`
- `handoff.md`

## 2. 当前总体状态

当前主仓里，matrix 路径已经具备一条可工作的最小闭环：

- `decode` 识别并生成 matrix 指令的最小 payload
- `rename` 依赖现有通用寄存器重命名，承接 matrix tile CSR carrier
- `execute` 对普通 matrix 指令直接执行，对 matrix mem 指令走 `MlsUnit`
- `lsu` 负责参数检查、地址翻译和 payload finalize
- `commit` 之后才允许进入 backend，可见性边界明确
- backend 通过 `DetailedCuteBackend` 执行 `mzero / mma / lsu / release`

当前已验证：

- `hello_xsai` 能在 SE 下正常运行
- `mem_test` `16/16` 通过
- `gemm_precomp` `8/8` 通过

## 3. Decode

### 当前实现

matrix 指令的前端入口在：

- `src/arch/riscv/isa/decoder.isa`

当前 decode 阶段主要做三件事：

1. 识别 matrix 指令族，并按指令类型生成最小 `MatrixExecPayload`
2. 在需要时读取 tile renamed carrier，例如：
   - `mlae8` 读取 `M/K`
   - `mlbe8` 读取 `K/N`
   - `mlce32` / `msce32` 读取 `M/N`
   - `mzero` 读取 renamed `M/N`
   - `mmacc_w_b` 读取 renamed `M/N/K`；`rm/frm/sat` 来自普通 matrix CSR sideband
3. 对参数做最小合法性检查，例如目标 bank、tile 上界、宽度编码

当前关键点：

- `mlbe8` 已按 benchmark 语义带上 `transpose = true`
- `mzero` 已改成使用 renamed `M/N`，不再只读 arch misc reg

### 当前未对齐

- decode 覆盖范围仍以当前验证所需的 matrix 指令子集为主，不等价于完整 IME 指令集
- decode 里的 fallback 仍保留了 `recordMatrix*Request()` 这类 stub 路径，用于非 O3 / 非完整闭环场景
- 当前参数检查是最小功能语义，不代表已经与 RTL 所有前端 gating 完全一致

## 4. Rename

### 当前实现

当前没有单独的 matrix rename 子系统，matrix 路径复用了现有 O3 rename 机制。

关键承载方式：

- `mtilem/mtilen/mtilek` 通过 `RMiscRegClass` 暴露为 renamed operand
- `XMCSR/XMXRM/XMFRM/XMSATEN` 走普通 CSR sideband，不作为 renamed carrier
- `DynInst::initMatrixInstInfo()` 在 decode 后补充 matrix route / commit boundary / state read-write 摘要

相关代码：

- `src/arch/riscv/regs/matrix.hh`
- `src/cpu/o3/dyn_inst.cc`
- `src/cpu/o3/dyn_inst.hh`

rename 阶段当前主要做两件事：

1. 让 `msettile*` 更新 `mtilem/mtilen/mtilek` renamed carrier；`xmcsr` 相关状态由普通 CSR sideband 读取
2. 在 `DynInst` 上形成 matrix 指令摘要，给 execute / ROB / commit 使用

### 当前未对齐

- 仍然完全复用通用 rename，不是 RTL 那种独立 matrix state 通路
- 没有引入额外的 matrix rename resource / rename stall / rename credit 模型
- 当前 `stateReads/stateWrites` 只覆盖当前闭环必需项，不是完整 matrix state 依赖表

## 5. Execute

### 当前实现

执行阶段分成两类：

1. 非 matrix-mem 指令：
   - 在 `IEW` 里走普通 `inst->execute()`
   - `mzero`、`mmacc_w_b`、`mrelease` 等在这里形成 payload
2. matrix-mem 指令：
   - 在 `IEW` 里识别后转给 `MlsUnit`
   - `MlsUnit` 分阶段完成 capture / translate / resolve / payload finalize

相关代码：

- `src/cpu/o3/iew.cc`
- `src/cpu/o3/mls_unit.cc`

当前 execute 阶段的关键实现思路：

- matrix mem 指令不直接 cute-visible
- execute 只负责形成或 finalize payload
- 真正进入 cute 的边界被推迟到 commit 后

### 当前未对齐

- `IEW` 调度、issue queue、执行延迟仍然是 GEM5 当前模型，不是 RTL 原始实现
- `mmacc / mzero` 当前仍不是精确 RTL timing 语义
- execute 之后的 cute 完成时间当前由 `DetailedCuteBackend` 的事件/任务模型驱动，不是精确微结构时序

## 6. LSU

### 当前实现

matrix load/store 的核心在：

- `src/cpu/o3/mls_unit.cc`

当前 `MlsUnit` 主要负责：

1. 读取基址、stride、tile 维度
2. 做最小参数检查
3. 调用 `mmu->translateAtomic()` 拿到当前 access 的地址翻译结果
4. finalize `MatrixExecPayload`

当前 payload 的关键点：

- `baseAddr` 保留 guest virtual address
- `physBaseAddr` 额外保留首地址物理翻译结果
- `row/column/mtilem/mtilen/mtilek` 在这里统一补齐

backend 访存适配层在：

- `src/matrix/matrix_memory_adapter_gem5.cc`

当前行为：

- `SE` 模式下，通过 `SETranslatingPortProxy` 按虚拟地址逐元素访问
- `FullSystem` 下，当前仍使用 `physBaseAddr + offset` 路径

### 当前未对齐

- `SE` 路径已修正为逐地址翻译；`FS` 路径仍是简化实现，尚未切到 `TranslatingPortProxy`
- replay / replace / cancel / load wake / loadcancel 仍然不是 RTL 级别对齐
- fault owner、LSQ fault 细分、异常归属只完成了当前功能闭环所需最小覆盖

## 7. Commit

### 当前实现

commit 阶段的核心结构是：

- `ROB::MatrixAmuEntry`
- `cpu->consumeMatrixAmuProxy()`
- `cpu->submitMatrixBackendReq()`
- `cpu->serviceMatrixBackend()`

相关代码：

- `src/cpu/o3/rob.cc`
- `src/cpu/o3/commit.cc`
- `src/cpu/o3/cpu.cc`

当前 commit 思路：

1. execute/writeback 后，ROB 记录 matrix payload
2. 只有 commit 后，`MatrixAmuEntry` 才允许 dequeue
3. dequeue 后进入 `consumeMatrixAmuProxy()`
4. `Lsu / Mma / Arith / Release` 在这里转成 cute request
5. cute completion 由 `serviceMatrixBackend()` 在 CPU tick 中轮询并回收

这条边界解决了一个关键问题：

- commit 前 matrix request 不可 backend-visible
- wrong-path / squash / faulted request 不应进入 backend

### 当前未对齐

- 当前用的是 `MatrixAmuEntry`，不是 RTL 原始 `AMUCtrlBuffer`
- completion 处理是 CPU tick 驱动的 functional polling，不是 RTL 原始完成路径
- token/release 已闭环，但并不代表完整时序、吞吐和资源模型已与 RTL 等价

## 8. 当前未对齐项汇总

按阶段总结，当前仍明确未对齐的点包括：

### Decode

- 指令覆盖范围仍不完整
- 仅完成当前 workload 所需子集

### Rename

- 复用通用 RMiscReg rename
- 没有独立 matrix rename 资源与时序约束

### Execute

- `mzero / mma` 仍是 functional 语义
- 不具备 detailed timing CUTE 模型

### LSU

- `FS` 模式下的 matrix backend 访存仍未做逐地址翻译
- `replay / cancel / replace / exception ownership` 仍是阶段性简化

### Commit / Backend

- `MatrixAmuEntry` 是近似替代，不是 RTL 原始缓冲结构
- backend completion 是 simplified functional completion

## 9. 测试命令与结果

### 构建

```bash
cd /nfs/home/hujun/GEM5
scons -Q build/RISCV/gem5.opt --linker=gold -j64 --rvv-impl=simple
```

结果：

- PASS

### `gemm_precomp`

```bash
cd /nfs/home/hujun/GEM5
build/RISCV/gem5.opt --outdir=/tmp/gem5-se-gemm-precomp-cleantrace \
  configs/example/se.py \
  -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/gemm_precomp/build/gemm_precomp \
  --enable-riscv-vector --no-pf
```

结果：

- `All 8 precomp tests PASSED.`

### `hello_xsai`

```bash
cd /nfs/home/hujun/GEM5
build/RISCV/gem5.opt --outdir=/tmp/gem5-se-hello-xsai-rerun \
  configs/example/se.py \
  -c /nfs/home/hujun/workspace/xsai/xsai-env/firmware/riscv-rootfs/apps/hello_xsai/build/hello_xsai \
  --enable-riscv-vector --no-pf
```

结果摘要：

- `Hello, XiangShan AI!`
- `hello_xsai is running from XSAI init flow.`
- `mem_test`: `16 passed, 0 failed`

## 10. 当前结论

当前主仓的 matrix O3 路径已经具备：

- decode 到 commit 的最小闭环
- commit 后 detailed backend 执行闭环
- `SE` 模式下正确的 guest virtual access
- `hello_xsai` 与 `gemm_precomp` 的功能通过

但它仍然是**阶段性 detailed 近似**，不是完整 RTL 等价实现。

下一步如果要继续推进，优先级建议是：

1. 完成 `FS` 模式下的 matrix backend 虚拟地址访问
2. 继续补 fault / replay / exception ownership 对齐
3. 再讨论更细粒度的时序与资源模型
4. 完善cute与l2cache建模

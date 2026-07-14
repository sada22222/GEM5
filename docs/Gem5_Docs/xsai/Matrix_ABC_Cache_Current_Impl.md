# Matrix ABC Load/Cache Current Implementation

本文档记录 `/nfs/home/hujun/GEM5` 当前 dirty worktree 中与 Matrix A/B/C load 缓存路径、Matrix L2 fill、以及 cache/xbar 通路相关的实现。当前分支是 `codex/matrix-src-only-pr-unified`；本文不描述 `/tmp/gem5-matrix-l2-xs-l2-contract-20260706` worktree 中那套更大的改动。

## 范围

本文覆盖这些 dirty 文件中的实现：

- `src/matrix/CUTETOP.cc`
- `src/matrix/CUTETOP.hh`
- `src/matrix/LocalMMUModel.cc`
- `src/matrix/LocalMMUModel.hh`
- `src/matrix/TaskController.cc`
- `src/matrix/TaskController.hh`
- `src/cpu/o3/BaseO3CPU.py`
- `src/cpu/o3/cpu.cc`
- `src/cpu/o3/cpu.hh`
- `configs/common/CacheConfig.py`
- `src/mem/XBar.py`
- `src/mem/coherent_xbar.cc`
- `src/mem/coherent_xbar.hh`

当前 worktree 也有 `src/mem/dramsim3.cc/.hh` admission 修改；它影响内存控制器 admission，但不是本文的 ABC cache 主体。

## kmhv2.py 运行入口

完整命令模板见 [Matrix_Readme.md](Matrix_Readme.md) 的 `kmhv2.py 运行命令` 小节。用于观察当前 ABC load/cache/xbar dirty 修改时，建议优先使用 `configs/example/kmhv2.py`，因为它会设置 `args.kmh_align = True`，从而走当前 `config_aligned_l2()` 中打开 matrix response lane 的 L1-to-L2 bus 配置。

raw binary 形态：

```bash
./build/RISCV/gem5.opt \
  --outdir=/tmp/gem5-kmhv2-matrix-abc-raw \
  --debug-flags=MatrixCuteTrace \
  --debug-file=matrix_cute.trace \
  configs/example/kmhv2.py \
  --raw-cpt \
  --generic-rv-cpt=<matrix-raw-bin> \
  --disable-difftest \
  --enable-riscv-vector
```

GCPT / checkpoint slice 形态不要加 `--raw-cpt`：

```bash
./build/RISCV/gem5.opt \
  --outdir=/tmp/gem5-kmhv2-matrix-abc-gcpt \
  --debug-flags=MatrixCuteTrace \
  --debug-file=matrix_cute.trace \
  configs/example/kmhv2.py \
  --generic-rv-cpt=<checkpoint.zstd> \
  --disable-difftest \
  --enable-riscv-vector
```

## 术语

- ALoad：`entry.isLoad && !entry.request.lsu.isAcc && !entry.request.lsu.isB`，目标是 A bank。
- BLoad：`entry.isLoad && !entry.request.lsu.isAcc && entry.request.lsu.isB`，目标是 B bank。
- CLoad：`entry.isLoad && entry.request.lsu.isAcc`，当前实现把它归入 CML memory load。
- zero-C：`entry.isZeroAcc`，归入 CML load-like task，但不发 memory request，而是通过固定延迟建模 C 清零占用。
- AML/BML/CML：LocalMMU client，用来区分 A/B/C/StoreC 请求来源。
- Matrix L2 fill table：CUTE 内部的响应填充缓冲，接收 timing memory read response 后，按 matrix register bank FIFO 分发写回。

这里的“ABC 缓存”不是传统 CPU cache tag/data array，而是 Matrix A/B/C load response 从 timing memory 返回后进入 Matrix L2 fill table、再写入 matrix register 的建模路径。

## 总体数据流

1. CUTE 解码出的 Matrix LSU task 被分类为 ALoad、BLoad、CLoad、StoreC 或 zero-C。
2. A/B/C memory load 和 StoreC 在 CUTE 内生成 LocalMMU beat request。
3. `issueLocalMmuTimingRequest()` 从 LocalMMU 取出可发 beat，组装 `MatrixTimingMemoryAdapter::Request`，通过 CPU 的 `MatrixMemPort` 发到 cache/memory path。
4. memory response 回到 `MatrixMemPort::recvTimingResp()` 后，调用 CUTE `completeTimingMemoryResponse()`。
5. CUTE 把 response 交给 `serviceLocalMmuReadResponse()` 或 store ack 处理。
6. A/B/C load response 根据策略进入 Matrix L2 fill table 或 B bypass path。
7. `serviceLsuMatrixRegWriteChunks()` 每周期从 fill table 按 bank drain chunk，并写入 matrix register file。

## A/B/C Load 分类与统计

`DetailedCuteBackend` 新增明确分类函数：

- `isALoad()`
- `isBLoad()`
- `isCLoad()`
- `isCmlMemLoad()`
- `isCmlZeroLoad()`

`recordCutePhaseIssue()` 中：

- ALoad 增加 `matrixCuteALoadIssue`。
- BLoad 增加 `matrixCuteBLoadIssue`。
- CLoad 同时增加 `matrixCuteCLoadIssue`、`matrixCmlLoadTaskStart`、`matrixCmlMemLoadTaskStart`。
- zero-C 增加 `matrixCmlLoadTaskStart`、`matrixCmlZeroLoadTaskStart`。
- StoreC 增加 `matrixCuteStoreIssue`、`matrixCmlStoreTaskStart`。

新增的 load phase breakdown 记录 A/B load 的：

- issue 到最后一次 adapter request。
- 最后一次 adapter request 到最后一次 response。
- 最后一次 adapter request 到最后一次 actual memory request。
- 最后一次 actual memory request 到最后一次 response。
- 最后一次 response 到 task finish。

`lastReqStep` 记录 adapter 接受 LocalMMU request 的时间；`lastActualReqStep` 由 `MatrixMemPort` 在 `sendTimingReq()` 真正成功后回调 `noteTimingMemoryRequestSent()` 记录。

## CLoad 与 zero-C

CLoad 走 CML memory load 路径：

- enqueue beat 时设置 `request.client = CML`。
- metadata 中 `isRMW = task.entry.isLoad && desc.isAcc`。
- timing request 的 `matrixKey` 使用 `MatrixModify`，其他 matrix load 使用 `Matrix`。

zero-C 走 CML load-like 统计，但不走 timing memory：

- `BaseO3CPU.py` 新增 `matrixCZeroLoadLatency`，默认 `256`。
- CPU 构造 `DetailedCuteBackend::TimingConfig` 时把参数传入 `matrixCZeroLoadLatencyCycles`。
- `dispatchTask()` 对 `entry.isZeroAcc` 初始化 `task.zeroCyclesRemaining`。
- `advanceZeroAccDelay()` 在 task finish 前消耗该固定延迟。

因此 zero-C 当前是固定 CUTE 占用模型，不是 cache/memory request 模型。

## LocalMMU 请求与 source 跟踪

`LocalMMUModel` 新增查询接口：

- `hasPendingMatching(client, is_store)`：判断是否有某类 pending request。
- `outstandingCount(client, is_store)`：统计某类 outstanding source。
- `completedOutstandingCount()`：统计 response 已完成但 source 尚未 release 的 entry。
- `outstandingFull()`：判断 external source 是否满。
- `outstandingRequest(source_id)`：按 source id 找回原始 request。

CUTE 用这些接口统计 CML load 被堵在哪里：

- source 满。
- source 仲裁未发。
- timing memory adapter / LLC 不接受。
- ready response 未被 CUTE service。
- completed source 尚未 release。

`localMmuSourceTiming` 记录每个 source 的 issue、response、release 时间，用来拆 CML load source 的：

- issue 到 response。
- response 到 release。
- issue 到 release。

## Matrix L2 Fill Table

`MatrixL2FillTable` 是当前 ABC load response 的核心缓冲结构，默认参数来自 `DetailedCuteBackend::TimingConfig`：

- `matrixL2FillTableEntries = 4`
- `matrixL2FillBankFifoDepth = 2`

每个 fill response 会形成一个 `MatrixL2FillTable::Request`，包含：

- `seq`
- `client`
- `beatIndex`
- `destBank`
- `destReg`
- `byteSize`
- `fillChunks`
- `targetBank`
- `targetEntry`

accept 条件分两层：

1. `canAccept()`：response byte size 合法、fill chunk 非 0、且有空闲 entry。
2. `canAcceptResponse()`：除 entry 外，还要求目标 physical bank FIFO 未满。

接受后：

- `acceptResponseToBank()` 分配 fill entry。
- 把 fill handle 放入对应 target bank FIFO。
- CUTE 释放 LocalMMU source。
- 统计 `matrixL2FillAcceptedResponses` 和 `matrixL2FillAcceptedChunks`。

drain 时：

- 每个 matrix physical bank 取一个 `drainCandidate()`。
- A/B bank 需要通过 `matrixRegResource` 仲裁 MemoryLoader 写口。
- C bank 当前直接 `retire_candidate()`，不走 A/B 那套 matrix register write arbitration。
- 每 retire 一个 chunk，减少 task 的 `lsuPendingMatrixRegWriteChunks`。
- entry 的所有 chunks drain 完后释放 fill table slot。

fill table 堵塞统计包括：

- `matrixCmlLoadRespBlockedFillTableFull`
- `matrixCmlLoadRespBlockedFillBankFifoFull`
- `matrixCuteALoadFillTableFullBlocked`
- `matrixCuteBLoadFillTableFullBlocked`
- `matrixCuteALoadFillBankFifoFullBlocked`
- `matrixCuteBLoadFillBankFifoFullBlocked`
- `matrixCuteALoadMatrixRegWriteBlocked`
- `matrixCuteBLoadMatrixRegWriteBlocked`

## BLoad Bypass

BLoad 有可选 bypass fill table 路径：

- `matrixBmlBypassFillTable` 若显式配置则按配置。
- 未配置时，默认条件是 `matrixReduceWidthBytes >= matrixOutsideDataWidthBytes`。

命中 bypass 时，BLoad response 不进入 fill table，而是在 `serviceBmlBypassResponse()` 中直接申请 B bank matrix register write。若写口未 grant，统计 `matrixCuteBLoadMatrixRegWriteBlocked`，response service 保持 blocked。

## StoreC 路径

StoreC 被归入 CML store task：

- issue 统计：`matrixCmlStoreTaskStart`。
- request fire 统计：`matrixCmlStoreWriteFire`。
- response/ack fire 统计：`matrixCmlStoreRespFire`。
- finish 统计：`matrixCmlStoreTaskFinish` 及 start-to-first/last write、first/last response、finish latency。

`executeStoreWrite()` 在 timing memory 可用时返回 success。真正的数据外发由 LocalMMU beat 和 `MatrixMemPort` 完成。

## MatrixMemPort 修改

`CPU::MatrixMemPort` 新增 `noteTimingRequestSent(PacketPtr pkt)`：

- 当 `sendOrBlock()` 中 `sendTimingReq(pkt)` 立即成功时调用。
- 当 `trySendBlocked()` 中 blocked packet retry 成功时调用。
- 回调 `matrixBackend->noteTimingMemoryRequestSent(sourceId)`。

这个回调用来区分：

- CUTE/LocalMMU adapter 已经发出 request。
- request 真正通过 gem5 timing port 被 cache/memory path 接受。

因此 `lastReqStep` 与 `lastActualReqStep` 可以分开统计 xbar/cache backpressure。

## Cache/XBar 相关修改

当前没有直接改 `src/mem/cache/*` 的 tag/data 行为；cache 相关修改集中在 `L1ToL2Bus` / coherent xbar response path。

`configs/common/CacheConfig.py`：

- 在 `config_aligned_l2()` 创建每个 `L1ToL2Bus` 后设置：
  - `enable_matrix_response_lane = True`
  - `matrix_response_max_per_cycle = 2`

`src/mem/XBar.py`：

- `CoherentXBar` 新增参数：
  - `enable_matrix_response_lane`
  - `matrix_response_max_per_cycle`

`src/mem/coherent_xbar.cc/.hh`：

- 每个 CPU-side port 除原 `respLayers` 外新增一个 `matrixRespLayers`。
- `matrixRespLayers` 使用 `matrix_response_max_per_cycle` 设置每周期吞吐。
- `isMatrixLoadResponse()` 判断条件：
  - feature enabled。
  - packet 是 response。
  - request 带 XS metadata。
  - packet 有 data 且 size 非 0。
  - `xs_metadata.matrixTask()` 为真。
- `recvTimingResp()` 对 matrix load response 使用 `matrixRespLayers[cpu_side_port_id]`，普通 response 仍走原 `respLayers`。
- 新增统计：
  - `matrixLoadRespLanePackets`
  - `matrixLoadRespLaneBlocked`

这个改动的目的不是改变 cache 一致性语义，而是给 Matrix load data response 一个独立 response lane，减少与普通 response layer 的结构冲突。

## 与 DRAMSim3 admission 的关系

当前主 worktree 中 DRAMSim3 dirty 修改把 request admission 改为直接看 `wrapper.canAccept(addr, isWrite)`，并让 retry 也基于被拒请求的 addr/isWrite 判断。这个修改与本文的 Matrix CLoad lower request 压力有关，但它属于 memory controller admission 语义，不属于 ABC fill/cache 主路径。

## 当前实现边界

- 本文只描述当前 dirty 代码语义，没有声明 RTL 完全对齐。
- Matrix L2 fill table 是 CUTE 内部 fill 缓冲，不是完整 RTL L2 cache。
- C bank fill drain 当前直接 retire，不走 A/B 的 matrix register write arbitration；这是当前实现事实，是否 RTL-like 需要单独确认。
- BLoad bypass 是否启用依赖 `matrixBmlBypassFillTable` 或带宽默认判断。
- zero-C 是固定延迟模型，不产生 memory/cache traffic。
- coherent xbar 新 response lane 只按 XS metadata 的 `matrixTask()` 区分 matrix data response，不区分 A/B/C。
- 当前未新增针对这些改动的独立测试文件。

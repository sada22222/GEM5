# Round 0 Summary

## What Was Implemented

- Added explicit MLS replay cause/context state:
  - `MlsReplayQueue::ReplayCause::{None,TlbMissPending}`
  - replay request flags
  - translation completion state
- Tightened replay allocation so replay entries must carry a named cause.
- Changed MLS issue semantics so a completed translation with a valid request does not enter replay even if the pre-translation DTLB lookup missed.
- Kept the direct physical/bare path bypass intact and extended the debug text to distinguish physical vs completed translations.
- Left scalar LSQ/dcache/TLB/PTW internals and matrix backend counter/timing code untouched.
- Clarified the next-step boundary: CUTE `LocalMMU` / GEM5 `LocalMmuModel` is a post-translation request/sourceId/response layer, not the TLB repair point for this bug.

## Files Changed

- `src/cpu/o3/mls_unit.hh`
- `src/cpu/o3/mls_unit.cc`
- `docs/exec-plans/active/mls-tlb-replay-fix.md`
- `.humanize/rlcr/2026-05-28_16-05-55/goal-tracker.md`
- `.humanize/rlcr/2026-05-28_16-05-55/round-0-summary.md`

## Validation

- Passed: `scons build/RISCV/gem5.opt -j8`
  - Relevant coverage: `src/cpu/o3/mls_unit.cc` compiled and final `build/RISCV/gem5.opt` linked successfully.
  - Warnings were environment/dependency warnings only: PNG, HDF5, backtrace support.
- Passed after making default-memory dependency locally visible:
  - Temporary, untracked validation-only symlink: `ext/dramsim3/DRAMsim3 -> /nfs/home/hujun/GEM5/ext/dramsim3/DRAMsim3`.
  - Rebuilt with `scons build/RISCV/gem5.opt -j8`; `DRAMsim3` params compiled and linked.
  - Ran default DRAMsim3 window:
    `build/RISCV/gem5.opt --debug-flags=IEW,LSQ,Commit,MatrixCuteTrace --debug-start=1118680000 --debug-end=1118696000 --outdir=/tmp/gem5-kmhv3-ametest-mlce32-debug-rlcr-dramsim3-window configs/example/kmhv3.py --generic-rv-cpt /nfs/home/hujun/workspace/xsai/xsai-env/nexus-am/tests/ame0.6/build/ametest-riscv64-xs.bin --raw-cpt --disable-difftest -I 10000000 --mem-size=4GB`.
  - The fixed IEW/LSQ/Commit debug window printed five PASS iterations and exited by max instruction count, but did not include the target `mlce32`/MLS event because DRAMsim3 timing moved it outside that window.
  - Ran full `MatrixCuteTrace` default DRAMsim3 validation:
    `build/RISCV/gem5.opt --debug-flags=MatrixCuteTrace --outdir=/tmp/gem5-kmhv3-ametest-mlce32-debug-rlcr-dramsim3-matrixtrace configs/example/kmhv3.py --generic-rv-cpt /nfs/home/hujun/workspace/xsai/xsai-env/nexus-am/tests/ame0.6/build/ametest-riscv64-xs.bin --raw-cpt --disable-difftest -I 10000000 --mem-size=4GB`.
  - Evidence in `/tmp/gem5-kmhv3-ametest-mlce32-debug-rlcr-dramsim3-matrixtrace.log`: backend `local_mmu_enqueue/issue/response` traffic is present, five PASS iterations printed, and the run exited by max instruction count.
- Passed with available memory model:
  - `build/RISCV/gem5.opt --debug-flags=IEW,LSQ,Commit,MatrixCuteTrace --debug-start=1118680000 --debug-end=1118696000 --outdir=/tmp/gem5-kmhv3-ametest-mlce32-debug-rlcr-simplemem-window configs/example/kmhv3.py --generic-rv-cpt /nfs/home/hujun/workspace/xsai/xsai-env/nexus-am/tests/ame0.6/build/ametest-riscv64-xs.bin --raw-cpt --disable-difftest -I 10000000 --mem-size=4GB --mem-type=SimpleMemory`
  - Evidence in `/tmp/gem5-kmhv3-ametest-mlce32-debug-rlcr-simplemem-window.log`:
    - `sn:1696809` `mlce32` `S1 translate ... paddr=0x80327000 ... fault=0 tlbMiss=1`
    - `MlsUnit bypass replay on physical translation ... flags=0x200`
    - `MlsUnit S4 handoff ... needReplay=0`
    - `Matrix execute->commit`, commit, `Matrix toAMU`, backend submit, `local_mmu_enqueue`, and `local_mmu_response`
    - `rg MlsReplayQueue /tmp/gem5-kmhv3-ametest-mlce32-debug-rlcr-simplemem-window.log` returned no hits.
    - Workload printed five PASS iterations and exited by max instruction count.

## Remaining Items

- Temporary DRAMsim3 symlink was removed before final git-clean handoff; it must not be committed in future runs.
- Full pageable/asynchronous translation behavior is not proven by this bare/direct-path workload; current MLS still uses `translateAtomic()`, so this round only contracts replay semantics around completed translations and named pending causes.
- SE `gemm_precomp` remains a later regression target because the plan did not identify its binary path.
- Do not implement CUTE `LocalMMU` / GEM5 `LocalMmuModel` behavior changes in this flow; that layer is not where RTL TLB translation happens.

## BitLesson Delta

Action: none
Lesson ID(s): NONE
Notes: Selector returned no usable lessons and the local bitlesson database has no entries for this round.

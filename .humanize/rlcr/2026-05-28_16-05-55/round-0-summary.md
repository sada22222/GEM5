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

## Files Changed

- `src/cpu/o3/mls_unit.hh`
- `src/cpu/o3/mls_unit.cc`
- `.humanize/rlcr/2026-05-28_16-05-55/goal-tracker.md`
- `.humanize/rlcr/2026-05-28_16-05-55/round-0-summary.md`

## Validation

- Passed: `scons build/RISCV/gem5.opt -j8`
  - Relevant coverage: `src/cpu/o3/mls_unit.cc` compiled and final `build/RISCV/gem5.opt` linked successfully.
  - Warnings were environment/dependency warnings only: PNG, HDF5, backtrace support.
- Blocked for default memory model: same debug run with default `DRAMsim3` failed before simulation because this clean RLCR worktree does not have the untracked `ext/dramsim3/DRAMsim3` dependency from the original dirty worktree.
  - Failure: `KeyError: 'DRAMsim3'` / `DRAMsim3 is not a valid sub-class of AbstractMemory`.
  - This was not treated as a code failure because the user explicitly did not want unrelated original-worktree changes committed.
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

- Default `DRAMsim3` validation remains pending until the DRAMsim3 dependency is made available in the RLCR worktree without committing the user's unrelated original-worktree changes.
- Full pageable/asynchronous translation behavior is not proven by this bare/direct-path workload; current MLS still uses `translateAtomic()`, so this round only contracts replay semantics around completed translations and named pending causes.
- SE `gemm_precomp` remains a later regression target because the plan did not identify its binary path.

## BitLesson Delta

Action: none
Lesson ID(s): NONE
Notes: Selector returned no usable lessons and the local bitlesson database has no entries for this round.

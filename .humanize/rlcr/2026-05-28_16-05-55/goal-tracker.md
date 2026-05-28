# Goal Tracker

<!--
This file tracks the ultimate goal, acceptance criteria, and plan evolution.
It prevents goal drift by maintaining a persistent anchor across all rounds.

RULES:
- IMMUTABLE SECTION: Do not modify after initialization
- MUTABLE SECTION: Update each round, but document all changes
- Every task must be in one of: Active, Completed, or Deferred
- Deferred items require explicit justification
-->

## IMMUTABLE SECTION
<!-- Do not modify after initialization -->

### Ultimate Goal
Bring GEM5 MLS translation/replay behavior closer to XiangShan/CUTE RTL semantics for matrix memory operations: successful translations must not enter replay, and any remaining MLS replay state must represent a true pending translation/request condition with an explicit cause and a result-driven recovery path, without copying scalar LSQ/dcache/TLB/PTW internals.

### Acceptance Criteria
<!-- Each criterion must be independently verifiable -->

AC1. Direct/bare physical translations with `Request::PHYSICAL`, `NoFault`, and a valid request/paddr complete without allocating or scheduling `MlsReplayQueue` entries.

AC2. MLS replay state carries explicit cause/context so replay no longer means only "`dtb->lookup()` missed"; successful translations cannot set `needReplay`.

AC3. The O3 matrix memory handoff remains coherent: non-replay MLS operations still stage payload, mark the virtual queue finished, reach `IEW::readyToFinish()`, commit, and submit to the matrix backend.

AC4. The second-stage implementation is narrowly scoped to MLS replay/translation boundaries; it does not copy scalar LSQ/dcache/TLB/PTW implementation or modify matrix backend counters/timing behavior.

AC5. Verification evidence covers the current `mlce32` debug window and shows no `MlsReplayQueue` entry for the physical direct-path case.

---

## MUTABLE SECTION
<!-- Update each round with justification for changes -->

### Plan Version: 1 (Updated: Round 0)

#### Plan Evolution Log
<!-- Document any changes to the plan with justification -->
| Round | Change | Reason | Impact on AC |
|-------|--------|--------|--------------|
| 0 | Initialized tracker from exec plan and moved work into clean RLCR worktree | Original worktree contains unrelated counter/timing modifications that user does not want committed | AC4 scope is explicit; RLCR branch excludes counter changes |

#### Active Tasks
<!-- Mainline tasks only: each task must directly advance the current round objective and carry routing metadata -->
| Task | Target AC | Status | Tag | Owner | Notes |
|------|-----------|--------|-----|-------|-------|
| Implement explicit MLS replay cause/context contraction | AC2, AC4 | completed | coding | claude | Replay state now carries explicit cause, request flags, and translation-complete context |
| Verify `mlce32` physical direct path and absence of replay queue allocation | AC1, AC3, AC5 | completed | coding | claude | Verified in RLCR SimpleMemory debug window; DRAMsim3 run is blocked by untracked local dependency in original worktree |

### Blocking Side Issues
<!-- Only issues that directly block current mainline progress belong here -->
| Issue | Discovered Round | Blocking AC | Resolution Path |
|-------|------------------|-------------|-----------------|

### Queued Side Issues
<!-- Non-blocking issues stay queued and must NOT replace the round objective -->
| Issue | Discovered Round | Why Not Blocking | Revisit Trigger |
|-------|------------------|------------------|-----------------|
| Original worktree has unrelated matrix backend counter/timing dirty changes | 0 | Isolated RLCR worktree excludes them; current round only touches MLS replay semantics | Revisit only if backend validation reveals an actual semantic dependency |
| SE `gemm_precomp` binary path is not identified in the plan | 0 | Current round targets FS/raw-cpt `mlce32` replay evidence | Revisit before final regression signoff |

### Completed and Verified
<!-- Only move tasks here after Codex verification -->
| AC | Task | Completed Round | Verified Round | Evidence |
|----|------|-----------------|----------------|----------|
| AC1, AC3, AC5 | Direct-path fix baseline carried into RLCR branch | 0 | 0 | Commit `38683e94ef`; prior trace showed `sn:1696826` bypass replay, `needReplay=0`, commit/toAMU/backend response, and no `MlsReplayQueue` hits |
| AC2, AC4 | Explicit replay cause/context contraction | 0 | 0 | `src/cpu/o3/mls_unit.hh/cc` adds `ReplayCause`, request flags, and translation-complete context; `scons build/RISCV/gem5.opt -j8` passed |
| AC1, AC3, AC5 | RLCR debug-window validation for first `mlce32` | 0 | 0 | `/tmp/gem5-kmhv3-ametest-mlce32-debug-rlcr-simplemem-window.log`: `sn:1696809` `tlbMiss=1`, `flags=0x200`, bypass replay, `needReplay=0`, `Matrix execute->commit`, `Matrix toAMU`, `local_mmu_enqueue/response`; `rg MlsReplayQueue` had no hits |

### Explicitly Deferred
<!-- Items here require strong justification -->
| Task | Original AC | Deferred Since | Justification | When to Reconsider |
|------|-------------|----------------|---------------|-------------------|
| Matrix backend counter/timing changes from original worktree | AC4 | 0 | User explicitly does not want those counter changes committed in this flow | Separate counter/statistics task |

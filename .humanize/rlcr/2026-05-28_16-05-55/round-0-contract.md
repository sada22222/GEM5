# Round 0 Contract

## Mainline Objective

Implement the second-stage MLS replay semantic contraction so replay represents only a true pending translation/request condition with explicit cause/context, while keeping the already-verified direct physical translation bypass intact.

## Target ACs

- AC2: MLS replay state carries explicit cause/context so replay no longer means only "`dtb->lookup()` missed"; successful translations cannot set `needReplay`.
- AC4: The implementation stays narrowly scoped to MLS replay/translation boundaries and does not copy scalar LSQ/dcache/TLB/PTW or matrix backend counter behavior.

## Blocking Side Issues In Scope

None currently. If a compile-time interface mismatch blocks the MLS replay contraction, it may be fixed in scope only when directly caused by the MLS changes.

## Queued Side Issues Out Of Scope

- Existing counter/timing modifications in the original worktree.
- Full matrix backend response/completion semantic restructuring.
- SE `gemm_precomp` path discovery.
- Large scalar LSQ/dcache/TLB/PTW refactors.

## Round Success Criteria

- `MlsReplayQueue::ReplayState` or equivalent carries an explicit replay cause.
- `MlsUnit::issue()` does not set `needReplay` after a successful translation with a valid request/paddr, including direct physical translation.
- Any remaining replay entry is created only through a named true-pending translation/request cause.
- Debug logging identifies the replay cause or direct physical bypass path.
- The current `mlce32` debug window still shows bypass replay, payload handoff, commit/toAMU/backend progress, and no `MlsReplayQueue` entry for the direct physical path.

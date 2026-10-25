# Beyond-5s · Engineering journal — resolver v1 snapshot

[Documentation index](README.md) · [Evolution](04-decisions-and-evolution.md)

## Journal scope

This journal records the resolver v1 milestones and their technical consequences. Each entry is dated and names the revision it refers to; the evaluation entry refers to the campaign, not to the dates on which individual algorithms were implemented.

## 6 October 2026 — sequential temporal-tree baseline

**Component:** resolver v1. **Revision:** `aff127286e68ea7933dc9ef502a7dbd760e1e43d`, the `btree-v1` campaign baseline.

**Implementation:** a shared `PagePool` for nodes and key records; disjoint write intervals with commit versions; compact point encoding; logical ordering over physical slots; parent and leaf links; proactive split preparation; sequential read checks and writes; pressure-driven reclamation.

**Design consequence:** logical expiry, physical byte lifetime and key order are separate concerns. Searches check versions and refresh separators before accessing potentially reclaimed bytes. See [design sections 1–8 and 13](03-resolver-v1-design.md).

## 23 October 2026 — evaluate the retention/time trade-off at 6 GiB

**Configuration:** sequential v1 with 2 KiB blocks and 98,300 pool pages of 64 KiB, compared with FDB's original skip list using its calibrated 265-second window. Both process the same 5,400-second trace segment. Accounting and native timing use separate builds; native timing consists of three sequential, isolated rounds.

**Retention:** v1 first discards history at trace time 702.91 s. Its minimum retention thereafter is 582.938 s, or 2.200× FDB's configured window. Its median retention over the last 600 s of trace time is 584.72 s. Counted pool memory is 6,442,188,800 bytes.

**Native execution:** full-trace medians are 3,375.50 s for v1 and 1,240.68 s for FDB, with a reported per-round time ratio of 2.721×. V1 retains more history at a higher resolution cost. Reads show the larger relative timing gap; grouped traversal and prefetch are planned optimizations, but their absence alone does not quantify the cause of the measured gap.

**Validation:** full runs have matching offered-work and decision hashes and no reads rejected for age or memory. A separate 200-second validation exercises reclamation with a small v1 pool before timing.

**Final operating regime:** v1's retention continues to decline slowly; FDB's memory use continues to increase slowly at its fixed window. The final interval is treated as practically stable, with both residual trends quantified rather than extrapolated away.

**Next work:** profile execution costs, measure occupancy under churn, and evaluate traversal optimizations. The [evaluation](06-evaluation.md) contains the complete tables, methodology and limitations.

## Future entries

Record the date, component and revision, observation, hypothesis, change, validation, conclusion and next unresolved question. Link to the relevant design, decision and experiment instead of duplicating their full explanations.

# 000 — Origin, method, and what this log is

*August 2026*

## Why this project

Earlier this year I built a working FoundationDB fork exploring isolation levels and O(1) MVCC garbage collection on the Resolvers. That work pulled me deep into FDB internals — and left me staring at the most famous constraint in the system: the five-second transaction limit. It is not a hardware limit; it is a memory-architecture decision (the MVCC window lives in Storage Server RAM) with a twin echo in the Resolvers. It can be removed. This project removes it for the read path, in public, in phases small enough to merge.

## What this log is

Most engineering write-ups publish conclusions. This log publishes the *reasoning* — including the wrong turns, because the wrong turns are where the design earned its shape. Entries record, with explicit attribution:

- initial idea: whose;
- implementation: whose;
- what review found, and who ran the review;
- what changed in the design or invariant as a result;
- the final decision and why.

## On AI leverage — stated plainly

I write the core C++ myself: architecture, data structures, algorithms, the debugging that matters, and the interpretation of results. I use Claude Code as an accelerator for the surrounding engineering — codebase exploration, scaffolding, tests, adversarial workloads, benchmark harnesses, profiling, review — and I also spar designs against AI assistants the way one would against colleagues. Attribution in this log runs **in both directions**: entries record when an AI found a real problem in my design, and when I found a real problem in an AI's proposal. Neither happens rarely. The point is to demonstrate technical judgment *and* honest tool leverage — not to pretend the tools don't exist, and not to pretend they drive.

## Exhibit: the design week (a preview of the format)

The Resolver conflict-set design went through seven iterations in one week of design sparring, each killing the previous one's weakness:

1. Hash-based tracking (my old school) — killed by a theorem: range read sets force *order*; clearRange forces *range values*; either alone kills the hash.
2. The inherited skip list — carries per-entry reclamation, the very cost to remove.
3. Per-epoch sealed runs (an "LSM where compaction is replaced by expiry") — objection: k searches multiply validation.
4. A global pointer-skeleton index over pages — killed by a second theorem: *a linked ordered index cannot die by a version floor* (navigation cannot jump through vanished nodes).
5. Generational index rebuild (Redis-rehash style) — correct, but heavier than needed.
6. Redundant pointers with era heal-sweeps — correct, finer invariants for the same budget.
7. **Two independent epoch skip lists with a one-integer band filter** — the first idea's structure with one number changed (epoch = window ⇒ k = 2 ⇒ effectively 1), and zero of the machinery the intermediate designs required.

Along the way: I mis-assumed batches arrive key-sorted (corrected — they arrive in version order; the sort is paid once and likely already exists); an AI proposal attributed the epoch architecture's virtues to a cross-linked variant (caught — the virtues are common; the differential is an insert tax for a rare-path benefit, reduced to the falsifiable criterion q\* = I/S); and an accounting error of mine about who creates retention (readers pin, writes produce) reshaped the cluster control chapter. Every one of these corrections made the design stronger, and every one is in the design docs' "alternatives considered" — with its author named.

That is the format. Entry 001 will map the real ConflictSet/SkipList code against the recollections the design was built on, and replace assumptions with measurements.

— Ricardo Jiménez-Peris

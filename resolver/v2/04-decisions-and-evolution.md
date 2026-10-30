# Resolver v2 · Decisions and evolution

[Index](README.md)

This record preserves experimental identities while the design document describes only the final behavior. It is not a substitute for a buildable public commit sequence.

| Development milestone | Identity | Design consequence |
|---|---|---|
| Frozen sequential v1 | `aff127286e` | Scalar reference algorithm and retained-history baseline |
| Representation preparation | `3f0fd8ad97`, `044e93a1fa`, `1e16451468`, `589013487d` | Per-type order/free maps, compile-time block size, one range-reference representation |
| Shared native batch records | `8e01939d26` | Grouped checks consume the native sorted endpoints and transaction/read records |
| v2 | `80b3b4a5e2` | Linked pending-read subset removes reads resolved by the scan |
| v2.1 | `fb4a6ea83d` | Rebuild task order only on completion/splitting; handle stored-key prefetch across blocks |
| v2.2 | `066953aed2` | Same-block prefetch fast path avoids unnecessary header traversal |
| v2.3 | `ade56c47de` | Remember the reached begin prefix instead of comparing it again at every write |
| v2.4 | `34a7791c6d` | Fuse a leaf's scan into one visit rather than scheduling each pair |
| v2.5 | `7809eea63f` | One consuming-state dispatch; continuation helpers issue subsequent hints |
| Read-check suite | `56b2b57d8a` | Sixteen grouped-read tests added to the component suite |
| v2.6 / public v2 baseline | `dc2802fc2f` | Refresh before hinting expired separator bytes; mark wholly empty internal nodes safely |

The early two-group prefetch plan evolved into G resident tasks, a new-task queue and a suspended-task stack. The final design has neither a global frontier nor separate current/next task-order arrays. A temporary combined index order is built only at structural scheduling changes.

Profiling changed the granularity choice. Fine-grained scan scheduling added instructions, branches and prefetch work around cheap comparisons. Fusing the leaf scan removed repeated dispatches per pair. Single dispatch subsequently improved the reported AMD read-check time by 6.6% on points and 7.4% on ranges against v2.4 in paired runs. Stage-attribution savings must not be confused with total savings, because some prefetch instructions moved into their consuming handlers' continuations.

The conflictset range workload also has a low commit rate (16.5%). Transaction rejection can make later reads unnecessary. Grouped navigation may begin before another task rejects that transaction, so fewer comparisons per checked read do not imply less total work than the scalar reference. The final implementation checks eligibility again in predecessor and scan processing; it does not guarantee that every unnecessary descent is eliminated.

Lazy separator repair was chosen over eagerly refreshing every child. It avoids touching separators a small slice never compares, at the cost of a synchronous refresh descent when one selected separator is expired. Correctness requires repair before the first hint as well as before comparison; a block may already have been reused for a different purpose.

The next milestone is v3. It may route a mixed read/write begin sequence once, defer applying writes until acceptance, record destination leaves and repair cursors through node/task associations. Those proposals change ownership and mutation rules and require a new design and validation; they must not be back-described as v2 features.

# Beyond-5s · Architecture at the resolver v2 milestone

[Index](README.md)

The local history component is `TempConflictBTree`, with storage provided by `PagePool`, `BNodeStore` and `KeyStore`. Its inputs are conflict intervals and transaction metadata, not database records or mutations. Its outputs are transaction/per-read conflict decisions and an observable reclamation cutoff.

| Boundary | Responsibility |
|---|---|
| Native FDB batch processing | Construct sorted endpoints and read records, apply transaction admission and native reporting rules, select accepted writes |
| `checkReads<G>` | Filter eligible reads, group navigation, mark conflicts/too-old transactions, maintain safe local cleanup |
| `addWrites` | Materialize accepted sorted disjoint write intervals at a common CV |
| `BNodeStore` | Node allocation, age tracking, slot/order primitives and release generations |
| `KeyStore` | Range-record append/comparison and generation-safe allocation-order reclamation |
| `PagePool` | Shared interchangeable node/key blocks within the configured history pool |
| Storage Server retention | Retain database versions required by admitted reads; separate component |
| Read-version protection | Coordinate admission, protection and safe distributed reclamation; separate protocol |

`ResolverBatchTypes.h` shares the native batch record definitions with the component. The standalone compatibility layer supplies substitutes for local tests. Matching type identity, include order, native reporting and explicit `checkReads` instantiations must be verified in the final FDB integration; a standalone component test does not prove adapter correctness.

Read validation and applying accepted writes are separate phases. Within a read-check call there is no concurrent history insertion or key-block reclamation, although leaf compaction and separator metadata refresh occur. After validation, the adapter applies accepted writes in its required transaction/version order.

The task scheduler is local to `checkReads`. Suspended records are copied into a vector, not held in a stable global task pool. Several tasks can partition one internal node or scan one leaf for different read slices. Neither tasks nor their current leaf destinations are a durable public interface.

The component is in-memory and has no persistent on-disk tree format. Deploying it still requires normal FDB correctness, simulation and integration validation. No distributed long-transaction protocol or production rollout result is asserted by this milestone.

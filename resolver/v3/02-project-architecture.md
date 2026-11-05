# Beyond-5s · Architecture at the resolver v3 milestone

[Index](README.md)

TempConflictBTree is the in-memory conflict-history component. It stores write-conflict intervals and versions, not database records. BTreeV3.cpp compiles it into fdbserver inside namespace btree_v3; ConflictSet supplies native sorted endpoints, transaction state and the selected commit version.

| Boundary | Responsibility |
|---|---|
| Native batch processing | Sort endpoints, provide transaction/read metadata and retain input key bytes for both phases |
| checkReads<G> | Check read conflicts and too-old transactions; route mixed begins; prepare ordered write-leaf destinations |
| addWrites | Filter admitted writes, union overlaps, schedule speculative searches and apply writes in order |
| BNodeStore | Node allocation, age links, physical slots and release generations |
| KeyStore | Stored range records and key-block allocation-order reclamation |
| PagePool | Shared fixed-budget blocks for nodes and keys |
| Storage Servers | Retain database versions needed by read versions; separate implementation |
| Commit Proxies / GRV | Admission and protection of read versions; separate protocol |

The two resolver phases are sequential. checkReads may clean leaf entries and refresh separators, but does not add writes or reclaim key blocks while the read scheduler is running. Its write-leaf records remain valid for the immediately following addWrites call only, with explicit redirection/validation when a leaf changes. addWrites may reserve space and reclaim history. The oldest write applies each overlay and maintains leaf links, separators and split state.

The read task capacity BTREE_V3_TASKS defaults to 16 in CMake. The write task defaults are RESOLVER1_WRITE_GROUPS=2 and RESOLVER1_WRITE_LG=8 in the source; these parameters control separate schedulers. 


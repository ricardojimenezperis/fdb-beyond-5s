# Beyond-5s · Project architecture at the resolver v1 milestone

[Documentation index](README.md)

## Scope and maturity

Beyond-5s aims to support transaction histories beyond a fixed short retention window. This snapshot documents one concrete local component: resolver v1. Storage Server work and transactional minRV tracking require their own designs and version identities; their implementation is outside this resolver milestone.

| Component | Responsibility | Status within this snapshot |
|---|---|---|
| Resolver conflict history | Determine conflicts against retained writes; expose reclamation cutoff | Sequential v1; evaluated under a 6 GiB history budget |
| Storage Server version retention | Preserve data versions needed by admitted snapshot reads; reclaim safely | Separate component; outside this milestone |
| Transactional minRV tracking | Track protected read versions and communicate a safe retention floor | Future design boundary in this snapshot |
| FDB adapter | Translate native transactions into tree operations and native results | Campaign integration revision `5a07ab193`; validation scope in the evidence register |

## Cross-component correctness

A longer resolver history is necessary for older conflict checks, but is not sufficient for a longer-running transaction: the storage layer must still have the data versions it needs. Conversely, preserving old values does not help if the resolver can no longer validate the transaction's reads.

A complete protocol must coordinate admission, protection and reclamation. It must define when a read version becomes protected, which processes hold that protection, how advancement is propagated, and what happens on failure or loss of communication. Those protocol guarantees require a separate design from the local tree implementation.

## Documentation and version model

Each component should have a current design, immutable milestone snapshots, a concise evolution record, and versioned measurement evidence. The shared journal records cross-component decisions without duplicating their full technical explanation.

For this milestone, the campaign combines resolver `aff127286e68ea7933dc9ef502a7dbd760e1e43d` with FDB integration `5a07ab193`.

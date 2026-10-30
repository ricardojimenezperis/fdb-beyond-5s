# FDB Beyond 5s

Extending FoundationDB beyond its five-second transaction window: budgeted conflict history in Resolvers, paged version storage in Storage Servers, and local history boundaries in Commit Proxies.

## Design

- [Overview](design/00-overview.md)
- [Commit Proxy](design/01-commitproxy.md)
- [Resolver](design/02-resolver.md)
- [Storage Server](design/03-storage.md)

`design/` holds the current design of each component and changes as the implementation evolves; the Resolver design currently describes resolver v2.

## Milestones

- [Resolver v1](resolver/v1/README.md) — sequential temporal B+ tree, evaluated under a 6 GiB history budget (23 October 2026)
- [Resolver v2](resolver/v2/03-resolver-v2-design.md) — grouped read checks with shared navigation and software prefetching; the design as implemented is published, the remaining documents follow

Each milestone is an immutable snapshot: design as implemented, decision record, journal, evaluation and evidence register.

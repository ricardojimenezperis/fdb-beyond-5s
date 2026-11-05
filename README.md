# FDB Beyond 5s

Extending FoundationDB beyond its five-second transaction window: budgeted conflict history in Resolvers, paged version storage in Storage Servers, and local history boundaries in Commit Proxies.

## Design

- [Overview](design/00-overview.md)
- [Commit Proxy](design/01-commitproxy.md)
- [Resolver](design/02-resolver.md)
- [Storage Server](design/03-storage.md)

The design documents describe the current selected implementation and evolve as the components change. The Resolver design now describes v3.8. Milestone snapshots below preserve each version's implemented design and evidence.

## Milestones

- [Resolver v1](resolver/v1/README.md) — sequential temporal B+ tree, evaluated under a 6 GiB history budget (16–22 October 2026)
- [Resolver v2](resolver/v2/README.md) — grouped read checks with shared navigation and software prefetching, evaluated on Intel, AMD and Apple M3 (23–31 October 2026)
- [Resolver v3](resolver/v3/README.md) — mixed read/write routing and two groups of eight interleaved write tasks; v3.8 is the selected release (1–4 November 2026)

The v3.9 fast-path experiment and the v3.10–v3.17 experimental line branch from v3.8. Their publication preserves the development record; the selected v3 implementation is v3.8.

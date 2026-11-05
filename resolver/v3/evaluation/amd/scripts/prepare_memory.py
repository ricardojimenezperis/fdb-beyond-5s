#!/usr/bin/env python3
"""Compact before allocation and record actual HugeTLB reservation. Run outside timing."""
import argparse
import datetime
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node", type=int, required=True)
    parser.add_argument("--pool-bytes", type=int, required=True)
    parser.add_argument("--reserve-hugetlb", action="store_true",
                        help="session setup only: increase this node's 2 MiB pool if needed; never reduce it")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.node < 0 or args.pool_bytes <= 0:
        parser.error("invalid node or pool size")
    node = Path(f"/sys/devices/system/node/node{args.node}")
    if not node.is_dir():
        parser.error("NUMA node does not exist")
    hp = node / "hugepages/hugepages-2048kB"
    compact = node / "compact"
    if not compact.exists():
        compact = Path("/proc/sys/vm/compact_memory")
    def state():
        return {"meminfo": Path("/proc/meminfo").read_text(),
                "node_meminfo": (node / "meminfo").read_text(),
                "vmstat": Path("/proc/vmstat").read_text(),
                "node_hugepages": {p.name: p.read_text().strip() for p in hp.glob("*") if p.is_file()},
                "thp_enabled": Path("/sys/kernel/mm/transparent_hugepage/enabled").read_text().strip()}
    record = {"timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "node": args.node, "pool_bytes": args.pool_bytes,
              "required_2MiB_pages": (args.pool_bytes + 2 * 1024 * 1024 - 1) // (2 * 1024 * 1024),
              "dry_run": args.dry_run, "compaction_path": str(compact), "before": state()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        if not args.dry_run:
            compact.write_text("1\n")
        record["compaction_completed"] = not args.dry_run
        if args.reserve_hugetlb:
            nr = int((hp / "nr_hugepages").read_text())
            free = int((hp / "free_hugepages").read_text())
            # Conservatively deduct all existing 2 MiB reservations from node free.
            reserved = int(Path("/sys/kernel/mm/hugepages/hugepages-2048kB/resv_hugepages").read_text())
            usable = max(0, free - reserved)
            target = nr + max(0, record["required_2MiB_pages"] - usable)
            record.update({"old_nr_hugepages": nr, "target_nr_hugepages": target,
                           "global_reserved_2MiB_pages": reserved})
            if not args.dry_run and target > nr:
                (hp / "nr_hugepages").write_text(str(target) + "\n")
            if not args.dry_run:
                achieved = max(0, int((hp / "free_hugepages").read_text()) - reserved)
                record["achieved_unreserved_node_pages"] = achieved
                if achieved < record["required_2MiB_pages"]:
                    raise RuntimeError("insufficient HugeTLB pages after compaction/reservation; do not run a huge-labelled arm")
    except Exception as error:
        record["error"] = str(error)
    record["after"] = state()
    args.output.write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({k: v for k, v in record.items() if k not in ("before", "after")}, indent=2))
    return 2 if "error" in record else 0


if __name__ == "__main__":
    raise SystemExit(main())

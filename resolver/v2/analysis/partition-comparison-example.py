"""Count routing comparisons in a declared synthetic tree, not CPU timings.

Python 3, no dependencies. Run from any directory; JSON is printed to stdout.
The geometry models 70% occupancy of 40-child / 59-entry capacities, except
for a two-child root. All history is live. Reads are between history begins,
so equality shortcuts do not influence the comparison.
"""
import bisect
import json
import random

BRANCHES = [2, 28, 28, 28, 28]
LEAF_ENTRIES = 41
READS = 10_000
SEED = 1
BLOCK_BYTES = 2048
POOL_PAGES = 98_300
PAGE_BYTES = 65536
KEY_HEADER_BYTES = 24


def model():
    counts = [1]
    for branch in BRANCHES:
        counts.append(counts[-1] * branch)
    node_blocks = sum(counts)
    entries = counts[-1] * LEAF_ENTRIES
    key_blocks = POOL_PAGES * (PAGE_BYTES // BLOCK_BYTES) - node_blocks
    return {
        'pool_bytes': POOL_PAGES * PAGE_BYTES,
        'nodes_by_depth': counts,
        'node_blocks': node_blocks,
        'node_bytes': node_blocks * BLOCK_BYTES,
        'leaves': counts[-1],
        'history_intervals': entries,
        'remaining_key_blocks': key_blocks,
        'remaining_key_bytes': key_blocks * BLOCK_BYTES,
        'key_payload_bytes_per_interval_if_filled':
            key_blocks * (BLOCK_BYTES - KEY_HEADER_BYTES) / entries,
    }


def count_case(reads):
    depth_count = len(BRANCHES)
    scalar = [0] * (depth_count + 1)
    v1_style = [0] * (depth_count + 1)
    grouped = [0] * (depth_count + 1)
    visited = [0] * (depth_count + 1)
    spans = [LEAF_ENTRIES]
    for branch in reversed(BRANCHES):
        spans.append(spans[-1] * branch)
    spans.reverse()

    def binary_search(query, start, stride, lo, hi):
        comparisons = 0
        while lo < hi:
            mid = (lo + hi) // 2
            comparisons += 1
            key = 2 * (start + mid * stride)
            if query < key:
                hi = mid
            else:
                lo = mid + 1
        return comparisons

    for query in reads:
        start = 0
        for depth, branch in enumerate(BRANCHES):
            stride = spans[depth + 1]
            scalar[depth] += binary_search(query, start, stride, 1, branch)
            v1_style[depth] += binary_search(query, start, stride, 0, branch)
            child = (query // 2 - start) // stride
            start += child * stride
        scalar[-1] += binary_search(query, start, 1, 0, LEAF_ENTRIES)
        v1_style[-1] += binary_search(query, start, 1, 0, LEAF_ENTRIES)

    def visit(depth, start, read_lo, read_hi):
        if read_lo == read_hi:
            return
        assert 2 * start <= reads[read_lo]
        assert reads[read_hi - 1] < 2 * (start + spans[depth])
        visited[depth] += 1
        if depth == depth_count:
            grouped[depth] += binary_search(reads[read_lo], start, 1, 0, LEAF_ENTRIES)
            return
        branch = BRANCHES[depth]
        stride = spans[depth + 1]

        def partition(lo, hi, first, last):
            if first == last:
                return
            if lo == hi:
                visit(depth + 1, start + (lo - 1) * stride, first, last)
                return
            mid = (lo + hi) // 2
            key = 2 * (start + mid * stride)
            left, right = first, last
            while left < right:
                middle_read = (left + right) // 2
                grouped[depth] += 1
                if reads[middle_read] < key:
                    left = middle_read + 1
                else:
                    right = middle_read
            split = left
            # Check the counted lower_bound independently.
            assert split == bisect.bisect_left(reads, key, first, last)
            partition(lo, mid, first, split)
            partition(mid + 1, hi, split, last)

        partition(1, branch, read_lo, read_hi)

    visit(0, 0, 0, len(reads))
    scalar_total = sum(scalar)
    grouped_total = sum(grouped)
    return {
        'reads': len(reads),
        'independent_boundary_comparisons_by_depth': scalar,
        'v1_style_including_minimum_comparisons_by_depth': v1_style,
        'grouped_comparisons_by_depth': grouped,
        'visited_nodes_by_depth': visited,
        'independent_internal_comparisons': sum(scalar[:-1]),
        'grouped_internal_comparisons': sum(grouped[:-1]),
        'independent_total_locating_comparisons': scalar_total,
        'grouped_total_locating_comparisons': grouped_total,
        'locating_comparison_reduction_percent': 100 * (1 - grouped_total / scalar_total),
        'locating_comparison_ratio': scalar_total / grouped_total,
        'excluded': ['predecessor checks', 'overlap scans', 'eligibility checks',
                     'cleanup', 'separator repair', 'scheduler', 'key-byte costs',
                     'prefetch', 'input extraction and sorting'],
    }


if __name__ == '__main__':
    geometry = model()
    n = geometry['history_intervals']
    rng = random.Random(SEED)
    uniform = sorted(2 * rng.randrange(n) + 1 for _ in range(READS))
    rng = random.Random(SEED)
    width = 100_000
    start = n // 2
    clustered = sorted(2 * (start + rng.randrange(width)) + 1 for _ in range(READS))
    print(json.dumps({'kind': 'synthetic comparison-count model, not a benchmark',
                      'seed': SEED, 'geometry': geometry,
                      'uniform': count_case(uniform),
                      'clustered_in_100000_history_entries': count_case(clustered)},
                     indent=2))

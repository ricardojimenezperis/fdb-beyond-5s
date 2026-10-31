"""Expected comparisons for the uniform 10,000-read example, without sampling.

Python 3, NumPy and SciPy. Prints JSON to stdout. The tree geometry and read
distribution match partition-comparison-example.py: five internal levels,
41 intervals per leaf, and independent uniform read begins drawn with replacement.

Each partition covers c of its node's B children. Its population K is binomial
with probability c / (B * nodes_at_depth). Conditional on K=n, its lower_bound
insertion rank is binomial with probability left_children/c. We average the
actual binary-search depths over these distributions and sum over partitions.
The K distribution is truncated at its 1e-13 and 1-1e-13 quantiles; discarded
tail mass is not renormalized. Results are numerical expectations, not timings.
"""

from collections import Counter
from functools import lru_cache
import json
import math

import numpy as np
from scipy.stats import binom

READS = 10_000
BRANCHES = [2, 28, 28, 28, 28]
LEAF_ENTRIES = 41
TAIL_PROBABILITY = 1e-13


def search_depths(n):
    """Binary-search comparisons for each possible insertion rank, 0..n."""
    ranks = np.arange(n + 1)
    lo = np.zeros(n + 1, dtype=np.int32)
    hi = np.full(n + 1, n, dtype=np.int32)
    result = np.zeros(n + 1, dtype=np.int32)
    while np.any(lo < hi):
        live = lo < hi
        mid = (lo + hi) // 2
        result[live] += 1
        go_right = live & (mid < ranks)
        lo[go_right] = mid[go_right] + 1
        go_left = live & ~go_right
        hi[go_left] = mid[go_left]
    return result


@lru_cache(None)
def lower_bound_expectation(n, left_children, children):
    if n == 0:
        return 0.0
    probabilities = binom.pmf(np.arange(n + 1), n, left_children / children)
    return float(np.dot(search_depths(n), probabilities))


def partition_groups(children):
    """Multiplicity of each (left children, total children) partition shape."""
    groups = Counter()

    def visit(count):
        if count <= 1:
            return
        left = (count + 1) // 2
        groups[(left, count)] += 1
        visit(left)
        visit(count - left)

    visit(children)
    return groups


def expected_partition(node_count, children):
    result = 0.0
    for (left, count), multiplicity in partition_groups(children).items():
        probability = count / (children * node_count)
        if probability == 1.0:
            subtotal = lower_bound_expectation(READS, left, count)
        else:
            first = max(0, int(binom.ppf(TAIL_PROBABILITY, READS, probability)))
            last = min(READS, int(binom.ppf(1 - TAIL_PROBABILITY, READS, probability)))
            populations = np.arange(first, last + 1)
            weights = binom.pmf(populations, READS, probability)
            subtotal = sum(
                float(weight) * lower_bound_expectation(int(n), left, count)
                for n, weight in zip(populations, weights)
            )
        result += multiplicity * subtotal
    return node_count * result


def expected_visited(nodes):
    if nodes == 1:
        return 1.0
    return nodes * (-math.expm1(READS * math.log1p(-1 / nodes)))


def main():
    rows = []
    nodes = 1
    for level, children in enumerate(BRANCHES):
        rows.append({
            'level': level,
            'nodes': nodes,
            'boundaries_per_node': children - 1,
            'mean_reads': READS / nodes,
            'expected_visited_nodes': expected_visited(nodes),
            'mean_reads_if_visited': READS / expected_visited(nodes),
            'independent': READS * float(np.mean(search_depths(children - 1))),
            'grouped': expected_partition(nodes, children),
        })
        nodes *= children

    # Queries lie immediately after stored entry begins: insertion ranks 1..41.
    # A grouped leaf search uses the minimum read begin in that occupied leaf.
    leaf_depths = search_depths(LEAF_ENTRIES)[1:]
    probability_per_entry = 1 / (nodes * LEAF_ENTRIES)
    minimum_probabilities = []
    for rank in range(LEAF_ENTRIES):
        before = READS * math.log1p(-rank * probability_per_entry)
        after = READS * math.log1p(-(rank + 1) * probability_per_entry)
        minimum_probabilities.append(math.exp(before) * (-math.expm1(after - before)))
    rows.append({
        'level': len(BRANCHES),
        'nodes': nodes,
        'entries_per_leaf': LEAF_ENTRIES,
        'mean_reads': READS / nodes,
        'expected_visited_nodes': expected_visited(nodes),
        'mean_reads_if_visited': READS / expected_visited(nodes),
        'independent': READS * float(np.mean(leaf_depths)),
        'grouped': nodes * float(np.dot(leaf_depths, minimum_probabilities)),
    })
    print(json.dumps({
        'kind': 'analytical comparison expectation, not a benchmark',
        'reads': READS,
        'children_by_internal_depth': BRANCHES,
        'entries_per_leaf': LEAF_ENTRIES,
        'population_tail_probability': TAIL_PROBABILITY,
        'rows': rows,
        'independent_internal': sum(row['independent'] for row in rows[:-1]),
        'grouped_internal': sum(row['grouped'] for row in rows[:-1]),
        'independent_total': sum(row['independent'] for row in rows),
        'grouped_total': sum(row['grouped'] for row in rows),
    }, indent=2))


if __name__ == '__main__':
    main()

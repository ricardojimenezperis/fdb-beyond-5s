#!/usr/bin/env python3
# Build patch for btree-v1 and btree-v2 binaries, which resolve every batch with
# TempConflictBTree (resolver1) whatever the knobs select. Applied to a copy of the
# sources after by5-membuf.py (native timing) or by5-acct-v107.py (accounting), it
# makes by5resolvertest treat that history as a capacity history, like v107's:
# - no window (BY5_WINDOW_US must be 0 or unset): reads are limited only by the
#   tree's boundary, reclaimCV, which getConflictSetStats reports as validationMinRV;
# - a read rejected below that boundary counts as rejected for memory;
# - the accounting build's history_bytes is the tree's pool, which it allocates and
#   touches whole at construction (PAGED_RESOLVER_POOL_PAGES x 65,536 bytes), plus
#   the persistent memory outside it: for btree-v2, the capacity of its list of
#   nodes to clean (pending_), which keeps its capacity between batches;
#   btree-v1 has nothing outside its pool.
# The binary must run with --knob_resolver_use_paged_conflict_set=0, so that the
# harness takes no paged B+ path.
import sys

D = sys.argv[1].rstrip('/') + '/'
cs = open(D + 'ConflictSet.cpp').read()
if 'btree_v2::TempConflictBTree btreeV2;' in cs:
    member = 'btreeV2'
elif 'btree_v1::TempConflictBTree btreeV1;' in cs:
    member = 'btreeV1'
else:
    sys.exit('not a btree-v1 or btree-v2 ConflictSet.cpp')

p = D + 'By5ResolverTest.inc'
s = open(p).read()
acct = 'int64_t history=btree?pool+aux:live' in s


def rep(a, b):
    global s
    assert s.count(a) == 1, (a[:80], s.count(a))
    s = s.replace(a, b)


head = '#define BY5_TEMP_BTREE 1\n'
if acct:
    if member == 'btreeV2':
        head += ('struct By5TPending {};\nauto by5Td(By5TPending);\n'
                 'template struct By5Steal<By5TPending, &btree_v2::TempConflictBTree::pending_>;\n'
                 'static int64_t by5TempOutsidePool(ConflictSet* cs) { const auto& v = cs->btreeV2.*by5Td(By5TPending{}); '
                 'return int64_t(v.capacity() * sizeof(v[0])); }\n')
    else:
        head += 'static int64_t by5TempOutsidePool(ConflictSet*) { return 0; }\n'
    # After the member-pointer helpers that by5-acct-v107.py puts at the top of the file
    anchor = 'template <class Tag> int64_t by5ThreadFree(int64_t size)'
    i = s.index(anchor)
    j = s.index('\n', i) + 1
    s = s[:j] + head + s[j:]
else:
    s = head + s

rep('bool btree=backend==ConflictHistory::PagedBTree;',
    'bool btree=backend==ConflictHistory::PagedBTree;'
    'by5::require(!btree,"btree-v1/v2 resolve with TempConflictBTree: run with --knob_resolver_use_paged_conflict_set=0");'
    'const bool capacity=true;')
rep('auto window=by5Env("BY5_WINDOW_US",btree?0:5000000);',
    'auto window=by5Env("BY5_WINDOW_US",capacity?0:5000000);'
    'by5::require(window==0,"btree-v1/v2 keep history by capacity: BY5_WINDOW_US must be 0");')
rep(' std::unique_ptr<ConflictSet> cs(newConflictSet());by5::History model;\n',
    ' std::unique_ptr<ConflictSet> cs(newConflictSet());by5::History model;\n'
    ' by5::require(std::strncmp(conflictSetIndexName(cs.get()),"btree-v",7)==0,"not a btree-v1/v2 binary");'
    'printf("by5tempbtree index=%s poolPages=%lld\\n",conflictSetIndexName(cs.get()),(long long)SERVER_KNOBS->PAGED_RESOLVER_POOL_PAGES);\n')
rep('mr=!wr && btree && rvs[i]<before.validationMinRV;', 'mr=!wr && capacity && rvs[i]<before.validationMinRV;')
rep(':btree?now-after.validationMinRV:now-floor)', ':capacity?now-after.validationMinRV:now-floor)')
if acct:
    rep('   int64_t history=btree?pool+aux:live,',
        '   pool=int64_t(SERVER_KNOBS->PAGED_RESOLVER_POOL_PAGES)*65536;used=pool;aux=by5TempOutsidePool(cs.get());\n'
        '   int64_t history=capacity?pool+aux:live,')
    rep('btree?"v104":"fdb"', 'conflictSetIndexName(cs.get())')
open(p, 'w').write(s)
print('by5-tempbtree: %s, %s build' % (member, 'accounting' if acct else 'native'))

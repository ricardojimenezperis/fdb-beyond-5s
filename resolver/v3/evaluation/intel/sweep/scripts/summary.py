import re, statistics as st, sys
C = sys.argv[1] if len(sys.argv) > 1 else '$CAMPAIGNS/2026-11-05-v38sweep-hp'
R = {}
for l in open(C + '/runs/runs.txt'):
    d = dict(re.findall(r'(\w+)=([^\s]+)', l))
    if d['run'] != 'warm':
        R[(d['arm'], d['workload'], d['run'])] = d
arms = ['2x8', '2x10', '2x12', '3x12', '3x20']
rounds = ('r1', 'r2', 'r3')
print('workload,config,totalS_r1,totalS_r2,totalS_r3,totalS_median,spread_pct,ratio_r1,ratio_r2,ratio_r3,ratio_median,prepS_median,readS_median,writeS_median,write_ratio_median,decisions')
for w in ('points', 'ranges'):
    for a in arms:
        t = [float(R[(a, w, r)]['totalS']) for r in rounds]
        q = [float(R[(a, w, r)]['totalS']) / float(R[('2x8', w, r)]['totalS']) for r in rounds]
        qw = [float(R[(a, w, r)]['writeS']) / float(R[('2x8', w, r)]['writeS']) for r in rounds]
        ph = [st.median(float(R[(a, w, r)][k]) for r in rounds) for k in ('prepS', 'readS', 'writeS')]
        dec = '|'.join(sorted({R[(a, w, r)]['decisions'] for r in rounds}))
        print(f'{w},{a},' + ','.join('%.3f' % x for x in t) + f',{st.median(t):.3f},{(max(t) - min(t)) / st.median(t) * 100:.2f},' +
              ','.join('%.4f' % x for x in q) + f',{st.median(q):.4f},' + ','.join('%.3f' % x for x in ph) + f',{st.median(qw):.4f},{dec}')

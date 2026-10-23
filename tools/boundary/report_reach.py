"""Summarize reach_<tag>_<scenario>.json: icalls by provider/namespace, pinvokes by lib.
usage: python report_reach.py <tag> <scenario> [-v]"""
import sys, os, json, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bconfig
tag, scen = sys.argv[1], sys.argv[2]
d = json.load(open(bconfig.out(f'reach_{tag}_{scen}.json')))
nat = d['native']
ic = [n for n in nat if n['kind'] == 'icall']; pv = [n for n in nat if n['kind'] == 'pinvoke']
print(f"## {tag}/{scen}: methods={d['reachable_methods']} icalls={len(ic)} pinvokes={len(pv)}")
def ns(k):
    t = k.split('::')[0]; return t.rsplit('.', 1)[0] if '.' in t else t
c = collections.Counter((n['provider'].split('(')[0], ns(n['key'])) for n in ic)
print('### icalls by provider / namespace')
for (p, n), v in sorted(c.items(), key=lambda x: (x[0][0], -x[1])): print(f'  {v:4d}  {p:28s} {n}')
print('### pinvokes by lib (provider)')
c2 = collections.Counter((n['lib'], n['provider'].split('+')[0]) for n in pv)
for (l, p), v in c2.most_common(): print(f'  {v:4d}  {l:45s} {p}')
if '-v' in sys.argv:
    print('### sony icalls list')
    for n in sorted(ic, key=lambda x: x['key']):
        if not n['provider'].startswith('mono'): print('   ', n['provider'][:25], n['key'])

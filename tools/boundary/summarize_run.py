"""Classify the dynamic first-frame boundary list (spikelog) with static attribution.
usage: python summarize_run.py <spikelog.txt>   (uses $SPIKE_OUT/{icalls,pinvokes}_attributed_<SPIKE_TAG>.json)"""
import json, sys, os, collections, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bconfig
log = sys.argv[1]
ic = {r['key']: r['provider'] for r in json.load(open(bconfig.out(f'icalls_attributed_{bconfig.TAG}.json')))}
pv = {(r['lib'], r['entry']): r['provider'] for r in json.load(open(bconfig.out(f'pinvokes_attributed_{bconfig.TAG}.json')))}
rows = []
for line in open(log, encoding='utf-8', errors='replace'):
    p = line.rstrip('\n').split('\t')
    if p[0] == 'COUNT' or len(p) < 4: continue
    d = p[3]
    if d.startswith('icall '):
        k = d[6:]; rows.append(('icall', k, ic.get(k, '?')))
    else:
        m = re.match(r'pinvoke (\S+) \[(.*)!(.*)\]', d)
        rows.append(('pinvoke', m.group(1) + ' ' + m.group(2) + '!' + m.group(3), pv.get((m.group(2), m.group(3)), '?')))
print('distinct boundary entry points hit:', len(rows))
print(collections.Counter((r[0], r[2].split(':')[0] if r[0] == 'pinvoke' else r[2]) for r in rows))

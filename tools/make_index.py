# -*- coding: utf-8 -*-
"""Write samples/INDEX.txt: every rendered voice, grouped by language."""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'bin'))
from _nokia.profiles import PROFILES                    # noqa: E402

report = json.load(open(os.path.join(ROOT, 'samples', 'report.json')))
rows = [v for v in report['voices'] if v.get('ok')]

by_lang = {}
for v in rows:
    by_lang.setdefault((v['language_name'], v['language']), []).append(v)

lines = [
    'Nokia Klatt - rendered speech samples',
    '=' * 60,
    f"{report['spoke']} of {report['total']} voices rendered, "
    f"{report['generated']}",
    '',
    'A voice is one phone ROM speaking one language. Two phones that speak',
    'the same language are different builds of the engine and sound',
    'different, so both are listed. 16 kHz, 16-bit, mono.',
    '',
]

for (name, lang), items in sorted(by_lang.items()):
    lines.append(f'{name}  (language id {lang})')
    for v in sorted(items, key=lambda r: (r['build'], r['voice'])):
        phone = PROFILES[v['build']].short
        voice = v['voice'] or 'default'
        lines.append(f"    {phone:<14} {voice:<8} {v['seconds']:>5.2f}s   "
                     f"{v['path']}")
    lines.append('')

total = sum(v['seconds'] for v in rows)
lines += [
    '-' * 60,
    f'{len(rows)} samples, {total / 60:.1f} minutes of speech in total.',
    '',
    'Builds:',
]
for key in sorted({v['build'] for v in rows}):
    p = PROFILES[key]
    n = len([v for v in rows if v['build'] == key])
    langs = len({v['language'] for v in rows if v['build'] == key})
    lines.append(f'    {p.short:<14} {n:>3} voices, {langs:>2} languages   '
                 f'{p.description}')

out = os.path.join(ROOT, 'samples', 'INDEX.txt')
with open(out, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')
print(f'wrote {out} ({len(lines)} lines)')

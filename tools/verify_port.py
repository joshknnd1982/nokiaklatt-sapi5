# -*- coding: utf-8 -*-
"""Render every voice with the C++ engine and diff it against the Python one.

A port of an emulator is only correct if it produces the same samples. Anything
softer - "it speaks", "it sounds about right" - would not have caught a wrong
descriptor length or a mis-ordered scheduler turn.
"""
import argparse
import json
import os
import subprocess
import sys
import wave
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'bin'))

from phrases import phrase                                   # noqa: E402

VOICE_ENGINE_NAME = {'male': 'DefaultMale', 'female': 'DefaultFemale',
                     '': '-'}


def frames(path):
    with wave.open(path, 'rb') as w:
        return w.readframes(w.getnframes())


def run_one(args):
    exe, row, outdir = args
    src = os.path.join(ROOT, row['path'])
    dest = os.path.join(outdir, row['build'],
                        os.path.basename(row['path']))
    os.makedirs(os.path.dirname(dest), exist_ok=True)

    d = os.path.join(ROOT, 'bin', 'roms', row['build'])
    roms = [os.path.join(d, n) for n in os.listdir(d)
            if os.path.isfile(os.path.join(d, n))
            and not n.lower().endswith('.rpkg')]
    rom = max(roms, key=os.path.getsize)

    cmd = [exe, rom, os.path.join(d, 'files'), str(row['language']),
           VOICE_ENGINE_NAME[row['voice']], dest, phrase(row['language'])]
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding='utf-8', errors='replace')
    if proc.returncode != 0:
        return row, 'FAILED', (proc.stderr or proc.stdout).strip()[:160]

    try:
        same = frames(src) == frames(dest)
    except Exception as e:                                   # noqa: BLE001
        return row, 'FAILED', f'could not compare: {e}'

    speed = ''
    for token in proc.stdout.split():
        if token.endswith('x'):
            speed = token
    return row, ('SAME' if same else 'DIFFERS'), speed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=os.path.join(
        ROOT, 'build', 'x64', 'bin', 'Release', 'nk_render.exe'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'samples-cpp'))
    ap.add_argument('--jobs', type=int, default=6)
    args = ap.parse_args()

    report = json.load(open(os.path.join(ROOT, 'samples', 'report.json')))
    rows = [v for v in report['voices'] if v.get('ok')]
    print(f'checking {len(rows)} voice(s) against the Python renders')

    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        jobs = [(args.exe, row, args.out) for row in rows]
        for i, (row, verdict, note) in enumerate(pool.map(run_one, jobs), 1):
            results.append((row, verdict, note))
            if verdict != 'SAME' or i % 20 == 0 or i == len(rows):
                voice = f"-{row['voice']}" if row['voice'] else ''
                print(f"[{i:>3}/{len(rows)}] {verdict:<8} {row['build']:<7} "
                      f"{row['language']:>3} {row['locale']:<7}{voice:<8} "
                      f"{note}")

    same = sum(1 for _r, v, _n in results if v == 'SAME')
    print(f'\n{same}/{len(results)} byte-identical to the Python engine')
    for row, verdict, note in results:
        if verdict != 'SAME':
            print(f'  {verdict} {row["build"]}:{row["language"]} '
                  f'{row["voice"]}: {note}')
    return 0 if same == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())

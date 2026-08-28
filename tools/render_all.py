# -*- coding: utf-8 -*-
"""Render one WAV per (ROM build, language, voice name) the engine can speak.

Each CDevTTS is bound to one language for its lifetime and a second one in the
same emulator faults, so every combination needs its own emulator. That is
about a second each, and they are independent, so the sweep is run across a
pool of processes.

Writes samples/<build>/<lang id>-<code>-<voice>.wav and a report.json saying
what spoke, what refused, and how fast.
"""
import argparse
import json
import os
import sys
import time
import traceback
import wave
from concurrent.futures import ProcessPoolExecutor, as_completed

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, 'bin')
sys.path.insert(0, BIN)
sys.path.insert(0, HERE)

from phrases import phrase                                   # noqa: E402
from _nokia import languages as langtable                    # noqa: E402
from _nokia import romdir                                    # noqa: E402
from _nokia.profiles import PROFILES, WORKING                # noqa: E402

VOICE_NAMES = (('male', 'DefaultMale'), ('female', 'DefaultFemale'))
SAMPLE_RATE = 16000


def candidate_languages(tree, blocked=()):
    """Languages whose packages are all present, the way the driver decides.

    Synthesis needs the text-to-phoneme package (srsf_0_*), the voice data
    (srsf_2_*) and, on builds that ship any, the prosody package (srsf_4_*).
    """
    have = {}
    for _root, _dirs, names in os.walk(tree):
        for n in names:
            low = n.lower()
            if not low.startswith('srsf_') or not low.endswith('.bin'):
                continue
            parts = low[5:-4].split('_')
            if len(parts) != 2:
                continue
            try:
                kind, lang = int(parts[0]), int(parts[1])
            except ValueError:
                continue
            have.setdefault(kind, set()).add(lang)
    cand = have.get(0, set()) & have.get(2, set())
    if have.get(4):
        cand &= have[4]
    cand.discard(0)
    return sorted(cand - set(blocked))


def write_wav(path, pcm):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)


def render(job):
    """One (build, language, voice) in this worker's own emulator."""
    key, rom, tree, lang, suffix, engine_voice, outdir = job
    sys.path.insert(0, BIN)
    from _nokia.engine import Engine, EngineError

    code = langtable.locale(lang) or f'lang{lang}'
    name = f'{lang:03d}-{code}' + (f'-{suffix}' if suffix else '')
    out = os.path.join(outdir, key, name + '.wav')
    row = {'build': key, 'language': lang,
           'language_name': langtable.name(lang), 'locale': code,
           'voice': suffix or '', 'path': os.path.relpath(out, ROOT)}

    started = time.monotonic()
    engine = None
    try:
        engine = Engine(rom, tree, lang, voice=engine_voice)
        build_time = time.monotonic() - started
        pcm = bytearray()
        t0 = time.monotonic()
        engine.speak(phrase(lang), pcm.extend)
        speak_time = time.monotonic() - t0
        if not pcm:
            row.update(ok=False, error='no audio produced')
            return row
        write_wav(out, bytes(pcm))
        seconds = len(pcm) / 2.0 / SAMPLE_RATE
        row.update(ok=True, bytes=len(pcm), seconds=round(seconds, 2),
                   build_seconds=round(build_time, 2),
                   speak_seconds=round(speak_time, 2),
                   realtime=round(seconds / speak_time, 2) if speak_time else 0,
                   voice_applied=engine.voice_applied)
        return row
    except EngineError as e:
        row.update(ok=False, error=f'{e.kind}: {e}')
        return row
    except Exception as e:                                  # noqa: BLE001
        row.update(ok=False, error=f'{type(e).__name__}: {e}',
                   traceback=traceback.format_exc()[-800:])
        return row
    finally:
        if engine is not None:
            try:
                engine.close()
            except Exception:
                pass


def build_jobs(outdir, only_builds=None, only_langs=None):
    jobs = []
    for key in WORKING:
        if only_builds and key not in only_builds:
            continue
        d = os.path.join(BIN, 'roms', key)
        rom = romdir.find_rom_image(d)
        tree = os.path.join(d, 'files')
        if not rom or not os.path.isdir(tree):
            print(f'  {key}: no ROM or data tree, skipping')
            continue
        profile = PROFILES[key]
        langs = candidate_languages(tree, profile.blocked)
        if only_langs:
            langs = [l for l in langs if l in only_langs]
        # named_voices=False means the build is already known to ignore the
        # voice name, so male/female would be the same audio twice.
        entries = (VOICE_NAMES if profile.named_voices is not False
                   else ((None, ''),))
        for lang in langs:
            for suffix, engine_voice in entries:
                jobs.append((key, rom, tree, lang, suffix, engine_voice,
                             outdir))
    return jobs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(ROOT, 'samples'))
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--build', action='append')
    ap.add_argument('--lang', action='append', type=int)
    args = ap.parse_args()

    jobs = build_jobs(args.out, args.build, args.lang)
    print(f'{len(jobs)} voice(s) to render into {args.out}')
    os.makedirs(args.out, exist_ok=True)

    rows = []
    started = time.monotonic()
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(render, j): j for j in jobs}
        for i, fut in enumerate(as_completed(futures), 1):
            row = fut.result()
            rows.append(row)
            mark = 'ok  ' if row.get('ok') else 'FAIL'
            extra = (f"{row['seconds']:>5.2f}s  {row['realtime']:>5.2f}x"
                     if row.get('ok') else row.get('error', '')[:70])
            voice = f"-{row['voice']}" if row['voice'] else ''
            print(f"[{i:>3}/{len(jobs)}] {mark} {row['build']:<7} "
                  f"{row['language']:>3} {row['locale']:<7}{voice:<8} {extra}")

    rows.sort(key=lambda r: (r['build'], r['language'], r['voice']))
    ok = [r for r in rows if r.get('ok')]
    report = {
        'generated': time.strftime('%Y-%m-%d %H:%M:%S'),
        'total': len(rows), 'spoke': len(ok), 'failed': len(rows) - len(ok),
        'wall_seconds': round(time.monotonic() - started, 1),
        'voices': rows,
    }
    with open(os.path.join(args.out, 'report.json'), 'w') as f:
        json.dump(report, f, indent=1)

    print(f'\n{len(ok)}/{len(rows)} spoke in '
          f'{report["wall_seconds"]:.0f}s wall')
    for r in rows:
        if not r.get('ok'):
            print(f'  FAILED {r["build"]}:{r["language"]} '
                  f'{r["language_name"]} {r["voice"]}: {r.get("error")}')
    return 0 if len(ok) == len(rows) else 1


if __name__ == '__main__':
    sys.exit(main())

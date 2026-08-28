# -*- coding: utf-8 -*-
"""What does TTtsStyle actually control?

The add-on's engine.py asserts that AddStyleL rejects any iVolume but 100 and
ignores iRate, iQuality and iNlp - which is why it does rate and volume in
software afterwards. That claim decides which knobs the SAPI5 wrapper can hand
to the engine and which it has to emulate, so it is worth testing rather than
inheriting.

For each field: write a value, see whether AddStyleL accepts it, and hash the
audio to see whether it changed anything.
"""
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'bin'))

from _nokia.engine import Engine, EngineError            # noqa: E402
from _nokia.harness.devtts import Dev                    # noqa: E402
from _nokia.harness.epoc import SymbianLeave             # noqa: E402

FIELDS = {
    'iRate':         112,
    'iVolume':       116,
    'iSamplingRate': 120,
    'iQuality':      124,
    'iDuration':     128,
    'iNlp':          132,
}
TEXT = 'Testing one two three four.'


class Probe(Engine):
    """An engine that will add a style with one field overridden."""

    def style_with(self, field_offset=None, value=None):
        t = self.tts
        style = t.epoc.alloc(0x200)
        t.epoc.call(t.common_eps[15], (style,))          # TTtsStyle ctor
        t.uc.mem_write(style, struct.pack('<i', self.language))
        if field_offset is not None:
            t.uc.mem_write(style + field_offset, struct.pack('<i', value))
        return style

    def defaults(self):
        style = self.style_with()
        out = {}
        for name, off in FIELDS.items():
            out[name] = struct.unpack(
                '<i', self.tts.uc.mem_read(style + off, 4))[0]
        return out

    def try_field(self, field_offset, value):
        """(accepted, audio md5) for one field value, or (False, reason)."""
        self.style_id = None
        try:
            sid = self.tts.epoc.call_l(
                self.tts.eps[Dev.ADD_STYLE_L - 1],
                (self.dev, self.style_with(field_offset, value)))
        except SymbianLeave as e:
            return False, f'leave {e.code}'
        self.style_id = sid
        pcm = bytearray()
        try:
            self.speak(TEXT, pcm.extend)
        except EngineError as e:
            return False, f'speak failed: {e.kind}'
        if not pcm:
            return False, 'no audio'
        return True, f'{hashlib.md5(bytes(pcm)).hexdigest()[:10]} ' \
                     f'{len(pcm) // 2 / 16000.0:.2f}s'


def probe(key, rom, tree, language=1):
    print(f'\n===== {key} (language {language}) =====')
    eng = Probe(rom, tree, language, voice='')
    try:
        print('constructor defaults:', eng.defaults())
    finally:
        eng.close()

    # A baseline the variants are compared against.
    eng = Probe(rom, tree, language, voice='')
    try:
        ok, base = eng.try_field(None, None)
        print(f'baseline: {base}')
    finally:
        eng.close()

    trials = {
        'iRate':         (-1, 0, 50, 100, 150, 200, 32767),
        'iVolume':       (-1, 0, 25, 50, 75, 100, 150, 200),
        'iSamplingRate': (-1, 8000, 11025, 16000, 22050, 44100),
        'iQuality':      (-1, 0, 1, 2, 3),
        'iDuration':     (-1, 0, 1, 1000, 5000),
        'iNlp':          (-1, 0, 1, 2),
    }
    for name, values in trials.items():
        off = FIELDS[name]
        print(f'  {name} (offset {off}):')
        for v in values:
            eng = Probe(rom, tree, language, voice='')
            try:
                ok, info = eng.try_field(off, v)
            except Exception as e:                        # noqa: BLE001
                ok, info = False, f'{type(e).__name__}: {e}'
            finally:
                eng.close()
            same = ' (== baseline)' if ok and info == base else ''
            print(f'    {v:>7} -> {"ok  " if ok else "NO  "} {info}{same}')


if __name__ == '__main__':
    builds = sys.argv[1:] or ['5320']
    for key in builds:
        d = os.path.join(ROOT, 'bin', 'roms', key)
        roms = [os.path.join(d, n) for n in os.listdir(d)
                if os.path.isfile(os.path.join(d, n))
                and not n.lower().endswith('.rpkg')]
        rom = max(roms, key=os.path.getsize)
        probe(key, rom, os.path.join(d, 'files'))

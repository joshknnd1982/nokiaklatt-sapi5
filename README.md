# Nokia Klatt SAPI5

A 32-bit and 64-bit SAPI5 speech engine for the Klatt formant synthesiser that
Nokia shipped inside its S60 phones — recovered by running the phones' own ARM
firmware under emulation on Windows.

**139 voices**: 138 real ones across 39 languages, plus a Custom Voice that
follows the settings utility.

Download the installer from the
[Releases](https://github.com/joshknnd1982/nokiaklatt-sapi5/releases) page.

---

## What this is, and why it only exists like this

Between roughly 2005 and 2010, Nokia's S60 phones carried a small
formant speech synthesiser as part of their Speech Recognition and Synthesis
Framework (`srsf`). It read text messages aloud, spoke caller names over the
ringtone, and drove the phones' voice commands. It is a Klatt-style formant
synthesiser: it does not play back recorded human speech, it models the vocal
tract mathematically and generates the waveform from scratch. That is why it
runs in a few hundred kilobytes, why it can be sped up enormously without
turning to mush, and why it sounds the way it does.

**It is discontinued abandonware.** Nokia announced the end of Symbian in
February 2011, shipped its last Symbian handset in 2012, and sold its devices
division to Microsoft in 2014. The Symbian software store closed the same year.
This engine was never sold, packaged, or released on its own — it existed only
as a component inside phone firmware images, was never ported to the desktop,
and has had no vendor, no updates, and no support for well over a decade. There
is no official download and no one left to ask.

Being abandoned does not make it free to use. The firmware is still
Nokia's copyrighted work. Nothing here is a grant of rights to it; the ROM
images are redistributed on the same footing as every other Symbian firmware
archive that has been circulating for years, because without them the engine
cannot run at all. If you own a rights claim and want them gone, open an issue.

### Why the whole phone has to be emulated

The synthesiser is not a library. There is no `nokiatts.dll` to call. It is ARM
code living inside an execute-in-place firmware image, and it will not start
without a Symbian kernel underneath it, a file server to hand it its language
data, and an active scheduler to turn. So this project supplies exactly those
things and nothing more: the real ROM code runs under emulation, and everything
below the kernel boundary is reimplemented.

## The voices

A voice here is **one phone speaking one language**. Every phone carries its own
build of the engine with its own language set, and two phones speaking the same
language genuinely do not sound the same — so both are offered, and every voice
says in parentheses which phone it came from.

| Phone | Languages | Voices | Notes |
|---|---:|---:|---|
| Nokia 5320 XpressMusic | 33 | 66 | male and female |
| Nokia E65 | 30 | 30 | one voice per language |
| Nokia N95 8GB | 30 | 30 | one voice per language |
| Nokia 6650 Fold | 4 | 8 | the only source of en-US, fr-CA, pt-BR, es-419 |
| Nokia N85 | 2 | 4 | the only source of Tagalog and Vietnamese |

**39 languages**: Arabic, Basque, Bulgarian, Catalan, Croatian, Czech, Danish,
Dutch, English (UK, US), Estonian, Finnish, French (France, Canada), Galician,
German, Greek, Hebrew, Hungarian, Icelandic, Italian, Latvian, Lithuanian,
Norwegian, Polish, Portuguese (Portugal, Brazil), Romanian, Russian, Serbian,
Slovak, Slovenian, Spanish (Spain, Latin America), Swedish, Tagalog, Turkish,
Ukrainian, Vietnamese.

`samples/` holds a rendered example of all 138, with `samples/INDEX.txt` as the
listening index.

The 5320, 6650 and N85 builds honour Symbian's `DefaultMale` and
`DefaultFemale` voice names and really do sound different. The E65 and N95 8GB
builds reject any named voice, so they offer one voice per language rather than
the same audio twice.

## What the engine can and cannot be told

Every field of Symbian's `TTtsStyle` was probed against a real build by writing
a value and hashing the audio that came out. The result is unambiguous:

| Field | Behaviour |
|---|---|
| `iLanguage` | honoured |
| `iVoice` | honoured, on the builds that accept a named voice |
| `iRate` | **ignored** — byte-identical audio at every value |
| `iSamplingRate` | **ignored** |
| `iQuality` | **ignored** |
| `iNlp` | **ignored** |
| `iVolume` | **rejected** — anything but 100 leaves `KErrNotSupported` |
| `iDuration` | **rejected** — any value leaves `KErrNotSupported` |

So rate, pitch and volume cannot be handed to the engine and are applied to the
PCM afterwards (`src/nk/dsp.cpp`): WSOLA time-scaling for rate, and
time-scaling plus resampling for pitch, so the two are independent — rate
changes duration at constant pitch, pitch changes pitch at constant duration.
The phones themselves had no pitch control at all.

Two further engine-level parameters were found and are exposed:

- **Audio buffer size**, which the framework reads from `nssdevtts.rsc`. The
  phone's own 4096 bytes is 128 ms of audio that must exist before anything can
  play; 1024 starts speech about a third sooner and costs nothing in
  throughput.
- **Voice bank override.** The engine asks the host for its voice data by
  language, and that language is not always the one being spoken — the 5320
  speaking Polish asks for the *Russian* bank. Answering with a different one
  works, but a mismatched bank changes utterance length two- or three-fold, so
  it garbles rather than re-timbres. It is offered in the settings utility,
  defaulting to the engine's own choice.

## How it fits together

```
application
    |  SAPI5
    v
NokiaKlattSAPI.dll  (x86 and x64)          thin client
    |  named pipe, overlapped in both directions
    v
NokiaKlattHost.exe  (x64)                  owns the engines, keeps them warm
    |
    +-- Unicorn: ARM emulation of the phone's execute-in-place ROM
    +-- a minimal EPOC/EKA2 kernel surface below the SVC boundary
    +-- an F32 file server, for the engine's language packages
    +-- an E32 loader, for builds that keep their speech DLLs in ROFS
```

The emulator lives in its own process because the Unicorn build used here is
x64 only. That also means one warm engine cache is shared by every application
on the desktop, so returning to a language you have already used costs nothing.

Measured on a desktop of the era this was written:

| | |
|---|---|
| first audio, warm engine | ~80 ms |
| first audio, cold engine | ~260 ms |
| cancel | ~1.7 ms |
| synthesis speed | ~4.4× real time |

## Installing

Download the installer from
[Releases](https://github.com/joshknnd1982/nokiaklatt-sapi5/releases) and run it
**as administrator** — registering a SAPI voice enumerator writes to `HKLM`, and
SAPI silently ignores one registered under `HKCU`.

It installs both engines, the emulator host, the settings utility and the phone
firmware, registers each DLL with the `regsvr32` of its own architecture, and
offers a desktop shortcut. 64-bit Windows is required: the 32-bit SAPI engine is
installed and works, but it reaches the 64-bit host through a pipe.

### Choosing what gets installed

138 voices is a long list to arrow through, so every one of them is selectable
on the components page. Four presets sit above the tree:

| Preset | What it installs |
| --- | --- |
| Everything | all 138 voices from all five phones |
| English only | every English voice, from each phone that has one |
| Compact | English (UK) male, from the Nokia 5320 only |
| Custom | the tree, checked item by item |

The tree is grouped by phone, because a phone's languages are its own — the same
language from two phones sounds different. Under each phone are its languages,
and above them, on the phones that offer both, **Male voices** and **Female
voices**.

This shortens the voice list; it does not shrink the install much. Nearly all of
a phone's size is its ROM image, which every one of its languages shares, so
clearing languages saves kilobytes and clearing a whole phone saves tens of
megabytes. Reinstalling with a different selection is the supported way to
change your mind: the installer clears the per-language speech data first, so
languages you deselect really do leave the voice list.

## The settings utility

`NokiaKlattConfig.exe`, on the desktop and in the Start menu. Everything on it
takes effect on the next thing spoken — the host re-reads its settings once per
utterance — and is saved as it is changed, so there is no Apply button.

- **Custom voice**: which of the 138 voices it is built on, and its voice bank.
- **Speech**: speed, speed step size, pitch, pitch increment, volume.
- **Responsiveness**: audio buffer, how many engines to keep warm, whether to
  trim the silence before each utterance, extra silence after.
- **Try it out**: type something and hear it with the current settings.
- **Diagnostics**: turn the log file on, and open the folder holding it.

Every control is a labelled drop-down list rather than a slider. A trackbar
reports its position to MSAA as a percentage, so a nine-step control announces
step 5 as "55" and a screen reader user has no way to tell where they are.
`tools/check_accessibility.ps1` walks the dialog through MSAA — not UI
Automation, whose PowerShell client calls every Win32 control a nameless
"Pane" — and asserts that every interactive control has an accessible name and
a tab stop.

## Logs

The engine and the host write to `%LOCALAPPDATA%\NokiaKlatt\nokiaklatt.log` when
logging is on. Turn it on with the checkbox in the settings utility, the
installer's diagnostics task, or by setting `NOKIAKLATT_LOG=1`. The installer
keeps its own log as well; Windows puts it in `%TEMP%`.

## Building from source

Needs CMake, the Visual Studio 2022 build tools, and Inno Setup 6.

```
build_all.bat
```

That configures and builds both architectures and then compiles the installer
into `dist\`. Pass `--no-installer` to stop after the binaries.

**A clone is a working copy.** The phone firmware and the built binaries are
committed, so nothing has to be downloaded or built before the engine will
speak. That costs about 250 MB of clone, which is a deliberate trade — and one
that cannot be undone without rewriting history.

```
bin\NokiaKlattHost.exe       the emulator host
bin\NokiaKlattSAPI.dll       the 64-bit SAPI engine
bin\x86\NokiaKlattSAPI.dll   the 32-bit SAPI engine
bin\NokiaKlattConfig.exe     the settings utility
bin\unicorn.dll              the emulator, beside the host that loads it
bin\roms\<profile>\          each phone's ROM image and its files\ tree
bin\_nokia\lib\unicorn\lib\  the copy of unicorn.dll that the build and the
                             installer read from
```

Each ROM directory holds the ROM image and a `files\` tree with the `resource\`
and `system\data\` directories from the same firmware. The engine finds a ROM by
content rather than by name, so whatever the dump is called is fine.

The compiled installer is **not** committed. It is another hundred megabytes of
the same ROMs, and a blob committed once is in every clone forever, so each one
is attached to its
[release](https://github.com/joshknnd1982/nokiaklatt-sapi5/releases) instead.
`build_all.bat` writes a fresh one to `dist\`.

The rest of `bin\_nokia\` — the Python reference implementation the C++ core was
ported from — is not included. The Python test harnesses in `tools\` need it;
the build and the installer need only the `unicorn.dll` beneath it.

## Layout

```
src/nk/       the emulation core and the shared pipe client
src/host/     NokiaKlattHost.exe and its engine cache
src/sapi/     the SAPI5 engine, built for both architectures
src/config/   the settings utility
src/tools/    nk_render, nk_speak, nk_sapitest
tools/        Python and PowerShell test harnesses
samples/      a rendered example of all 138 voices, and INDEX.txt
installer/    the Inno Setup script, and its generated language components
```

`installer/rom_languages.iss` is the five phones, their 99 (phone, language)
components and the speech packages each one owns. It is generated — after adding
or removing a ROM, regenerate it with `python tools/gen_rom_components.py`, which
reads the language names out of `src/nk/catalog.cpp` so the installer and the
voice list cannot disagree.

The phones are generated together with their languages rather than being listed
in `nokia_klatt.iss`, and that is load-bearing. The wizard's components list is a
tree, but not one built from the component names: each entry is handed to the
checkbox control with a depth taken from its name, and the control decides
parents and children by *adjacency* — an entry's children are the entries that
follow it until one appears at its own depth or shallower. Splitting the phones
from their languages across two files put all 99 languages under whichever phone
came last. It compiled cleanly, because the compiler resolves parents by name;
it only went wrong in front of the person installing it.

`python tools/check_components.py` is what catches that. It reads both scripts
the way ISCC does, checks every component nests under its own name, and then
walks each preset through the wizard's own “this phone would add no voices” rule
to confirm the components page will let it through.

## Verification

The C++ core is a port of a Python emulation harness, and it is checked against
it rather than by ear. `tools/verify_port.py` renders all 138 voices with both
engines and compares the samples: **138/138 byte-identical**, including the
E65's ROFS-resident DLLs, which need Symbian's own non-RFC1951 inflate to load
at all. The C++ runs about twice as fast and builds an engine about three times
faster, mostly because it can map the ROM with `uc_mem_map_ptr` instead of
copying 70 MB into the emulator for every instance.

`src/tools/nk_sapitest.cpp` drives each SAPI DLL through its own class objects —
no registration, so both architectures can be tested — and asserts that
bookmarks fire as events instead of being read aloud, that a requested pause
produces real silence, and that the voice list, output format and boundary
events are what SAPI expects.

## Credits

The emulation approach, the ROM profiles and the Python harness the C++ core was
ported from come from the **Nokia TTS add-on for NVDA**. That work is what
established that these ROMs could be made to speak at all; this project moves it
to C++ and puts a SAPI5 interface on it so the voices are available to every
Windows application rather than to one screen reader.

Speech emulation runs on [Unicorn Engine](https://www.unicorn-engine.org/).

Nokia, S60 and Symbian are trademarks of their respective owners. This project
is not affiliated with, endorsed by, or supported by any of them.

#!/usr/bin/env python3
"""Generate installer\\rom_languages.iss from the ROM tree and the language table.

There are 99 (build, language) pairs across the five phone builds, and writing
their components and file entries by hand would be both tedious and a place for
a typo to silently drop a voice. The two facts this needs are already recorded
elsewhere in the tree, so it reads them rather than repeating them:

  - the language names come from kLanguages in src/nk/catalog.cpp, so the
    installer and the voice list cannot disagree about what language 42 is;
  - which languages a build actually offers is derived from the srsf packages
    on disk, by the same rule catalog.cpp's candidate_languages() uses;
  - how big a phone is, by measuring its ROM tree.

The phone components are generated here as well, rather than being written out
in nokia_klatt.iss, because the wizard builds its components tree out of the
order the entries appear in and the depth of each name. A phone has to be
immediately followed by its own languages, which is a property this file can
guarantee and two files kept in step by hand cannot.

Run it after adding or removing a ROM:

    python tools\\gen_rom_components.py
"""

import os
import re
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
ROMS = os.path.join(ROOT, "bin", "roms")
CATALOG = os.path.join(ROOT, "src", "nk", "catalog.cpp")
OUT = os.path.join(ROOT, "installer", "rom_languages.iss")

# The builds, in the order catalog.cpp's profiles() lists them, with the
# component name the main script gives each one and whether the build offers a
# male and a female voice per language (Profile::named_voices != 0).
BUILDS = [
    ("5320",   "p5320",   "Nokia 5320",   True),
    ("e65",    "pe65",    "Nokia E65",    False),
    ("n958gb", "pn958gb", "Nokia N95 8GB", False),
    ("6650",   "p6650",   "Nokia 6650",   True),
    ("n85",    "pn85",    "Nokia N85",    True),
]

# A phone whose languages are few enough to name is named, rather than counted:
# "Tagalog and Vietnamese" tells someone deciding more than "2 languages" does.
NAME_LANGUAGES_UP_TO = 4

# Languages a build has complete packages for and which fault the emulator
# anyway - Profile::blocked in catalog.cpp. Offering them here would put a
# component in the list that installs a voice the engine never shows.
BLOCKED = {"n85": {1}}


def language_names():
    """The id -> English name map, read out of kLanguages in catalog.cpp."""
    with open(CATALOG, "r", encoding="utf-8") as f:
        text = f.read()
    start = text.index("const LanguageInfo kLanguages[]")
    end = text.index("};", start)
    names = {}
    for m in re.finditer(r'\{(\d+),\s*L"([^"]+)"', text[start:end]):
        names[int(m.group(1))] = m.group(2)
    if not names:
        sys.exit("no languages found in %s" % CATALOG)
    return names


def data_dir(build):
    """A build's system\\data directory, whatever case the dump kept."""
    base = os.path.join(ROMS, build, "files")
    for parts in (("system", "data"), ("System", "data"), ("SYSTEM", "data")):
        candidate = os.path.join(base, *parts)
        if os.path.isdir(candidate):
            return candidate
    sys.exit("no data directory under %s" % base)


def inventory(build):
    """kind -> {language: filename} for every srsf package a build ships."""
    have = defaultdict(dict)
    for name in os.listdir(data_dir(build)):
        m = re.fullmatch(r"srsf_(\d+)_(\d+)\.bin", name, re.IGNORECASE)
        if m:
            have[int(m.group(1))][int(m.group(2))] = name
    return have


def candidate_languages(have):
    """The rule catalog.cpp uses: text-to-phoneme and a voice bank, narrowed by
    the prosody package on the builds that ship any."""
    cand = set(have.get(0, {})) & set(have.get(2, {}))
    prosody = set(have.get(4, {}))
    if prosody:
        cand &= prosody
    cand.discard(0)  # ELangTest, not a real voice
    return cand


def escape(text):
    """Inno Setup takes a doubled quote inside a quoted string."""
    return text.replace('"', '""')


def rom_size_mb(build):
    """How much disk a phone costs, which is very nearly all ROM image.

    Read off the tree rather than written down, so the number in the
    components list cannot drift away from the dump it describes.
    """
    total = 0
    for dirpath, _, filenames in os.walk(os.path.join(ROMS, build)):
        for name in filenames:
            total += os.path.getsize(os.path.join(dirpath, name))
    return int(round(total / (1024.0 * 1024.0)))


def phone_description(short_name, langs, names, has_genders, size_mb):
    """The one line that has to carry a whole phone's worth of decision."""
    if len(langs) <= NAME_LANGUAGES_UP_TO:
        spelled = [names.get(l, "Language %d" % l) for l in langs]
        if len(spelled) > 1:
            what = ", ".join(spelled[:-1]) + " and " + spelled[-1]
        else:
            what = spelled[0]
    else:
        what = "%d languages" % len(langs)
        if has_genders:
            what += ", male and female"
    return "%s - %s (%d MB)" % (short_name, what, size_mb)


def main():
    names = language_names()
    out = []
    w = out.append

    w("; Generated by tools\\gen_rom_components.py - do not edit by hand.")
    w(";")
    w("; One component per (phone, language) pair, and the package files that")
    w("; pair owns. A language whose text-to-phoneme and prosody packages are")
    w("; not laid down is not a candidate language when the engine scans the")
    w("; data tree, so its voices never reach the voice list.")
    w(";")
    w("; The voice banks (srsf kind 2) are deliberately NOT split by language:")
    w("; the engine asks for a bank by an id that is not always the language")
    w("; being spoken, because builds share banks between languages. They come")
    w("; to well under a megabyte per phone, so they all come along with the")
    w("; phone and only the per-language packages are selectable.")
    w(";")
    w("; The phone components are here too, each one directly above the entries")
    w("; that belong to it. The wizard nests its components list by adjacency")
    w("; and depth rather than by name, so a phone and its languages have to be")
    w("; kept together: split them up and the languages become children of")
    w("; whichever phone happens to be listed last.")
    w("")

    totals = {}
    components = []
    files = []

    for build, comp, short_name, has_genders in BUILDS:
        have = inventory(build)
        langs = sorted(candidate_languages(have) - BLOCKED.get(build, set()))
        totals[build] = (len(langs), len(langs) * (2 if has_genders else 1))

        english = sorted(l for l in langs if names.get(l, "").startswith("English"))
        # The compact preset is one phone and one voice: English (UK) male from
        # the Nokia 5320, the smallest install that still speaks.
        compact = build == "5320"

        components.append("; ---- %s ----" % short_name)

        # The phone itself, immediately before the entries that belong to it.
        # The wizard's components list is a tree built from adjacency and
        # indent depth, not from the component names: everything that follows
        # a phone at a deeper level is treated as that phone's child. Emitting
        # all five phones first and their languages afterwards therefore hung
        # every language in the package off whichever phone came last, which
        # is how checking English used to leave the Nokia N85 half-checked and
        # clearing the N85 used to clear every other phone's languages too.
        phone_types = ["full", "custom"] + (["english"] if english else [])
        if compact:
            phone_types.append("compact")
        components.append(
            'Name: "roms\\%s"; Description: "%s"; Types: %s'
            % (comp,
               escape(phone_description(short_name, langs, names,
                                        has_genders, rom_size_mb(build))),
               " ".join(sorted(phone_types))))

        if has_genders:
            male_types = ["full", "custom"] + (["english"] if english else [])
            if compact:
                male_types.append("compact")
            components.append(
                'Name: "roms\\%s\\male"; Description: "Male voices"; Types: %s'
                % (comp, " ".join(sorted(male_types))))
            components.append(
                'Name: "roms\\%s\\female"; Description: "Female voices"; Types: %s'
                % (comp, " ".join(sorted(
                    ["full", "custom"] + (["english"] if english else [])))))
        components.append("")

        files.append("; ---- %s ----" % short_name)

        for lang in langs:
            name = names.get(lang, "Language %d" % lang)
            leaf = "roms\\%s\\l%d" % (comp, lang)

            # An English variant is what someone who wants a short voice list
            # almost always wants, so the presets are built around them.
            types = ["full", "custom"]
            if name.startswith("English"):
                types.append("english")
            if compact and name == "English (UK)":
                types.append("compact")

            components.append(
                'Name: "%s"; Description: "%s"; Types: %s'
                % (leaf, escape(name), " ".join(sorted(types))))

            for kind in (0, 4):
                filename = have.get(kind, {}).get(lang)
                if not filename:
                    continue
                files.append(
                    'Source: "{#RomRoot}\\%s\\files\\system\\data\\%s"; '
                    'DestDir: "{app}\\roms\\%s\\files\\system\\data"; '
                    'Flags: ignoreversion; Components: %s'
                    % (build, filename, build, leaf))

        components.append("")
        files.append("")

    w("[Components]")
    w("")
    out.extend(components)

    w("[Files]")
    w("")
    out.extend(files)

    with open(OUT, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(out).rstrip() + "\n")

    print("wrote %s" % OUT)
    grand_langs = grand_voices = 0
    for build, comp, short_name, _ in BUILDS:
        n_lang, n_voice = totals[build]
        grand_langs += n_lang
        grand_voices += n_voice
        print("  %-14s %2d languages, %3d voices" % (short_name, n_lang, n_voice))
    print("  %-14s %2d language components, %3d voices"
          % ("total", grand_langs, grand_voices))


if __name__ == "__main__":
    main()

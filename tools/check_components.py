#!/usr/bin/env python3
"""Check the installer's components tree, and every preset it offers.

The wizard's components list is a tree, but not a tree built out of the
component names. Inno Setup hands the list to a checkbox control one entry at
a time along with a depth taken from the number of backslashes in the name,
and that control works out parents and children from *adjacency*: an entry's
children are the entries that follow it, up to the next one at its own depth
or shallower. The names are never consulted.

Which means a component can be a child of the wrong thing while still
compiling without a murmur, because the compiler resolves parents by name.
That is the bug this file exists to catch. When the five phones were listed
in nokia_klatt.iss and their languages arrived afterwards from the include,
every language in the package became a child of the Nokia N85 - it was the
last phone in the list - and so:

  - the English preset left the N85 half-checked, and the wizard refused to
    go on until an N85 voice was chosen;
  - clearing the N85 cleared every other phone's languages along with it.

Run it after touching either .iss file:

    python tools\\check_components.py
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
# A path can be given, so the check can be pointed at an older copy of the
# script to confirm it still catches what it was written for.
MAIN = (sys.argv[1] if len(sys.argv) > 1
        else os.path.join(ROOT, "installer", "nokia_klatt.iss"))

NAME_RE = re.compile(r'^\s*Name:\s*"([^"]+)"', re.IGNORECASE)
TYPES_RE = re.compile(r'\bTypes:\s*([^;]+)', re.IGNORECASE)
FLAGS_RE = re.compile(r'\bFlags:\s*([^;]+)', re.IGNORECASE)
INCLUDE_RE = re.compile(r'^\s*#include\s+"([^"]+)"', re.IGNORECASE)
SECTION_RE = re.compile(r'^\s*\[(\w+)\]')


class Component(object):
    def __init__(self, name, types, flags, source):
        self.name = name
        self.types = types
        self.flags = flags
        self.source = source
        self.level = name.count("\\")

    @property
    def name_parent(self):
        return self.name.rsplit("\\", 1)[0] if self.level else None


def walk(path, section=None, seen=None):
    """Every line of a script and the ones it includes, in the order ISCC
    reads them, tagged with the section each one lands in.

    The section carries across an #include, which is the whole point: the
    included file's entries arrive in the middle of the section that included
    it, and that position is what decides the shape of the tree.
    """
    if seen is None:
        seen = set()
    real = os.path.abspath(path)
    if real in seen:
        sys.exit("include loop at %s" % path)
    seen.add(real)

    with open(path, "r", encoding="utf-8-sig") as f:
        for lineno, line in enumerate(f, 1):
            inc = INCLUDE_RE.match(line)
            if inc:
                nested = os.path.join(os.path.dirname(path), inc.group(1))
                for row in walk(nested, section, seen):
                    section = row[3]
                    yield row
                continue
            header = SECTION_RE.match(line)
            if header:
                section = header.group(1).lower()
                continue
            yield (os.path.basename(path), lineno, line, section)


def components():
    out = []
    for filename, lineno, line, section in walk(MAIN):
        if section != "components":
            continue
        m = NAME_RE.match(line)
        if not m:
            continue
        types = TYPES_RE.search(line)
        flags = FLAGS_RE.search(line)
        out.append(Component(
            m.group(1),
            set(types.group(1).split()) if types else set(),
            set(flags.group(1).split()) if flags else set(),
            "%s:%d" % (filename, lineno)))
    return out


def setup_types():
    out = []
    for _, _, line, section in walk(MAIN):
        if section != "types":
            continue
        m = NAME_RE.match(line)
        if m:
            out.append((m.group(1), "iscustom" in (
                FLAGS_RE.search(line).group(1).split()
                if FLAGS_RE.search(line) else [])))
    return out


def positional_parent(comps, i):
    """The parent the checkbox control will give entry i: the nearest entry
    above it at one less depth."""
    want = comps[i].level - 1
    for j in range(i - 1, -1, -1):
        if comps[j].level == want:
            return comps[j]
        if comps[j].level < want:
            return None
    return None


def check_tree(comps):
    """Every entry's parent by position has to be its parent by name."""
    problems = []
    for i, comp in enumerate(comps):
        if comp.level == 0:
            continue
        actual = positional_parent(comps, i)
        expected = comp.name_parent
        if actual is None:
            problems.append("%s (%s) has no parent above it; expected %s"
                            % (comp.name, comp.source, expected))
        elif actual.name.lower() != expected.lower():
            problems.append(
                "%s (%s) is nested under %s, not under %s"
                % (comp.name, comp.source, actual.name, expected))
    return problems


def apply_type(comps, type_name):
    """The state of every entry after the user picks a setup type.

    Leaves are checked when the type names them; a parent is then whatever its
    own children make it - unchecked with none of them checked, checked with
    all of them, grayed in between. Grayed counts as selected, which is what
    WizardIsComponentSelected reports and what the install script acts on.
    """
    state = {}
    for i in reversed(range(len(comps))):
        comp = comps[i]
        # the contiguous run of deeper entries directly after this one
        kids = []
        for j in range(i + 1, len(comps)):
            if comps[j].level <= comp.level:
                break
            if comps[j].level == comp.level + 1:
                kids.append(comps[j])
        if kids:
            checked = [state[k.name] for k in kids]
            if all(c == "checked" for c in checked):
                state[comp.name] = "checked"
            elif all(c == "unchecked" for c in checked):
                state[comp.name] = "unchecked"
            else:
                state[comp.name] = "grayed"
        else:
            state[comp.name] = ("checked"
                                if type_name in comp.types
                                or "fixed" in comp.flags
                                else "unchecked")
    return state


def selected(state, name):
    return state.get(name, "unchecked") != "unchecked"


PHONES = [
    ("roms\\p5320",   "Nokia 5320",   True),
    ("roms\\pe65",    "Nokia E65",    False),
    ("roms\\pn958gb", "Nokia N95 8GB", False),
    ("roms\\p6650",   "Nokia 6650",   True),
    ("roms\\pn85",    "Nokia N85",    True),
]


def check_preset(comps, state, type_name):
    """The wizard's own rule from nokia_klatt.iss: a selected phone with no
    language, or none of its genders, is a phone that installs its ROM and
    adds no voice, and the wizard will not leave the components page."""
    problems = []
    for comp_name, phone, has_genders in PHONES:
        if not selected(state, comp_name):
            continue
        langs = [c for c in comps
                 if c.name.lower().startswith(comp_name.lower() + "\\l")
                 and selected(state, c.name)]
        if not langs:
            problems.append(
                '"%s" would refuse to continue: the %s is selected but none '
                'of its languages are' % (type_name, phone))
        if has_genders and not (selected(state, comp_name + "\\male")
                                or selected(state, comp_name + "\\female")):
            problems.append(
                '"%s" would refuse to continue: the %s is selected but '
                'neither its male nor its female voices are'
                % (type_name, phone))
    return problems


def main():
    comps = components()
    if not comps:
        sys.exit("no components found in %s" % MAIN)

    failures = []

    print("%d components" % len(comps))
    tree = check_tree(comps)
    if tree:
        failures.extend(tree)
        print("  tree:    BROKEN")
        for line in tree[:10]:
            print("    %s" % line)
        if len(tree) > 10:
            print("    ... and %d more" % (len(tree) - 10))
    else:
        print("  tree:    every component nests under its own name")

    for type_name, is_custom in setup_types():
        if is_custom:
            continue  # nothing is preselected, so there is nothing to check
        state = apply_type(comps, type_name)
        chosen = sum(1 for c in comps if state[c.name] == "checked")
        bad = check_preset(comps, state, type_name)
        failures.extend(bad)
        print("  %-8s %d components checked - %s"
              % (type_name + ":", chosen, "OK" if not bad else "REFUSED"))
        for line in bad:
            print("    %s" % line)

    if failures:
        print("\n%d problem(s)." % len(failures))
        return 1
    print("\nAll presets reach the next page.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

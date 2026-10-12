"""The Wii's translation of an Xbox path to the card's (port/linux/src/xbox_files.c, append_path with the directories'
listings): a directory is read once and the spellings of its entries come from what was read; the answer is the one
the look into every part of the path gives, and the directories read are counted."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import build, function, mutated, read, read_wiilibs, run, wiilibs  # noqa: E402

FILES = "port/linux/src/xbox_files.c"
LISTINGS_BEGIN = "/* ---- the directories' listings (begin)"
LISTINGS_END = "/* ---- the directories' listings (end) */"

# a fault in the translation, and the check that must catch it (the program's exit status is not 0); the listings
# are WiiLibs' (wl/platform/dircache.c), whose own tests (tests/platform/test_platform_dircache.c) see these as well
NEGATIVE_CONTROLS = {
    "listing-never-kept": (("        if (strcmp(cache->lists[i]->directory, directory) == 0) {\n            return cache->lists[i];",
                            "        if (0) {\n            return cache->lists[i];"), "dircache"),
    "exact-spelling-not-preferred": (("            if (strcmp(name, component) == 0) {\n                match = name;\n                break;\n            }\n",
                                      ""), "dircache"),
    "first-spelling-not-kept": (("            if (match == NULL && strcasecmp(name, component) == 0) {",
                                 "            if (strcasecmp(name, component) == 0) {"), "dircache"),
    "large-directory-cut": (("        r->too_large = true;\n        return false;", "        return false;"), "dircache"),
    "not-found-without-the-name": (("match != NULL ? match : component", "match != NULL ? match : \"\""), "dircache"),
    "listing-not-counted": (("    cache->lists[cache->count++] = list;", "    cache->lists[cache->count] = list;"), "dircache"),
    "dots-kept": (("		if (length && strspn(component, \". \") != length)", "		if (length)"),
                  "next_path_component"),
}


pytestmark = pytest.mark.skipif(wiilibs() is None, reason="WiiLibs not found (WIILIBS, ./WiiLibs or ../WiiLibs)")


def listings(source):
    return source[source.index(LISTINGS_BEGIN):source.index(LISTINGS_END) + len(LISTINGS_END)]


def generated(fault=None, faulty=None):
    source = read(FILES)
    code = ""
    for name in ("next_path_component", "look_for_entry"):
        text = function(source, name)
        if fault and name == faulty:
            text = mutated(text, *fault)
        code += text + "\n"
    code += listings(source) + "\n"
    for name in ("append_path_component", "append_path"):
        code += function(source, name) + "\n"
    dircache = read_wiilibs("wl/platform/dircache.c")
    if fault and faulty == "dircache":
        dircache = mutated(dircache, *fault)
    return (("under_test.inc", "#define HALO_WII 1\n" + code), ("dircache.inc", dircache), ("posix.h", read("port/linux/src/posix.h")),
            ("posix_files.inc", read("port/linux/src/posix_files.c")))


def test_translation():
    status, output = run(build("path_translation", generated()), "")
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty = NEGATIVE_CONTROLS[control]
    status, output = run(build("path_translation", generated(fault, faulty)), "")
    assert status != 0, f"the translation with a fault ({control}) passed: the test cannot see it"

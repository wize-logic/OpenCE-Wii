"""The Wii's translation of an Xbox path to the card's (port/linux/src/xbox_files.c, append_path with the directories'
listings): a directory is read once and the spellings of its entries come from what was read; the answer is the one
the look into every part of the path gives, and the directories read are counted."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import build, function, mutated, read, run  # noqa: E402

FILES = "port/linux/src/xbox_files.c"
LISTINGS_BEGIN = "/* ---- the directories' listings (begin)"
LISTINGS_END = "/* ---- the directories' listings (end) */"

# a fault in the translation, and the check that must catch it (the program's exit status is not 0)
NEGATIVE_CONTROLS = {
    "listing-never-kept": (("		if (!strcmp(directory_listings[index].directory, directory))",
                            "		if (0)"), "listings"),
    "exact-spelling-not-preferred": (("			if (!strcmp(name, component))\n			{\n				match = name;\n				break;\n			}\n",
                                      ""), "listings"),
    "first-spelling-not-kept": (("			if (!match && !strcasecmp(name, component))", "			if (!strcasecmp(name, component))"),
                                "listings"),
    "large-directory-cut": (("				state = _listing_too_large;\n				break;", "				break;"), "listings"),
    "not-found-without-the-name": (("		snprintf(on_disk, size, \"%s\", match ? match : component);",
                                    "		snprintf(on_disk, size, \"%s\", match ? match : \"\");"), "listings"),
    "listing-not-counted": (("	listing->state = state;\n	directory_listing_count++;",
                                          "	listing->state = state;"), "listings"),
    "dots-kept": (("		if (length && strspn(component, \". \") != length)", "		if (length)"),
                  "next_path_component"),
}


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
    text = listings(source)
    if fault and faulty == "listings":
        text = mutated(text, *fault)
    code += text + "\n"
    for name in ("append_path_component", "append_path"):
        code += function(source, name) + "\n"
    return (("under_test.inc", "#define HALO_WII 1\n" + code), ("posix.h", read("port/linux/src/posix.h")),
            ("posix_files.inc", read("port/linux/src/posix_files.c")))


def test_translation():
    status, output = run(build("path_translation", generated()), "")
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, faulty = NEGATIVE_CONTROLS[control]
    status, output = run(build("path_translation", generated(fault, faulty)), "")
    assert status != 0, f"the translation with a fault ({control}) passed: the test cannot see it"

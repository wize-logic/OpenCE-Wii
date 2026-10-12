"""The Wii's translation of an Xbox path to the card's (port/linux/src/xbox_files.c, translate_as_written): a path
that is there as written needs one stat, a file to be made in a folder that is there one stat and one look into the
folder, and the answer is the one the look at every part of the path gives."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import build, function, mutated, read, run  # noqa: E402

FILES = "port/linux/src/xbox_files.c"

# a fault in the translation, and the check that must catch it (the program's exit status is not 0)
NEGATIVE_CONTROLS = {
    "keeps-the-whole-path-unchecked": (("	if (posix_stat(resolved, &information) == 0)\n	{\n		snprintf(host_path",
                                        "	if (1)\n	{\n		snprintf(host_path"), "translate_as_written"),
    "folder-not-checked": (("	if (parent_length && posix_stat(resolved, &information) == 0 && (information.flags & "
                            "_posix_file_is_directory))", "	if (parent_length)"), "translate_as_written"),
    "last-part-as-written": (("		append_path_component(resolved, size, last);\n		snprintf(host_path",
                              "		snprintf(resolved + parent_length, size - parent_length, \"/%s\", last);\n"
                              "		snprintf(host_path"), "translate_as_written"),
    "dots-kept": (("		if (length && strspn(component, \". \") != length)", "		if (length)"),
                  "next_path_component"),
}


def generated(fault=None, faulty=None):
    source = read(FILES)
    code = ""
    for name in ("next_path_component", "append_path_component", "translate_as_written"):
        text = function(source, name)
        if fault and name == faulty:
            text = mutated(text, *fault)
        code += text + "\n"
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

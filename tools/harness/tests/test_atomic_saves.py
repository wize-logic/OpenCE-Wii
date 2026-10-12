"""The Wii's saves (port/linux/src/xbox_files.c, "saves that survive a cut"; atomic_saves.c, built with HALO_WII): a file
written under the save root is the old one or the new one whole after a cut at every call that changes the card (and
half of every write), on a card that renames over a file and on FAT's, which does not; the next start puts back what a
cut left; a full, read-only or gone card gives the game the error its handlers know and leaves the old file."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import build, function, mutated, read, run  # noqa: E402

FILES = "port/linux/src/xbox_files.c"
SEQUENCES = ["profile", "savegame", "patch", "head", "append", "create", "replace", "grow", "made"]
CUT_CASES = [f"{sequence}:{kind}" for sequence in SEQUENCES for kind in ("cut", "cut-fat")]
FAILURE_CASES = [f"{sequence}:{kind}" for sequence in ("profile", "patch", "create")
                 for kind in ("full", "no-room", "read-only", "missing", "rename-fails")] + ["savegame:full-later"]
OTHER_CASES = ["profile:not-shadowed", "profile:two-writers", "profile:create-new", "profile:sweep",
               "profile:find-skips-copies", "profile:regen-first", "profile:regen-again", "profile:regen-changed",
               "profile:regen-other-deleted"]

# a fault in the saves, and a case that must catch it (its exit status is not 0; a hang is killed by the case's alarm)
NEGATIVE_CONTROLS = {
    "written-in-place": (("	return length && !strncmp(path, root, length) && path[length] == '/';",
                          "	return 0 && length && !strncmp(path, root, length) && path[length] == '/';"), "profile:cut"),
    "not-synced": (("		if (fsync(file->descriptor) != 0)\n			error = errno ? errno : EIO;",
                    "		if (0)\n			error = errno ? errno : EIO;"), "profile:cut"),
    "unlink-first": (("	if (rename(path, old_path) != 0)\n		return errno ? errno : EIO;",
                      "	if (unlink(path) != 0)\n		return errno ? errno : EIO;"), "profile:cut-fat"),
    "failed-write-kept": (("	if (file->damaged)\n	{\n		error = -1;", "	if (0)\n	{\n		error = -1;"), "profile:full"),
    "no-sweep": (("guard < 1000 && shadow_sweep_directory(root, 0)", "guard < 0 && shadow_sweep_directory(root, 0)"),
                 "profile:cut-fat"),
    "zero-write-repeats": (("		if (result == 0)\n		{\n			/* port: a card with no room",
                            "		if (0)\n		{\n			/* port: a card with no room"), "profile:no-room"),
    "whole-write-copied-short": (("resize || offset != 0 || count < file->original_size", "resize || offset != 0"),
                                 "head:cut"),
    "partial-write-not-copied": (("resize || offset != 0 || count < file->original_size", "0"), "patch:cut"),
    "copy-shared": (("	if (shadow_busy(file, temporary_path))\n		return EBUSY;", ""), "profile:two-writers"),
    "regenerated-written-again": (("		if (regenerated_holds(file, buffer, count, at))",
                                   "		if (0 && regenerated_holds(file, buffer, count, at))"), "profile:regen-again"),
    "regenerated-deleted": (("	if (regenerated_wanted(path) && posix_stat(path, &information) == 0 &&",
                             "	if (0 && regenerated_wanted(path) && posix_stat(path, &information) == 0 &&"),
                            "profile:regen-again"),
    "regenerated-not-cut": (("		information.size_high == 0 && information.size_low > file->written_end &&",
                             "		0 && information.size_high == 0 && information.size_low > file->written_end &&"),
                            "profile:regen-changed"),
    "regenerated-copied": (("(existed || (flags & O_CREAT)) && regenerated_wanted(path))\n	{",
                            "(existed || (flags & O_CREAT)) && 0 && regenerated_wanted(path))\n	{"),
                           "profile:regen-first"),
    "copies-listed": (("	return length > suffix_length && !strcmp(name + length - suffix_length, suffix);",
                       "	return length >= suffix_length && !strcmp(name + length - suffix_length, suffix);"),
                      "profile:find-skips-copies"),
}


def generated(fault=None):
    text = read(FILES)
    kernel = read("port/linux/src/xbox_kernel.c")
    win32 = read("port/include/xdk/xdk_win32.h")
    gate = text[text.index("#ifdef HALO_WII\n/* port: what the Wii's leaving"):text.index("static void file_destroy")]
    writes = text[text.index("#ifdef HALO_WII\n#define write_at write_at_ungated"):
                  text.index("static unsigned long long overlapped_offset")]
    start = text.index("struct platform_file\n{")
    code = "#define HALO_WII 1\n" + text[start:text.index("};", start) + 2] + "\n"
    code += gate
    code += "static void file_end_positioned(struct platform_file *file) { (void)file; }\n"  # (reads by offset are not tested here)
    code += "\n".join(function(text, name) for name in ("file_destroy", "file_from_handle", "CreateFileA", "DeleteFileA")) + "\n"
    code += writes
    code += function(text, "overlapped_offset") + "\n"
    code += "\n".join(function(text, name) for name in ("WriteFile", "SetFilePointer", "SetEndOfFile")) + "\n"
    if fault:
        code = mutated(code, *fault)
    # the platform's mapping of errno to the game's errors, and the constants it and the sequences use
    mapping = function(kernel, "platform_set_last_error_from_errno")
    names = sorted(set(re.findall(r"\bERROR_\w+", mapping)) | {"ERROR_ALREADY_EXISTS", "ERROR_DISK_FULL",
                                                                "ERROR_ACCESS_DENIED", "ERROR_SUCCESS"})
    names += ["GENERIC_READ", "GENERIC_WRITE", "CREATE_NEW", "CREATE_ALWAYS", "OPEN_EXISTING", "OPEN_ALWAYS",
              "TRUNCATE_EXISTING", "FILE_ATTRIBUTE_NORMAL", "FILE_FLAG_NO_BUFFERING", "FILE_FLAG_SEQUENTIAL_SCAN",
              "FILE_BEGIN", "FILE_CURRENT", "FILE_END", "INVALID_SET_FILE_POINTER", "FILE_FLAG_OVERLAPPED"]
    constants = ""
    for name in names:
        match = re.search(r"^#define\s+" + name + r"\s+(.*)$", win32, re.M)
        assert match, name
        constants += f"#define {name} {match.group(1)}\n"
    mapping = mapping.replace("DWORD platform_set_last_error_from_errno", "static DWORD platform_set_last_error_from_errno", 1)
    return (("under_test.inc", code), ("constants.inc", constants), ("errors.inc", mapping),
            ("posix.h", read("port/linux/src/posix.h")), ("posix_files.inc", read("port/linux/src/posix_files.c")))


@pytest.mark.parametrize("case", CUT_CASES + FAILURE_CASES + OTHER_CASES)
def test_case(case):
    status, output = run(build("atomic_saves", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("atomic_saves", generated(fault)), case)
    assert status != 0, f"the saves with a fault ({control}) passed '{case}': the test cannot see it"

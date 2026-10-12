"""tools/wii_profile.py: the profile.txt it writes (and WiiLibs' reading of it, which has the
same names), and the command it runs for a report."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wii_profile  # noqa: E402


class SetupTests(unittest.TestCase):
    def test_writes_profile_txt_with_the_given_keys(self):
        with tempfile.TemporaryDirectory() as sd:
            code = wii_profile.main(["setup", "--sd", sd, "--seconds", "30", "--hz", "250", "--repeat", "2",
                                     "--note", "b30 then a10"])
            self.assertEqual(code, 0)
            text = (Path(sd) / "apps" / "halo" / "profile.txt").read_text()
            lines = [line for line in text.splitlines() if not line.startswith("#")]
            self.assertEqual(lines, ["seconds 30", "hz 250", "repeat 2", "log 1", "note b30 then a10"])
            wii_profile.main(["setup", "--sd", sd, "--remove"])
            self.assertFalse((Path(sd) / "apps" / "halo" / "profile.txt").exists())

    def test_every_key_is_one_wiilibs_parser_reads(self):
        wiilibs = Path(os.environ.get("WIILIBS", Path(__file__).resolve().parent.parent.parent / "WiiLibs"))
        parser = wiilibs / "wl" / "platform" / "profile_arm.c"
        if not parser.is_file():
            self.skipTest(f"no {parser}: WiiLibs' profiler arming is not there (WIILIBS)")
        source = parser.read_text()
        for key in (*wii_profile.KEYS, "log", "note"):
            self.assertIn(f'"{key}"', source)


class ReportTests(unittest.TestCase):
    def test_runs_wl_profile_with_the_elf_and_passes_other_options_on(self):
        with tempfile.TemporaryDirectory() as directory:
            wiilibs = Path(directory) / "WiiLibs"
            (wiilibs / "tools").mkdir(parents=True)
            (wiilibs / "tools" / "wl_profile.py").write_text("")
            elf = Path(directory) / "halo.elf"
            elf.write_text("")
            with mock.patch.object(subprocess, "run") as run:
                run.return_value.returncode = 0
                code = wii_profile.main(["report", "a.log", "--elf", str(elf), "--wiilibs", str(wiilibs),
                                         "--top", "5", "--tree"])
            self.assertEqual(code, 0)
            command = run.call_args[0][0]
            self.assertEqual(command[1:], [str(wiilibs / "tools" / "wl_profile.py"), str(elf), "a.log", "--top", "5",
                                           "--tree"])


if __name__ == "__main__":
    unittest.main()

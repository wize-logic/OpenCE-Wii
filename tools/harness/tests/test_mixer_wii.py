"""The Wii's mixing of dsound_sdl.c (mixer_wii.c, built with HALO_WII): its buses and upsampler against the same half-band
low pass worked out in double precision, sines through voices at their rates and pitched, voices out of earshot, mono
voices, a scene mixed in pieces of any size, a voice that runs out, and ADPCM read a block at a time.

    python tools/harness/tests/test_mixer_wii.py metrics   the figures
    python tools/harness/tests/test_mixer_wii.py bench mono stereo44 stereo22 silent flags chunks   a scene

The instructions an output frame cost on the Wii's processor are counted by building mixer_wii.c for 32-bit big-endian
PowerPC (clang --target=powerpc-linux-gnu -mcpu=750 -O2 -ffp-contract=fast -DHALO_WII, static) and running "bench"
under qemu-ppc with -d in_asm,exec,nochain, two lengths of the same scene apart (WiiLibs' tools/qemu_icount.py).
"""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run  # noqa: E402

SOURCE = "port/linux/src/dsound_sdl.c"
CASES = ["native", "sines", "pitched", "silent-voice", "mono", "chunks", "ends", "adpcm"]

# a fault in the Wii's mixing, and the case that must catch it
NEGATIVE_CONTROLS = {
    "upsampler-off-by-one": (("between_left += upsampler_weights[k] * (center[-2 * k] + center[2 * (k + 1)]);",
                              "between_left += upsampler_weights[k] * (center[-2 * k] + center[2 * k]);"), "native"),
    "cubic-weights": (("w1 = 0.5f * (t * t * (3.0f * t - 5.0f) + 2.0f);",
                       "w1 = 0.5f * (t * t * (3.0f * t - 4.0f) + 2.0f);"), "pitched"),
    "pass-one-short": (("		stream->cursor += run;\n		stream->silence = 0;\n		count -= run;",
                        "		stream->cursor += run - (run > 1);\n		stream->silence = 0;\n		count -= run;"),
                       "silent-voice"),
    "mix-ahead-lost": (("		wii_mix.ahead_used += count;", "		wii_mix.ahead_used += count + (count > 7);"),
                       "chunks"),
    "adpcm-clamp": (("		if (predictor > 32767) \\\n			predictor = 32767; \\",
                     "		if (predictor > 32000) \\\n			predictor = 32000; \\"), "adpcm"),
}


def generated(source=None, fault=None):
    """the mixer with HALO_WII: dsound_sdl.c from the #ifdef HALO_WII that picks the Wii's rates to the end of the
    mixing (before the output)"""
    text = source if source is not None else read(SOURCE)
    start = text.rfind("#ifdef HALO_WII", 0, text.index("#define OUTPUT_RATE"))
    code = text[start:text.index("/* ---------- output")]
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", "#define HALO_WII\n" + code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("mixer_wii", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("mixer_wii", generated(fault=fault)), case)
    assert status == CHECK_FAILED, f"the mixer with a fault ({control}) passed '{case}': the test cannot see it"


if __name__ == "__main__":
    import subprocess

    sys.exit(subprocess.run([str(build("mixer_wii", generated())), *(sys.argv[1:] or ["metrics"])]).returncode)

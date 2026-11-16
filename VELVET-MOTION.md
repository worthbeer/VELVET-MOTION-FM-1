# VELVET MOTION for the M-VAVE FM-1

A custom firmware for the FM-1 that puts the VELVET MOTION sounds, chord keys and chord
progressions on the hardware. It is a fork of [Felucca](https://github.com/hugelton/Felucca)
1.8.2 by Leo Kuroshita (Hügelton Instruments): everything Felucca does is still there, and
VELVET MOTION is added on top. Licence: GPL-3.0-only, like Felucca.

**Status: not yet run on an FM-1.** The sound engine, the screens and the sequencer were
tested with Felucca's computer-side simulation. The installable file has to be built first
(see below), and nobody has heard this on the hardware yet. Treat the first install as a test.

## What is in it

- **VELVET engine** (the last engine in PRESETS): seven models in one engine, picked with
  MODE: EP, BELL, VIBE (Soul Keys), BASS (Funk Bass), SYNTH, STRNG (Velvet Synths) and ORGAN
  (Soul Organ, nine drawbars with percussion and key click).
  The four knobs on HOME are TONE, DCY, MOVE and WARM.
- **64 sounds** from the VELVET MOTION 0.56 factory library: 16 electric pianos, 6 bells,
  6 vibes, 12 basses, 10 synths, 6 strings, 8 organs. They can be browsed by category and
  marked as favourites like any other sound.
- **Power-on set**: part 1 VELVET EP, part 2 DEEP POCKET (bass), part 3 SUNSET (strings),
  part 4 the drum kit.
- **Chord keys**: SCL > CHORD (Felucca's own). Set CHRD to DIA7 or DIA3 and one key plays a
  chord in the scale; VOIC picks the voicing.
- **Progressions**: SAVE > PHRASES, entries C01 to C10 after Felucca's own patterns. Loading
  one writes the chords into the part's sequencer, one chord a bar, ready to play.
  They are Backdoor Soul, Borrowed Sunset, C Major 251, Chromatic Turnaround, D Dorian 251,
  Dorian Late Night, Floating Suspensions, Minor Night, Soulful 1-6-2-5 and Velvet 2-5-1.
  Chords of five notes lose their fifth (a step holds four notes).
- **Look**: two palettes, VELVET (ink, sun yellow, paper, pink; the first-boot default) and
  SKY (paper by day), in MENU > COLOR after Felucca's ten. A VELVET MOTION splash with the
  sun-and-record badge. On HOME and on the VELVET engine page the sun's rays turn behind the
  scope and reach out with the level, and the record spins, while the part sounds or plays.

## What is different from the desktop plugin

The FM-1 has a small fixed-point processor, 1 MB of flash and a 240 x 240 screen, so this is
a new instrument modelled on the plugin, not the plugin's code:

- The engines are simplified fixed-point versions. They follow the desktop models' partials,
  envelopes and filters but will not sound identical.
- No sampled drums from VELVET MOTION (Felucca's drum kits are used) and no vocal engine.
- 8 voices are shared by the 4 parts.
- The chord and progression features are Felucca's chord keys and sequencer carrying
  VELVET MOTION's progressions, not the plugin's chord generator page.

## Build the installable file

The build needs JieLi's compiler and SDK, which are downloaded from JieLi's servers.

**On GitHub (no tools to install):** put this source in a GitHub repository. The workflow in
`.github/workflows/build.yml` runs on every push (or from the Actions tab, "Run workflow").
When it finishes, open the run and download the `velvet-motion-fm1` artifact: it holds
`felucca.fwsc`. If the Build step fails with "image ... exceeds the app slot" or a headroom
message, the firmware does not fit and must not be installed.

**On your own computer:** see [BUILDING.md](BUILDING.md) (`tools/get_toolchain.sh`, the AC79
SDK, then `./build.sh`).

## Install

1. Back up your projects and presets.
2. Recommended first: install official Felucca 1.8.2 with the
   [Felucca web installer](https://hugelton.github.io/Felucca/) (Chrome or Edge). This shows
   that your unit takes custom firmware, and the installer can take you back to the official
   V15 / V16 firmware.
3. Install this build over USB:

   ```
   pip install mido python-rtmidi
   python3 tools/fm1_install.py felucca.fwsc
   ```

To go back: the Felucca web installer's "Return to official firmware" with the official
`FM-1.fwsc` from M-VAVE, or `python3 tools/fm1_install.py FM-1.fwsc`.

The FM-1 has one firmware slot and no recovery button. Felucca has a boot-loop guard and an
update mode (OCT- and OCT+ held for five seconds while it runs), but a firmware that does not start can need
extra hardware to recover. Install at your own risk.

## For developers

- `firmware/src/eng_velvet.c` the engine; `tools/gen_velvet_presets.py` and
  `tools/gen_velvet_progs.py` make `velvet_presets.h` and `velvet_progs.h` from
  `assets/velvet/`.
- `FELUCCA_VELVET=0` builds Felucca without the engine. `FELUCCA_VELVET_BOOT=0` keeps
  Felucca's power-on parts and GREY palette; `tests/run_tests.sh` uses it so Felucca's own
  expectations hold (`VELVET_BOOT=1 tests/run_tests.sh` to try the other way; tests that
  name Felucca's power-on sounds then fail by design).
- VELVET takes engine number 14, the one the optional ADPCM engine would have, so the two
  are not built together.

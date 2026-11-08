# FM1-Hero

![FM1-Hero Screenshot](docs/card.png)

**FM1-Hero** is a falling-note MIDI piano trainer, rhythm game, and 6-voice 2-operator FM synthesizer for the **M-VAVE FM-1**, built with [foma](https://github.com/konsumer/foma).

Play along with falling **Green (Right Hand)** and **Blue (Left Hand)** note bars on the 240×240 screen while the physical keybed LEDs light up the next notes to press.

## Features

- **3 Practice & Game Modes** (`ALGORITHM` encoder or `SEL` / `ARP` buttons):
  - **WAIT (Melody Practice):** Scrolls to the next note and pauses until you press the lit key(s).
  - **GAME (Rhythm Practice):** Continuous waterfall with `PERFECT!`, `GREAT!`, `GOOD`, and `MISS` timing judgements, combo multipliers (`x2`, `x3`, `x4`), and end-of-song grades (`S`, `A`, `B`, `C`).
  - **AUTO (Watch & Listen):** Jukebox mode that plays the full song automatically while letting you improvise along on the keys.
- **Hand Separation (`ENV` / `LFO` buttons):**
  - **BOTH:** Play both Left Hand (Blue) and Right Hand (Green) parts.
  - **RH (Right Hand):** Practice the Right Hand (Green) while the Left Hand (Blue) accompaniment plays automatically.
  - **LH (Left Hand):** Practice the Left Hand (Blue) while the Right Hand (Green) melody plays automatically.
- **A–B Section Loop (`EDIT` button):** Press once to set point **A**, a second time to loop **A..B** on the top timeline bar, and a third time to clear the loop.
- **16 Built-in Songs + Custom `.MID` Slot 17 (`SELECT` / `PRESETS` encoders):**
  - Includes 16 built-in songs plus a **163-song web library** and drag-and-drop `.mid` uploader in the browser emulator/installer.
- **Live Song Recorder (`REC` button):** Record your own melodies directly from the 27 keys or USB/TRS MIDI input into Custom Slot 17.
- **6-Voice 2-Op FM Synth + Stereo Ping-Pong Echo:**
  - 6 patches (`GRAND PIANO`, `DX E-PIANO`, `CHIPTUNE`, `FM BELLS`, `BRASS`, `WARM PAD`), adjustable FM brightness, and stereo ping-pong delay.

## Controls Cheat Sheet

| Control | Function |
| :--- | :--- |
| **27 Keys / MIDI In** | Play notes & hit falling **Green (RH)** and **Blue (LH)** bars |
| **Key LEDs** | Light up the physical keys you need to press next |
| **PLAY** | Play / Pause song (or retry from the Recital Complete screen) |
| **HOME** | Rewind song to Measure 1 |
| **SELECT / PRESETS** | Browse 16 built-in songs + Slot 17 (Custom `.MID` / User Recording) |
| **ALGORITHM / SEL** | Cycle Mode: `WAIT` (Wait-for-Me), `GAME` (Rhythm), `AUTO` (Watch) |
| **ENV / LFO** | Cycle Hand Practice: `BOTH`, `RH` (Right Hand), `LH` (Left Hand) |
| **EDIT** | A–B Loop Bookmark (1st press = set A, 2nd = loop A–B, 3rd = clear) |
| **GLO** | Toggle Metronome beat click (`MET:ON`) |
| **FX / KNOB 2** | Cycle 6 FM Synth Patches (`GRND`, `EPNO`, `CHIP`, `BELL`, `BRAS`, `PAD`) |
| **KNOB 1** | Song Tempo / Speed (`25%` – `200%`) |
| **KNOB 3** | FM Brightness / Timbre (`0` – `127`) |
| **KNOB 4** | Stereo Echo FX Mix (`0` – `127`) |
| **OCT- / OCT+** | Transpose keyboard octave (`-2` to `+2`) |
| **REC** | Record live playing into Custom Slot 17 |
| **SAVE** | Save high scores, settings, and custom song to internal flash |

## Flash Storage & Updating

- **Saving (`SAVE` button):** High scores, settings, and your custom recorded/uploaded `.mid` song are stored in the user flash area (`0xFB000`, `4 KiB` max via `settings_save()`). Installing another firmware that uses the same user flash region may overwrite saved scores.
- **USB Rescue:** Installing firmware is at your own risk. If the FM-1 ever fails to start, hold **OCT-** while switching the power on to enter *USB RESCUE* mode, then install from the browser installer or restore the official firmware from the Rescue tab.

## Build & Develop

```sh
./fm1 dev          # http://localhost:8000: live emulator, WebMIDI .mid uploader, USB installer & rescue
./fm1 build        # builds build/web/firmware.wasm and build/fm1-hero-0.1.fwsc (identity FM-1_281)
```

## License

**GPL-3.0-only** (`LICENSE`). Credits and third-party components are documented in [LICENSING.md](LICENSING.md).

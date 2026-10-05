# Praccy

<div align="center">

<img src="resources/logo.png" alt="Praccy Logo" width="180"/>

### Ultra-lightweight ASIO VST3 & CLAP host for instant instrument practice and jamming.

[![Platform](https://img.shields.io/badge/platform-Windows%20x64-blue.svg)](https://github.com/BartekStaniak/Praccy)
[![Audio Architecture](https://img.shields.io/badge/audio-ASIO%20%7C%20VST3%20%7C%20CLAP-orange.svg)](https://github.com/BartekStaniak/Praccy)
[![Graphics](https://img.shields.io/badge/gui-DirectX%2011%20%7C%20Dear%20ImGui-purple.svg)](https://github.com/BartekStaniak/Praccy)
[![Standard](https://img.shields.io/badge/c%2B%2B-20-brightgreen.svg)](https://en.wikipedia.org/wiki/C%2B%2B20)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

</div>

> Cold boots in under 500 ms, streams directly to your audio interface via ASIO with zero added buffer latency, and organizes serial/parallel plugin rigs without the heavyweight baggage of a full DAW.

---

## Overview

Full digital audio workstations like REAPER, Ableton Live, or Cubase are indispensable for track production, multitrack recording, and mixing. But when you just want to pick up your guitar, bass, or synth and play for 15 minutes, opening a full DAW is pure friction:
- **Lengthy load times:** Waiting 20–45 seconds while hundreds of unused plugins, templates, and project media scan.
- **Resource bloat:** Heavy background memory usage, background disk indexing, and complex routing matrices.
- **Latency overhead:** Added buffer queues and driver bridging layers that dull instrument responsiveness.
- **Fragile routing:** Setting up dual-amp parallel splits or wet/dry delay rigs requires intricate track send/bus scaffolding.

**Praccy** was built from the ground up to solve this exact problem. It is an ultra-lean, native C++20 ASIO host designed specifically for practicing, noodling, and live pedalboard-style jamming. It bypasses all unnecessary DAW layers, talks directly to your audio interface via native ASIO, renders a high-framerate hardware-accelerated interface via DirectX 11 and Dear ImGui, and gets you playing in seconds.

---

## Features

- **⚡ Instant Cold Startup**: Fires up in under 500 milliseconds. No bloated splash screens, project indexing, or background update daemons.
- **⏱️ Zero Added Buffer Latency**: Direct ASIO hardware streaming. Audio buffers pass straight through an isolated, lock-free real-time processing thread boosted with Windows MMCSS Pro Audio scheduling (`AVRT`).
- **🔀 Hybrid Serial & Parallel Routing**:
  - Linear pedalboard-style signal chain.
  - **1-Click Parallel Splits (`[|| Split]`)**: Split any point in your chain into independent parallel branches (A/B or multi-branch) for dual-amp rigs, wet/dry delay chains, or multi-band processing.
  - **Branch Mixing**: Dedicated stereo pan, gain trim, phase invert (Ø), mute, and solo controls per branch.
  - **Per-Slot Mix Controls**: Built-in dry/wet percentage and output gain trim on every plugin container.
  - **Click-Free Transitions**: Equal-power crossfaded bypass and branch toggling to avoid jarring audio pops.
- **🔌 Modern Dual Plugin Hosting (VST3 & CLAP)**:
  - Native 64-bit hosting for both **VST3** (`IComponent` / `IEditController`) and **CLAP** (`clap_plugin`).
  - **Native Win32 GUI Windows**: Plugin editor interfaces load in dedicated, draggable Win32 windows with message pump isolation.
  - **Rack Faceplate Previews**: Hardware-style rack faceplates featuring parameter dials, status lamps, and live waveform previews let you monitor state and open the full plugin GUI with a single click.
  - **Plugin Scanner & Manager**: Background directory scanning with default Windows paths (`C:\Program Files\Common Files\VST3`, `CLAP`) and customizable user folders.
  - **Built-In Zero-Latency Effects**: Comes out-of-the-box with `Praccy Drive` (TS-style overdrive), `Praccy Amp` (tube amp simulation), and `Praccy Stereo Delay` for immediate jamming without third-party plugins.
- **🎸 Musician's Practice Suite**:
  - **Chromatic Strobe Tuner**: High-precision pitch detection powered by the **YIN autocorrelation algorithm**, displaying note name, octave, and sub-cent cents offset in real time.
  - **Sample-Accurate Metronome**: Click track with adjustable tempo (BPM), selectable time signatures (4/4, 3/4, 2/4, 6/8), accented downbeats, and visual pulsing LEDs.
  - **Adaptive Noise Gate**: Built-in input gate with threshold slider to eliminate high-gain guitar hum and single-coil buzz.
  - **Master Safety Limiter**: Zero-lookahead soft-saturation ceiling to protect your ears and monitors from unexpected feedback spikes or runaway gain.
- **🎛️ Hardware & MIDI Control**:
  - **1-Click MIDI Learn**: Map hardware MIDI footswitches, expression pedals, and knobs directly to bypass states, dry/wet mix, and output trim.
  - **Instant Scene Snapshots**: Switch between Clean, Rhythm, and Lead presets with seamless state transitions, and save custom full-chain presets.
- **💾 State Persistence**: Automatically stores your selected ASIO driver, sample rate, buffer size, input channel routing (Stereo, Left Mono, Right Mono), master volume, and plugin search directories across restarts.

---

## Signal Flow Architecture

```
[ ASIO Audio Input (Hardware) ]
               │
      [ Input Channel Router ] (Stereo / Left Mono / Right Mono)
               │
      [ Practice Tuner Tap ] ────────► [ YIN Strobe Tuner (Worker Thread) ]
               │
+──────────────▼─────────────────────────────────────────────────────+
│                   Praccy Audio Processing Graph                    │
│                                                                    │
│  [ Slot 1: Serial Plugin ] (e.g. Compressor / Tube Screamer)       │
│               │                                                    │
│        [ Branch Splitter ]                                         │
│        ┌──────┴──────┐                                             │
│        │             │                                             │
│  [ Branch A ]   [ Branch B ]  (e.g. Dual Amp Sim / Wet-Dry FX)     │
│  - Amp Clean    - Amp High-Gain                                    │
│  - Pan L / Gain - Pan R / Phase Inv                                │
│        │             │                                             │
│        └──────┬──────┘                                             │
│         [ Branch Summer ]                                          │
│               │                                                    │
│  [ Slot 3: Serial Plugin ] (e.g. Stereo Delay / Reverb)            │
+────────────────────────────────────────────────────────────────────+
               │
      [ Master Safety Limiter ] (Smooth tanh soft-saturation)
               │
[ ASIO Audio Output (Hardware) ]
```

---

## Quickstart Guide

1. **Launch Praccy**: Run `Praccy.exe`.
2. **Select Audio Device**: In the top navigation bar, choose your ASIO audio interface (Focusrite Scarlett, Universal Audio Apollo, RME, FL Studio ASIO, ASIO4ALL, etc.).
3. **Configure Input**: Set your instrument input channel:
   - **Stereo**: Both channels 1 & 2.
   - **Left Mono (Ch 1)**: Standard single-cable guitar / bass input into input 1.
   - **Right Mono (Ch 2)**: Input 2.
4. **Scan Plugins**: Click **Plugins...** in the top bar to open the Plugin Manager, or use **Search Paths...** to add custom plugin directories.
5. **Add to Rack**: Click **+ Add Plugin** on the serial rack or inside any parallel branch to insert effects.
6. **Parallel Rigs**: Click `[|| Split]` on any slot to branch your signal into parallel paths.
7. **Tune & Play**: The chromatic tuner is always active at the top of your rack.

---

## Building From Source

Praccy is written in modern **C++20** and builds cleanly using CMake on Windows with either MSVC (Visual Studio 2022) or MinGW-w64 (`w64devkit`).

### Prerequisites
- Windows 10 / 11 (64-bit)
- CMake 3.25 or newer
- **MSVC** (Visual Studio 2022 C++ Build Tools v143+) OR **MinGW-w64** (GCC 13+)
- DirectX 11 runtime (included with Windows 10/11)
- An ASIO-compatible audio interface or driver

### 1. Build with Visual Studio (MSVC)

```powershell
# Clone the repository
git clone https://github.com/BartekStaniak/Praccy.git
cd Praccy

# Configure and generate build files
cmake -B build -G "Visual Studio 17 2022" -A x64

# Compile Release binary
cmake --build build --config Release
```

The resulting executable will be generated at `build/Release/Praccy.exe`.

### 2. Build with MinGW / w64devkit

```powershell
# Configure Ninja / Makefiles
cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release

# Compile
cmake --build build
```

---

## Running Automated Tests

Praccy includes a comprehensive suite of unit and DSP pipeline tests covering audio buffer math, SIMD routines, serial and parallel processing, YIN pitch accuracy, metronome timing, and configuration persistence:

```powershell
# Run the test suite
cmake --build build --target test_praccy
ctest --test-dir build --output-on-failure
```

---

## Tech Stack & Dependencies

- **Language:** C++20
- **Audio API:** Steinberg ASIO SDK
- **Plugin Standards:** Steinberg VST3 SDK & CLAP (Clever Audio Plugin) API
- **Rendering & GUI:** DirectX 11 & [Dear ImGui](https://github.com/ocornut/imgui) (Docking branch)
- **Pitch Detection:** Custom YIN autocorrelation algorithm
- **Resource Management:** Native Win32 API & Windows MMCSS Real-Time Thread Scheduling

---

## Author

Developed with care by **Bartek Staniak**  
GitHub: [@BartekStaniak](https://github.com/BartekStaniak)

---

## License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.  
*VST is a registered trademark of Steinberg Media Technologies GmbH.*

# Praccy 🎸🎛️

**Praccy** is an ultra-fast, lightweight ASIO VST3 & CLAP host designed specifically for instrument practice and instant jamming.

## Features

- ⚡ **Near-Zero Cold Boot Time**: Starts up in milliseconds — plug in and play.
- ⏱️ **Zero Added Latency Audio Engine**: Direct ASIO streaming with an isolated, real-time lock-free thread (MMCSS Pro Audio priority).
- 🔀 **Rack & Branch Hybrid Routing**:
  - Linear serial signal chain.
  - 1-click parallel splits (A/B or A/B/C) with independent gain, pan, mute, solo, and phase inversion.
  - Built-in dry/wet mix and output trim on every plugin container.
  - Click-free, equal-power crossfaded bypass and branch toggling.
- 🔌 **Modern Plugin Support**:
  - Native **VST3** and **CLAP** plugin hosting with direct Win32 GUI embedding.
- 🎛️ **MIDI & Instant Scene Switching**:
  - 1-click MIDI learn (CC, Program Change, Notes).
  - Snapshot scenes for switching from Clean to Crunch to Lead without audio pops.
- 🛠️ **Built-in Practice Suite**:
  - Sub-cent precision chromatic/strobe tuner (YIN autocorrelation algorithm).
  - Sample-accurate metronome.
  - Backing track player and looper with A-B repeat.

## Building Praccy

### Requirements
- Windows 10/11 (64-bit)
- Visual Studio 2022 C++ Build Tools (MSVC v143 or Clang-CL)
- Windows 10/11 SDK
- CMake 3.25+
- DirectX 11 support
- ASIO-compatible audio interface or driver (e.g. Focusrite, Universal Audio, RME, FL Studio ASIO)

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

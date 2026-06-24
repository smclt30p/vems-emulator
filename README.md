# VEMS Emulator
<img width="1031" height="1037" alt="vems-emulator" src="https://github.com/user-attachments/assets/83715fb2-bda8-4b09-a8a3-7384f73755d1" />


Runs the **real VEMS firmware** on a simulated ATmega128 (via
[simavr](https://github.com/buserror/simavr)) and talks to **VemsTune**
over TCP — no ECU hardware required.

```
VemsTune --TCP 127.0.0.1:29000--> vems_emulator --> real vems firmware
```

## What it does

- Loads any `.hex` / `.bin` firmware and runs it.
- Serial bridge on `127.0.0.1:29000` for VemsTune.
- Fake inputs: crank/cam trigger, ADC sensors, MCP3208, HIP9011 knock.
- Live output scope: injectors, ignition, the 74x259 / TPIC latches and
  knock integrate/hold plotted against 0–720° crank angle.
- v3 bootloader support for serial firmware updates (load your own
  bootloader hex via **File → Open bootloader**).

The proprietary VEMS bootloader is **not** bundled.

## Build

Needs `clang++`, `make`, `git`, plus SDL2 and libelf:

```sh
brew install sdl2 libelf          # macOS
```

imgui and simavr are git submodules:

```sh
git clone --recurse-submodules <repo-url>
cd emulator
make
```

Already cloned? `git submodule update --init --recursive` first.

`make` patches and builds the simavr submodule, then links the emulator.
See `patches/README.md` for the simavr changes.

## Run

```sh
./vems_emulator                       # then File -> Open firmware
./vems_emulator path/to/vems.hex      # load + auto-start
```

In VemsTune: **Communication → TCP → Host `127.0.0.1`, Port `29000`**.

## Layout

```
src/        emulator sources (ImGui UI + simavr core)
vendor/     imgui + simavr submodules
patches/    local simavr patch (flash counters, libelf paths)
```

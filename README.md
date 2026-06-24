# VEMS Genboard v3.8 Emulator
<img width="1031" height="1037" alt="vems-emulator" src="https://github.com/user-attachments/assets/83715fb2-bda8-4b09-a8a3-7384f73755d1" />


Runs the real VEMS firmware (non-crypto) on a simulated ATmega128 (via
[simavr](https://github.com/buserror/simavr)) and talks to VemsTune
over TCP or COM — no ECU hardware required.

```
VemsTune --TCP 127.0.0.1:29000--> vems_emulator --> real vems firmware
```

## Status

**Working**

- VemsTune connection over TCP or serial
- Reading / writing config pages
- Non-crypto bootloader
- All inputs and outputs faked
- Mocked 60-2 crank signal
- EEPROM save / read to disk
- Knock chip emulation
- 20x4 HD44780 LCD (4-bit)

**TODO**

- Wideband controller
- PS2

## Build

Needs `clang++`, `make`, `git`, plus SDL2 and libelf:

Tested on macOS only (for now).

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

## License

[GPL-3.0-or-later](LICENSE). Submodules keep their own licenses:
simavr (GPL-3.0), Dear ImGui (MIT).

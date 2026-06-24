# simavr patches

`simavr-vems.patch` is applied to the pinned `vendor/simavr` submodule (upstream
buserror/simavr **v1.8**) before building `libsimavr.a`. The top-level `make`'s
`simavr` target applies it automatically (idempotently) and builds the library.

Contents:
- `simavr/sim/sim_avr.h`, `simavr/sim/avr_flash.c` — add three counters to `avr_t`
  (`flash_lockread`, `flash_pageerase`, `flash_pagewrite`) incremented on LPM lock/fuse
  reads and SPM page erase/write, so the emulator UI can show bootloader flash activity.
- `Makefile.common` — locate Homebrew libelf (`/usr/local/Cellar/libelf/*`) instead of
  MacPorts, for building on macOS with `brew install libelf`.

The simavr submodule itself is left at the pristine v1.8 commit; the patch lives in the
working tree only (so `git submodule status` will show it as locally modified after a build).

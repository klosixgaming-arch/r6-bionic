# R6-Bionic: Canon EOS R6 (DIGIC X) QEMU Emulation

**Goal:** Emulate the Canon EOS R6 in QEMU and boot real DryOS firmware 1.7.0 with
**visible graphics** (the camera GUI), so Magic Lantern can be developed and tested in
software.

**Status:** the `canon-r6` QEMU machine is implemented and **compiling**. The firmware
boots past power-init and runs real ROM code. The original boot panic (a deliberate
DryOS spinlock-timeout halt) is diagnosed and the fix — a **custom GIC shim** (the
DIGIC X GIC uses a non-standard register layout the stock QEMU GIC cannot serve) plus a
**core-1 release on GIC SGI 12** — is implemented. Now in the build-and-test loop.

> **Read [`docs/QEMU_FINDINGS.md`](docs/QEMU_FINDINGS.md) first** — it is the live,
> verified record of every address, the GIC protocol, core-1 bring-up, the boot flow,
> and the exact build/run commands.

---

## Why this is hard (and what we found)

QEMU's existing Canon support covers only DIGIC I–IV (ARM7/ARM9) cameras. The R6's
DIGIC X is a dual Cortex-A9 (ARMv7-A, Thumb-2) SoC, so a new QEMU machine is required.

The key reverse-engineering result (confirmed by disassembling the ROM, not guessed):

- **The GIC is at `0xC1000000` with a non-standard register map.** The firmware's IRQ
  dispatcher (`0xE022D470`) reads IAR at `0xC100010C`, writes EOIR at `0xC1000110`, and
  the IPI trigger writes SGIR at `0xC1001F00`. Standard GICv2 puts IAR/EOIR at
  `+0x008/+0x00C` — no stock base aligns DIGIC X's offsets, so a **custom GIC shim** is
  the only way. This was the root cause of the boot panic: with no working GIC, no SGIs
  fire, the dual-core scheduler deadlocks, and DryOS halts with a spinlock-timeout panic.
- **Core 1 is released by GIC SGI 12** (not a simple mailbox write). The entry
  (`0xE0100001`) reads MPIDR and routes core 1 to its secondary init.
- **No ARMv8 generic timer** is used (zero CP15 `c14` instructions in the ROM); timekeeping
  is SGIs + a Marius timer block.

---

## Hardware model (implemented)

```
CPU:        dual ARM Cortex-A9 (ARMv7-A, Thumb-2), core 1 released on SGI 12
RAM:        1 GB @ 0x00000000  +  2 GB @ 0x40000000
GIC:        custom shim @ 0xC1000000  (IAR +0x10C, EOIR +0x110, SGIR +0x1F00, ...)
UART:       PL011 @ 0xBF002000  (best guess — re-verify; not the boot blocker)
Marius:     timer @ 0xC0208000 (TODO), INTCs @ 0xD231A000 / 0xD233A000 (SPI 32/33)
LED GPIO:   0xD22390C0  (ON=0x24D0002, OFF=0x24C0003 — Magic-Lantern verified)
Power/SRAM: 0xDFFC0000  (core-1 mailbox 0xDFFC4FA0)
Flash:      gang100.bin @ 0xE0000000 (64 MB), gang200.bin @ 0xE4000000 (32 MB)
Entry:      0xE0100001 (boot00.bin, Thumb)
Display:    MARV framebuffer reader (descriptor at 0x14B000) -> SDL window, ~30 Hz
```

---

## Repository layout

```
R6-Bionic/
├── README.md               ← this file
├── STATUS.md               ← concise current state + next steps
├── patches/
│   ├── r6.c                ← the QEMU machine (source of truth)
│   └── qemu-r6.patch       ← build integration (Kconfig / default.mak / meson.build)
├── docs/
│   ├── QEMU_FINDINGS.md    ← LIVE verified findings + implementation status (read first)
│   └── PERIPHERAL_MAP.md   ← peripheral register map (from ROM analysis)
├── scripts/
│   └── build.sh            ← clone QEMU, apply patch, build
└── qemu/                   ← local QEMU checkout (gitignored; built in WSL)
```

ROM dumps are **not** committed (too large; see `docs/QEMU_FINDINGS.md` §4 for locations).

---

## Build (WSL2 Ubuntu)

```bash
# deps (once)
sudo apt update && sudo apt install -y g++ ninja-build \
    libglib2.0-dev libpixman-1-dev libslirp-dev libcap-ng-dev libnuma-dev \
    libattr1-dev python3 python3-pyelftools git meson

# build (from scripts/build.sh, or manually)
git clone <qemu-eos-fork> qemu        # QEMU 6.1 based
cd qemu && git apply ../patches/qemu-r6.patch && cp ../patches/r6.c hw/arm/r6.c
mkdir build && cd build
../configure --target-list=arm-softmmu --enable-docs=no --disable-werror
ninja -j$(nproc)
```

## Run (with graphics)

```bash
# put the ROMs in the current directory: boot00.bin gang100.bin gang200.bin
./qemu/build/qemu-system-arm -M canon-r6 -smp 2 -m 3G \
    -bios boot00.bin \
    -monitor unix:/tmp/r6mon.sock,server,nowait
# DO NOT pass -nographic — WSLg shows the SDL window.
# Success line on stderr: [r6] display: MARV@0x... bitmap=0x... 2048x1080
```

---

## What works / what's next

- ✅ `canon-r6` machine, dual Cortex-A9, memory map, ROM loading
- ✅ **Custom GIC shim** (DIGIC X layout) + core-1 release on SGI 12
- ✅ Vector table at the firmware VBAR (`0xE0300980`), IRQ → firmware dispatcher
- ✅ MARV framebuffer reader (GUI graphics once the firmware renders)
- ✅ MMIO logging stub (learn register layouts from the firmware's own behaviour)
- ⏳ Marius timer model (SPI 32/33) — if the OS tick needs it
- ⏳ UART base re-verification (console output)
- ⏳ Input injection (virtual keypad) — once the GUI is visible
- ⏳ SD controller / EDMAC — for ML file I/O and MLV

## Magic Lantern compatibility

The R6.150 ML platform is directly compatible with 1.7.0 (same CPU, memory map, boot
addresses, GUI codes, CBasic functions). Once the camera GUI renders in QEMU, ML modules
(hello_world, false_color, histogram, zebra, …) are the target workload.

---

*Project: R6-Bionic · Canon EOS R6, DIGIC X, firmware 1.7.0 · QEMU 6.1 fork*

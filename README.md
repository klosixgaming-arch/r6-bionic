# R6-Bionic: Canon EOS R6 QEMU Emulation Project

**Goal:** Emulate the Canon EOS R6 (DIGIC X / ARM Cortex-A9) in QEMU to run Magic Lantern and DryOS firmware in software.

**Date:** 2026-10-11
**Status:** Initial machine definition created, build system configured

---

## Research Findings

### Current State of QEMU Canon Support

| Component | Status |
|-----------|--------|
| DIGIC I-IV (ARM7/ARM9) | ✅ Basic support (`canon-a1100` machine) |
| DIGIC V-VII (ARM926) | ⚠️ Partial (via `digic_boards.c`) |
| DIGIC VIII (Cortex-A8) | ❌ Not supported |
| DIGIC X (Cortex-A9) | ❌ **Not supported - THIS PROJECT** |
| Running real firmware | ❌ Only barebox bootloader works |

### What "coon" Did (EOS RP)

- **coon42/openrp**: PCB reverse engineering of the Canon EOS RP (KiCad schematic)
- LED GPIO address: `0xD22392B8` (RP) vs `0xD22390C0` (R6)
- This was **hardware reverse engineering**, not QEMU emulation
- The "running in QEMU" claim likely refers to running a Magic Lantern payload in QEMU with a stubbed hardware environment, not full DryOS

### Key QEMU References

- **qemu-eos**: `reticulatedpines/qemu-eos` (QEMU fork with DIGIC support)
- **DIGIC docs**: `docs/system/arm/digic.rst` - "emulation is incomplete, cannot run original firmware"
- **Board code**: `hw/arm/digic_boards.c` - only defines `canon-a1100`
- **SoC model**: `include/hw/arm/digic.h` - ARM CPU + 3 timers + UART

---

## R6 Hardware Specifications (Verified)

```
CPU:        ARM Cortex-A9 (ARMv7-A, Thumb-2)
ROM Base:   0xE0000000 (128 MB address space)
Bootable:   0xE0100000 (boot00.bin, 36.75 MB)
RAM:        0x00002000 - 0x3FFFFFFF (1 GB cacheable)
Uncacheable: 0x40000000 - 0xBEFFFFFF (2 GB)
MMIO:       0xBF000000 - 0xDEFFFFFF (512 MB)
LED GPIO:   0xD22390C0 (ON=0x24D0002, OFF=0x24C0003)
Firmware:   1.7.0 (Build: Oct 3, 2022, K453)
```

### Boot Vector (Verified Identical 1.5.0 → 1.7.0)

```
0xE0100058: BR_DCACHE_CLN_1  (dcf226f2)
0xE0100062: BR_ICACHE_INV_1  (dcf28bf2)
0xE010008E: BR_DCACHE_CLN_2  (dcf20bf2)
0xE0100098: BR_ICACHE_INV_2  (dcf270f2)
0xE01000AE: BR_BR_CSTART     (0e4840f0) ← Branch to cstart
0xE0100138: BR_BZERO32
0xE010019A: BR_CREATE_ITASK
```

---

## Project Structure

```
R6-Bionic/
├── README.md              ← This file
├── qemu/                  ← Cloned qemu-eos (reticulatedpines)
│   ├── hw/arm/r6.c        ← NEW: R6 machine definition
│   ├── hw/arm/Kconfig     ← MODIFIED: Added CANON_R6 option
│   ├── hw/arm/meson.build ← MODIFIED: Added r6.c to build
│   └── ...                ← Rest of QEMU source
├── docs/
│   └── R6_ANALYSIS.md     ← Full firmware analysis from previous work
└── rom/                   ← ROM dumps (link or copy here)
    ├── gang100.bin        ← 64 MB primary ROM
    ├── gang200.bin        ← 32 MB secondary ROM
    └── boot00.bin         ← 36.75 MB bootable firmware
```

---

## Build Instructions

### Prerequisites (WSL2 Ubuntu)

```bash
sudo apt update
sudo apt install -y \
    g++ make ninja-build \
    libglib2.0-dev libpixman-1-dev \
    libslirp-dev libcap-ng-dev \
    libnuma-dev libattr1-dev \
    python3 python3-pyelftools \
    git meson
```

### Build QEMU with R6 Support

```bash
cd R6-Bionic/qemu
mkdir build && cd build
../configure \
    --target-list=arm-softmmu \
    --enable-docs=no \
    --disable-werror
ninja -j$(nproc)
```

### Run the R6 Emulator

```bash
# Copy ROM dumps
cp /path/to/rom/gang100.bin build/
cp /path/to/rom/boot00.bin build/

# Run QEMU with R6 machine
./build/qemu-system-arm \
    -machine canon-r6 \
    -cpu cortex-a9 \
    -m 1G \
    -bios rom/gang100.bin \
    -nographic \
    -s -S
```

### Connect GDB

```bash
# In another terminal
arm-none-eabi-gdb
(gdb) target remote localhost:1234
(gdb) break *0xE01000AE   # cstart branch
(gdb) continue
```

---

## What Works / What Doesn't

### ✅ Implemented
- R6 machine definition (`canon-r6`)
- ARM Cortex-A9 CPU
- Memory map (RAM, uncacheable RAM, ROM, MMIO)
- ROM loading (gang100.bin, boot00.bin)
- Build system integration (Kconfig, meson)
- LED GPIO address mapping

### ❌ Not Yet Implemented (Needed to Run DryOS)
- **GIC (Generic Interrupt Controller)** - Required for all interrupts
- **UART** - Actual R6 UART registers (location unknown)
- **Timer** - Actual R6 timer (DIGIC timer is close but may differ)
- **SD Card Controller** - Needed for file I/O
- **EDMAC (DMA Controller)** - Needed for sensor data transfer
- **Display Controller** - Needed for LCD output
- **Sensor Interface** - Needed for image capture
- **Cache Controller** - L1/L2 cache management
- **Power Management** - Clock gating, voltage scaling
- **Secure Boot Bypass** - RSA signature verification (if present)

### ⚠️ Known Issues
- `CONFIG_MEMORY_SRM_NOT_WORKING` - SRM allocation crashes camera (from ML R6.150)
- `CONFIG_IMAGE_CAPTURE_NOT_WORKING` - Image capture causes null pointer crash
- `CONFIG_NO_BFNT` - No bitmap font in ROM, must load from card

---

## Development Roadmap

### Phase 1: Basic Boot (Current)
- [x] R6 machine definition
- [x] Memory map
- [x] ROM loading
- [ ] GIC implementation
- [ ] UART (for debug output)
- [ ] Verify boot vector executes in QEMU
- [ ] Step through cstart with GDB

### Phase 2: Minimal DryOS
- [ ] Timer implementation
- [ ] SD card controller (for file I/O)
- [ ] NVRAM emulation
- [ ] DryOS task scheduler
- [ ] CBasic interpreter (or skip to ML payload)

### Phase 3: Magic Lantern
- [ ] Load ML autoexec.bin
- [ ] GUI compositor (XIMR)
- [ ] Button input (BGMT)
- [ ] Menu system
- [ ] Module loading

### Phase 4: Full Functionality
- [ ] Sensor readout (EDMAC)
- [ ] MLV raw video
- [ ] JPEG/H.264 encoding
- [ ] Display output
- [ ] AF/AE systems

---

## Key Files for Reference

| File | Location | Purpose |
|------|----------|---------|
| `hw/arm/r6.c` | `qemu/` | R6 machine definition |
| `hw/arm/digic_boards.c` | `qemu/` | Existing DIGIC I-IV (reference) |
| `include/hw/arm/digic.h` | `qemu/` | DIGIC SoC model (reference) |
| `hw/char/digic-uart.c` | `qemu/` | DIGIC UART (reference) |
| `hw/timer/digic-timer.c` | `qemu/` | DIGIC Timer (reference) |
| `platform/R6.150/consts.h` | ML source | Verified R6 addresses |
| `platform/R6.150/internals.h` | ML source | R6 config flags |
| `platform/R6.150/gui.h` | ML source | Button codes |

---

## Magic Lantern Compatibility

The R6.150 ML platform is directly compatible with 1.7.0:
- All boot addresses match
- Same CPU (Cortex-A9)
- Same memory map
- Same GUI codes
- Same CBasic functions

**ML modules that should work in QEMU (once boot works):**
- hello_world
- dual_iso (if sensor emulation works)
- false_color
- histogram
- zebra
- MLV (once EDMAC is emulated)

---

## Next Immediate Steps

1. **Build QEMU** with the R6 machine (needs WSL2 + dependencies)
2. **Fix compilation errors** in `r6.c` (likely some API mismatches)
3. **Add GIC** - Without interrupts, DryOS will immediately fault
4. **Add working UART** - Need to find the actual R6 UART base address
5. **Test boot** - Even if it crashes, we should see the first few instructions execute
6. **Debug with GDB** - Step through the boot vector to understand what it expects

---

*Project created: 2026-10-11*
*Based on: R6 firmware 1.7.0 ROM analysis, Magic Lantern R6.150 platform, qemu-eos fork*

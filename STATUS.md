# R6-Bionic - QEMU Emulation Status

**Date:** 2026-10-11
**Location:** `C:\Users\Mega\Downloads\R6-Bionic\`

---

## What We Did

### 1. Research

| Source | Finding |
|--------|---------|
| `reticulatedpines/qemu-eos` | QEMU fork with basic DIGIC I-IV support. Only `canon-a1100` (PowerShot, ARM9) machine exists. **No DIGIC X / R6 support.** |
| `docs/system/arm/digic.rst` | "Emulation is incomplete. Cannot run original camera firmware, only barebox bootloader." |
| `hw/arm/digic_boards.c` | Single board: `canon-a1100` (DIGIC IV, ARM946, 64MB RAM, 4MB flash) |
| `coon42/openrp` | PCB reverse engineering of EOS RP (KiCad). **Not QEMU emulation.** LED at 0xD22392B8. |
| ML forum topic 2864 | "How to run Magic Lantern into QEMU?!" - long thread, basic DIGIC I-IV only |
| ML forum topic 23814 | "Canon emulator" - "ARM CPU emulation is the smallest problem since it is provided by Qemu. All the custom or very specific chips around it are the difficult ones." |

**Bottom line:** No one has a working R6/RP in QEMU. The existing DIGIC support covers ARM7/ARM9 cameras only. The R6's Cortex-A9 (ARMv7-A) is a different architecture that needs new QEMU machine code.

### 2. Implementation

Created `hw/arm/r6.c` - a new QEMU machine definition for the Canon EOS R6:

```c
// Key features:
- ARM Cortex-A9 CPU
- 1 GB cacheable RAM (0x00002000)
- 2 GB uncacheable RAM (0x40000000)
- 512 MB MMIO region (0xBF000000)
- 128 MB ROM region (0xE0000000)
- ROM loading (gang100.bin + boot00.bin)
- Boot vector at 0xE0100000
- LED GPIO at 0xD22390C0
```

Modified:
- `hw/arm/Kconfig` - Added `CONFIG_CANON_R6` option
- `hw/arm/meson.build` - Added `r6.c` to build

### 3. Build Environment

- WSL2 Ubuntu: ✅ Working
- Dependencies installed: ✅ (ninja, glib, pixman, python3, etc.)
- CRLF fixed: ✅
- Full build: ⏳ Too slow on Windows filesystem (needs native WSL storage)

---

## What's Needed to Actually Run the Firmware

The R6's DryOS expects a LOT of hardware. Here's the gap:

| Component | Status | Effort |
|-----------|--------|--------|
| CPU (Cortex-A9) | ✅ QEMU has it | Done |
| Memory map | ✅ Defined in r6.c | Done |
| ROM loading | ✅ Implemented | Done |
| **GIC (Interrupts)** | ❌ Missing | **HIGH** - Without this, DryOS faults immediately |
| **UART** | ❌ Missing | **MEDIUM** - Need actual R6 UART registers |
| **Timer** | ⚠️ DIGIC timer exists but may differ | MEDIUM |
| **SD Card Controller** | ❌ Missing | HIGH - Needed for file I/O |
| **EDMAC (DMA)** | ❌ Missing | HIGH - Needed for sensor data |
| **Display Controller** | ❌ Missing | HIGH - Needed for LCD |
| **Cache Controller** | ⚠️ QEMU has L1/L2 | LOW |
| **Secure Boot** | ⚠️ Unknown | MEDIUM - RSA verification may block |

### Minimum to See the Boot Vector Execute

1. **GIC** - Without interrupt controller, the first exception (even a timer tick) will fault
2. **UART** - So we can see debug output
3. **Timer** - DryOS uses timers for scheduling

### Minimum to Run Magic Lantern

4. **SD Card** - ML loads from SD card
5. **Display** - ML GUI renders to LCD framebuffer
6. **Button input** - BGMT event system

---

## Recommended Next Steps

### Priority 1: Get the Boot Vector Running
1. Implement a basic GICv2 (or use QEMU's existing `arm_gic` with correct settings)
2. Find the R6 UART base address (search ROM for UART register patterns)
3. Wire up the timer
4. Run QEMU + GDB, step through the first 20 instructions
5. Identify what the firmware expects at each step

### Priority 2: Basic I/O
6. Implement SD card controller (can reuse QEMU's `sdhci` with R6-specific registers)
7. Implement framebuffer display (we know the VRAM address from ML: `FEATURE_VRAM_RGBA`)
8. Implement button input (BGMT codes from `gui.h`)

### Priority 3: Full Emulation
9. EDMAC DMA controller (236 references in firmware)
10. Sensor interface
11. ISP/JPEG pipeline (or stub)

---

## How to Build (When Ready)

```bash
# Copy source to WSL native filesystem for speed
cp -r /mnt/c/Users/Mega/Downloads/R6-Bionic ~/R6-Bionic
cd ~/R6-Bionic/qemu
mkdir build && cd build
../configure --target-list=arm-softmmu --disable-werror
ninja -j$(nproc)

# Run
cp /path/to/gang100.bin .
cp /path/to/boot00.bin .
./qemu-system-arm -machine canon-r6 -cpu cortex-a9 -m 1G -nographic -s -S
```

---

## File Manifest

```
R6-Bionic/
├── README.md                    ← Full project documentation
├── STATUS.md                    ← This file
└── qemu/                        ← qemu-eos fork with R6 additions
    ├── hw/arm/r6.c             ← NEW: R6 machine (283 lines)
    ├── hw/arm/Kconfig          ← MODIFIED: +CANON_R6
    ├── hw/arm/meson.build      ← MODIFIED: +r6.c
    └── ...                     ← QEMU source
```

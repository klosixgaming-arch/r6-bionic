# R6-Bionic — Current Status

**Updated:** 2026-10-12
**Location:** `C:\Users\Mega\Downloads\R6-Bionic\` (GitHub `klosixgaming-arch/r6-bionic`)

## Where we are

The `canon-r6` QEMU machine is **implemented and compiling**. The firmware (1.7.0)
boots past the boot ROM, power-init handshake, and runs real ROM code. It previously
hit a **deliberate DryOS halt** — a spinlock-timeout panic (Module 64 / Panic 6) at
`0xE01AE2DC`. That is a *symptom*, not a hang.

**Root cause (confirmed by disassembling the ROM):** the firmware's GIC lives at
`0xC1000000` with a **non-standard register layout** (IAR `+0x10C`, EOIR `+0x110`,
SGIR `+0x1F00`, distributor at the *higher* address). QEMU's stock GIC cannot serve
that map, so no IRQs routed and no SGIs fired → the dual-core scheduler's IPI
synchronization deadlocked → spinlock timeout → panic.

## What was fixed (this session)

1. **Custom GIC shim** at the DIGIC X address/offset map (IAR/EOIR/SGIR/ISENABLER/
   ICENABLER/ITARGETSR/ICFGR/CTR/PMR). Replaces the stock `arm_gic`.
2. **Core-1 release on GIC SGI 12** (the firmware's verified release kick; boot trace's
   last write is `0xC1001F00 <= 0x0002000C`). Core 1 starts powered-off and is started
   at the entry `0xE0100001` (which routes core 1 to its secondary init via MPIDR).
3. **Vector table** installed at the firmware VBAR `0xE0300980` (and the older
   `0x49234822`), IRQ → the firmware's own dispatcher `0xE022D470`.
4. **Removed** the wrong stock GIC + A9 PL320/PL031 timers (firmware uses neither).
5. **MMIO disambiguation:** the 512 MB logging stub is mapped first; GIC/UART/power/LED
   mapped after so they win (QEMU 6.1 rule: equal priority → newest-added wins).
6. Kept the working pieces: LED GPIO, power/mailbox block, battery seed, and the **MARV
   framebuffer reader** that renders the GUI once the firmware gets past the scheduler.

## Build status

- `patches/r6.c` — rewritten, **compiles clean** in the QEMU 6.1.93 fork (`ninja` OK).
- Next: run **with graphics** (no `-nographic`) and read the new boot trace.

## Immediate next steps

1. Run QEMU (graphics) and capture the new `boot.log`.
2. If a new panic appears, decode its Module/Panic code + the spinning PC and map it to
   the next missing peripheral (likely the **Marius timer** tick, SPI 32/33).
3. Implement the Marius timer model if the OS tick needs it.
4. Re-verify the **UART** base/flag offsets from the firmware TX poll loop (console output).
5. Once the GUI renders: add **input injection** (virtual keypad) and start ML modules.

## Hardware gap (what DryOS still needs)

| Component | Status | Needed for |
|-----------|--------|-----------|
| CPU (dual Cortex-A9) | ✅ | everything |
| Memory map + ROM loading | ✅ | boot |
| **GIC (custom shim)** | ✅ implemented | scheduler IPIs, ticks |
| Core-1 bring-up (SGI 12) | ✅ implemented | dual-core scheduler |
| **Marius timer** | ⏳ TODO | OS tick / HPTimer |
| UART | ⚠️ best-guess base | console debug |
| SD controller | ❌ | ML file I/O |
| EDMAC (DMA) | ❌ | sensor / MLV |
| Display (MARV reader) | ✅ wired | GUI graphics |
| Button input | ⏳ after GUI | ML menus |

## Build & run (quick reference)

```bash
# build (WSL)
cp patches/r6.c ~/r6b-qemu/hw/arm/r6.c
cd ~/r6b-qemu/build && ninja -j32 qemu-system-arm

# run WITH graphics (ROMs in cwd)
cd ~/r6b-run
~/r6b-qemu/build/qemu-system-arm -M canon-r6 -smp 2 -m 3G \
    -bios boot00.bin -monitor unix:/tmp/r6mon.sock,server,nowait
# success: [r6] display: MARV@0x... bitmap=0x... 2048x1080
```

> Full details, every verified address, and the agent-disagreement resolutions are in
> [`docs/QEMU_FINDINGS.md`](docs/QEMU_FINDINGS.md).

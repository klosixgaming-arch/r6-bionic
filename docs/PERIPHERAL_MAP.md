# R6-Bionic — Peripheral Register Map (DIGIC X, firmware 1.7.0)

**Source:** ROM disassembly of `boot00.bin` / `gang100.bin` (base `0xE0100000`),
cross-checked with the hardware agent. **The ROM is the tie-breaker** where reports
disagreed. See [`QEMU_FINDINGS.md`](QEMU_FINDINGS.md) for the full record.
**Updated:** 2026-10-12

> ⚠️ Earlier drafts listed the GIC at `0xBF000000` and the UART at `0xBF002000`.
> Disassembly shows the **GIC is at `0xC1000000`** with a non-standard layout, and the
> UART base is still unconfirmed. Corrected below.

---

## Confirmed peripherals

| Peripheral | Base Address | Evidence | Notes |
|-----------|-------------|----------|-------|
| **GIC (DIGIC X)** | **`0xC1000000`** | IRQ dispatcher `0xE022D470`; SGIR `0xE0163C34` | **Non-standard layout — custom shim required** |
| **Core-1 mailbox** | `0xDFFC4FA0` | boot ROM `gang100 @ 0xE0004900/0xE000B2DC` | value = `0xE0100001`; released on **SGI 12** |
| **LED GPIO** | `0xD22390C0` | Magic Lantern R6.150 (hardware-verified) | ON=`0x24D0002`, OFF=`0x24C0003` |
| **Marius timer** | `0xC0208000` | hardware agent | ch0 @ `0xC0208890`, 16 B/channel |
| **Marius INTC** | `0xD231A000` | hardware agent | read-id `+0x00`, ACK `+0x10`, SPI 32 |
| **Omar INTC** | `0xD233A000` | hardware agent | read-id `+0x00`, ACK `+0x10`, SPI 33 |
| **UART0** | `0xBF002000` (best guess) | 29 ROM literal refs (struct-table, unconfirmed) | **re-verify** — also a clean thunk to `0xC0800000` |
| **VBAR (vector table)** | `0xE0300980` | entry `0xE0100000` | set for both cores; IRQ dispatcher `0xE022D470` |
| **MARV layer 0 (GUI)** | `0x14B000` | hardware agent (live 1.7.0 dump) | sig `0x5652414D`; +0x04 bitmap, +0x1C w, +0x20 h |
| **Battery/Temp** | `0x11D28C`/`0x11D290` | hardware agent | mV / °C |

---

## GIC (DIGIC X) — the critical finding

**Absolute register map (implemented by the custom shim):**

| Address | Register | Notes |
|---------|----------|-------|
| `0xC1000100` | GICC_CTLR | boot writes `0x1` |
| `0xC1000104` | GICC_PMR | boot writes `0xFF` (allow all) |
| `0xC100010C` | **GICC_IAR** | read → INTID, or `1023` if none |
| `0xC1000110` | **GICC_EOIR** | write → INTID |
| `0xC1001000` | GICD_CTLR | boot writes `0x1` |
| `0xC1001080+n*4` | GICD_ISENABLER[n] | set enable bits (word n = IRQs 32n..32n+31) |
| `0xC1001180+n*4` | GICD_ICENABLER[n] | write-1-clears |
| `0xC1001800+n*4` | GICD_ITARGETSR[n] | per-IRQ CPU target mask |
| `0xC1001C00+n*8` | GICD_ICFGR[n] | edge-triggered |
| `0xC1001F00` | **GICD_SGIR** | `[31:16]=TargetList`, `[9:0]=INTID` |

**Why not the stock QEMU GIC:** standard GICv2 has IAR@`+0x008`/EOIR@`+0x00C` from the
CPU-interface base; DIGIC X puts them at `+0x10C`/`+0x110` and puts the distributor at
the *higher* address. No stock base aligns all four registers.

**SGI roles:** SGI 10 = scheduling (context switch), SGI 11 = timer tick, SGI 12 =
**core-1 release kick**. SPI 32/33 = Marius/Omar INTCs.

**Interrupt vector table:** the firmware's registered vector table has **640 entries**
(IRQ 0..639): EDMAC[0-63], POSTMAN[104-119], MARIUS_TIMER[120-151], DOLPHIN[170-185],
SDIO[260-275], MIPI DSI[340-355], TSENS[585], XIMR[614].

---

## Core-1 bring-up

```
core 0 (boot00 entry 0xE0100001):
    VBAR = 0xE0300980 ; SP = 0x2000
    read MPIDR
    core 0 -> cold-boot copy + cstart (0xE01000EC path)
    core 1 -> 0xE01000EC secondary init (calls 0xE01003B8)

release:
    core 0 fires GIC SGI 12 to CPU 1  (SGIR 0xC1001F00 <= 0x0002000C)
    core 1 wakeup: ldr r0, [0xDFFC4FA0] ; bx r0   ; -> 0xE0100001
```

QEMU: core 1 starts powered-off; the GIC shim starts it on SGI 12 (or a mailbox write).

---

## Boot entry (boot00, Thumb)

```
0xE0100000 : ldr r0, [pc, #0x24]      ; VBAR value
0xE0100002 : mcr p15, #0, r0, c12, c0, #0   ; VBAR = 0xE0300980
0xE010000A : movw r0, #0x2000 ; movt r0, #0 ; mov sp, r0   ; SP = 0x2000
0xE0100014 : mrc p15, #0, r5, c0, c0, #5   ; MPIDR
0xE0100018 : ands r0, r5, #0xf
0xE010001C : bne 0xE0100022            ; core != 0 -> secondary path
0xE010001E : b.w 0xE010002C            ; core 0 -> cold-boot copy + cstart
0xE0100022 : b.w 0xE01000EC            ; core 1 -> secondary init
```

---

## Print / panic internals (for debugging QEMU hangs)

- `0xE04BF000` = `print_msg(fmt, ...)` (AAPCS r0=fmt, r1..r3=args), spinlock `0x4144`.
- Spinlock primitive `0xE0354F64` (`ldrex/strex`), timeout counter from `0xE0CBD51C`.
- Panic print `0xE01AE2CC`: `"DRYOS PANIC: Module Code=%d, Panic Code=%d[puId %d]"`;
  Module 64 = kernel, Panic 6 = lock timeout; infinite `b .` at `0xE01AE2DC`.
- `0xE07C8F74` = `kernel_unlock()` (`*0x4024 = 0`); `0xE07C8B08` = yield;
  `0xE07C8E18` = send reschedule IPI (SGI 10).

---

## Still to confirm

- **UART base + flag offsets** — the TX poll loop will tell us (`0xBF002000` vs
  `0xC0800000` vs other). Not the boot blocker.
- **gang200 real base** — `0xE4000000` (current) vs `0xF0000000` (one report).
- **Marius INTC** read-id/ack protocol (only needed when modelling EDMAC/Omar).

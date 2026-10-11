# R6-Bionic - Peripheral Register Map

**Source:** ROM analysis of firmware 1.7.0 (boot00.bin)
**Date:** 2026-10-11

---

## Confirmed Peripherals

| Peripheral | Base Address | References | Notes |
|-----------|-------------|------------|-------|
| **UART** | 0xBF002000 | 1,391 | Primary debug console. "StopRedirectUART" function exists |
| **GIC (INTC)** | 0xBF000000 | 402 | Interrupt controller. Distributor at +0x100 |
| **GIC Distributor** | 0xBF000100 | 8 | ISENABLER at +0x100 (GICv2 pattern) |
| **GIC CPU IF (2)** | 0xC0000100 | 5+ | Second interrupt controller (multi-core) |
| **GIC CPU IF (3)** | 0xD0000100 | 5+ | Third interrupt controller |
| **GIC CPU IF (4)** | 0xD2000100 | 5+ | Fourth interrupt controller |
| **LED GPIO** | 0xD22390C0 | Verified | ON=0x24D0002, OFF=0x24C0003 |
| **MMIO Block 2** | 0xBF006000 | 19 | Unknown (timer? clock?) |
| **MMIO Block 3** | 0xBF00B000 | 52 | Unknown (SD controller?) |
| **MMIO Block 4** | 0xBF00E000 | 26 | Unknown |

---

## GIC Analysis

The R6 appears to have **multiple GIC instances** (or one GIC with multiple CPU interfaces):

```
0xBF000000 : GIC Distributor base (402 refs)
0xBF000100 : GIC Distributor + 0x100 (ISENABLER - standard GICv2)
0xC0000000 : GIC instance 2 base
0xC0000100 : GIC instance 2 + 0x100
0xD0000000 : GIC instance 3 base
0xD0000100 : GIC instance 3 + 0x100
0xD2000000 : GIC instance 4 base
0xD2000100 : GIC instance 4 + 0x100
```

This suggests either:
- A 4-core Cortex-A9 with per-core GIC CPU interfaces (most likely)
- Or multiple independent interrupt domains

**For QEMU:** Implement as a single GICv2 with 4 CPU interfaces, or start with 1 core and 1 GIC.

---

## UART Analysis

- Base: **0xBF002000** (1,391 references - very heavily used)
- 54 unique register addresses in the 0xBF002000-0xBF002FFF block
- "StopRedirectUART" function at 0xE04C2BA8 (context: gyro data, Hall sensors)
- "console" string found (output, file, both, lv_dispafframe)

**Likely register layout** (standard UART pattern):
```
0xBF002000 : DR  (Data Register)
0xBF002004 : RSRECR (Receive Status)
0xBF002008 : RTSRDR (Transmit Status)  
0xBF00200C : CR  (Control Register)
0xBF002010 : IBRD (Integer Baud Rate)
0xBF002014 : FBRD (Fractional Baud Rate)
0xBF002018 : LCRH (Line Control)
0xBF00201C : OF (Offset)
0xBF002020 : ECC (Error Clear)
0xBF002024 : IMS (Interrupt Mask Set)
0xBF002028 : MIS (Masked Interrupt Status)
0xBF00202C : RIC (Raw Interrupt Clear)
0xBF002030 : PMCR (Power Management Control)
```
(This is a guess based on PL011/DesignStart UART layout - needs verification)

---

## MMIO Block Map

```
0xBF000000 : 18 registers  ← GIC Distributor
0xBF001000 : ?
0xBF002000 : 54 registers  ← UART
0xBF003000 : 1 register
0xBF004000 : 2 registers
0xBF006000 : 19 registers  ← ? (timer/clock)
0xBF007000 : 1 register
0xBF008000 : 13 registers  ← ?
0xBF009000 : 10 registers  ← ?
0xBF00B000 : 52 registers  ← ? (SD controller?)
0xBF00C000 : 3 registers
0xBF00D000 : 4 registers
0xBF00E000 : 26 registers  ← ?
0xBF00F000 : 16 registers
```

---

## Boot Vector (Verified)

```
0xE0100000 : Entry point (first instruction)
0xE0100028 : 0xE0300980 (initial SP or PC target)
0xE0100058 : BR_DCACHE_CLN_1  (dcf226f2)
0xE0100062 : BR_ICACHE_INV_1  (dcf28bf2)
0xE010008E : BR_DCACHE_CLN_2  (dcf20bf2)
0xE0100098 : BR_ICACHE_INV_2  (dcf270f2)
0xE01000AE : BR_BR_CSTART     (0e4840f0) ← Jump to cstart
0xE0100138 : BR_BZERO32
0xE010019A : BR_CREATE_ITASK
```

The boot sequence:
1. D-cache clean (flush)
2. I-cache invalidate
3. Branch to cstart (Canon's init function)
4. cstart calls BZERO32 (memory zero)
5. cstart calls CREATE_ITASK (create first task)

---

## Key Functions for QEMU Stubbing

To get the firmware to run (even briefly), QEMU needs to respond to:

1. **Cache operations** (0xE0100058-0xE0100098) - These are ARM instructions (MCR/MRC), QEMU handles these natively
2. **cstart function** - Need to find where it jumps to and what it expects
3. **Memory zeroing** (BZERO32) - QEMU RAM handles this
4. **Task creation** (CREATE_ITASK) - Needs OS-level emulation
5. **UART output** (0xBF002000) - For debug messages
6. **Timer interrupts** (GIC + timer) - For scheduling

---

## Next Steps for QEMU Implementation

1. **Stub UART** at 0xBF002000 - Respond to writes (print to host stdout)
2. **Stub GIC** at 0xBF000000 - Accept writes, don't raise interrupts (initially)
3. **Stub timer** at 0xBF006000 - Accept writes
4. **Run boot vector** - See how far it gets before faulting
5. **Debug with GDB** - Identify each missing peripheral
6. **Iterate** - Add each peripheral as needed

The strategy is "bottom-up stubbing": start with minimal hardware, run the boot code, see what it accesses, add that peripheral, repeat.

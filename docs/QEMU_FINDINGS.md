# R6-Bionic QEMU — Findings & Implementation Status (LIVE)

> **Purpose:** single source of truth so work survives context-window compression.
> **Last updated:** 2026-10-12 by the LMSTUDIO agent. Cross-referenced with the
> "CLINE" agent (Discord + handoffs) and **re-verified against the ROM** where the
> two agents disagreed.
> **Status:** the `r6.c` machine is **rewritten and compiling**. The root cause of
> the boot panic (wrong GIC address + layout) is fixed with a **custom GIC shim** and
> a **core-1 release on SGI 12**. Now in the build-and-test loop to get the GUI to render.

---

## 0. EXECUTIVE SUMMARY

- **Target:** boot Canon EOS R6 (DIGIC X, fw 1.7.0) in QEMU with **visible graphics**
  (the camera GUI via the MARV framebuffer descriptor at `0x14B000`).
- **Progress:** past the bootloader, power-init handshake, and executing real ROM code.
  It previously hit a **DRYOS PANIC: spinlock timeout** (Module 64 / Panic 6) at
  `0xE01AE2DC` (an infinite `b .`) — a *deliberate halt*, not a hang.
- **Root cause (CONFIRMED by disassembly):** the firmware's GIC is at `0xC1000000`
  with a **non-standard register layout** that QEMU's stock GIC cannot serve. No IRQs
  routed, no SGIs fired ⇒ the dual-core scheduler's IPI sync deadlocks ⇒ spinlock
  times out ⇒ panic.
- **Fix (IMPLEMENTED, compiling):** a custom GIC shim at the DIGIC X address/offset
  map, dual Cortex-A9 (AArch32/Thumb), core-1 released on GIC **SGI 12** (the
  firmware's verified release kick), vector table installed at the firmware's VBAR
  (`0xE0300980`) with IRQ → the firmware's own dispatcher (`0xE022D470`), MMIO stub
  added first so real devices win disambiguation.
- **Next:** rebuild, run **without `-nographic`** (WSLg SDL window), and watch for
  `[r6] display: MARV@...` = GUI up. Iterate on any new panic/hang.

---

## 1. VERIFIED GROUND TRUTH (ROM disassembly + hardware agent)

> Where the two agents disagreed, the **ROM is the tie-breaker**. CLINE was wrong
> twice (see §7); both were corrected here from the ROM.

### 1.1 CPU model
- **Dual Cortex-A9-class** (firmware is **100% AArch32 Thumb-2**; it never switches to
  AArch64). SCTLR.TE==1 ⇒ exception vectors execute in Thumb.
- **ZERO CP15 `c14` (ARMv8 generic timer) instructions** in the whole 38.5 MB firmware
  (full-ROM scan). ⇒ Do NOT rely on the ARM generic timer / PL320 / PL031.
- Timekeeping + inter-core sync use:
  - **GIC SGIs:** SGI 10 = GIC_scheduling (context switch), SGI 11 = GIC_timer (tick),
    SGI 12 = GIC_suspend (**core-1 release kick**).
  - **Marius timer block** (peripheral) for HPTimer / OS tick.

### 1.2 GIC — THE CRITICAL FINDING (non-standard layout, CONFIRMED)
Firmware IRQ dispatcher at `0xE022D470`:
```
mov.w r0, #0xC1000000 ; ldr r7, [r0, #0x10c]   ; IAR  -> INTID (low 10 bits)
... dispatch on INTID ...
mov.w r0, #0xC1000000 ; str r7, [r0, #0x110]   ; EOIR <- INTID
```
SGIR trigger at `0xE0163C34`:
```
orr.w r0, r0, r1, lsl #16   ; (TargetList<<16) | INTID
movw  r1, #0x1f00 ; movt r1, #0xc100   ; -> 0xC1001F00
str   r0, [r1]                     ; GICD_SGIR
```
**Absolute register map (the shim implements these exact addresses):**

| Address    | Register        | Notes |
|------------|-----------------|-------|
| `0xC1000100` | GICC_CTLR      | boot writes `0x1` |
| `0xC1000104` | GICC_PMR       | boot writes `0xFF` (allow all) |
| `0xC100010C` | **GICC_IAR**   | read → pending INTID, or **1023** if none |
| `0xC1000110` | **GICC_EOIR**  | write → INTID |
| `0xC1001000` | GICD_CTLR      | boot writes `0x1` |
| `0xC1001080+n*4` | GICD_ISENABLER[n] | set enable bits (word n = IRQs 32n..32n+31) |
| `0xC1001180+n*4` | GICD_ICENABLER[n] | write-1-clears enable bits |
| `0xC1001800+n*4` | GICD_ITARGETSR[n] | per-IRQ CPU target mask |
| `0xC1001C00+n*8` | GICD_ICFGR[n]    | edge-triggered (ignored by shim) |
| `0xC1001F00` | **GICD_SGIR**    | `[31:16]=TargetList, [9:0]=INTID` |

**Why the stock GIC fails:** standard ARM GICv2 has IAR@+0x008 / EOIR@+0x00C from the
CPU-interface base. DIGIC X puts them at +0x10C/+0x110, and the distributor is at the
**higher** address (`0xC1001000`) — opposite of the textbook A9 map. No single stock
base aligns all four registers ⇒ **a custom GIC shim is required.**

**IRQ-dispatch detail (from `0xE022D470`):** IAR returns the INTID; the dispatcher
looks it up in a flat vector table (`base + INTID*8`), calls the ISR if non-null, then
EOIR. `INTID == 0x3FF (1023)` = "no interrupt". The dispatcher also handles the
`INTID == 32` (first SPI) special path (Marius/Omar INTC cascade).

### 1.3 UART
- **Base is genuinely ambiguous** across the agent reports; **not the boot blocker.**
  - 0xBF002000 : 29 ROM literal-pool refs (best current guess) — but the hit sites
    decode to struct-field accesses (offsets 0x11/0x14/0x17/0x1D/0x2C/0x34/0x40/0x54/
    0x64/0x74/0x84/0xA4/0xC4), i.e. a data table, so even this is not confirmed as MMIO.
  - 0xC0800000 : 1 clean ref — a base-getter thunk `movw/movt r0, #0xC0800000; b 0xE02870E2`.
  - 0xC0334000 : 0 refs (CLINE's "PL011" claim is NOT supported by the ROM).
- `print_msg` (`0xE04BF000`) is `vprintf`-style under spinlock `0x4144`; the byte
  writer is vtable-dispatched (driver vtable lives in runtime RAM, not statically
  resolvable), so the UART base must come from the driver's own constants.
- **Action:** wire a PL011 at `0xBF002000` (best guess) for now; re-verify the real
  base + flag offsets from the TX poll loop before relying on console output.

### 1.4 Marius timer block @ `0xC0208000`
16 bytes/channel, channel N base = `0xC0208890 + N*0x10`:
- `+0x00` MATCH (period) · `+0x04` CONTROL (bit0 = start) · `+0x08` STATUS (bit0 = match
  reached; write-1-ACK) · `+0x0C` COUNTER (RO).
- Channel→IRQ: Ch0=14, Ch1=30, Ch2=46, Ch3=62, Ch4=78.
- **Minimal model (TODO):** on CONTROL bit0 set, arm a QEMU timer for MATCH; on expiry
  set STATUS bit0 + assert the GIC SPI; on STATUS write-1, clear bit0 + deassert.

### 1.5 Cascaded secondary INTCs
- **Marius INTC** @ `0xD231A000` (read IRQ id @+0x00, ACK/EOI @+0x10), SPI 32.
- **Omar INTC** @ `0xD233A000` (same protocol), SPI 33.
- For boot, asserting the main GIC SPI 32/33 directly is sufficient; the secondary
  INTC read-id/ack shim is only needed when modelling EDMAC/Omar engines.

### 1.6 CORE-1 BRING-UP (CONFIRMED, implemented)
- Mailbox: `0xDFFC4FA0` (SRAM0 @ `0xDFFC0000`). Value = `0xE0100001` (Thumb entry).
- **Release mechanism (the firmware's own):** core 0 fires **GIC SGI 12 to CPU 1**
  (boot trace last write: `0xC1001F00 <= 0x0002000C`). Core 1's wakeup
  (`gang100 @ 0xE0004900`) reads the mailbox and branches to it.
- **Entry dispatch (boot00 `0xE0100001`):** sets VBAR + SP, reads MPIDR;
  - core 0 → cold-boot copy + `cstart` (`0xE01000EC` path)
  - core 1 → `0xE01000EC` secondary init (calls `0xE01003B8`)
- **QEMU implementation:** core 1 starts powered-off; on SGI 12 to CPU 1 the GIC shim
  starts core 1 at `0xE0100001` (SP=0x2000, interrupts masked). A mailbox-write
  fallback also releases it. **Do NOT pre-release core 1** before core 0 configures
  the GIC (causes lockups).

### 1.7 Spinlock / panic internals (reference)
- Spinlock primitive `0xE0354F64`: `ldrex/strex`; timeout counter (default `0x00100000`)
  from `0xE0CBD51C`; at 0 → `0xE01AE2A8/0xE01AE2CC`.
- `0xE01AE2CC`: prints `"DRYOS PANIC: Module Code=%d, Panic Code=%d[puId %d]"`
  (Module 64 = kernel, Panic 6 = lock timeout), then infinite `b .` at `0xE01AE2DC`.
- `0xE07C8F74` = `kernel_unlock()` (`*0x4024 = 0`, the scheduler spinlock).
- `0xE07C8B08` = `scheduler_yield()` / reschedule().
- `0xE07C8E18` = `send_reschedule_ipi()` → fires **SGI 10** to the peer core.
- `0xE04BF000` = `print_msg(fmt, ...)` (AAPCS r0=fmt, r1..r3=args), spinlock `0x4144`.

### 1.8 VBAR + vector table (CONFIRMED by disassembly)
- **Firmware VBAR = `0xE0300980`** (entry `0xE0100000`: `ldr r0,[pc,#0x24]` → literal at
  `0xE0100028` = `0xE0300980`; `mcr p15,0,r0,c12,c0,#0`). Set for BOTH cores by the entry.
- The table there has a working reset vector (`ldr pc,[pc,#0x18]` → `0xE0100001`) but the
  other vectors are not cleanly populated ⇒ **install a full table there** (and at the
  older `0x49234822`) with IRQ → the firmware dispatcher `0xE022D470`, other vectors to a
  safe `wfe; b .` loop.

---

## 2. PERIPHERAL / MEMORY MAP (as implemented in r6.c)

| Region | Address | What |
|--------|---------|------|
| DRAM 1 | `0x00000000` (1 GB) | DryOS RAM; boot00 preloaded at `0x4000` |
| DRAM 2 | `0x40000000` (2 GB) | second RAM window |
| MMIO stub | `0xBF000000` (512 MB) | logging catch-all; **mapped first** so real devices win |
| **GIC** | `0xC1000000` (16 KB) | **custom shim** (non-standard layout) |
| Marius timer | `0xC0208000` (ch0 @ `0xC0208890`) | tick model (TODO) |
| Marius INTC | `0xD231A000` | secondary controller, SPI 32 |
| Omar INTC | `0xD233A000` | secondary controller, SPI 33 |
| **UART0** | `0xBF002000` | PL011 (best guess; re-verify) |
| LED GPIO | `0xD22390C0` | ML-verified: on `0x24D0002`, off `0x24C0003` |
| Power/SRAM0/mailbox | `0xDFFC0000` | mailbox `0xDFFC4FA0`; `0xDFFC7000..` filled `bx lr` |
| Flash gang100 | `0xE0000000` (64 MB) | boot ROM @`0xE000xxxx`, main fw @`0xE0100000` |
| Flash gang200 | `0xE4000000` (32 MB) | |
| Entry | `0xE0100001` | Thumb, boot00 main firmware |

**MARV display (graphics):** `*(u32*)0x14B000` = GUI layer descriptor; signature
`0x5652414D`; +0x04 bitmap base, +0x1C width, +0x20 height; pixels BGRA/32-bit →
`PIXMAN_a8r8g8b8`. Rear LCD = top-left 960×540 of a 2048×1080 canvas.
(Display reader already implemented and wired; renders as soon as the firmware gets
past the GIC-dependent scheduler.)

---

## 3. WHAT r6.c NOW CONTAINS (implementation)

- **Custom GIC shim** (`R6GicState` + `r6_gic_ops`): CTR/PMR/IAR/EOIR (cpuif) +
  CTR/ISENABLER/ICENABLER/ITARGETSR/ICFGR/SGIR (dist) at the exact DIGIC X offsets.
  Per-CPU SGI pending; global SPI pending; enabled/active/target bitmaps. IAR returns
  the lowest pending+enabled INTID (or 1023). SGIR sets SGI pending; **SGI 12 to CPU 1
  releases core 1**. `r6_gic_update_irqs()` asserts/deasserts each CPU's `ARM_CPU_IRQ`.
- **Core-1 release** (`r6_release_core1`): on SGI 12 (or mailbox write), starts core 1
  at `0xE0100001` (SP=0x2000, I/A/F masked, `PSCI_ON`, kick). Idempotent.
- **Vector table install** (`r6_install_vectors`): at `0xE0300980` and `0x49234822`;
  reset → `0xE0100001`, IRQ → `0xE022D470`, others → `wfe; b .` loop at `0x49235000`.
- **Boot preload** (`r6_preload_boot`): boot00 → RAM `0x4000` (or carve from gang100).
- **MARV display reader** (~30 Hz poll) → `dpy_gfx_replace_surface`.
- **MMIO logging stub** (512 MB) mapped first; GIC/UART/power/LED mapped after (win).
- **Removed:** stock `arm_gic`, `a9gtimer`, `arm_mptimer` (wrong address + layout).

---

## 4. BUILD & RUN

- Repo: `C:\Users\Mega\Downloads\R6-Bionic` (git, GitHub `klosixgaming-arch/r6-bionic`).
  Source of truth: `patches/r6.c`. Integration: `patches/qemu-r6.patch`.
- Build tree (WSL Ubuntu, `~/r6b-qemu`, QEMU 6.1.93 fork):
  ```
  cp patches/r6.c ~/r6b-qemu/hw/arm/r6.c
  cd ~/r6b-qemu/build && ninja -j32 qemu-system-arm
  ```
- ROMs (WSL `~/r6b-run/` + Windows `C:\Users\Mega\Downloads\R6 Testing\2026 R6 Rom dumps\1.7.0\`):
  `boot00.bin` (38535168 B), `gang100.bin` (64 MB), `gang200.bin` (32 MB).
- Run **with graphics** (WSLg shows the SDL window — do NOT pass `-nographic`):
  ```
  cd ~/r6b-run
  ~/r6b-qemu/build/qemu-system-arm -M canon-r6 -smp 2 -m 3G \
      -bios boot00.bin -monitor unix:/tmp/r6mon.sock,server,nowait
  ```
  Watch for `[r6] display: MARV@...` = GUI up.
- **WSL kills background procs when a `wsl -e` session ends** → do launch + wait +
  monitor query in ONE `wsl -e` session.
- Host disassembler: `C:/Users/Mega/AppData/Local/Python/bin/python.exe` + capstone 5.0.7
  (boot00 base `0xE0100000`). Helper scripts in scratchpad.
- **Do NOT commit** the Discord bot token or the ROM dumps to the repo.

---

## 5. QEMU API NOTES (QEMU 6.1.93 fork)
- **No `memory_region_set_priority` in 6.1.** Disambiguation rule (verified in
  `softmmu/memory.c`): equal priority → **newest-added wins**. ⇒ map the 512 MB stub
  first, real devices after.
- `qemu_set_irq` is in `include/hw/irq.h`. `current_cpu` is `__thread CPUState*` in
  `include/hw/core/cpu.h`. ARM IRQ input = `qdev_get_gpio_in(DEVICE(cpu), ARM_CPU_IRQ)`
  with `ARM_CPU_IRQ = 0`.
- `env.vbar_ns` exists in `target/arm/cpu.h` but the compiler rejected it in this build;
  the entry code sets VBAR itself, so the shim does **not** set it for core 1.
- PSTATE bits: `PSTATE_I (1<<7)`, `PSTATE_A (1<<8)`, `PSTATE_F (1<<6)`.
- Power-on a powered-off CPU: `cpu_reset(); cpu_set_pc(); cs->halted=0;
  cpu->power_state=PSCI_ON; qemu_cpu_kick();` (same order as the working power-stub).

---

## 6. INPUT INJECTION (for later, once the GUI is visible)
- Ring buffer `0x0007F554`, 1025 slots × 16 B `[ev_type,ev_param,ev_obj,ev_arg]`,
  head index `*(u32*)0x00005474`; direct-enqueue subroutine `0xE09EA5CC`.
- Verified keycodes (press/release): SET `0x0A/0x0B`, MENU `0x0C/0x0D`, INFO
  `0x0E/0x0F`, PLAY `0x12/0x13`, MAGNIFY `0x1C/0x1D`, TRASH `0x14/0x15`, Q `0x2C/0x2D`,
  half-shutter `0x9F/0xA0`, full-shutter `0xA1/0xA2`, AF-Point+ `0x4A/0x4B`.
- Dials: Main `0x08/0x09`, Set wheel `0x04/0x05`, Exp `0x4E/0x4F`.
- Joystick: Up `0x77`, Down `0x7E`, Left `0x7B`, Right `0x7A`, Center/Release `0x76`.
- Ignore noise: `0x44/0x45` (SwMode_ButtonCBR), `0x3D` (touch/eye idle when ARG=0).

---

## 7. AGENT DISAGREEMENTS (resolved — ROM is the tie-breaker)
1. **Generic timer:** CLINE first said "ARMv8 CP15 → PPI 30", then retracted. Full-ROM
   scan = **zero `c14` instructions**. ⇒ SGIs + Marius timer only.
2. **UART base:** CLINE said `0xC0800000`, then `0xC0334000` (PL011). ROM shows 0 refs
   to `0xC0334000`, 29 to `0xBF002000` (struct-table, unconfirmed), 1 clean thunk to
   `0xC0800000`. ⇒ **genuinely ambiguous; wire `0xBF002000` and re-verify.**
3. **Memory-map versions:** CLINE's final spec lists gang200 @ `0xF0000000` and INTCs @
   `0xC0380000/0xC043F000`; r6.c uses gang200 @ `0xE4000000`, INTCs @ `0xD231A000/
   0xD233A000`. ⇒ keep current values; confirm gang200's real base from the ROM later.

---

## 8. OPEN / TODO
- [ ] Rebuild + run with graphics; capture the new boot trace.
- [ ] If a new panic: decode Module/Panic code + the spinning PC; map to a missing peripheral.
- [ ] Implement the Marius timer model (SPI 32/33) if the OS tick needs it.
- [ ] Re-verify the UART base + flag offsets from the TX poll loop.
- [ ] Add input injection (virtual keypad) once the GUI is visible.
- [ ] Commit + push each working milestone to GitHub.

/*
 * QEMU model of the Canon EOS R6 (DIGIC X)
 *
 * Copyright (C) 2026 R6-Bionic Project
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Based on reverse engineering of Canon EOS R6 firmware 1.7.0 ROM dumps
 * (boot00.bin / gang100.bin / gang200.bin), cross-checked against the
 * Magic Lantern R6.150 platform definitions and hardware-verified
 * findings (LED GPIO, secondary core release, FIO functions).
 *
 * SoC: DIGIC X, dual ARM Cortex-A9 (ARMv7-A), DryOS runs Thumb-2.
 *
 * Memory map (firmware 1.7.0, from ROM constant + disassembly analysis):
 *   0x00000000 - 0x3FFFFFFF : 1 GB DRAM. boot00 preloaded at 0x4000.
 *   0x40000000 - 0xBEFFFFFF : 2 GB DRAM.
 *   0xBF000000 - 0xDEFFFFFF : MMIO window (logging-stub catch-all).
 *     0xBF002000 : debug UART (best guess; 29 ROM literal refs)
 *     0xC0208000 : Marius timer block (ch0 @ 0xC0208890)
 *     0xC1000000 : GIC (DIGIC X NON-STANDARD layout, custom shim)
 *       0xC1000100 GICC_CTLR   0xC1000104 GICC_PMR
 *       0xC100010C GICC_IAR    0xC1000110 GICC_EOIR
 *       0xC1001000 GICD_CTLR   0xC1001080.. ISENABLER[n]
 *       0xC1001180.. ICENABLER[n]  0xC1001800.. ITARGETSR[n]
 *       0xC1001C00.. ICFGR[n]      0xC1001F00 GICD_SGIR
 *     0xD22390C0 : status/LED GPIO (ML: 0x24D0002 on / 0x24C0003 off)
 *     0xDFFC0000 : power / SRAM0 block; mailbox 0xDFFC4FA0 (core-1 entry)
 *   0xE0000000 - 0xE3FFFFFF : internal flash: gang100.bin (64 MB)
 *   0xE4000000 - 0xE5FFFFFF : internal flash: gang200.bin (32 MB)
 *
 * THE GIC (confirmed by disassembly, the boot blocker):
 *   IRQ dispatcher 0xE022D470:
 *       mov.w r0, #0xC1000000 ; ldr r7, [r0, #0x10c]   (IAR -> INTID)
 *       ...
 *       str r7, [r0, #0x110]                        (EOIR)
 *   SGIR trigger 0xE0163C34:
 *       orr.w r0, r0, r1, lsl #16 ; str r0, [0xC1001F00]
 *   Standard ARM GICv2 has IAR@+0x008/EOIR@+0x00C from the cpuif base;
 *   DIGIC X puts them at +0x10C/+0x110 and the distributor at the HIGHER
 *   address (0xC1001000). No stock GIC base aligns them => custom shim.
 *
 * CORE-1 BRING-UP (confirmed):
 *   The firmware releases core 1 by firing GIC SGI 12 to CPU 1
 *   (boot trace last write: 0xC1001F00 <= 0x0002000C).  Core 1's
 *   wakeup reads the mailbox 0xDFFC4FA0 (= 0xE0100001) and branches
 *   there; the entry (0xE0100001) reads MPIDR and routes core 1 to its
 *   secondary init (0xE01000EC).  We emulate this: core 1 starts
 *   powered-off; on SGI 12 to CPU 1 the shim starts core 1 at the
 *   entry with SP/VBAR set.  Do NOT pre-release core 1 before core 0
 *   has configured the GIC (causes lockups).
 *
 *   entry (0xE0100001): VBAR = 0xE0300980 ; SP = 0x2000
 *       if core 0: cold-boot copy + cstart (0xE01000EC path)
 *       if core 1: 0xE01000EC secondary init
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "qemu/datadir.h"
#include "cpu.h"
#include "hw/boards.h"
#include "hw/sysbus.h"
#include "qemu/error-report.h"
#include "hw/loader.h"
#include "sysemu/cpus.h"
#include "sysemu/qtest.h"
#include "sysemu/reset.h"
#include "sysemu/sysemu.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "exec/cpu-common.h"
#include "hw/char/pl011.h"
#include "hw/irq.h"
#include "chardev/char.h"
#include "qom/object.h"
#include "ui/console.h"
#include "qemu/timer.h"

/*
 * Canon EOS R6 (DIGIC X) map
 */
#define R6_RAM_BASE           0x00000000
#define R6_RAM_SIZE           (1 * GiB)   /* 0x00000000 - 0x3FFFFFFF */

#define R6_UCRAM_BASE         0x40000000
#define R6_UCRAM_SIZE         (2 * GiB)   /* 0x40000000 - 0xBEFFFFFF */

#define R6_MMIO_BASE          0xBF000000  /* generic logging-stub window base */
#define R6_MMIO_SIZE          (0x20000000)/* 0xBF000000 - 0xDEFFFFFF */

/* DIGIC X GIC (custom, non-standard layout) */
#define R6_GIC_BASE           0xC1000000
#define R6_GIC_SIZE           0x2000      /* cpuif (0x1000) + dist (0x1000) */
#define R6_GIC_NUM_IRQ        704         /* headroom over the 640-vector table */
#define R6_GIC_NUM_CPUS       2

#define R6_UART_BASE          0xBF002000  /* SoC debug UART (best guess) */
#define R6_MARIUS_BASE        0xC0208000  /* Marius timer block */

#define R6_LED_BASE           0xD22390C0  /* status/LED GPIO */
#define R6_POWER_BASE         0xDFFC0000  /* power/clock/secondary-core block */
#define R6_POWER_SIZE         0x10000
#define R6_SEC_CORE_POK       0x4FA0      /* offset: secondary core mailbox */

#define R6_FLASH_BASE         0xE0000000
#define R6_GANG100_SIZE       (64 * MiB)  /* gang100.bin @ 0xE0000000 */
#define R6_GANG200_BASE       0xE4000000  /* gang200.bin @ 0xE4000000 */
#define R6_GANG200_SIZE       (32 * MiB)

#define R6_BOOT00_FLASH_OFF   0x100000    /* boot00 = gang100[0x100000...] */
#define R6_BOOT00_SIZE        (36 * MiB + 0xC0000) /* 38535168 = 0x24C000 */
#define R6_ENTRY              0xE0100001  /* Thumb entry of boot00 */

#define R6_BOOT00_RAM_ADDR    0x00004000  /* SoC boot ROM preload target */
#define R6_VBAR_VALUE         0xE0300980  /* firmware vector table (entry sets it) */
#define R6_VBAR_OLD           0x49234822  /* older observed VBAR; install both */
#define R6_IRQ_DISPATCHER     0xE022D470  /* firmware GIC IRQ dispatcher (IAR/EOIR) */
#define R6_VEC_SAFE_ADDR      0x49235000  /* WFE/WFI loop for non-IRQ vectors */

/* MARV framebuffer reader state (defined fully before use below) */
typedef struct {
    QemuConsole *con;
    uint32_t last_bitmap;
    uint32_t last_w;
    uint32_t last_h;
    QEMUTimer *timer;
} R6GfxState;

/*
 * =========================================================================
 * DIGIC X GIC shim (custom, non-standard register layout)
 *
 * Serves the firmware's exact GIC protocol at the DIGIC X addresses:
 *   IAR  read  0xC100010C -> INTID of highest-priority pending IRQ (or 1023)
 *   EOIR write 0xC1000110 <- INTID
 *   SGIR write 0xC1001F00 <- (TargetList<<16) | INTID
 *   ISENABLER[n]  0xC1001080 + n*4   (set enable bits)
 *   ICENABLER[n]  0xC1001180 + n*4   (write-1-clears enable bits)
 *   ITARGETSR[n]  0xC1001800 + n*4   (per-IRQ CPU target mask)
 *   ICFGR[n]      0xC1001C00 + n*8   (edge-triggered; ignored)
 *
 * On SGIR SGI 12 to CPU 1, releases (starts) core 1.
 * =========================================================================
 */
typedef struct R6GicState {
    MemoryRegion mr;
    CPUState *cpu[R6_GIC_NUM_CPUS];
    qemu_irq nirq[R6_GIC_NUM_CPUS];      /* each CPU's ARM_CPU_IRQ input */

    uint32_t enabled[R6_GIC_NUM_IRQ / 32];
    uint32_t pending[R6_GIC_NUM_IRQ / 32]; /* SPIs (32+) pending, global */
    uint32_t active[R6_GIC_NUM_IRQ / 32];
    uint32_t target[R6_GIC_NUM_IRQ / 32]; /* per-IRQ CPU mask (bit0=cpu0) */
    uint32_t sgi_pending[R6_GIC_NUM_CPUS]; /* per-CPU SGI 0..15 pending */

    uint32_t ctr;                        /* GICC CTR */
    uint32_t pmr;                        /* GICC PMR (0xFF = allow all) */
    uint32_t dctr;                       /* GICD CTR */

    bool core1_released;
} R6GicState;

static inline bool r6_gic_pending(R6GicState *g, int irq, int cpu)
{
    if (irq < 16) {
        return (g->sgi_pending[cpu] & (1u << irq)) != 0;
    }
    return (g->pending[irq / 32] & (1u << (irq % 32))) != 0;
}

static void r6_gic_update_irqs(R6GicState *g);

static void r6_release_core1(R6GicState *g)
{
    if (g->core1_released || !g->cpu[1]) {
        return;
    }
    g->core1_released = true;

    CPUState *cs = g->cpu[1];
    ARMCPU *cpu = ARM_CPU(cs);

    fprintf(stderr, "[r6] GIC SGI12 -> releasing core1 (entry=0x%08x)\n",
            (unsigned)R6_ENTRY);
    cpu_reset(cs);
    cpu_set_pc(cs, R6_ENTRY);
    cpu->env.regs[13] = 0x2000;            /* SP (entry also sets it + VBAR) */
    cpu->env.pstate &= ~(PSTATE_I | PSTATE_A | PSTATE_F);
    cs->halted = 0;
    cpu->power_state = PSCI_ON;
    qemu_cpu_kick(cs);
}

/* IAR: return the lowest-numbered pending+enabled IRQ for the calling CPU. */
static uint32_t r6_gic_iar(R6GicState *g)
{
    int cpu = 0;

    if (current_cpu) {
        cpu = current_cpu->cpu_index & (R6_GIC_NUM_CPUS - 1);
    }
    for (int irq = 0; irq < R6_GIC_NUM_IRQ; irq++) {
        if (!r6_gic_pending(g, irq, cpu)) {
            continue;
        }
        if (!(g->enabled[irq / 32] & (1u << (irq % 32)))) {
            continue;
        }
        g->active[irq / 32] |= (1u << (irq % 32));
        return (uint32_t)irq;
    }
    return 1023;   /* no pending interrupt */
}

static void r6_gic_eoir(R6GicState *g, uint32_t intid)
{
    if (intid < R6_GIC_NUM_IRQ) {
        g->active[intid / 32] &= ~(1u << (intid % 32));
    }
    r6_gic_update_irqs(g);
}

static void r6_gic_sgir(R6GicState *g, uint32_t val)
{
    uint32_t targetlist = (val >> 16) & 0xFFFF;
    uint32_t intid = val & 0x3FF;

    for (int cpu = 0; cpu < R6_GIC_NUM_CPUS; cpu++) {
        if (!(targetlist & (1u << cpu))) {
            continue;
        }
        if (intid == 12 && cpu == 1) {
            /* Core-1 release kick (GIC_suspend). */
            r6_release_core1(g);
            continue;
        }
        if (intid < 16) {
            g->sgi_pending[cpu] |= (1u << intid);
        }
    }
    r6_gic_update_irqs(g);
}

static void r6_gic_update_irqs(R6GicState *g)
{
    for (int cpu = 0; cpu < R6_GIC_NUM_CPUS; cpu++) {
        bool has = false;
        for (int irq = 0; irq < R6_GIC_NUM_IRQ && !has; irq++) {
            if (r6_gic_pending(g, irq, cpu) &&
                (g->enabled[irq / 32] & (1u << (irq % 32)))) {
                has = true;
            }
        }
        if (g->nirq[cpu]) {
            qemu_set_irq(g->nirq[cpu], has ? 1 : 0);
        }
    }
}

static uint64_t r6_gic_read(void *opaque, hwaddr addr, unsigned size)
{
    R6GicState *g = opaque;
    addr &= (R6_GIC_SIZE - 1);

    if (addr == 0x100) {
        return g->ctr & 1;
    }
    if (addr == 0x104) {
        return g->pmr & 0xFF;
    }
    if (addr == 0x10C) {
        return r6_gic_iar(g);
    }
    if (addr == 0x110) {
        return 0;   /* EOIR read */
    }
    if (addr == 0x1000) {
        return g->dctr & 1;
    }
    if (addr >= 0x1080 && addr < 0x1080 + (R6_GIC_NUM_IRQ / 32) * 4) {
        return g->enabled[(addr - 0x1080) / 4];   /* ISENABLER read */
    }
    if (addr >= 0x1180 && addr < 0x1180 + (R6_GIC_NUM_IRQ / 32) * 4) {
        return g->enabled[(addr - 0x1180) / 4];   /* ICENABLER read */
    }
    if (addr >= 0x1800 && addr < 0x1800 + (R6_GIC_NUM_IRQ / 32) * 4) {
        return g->target[(addr - 0x1800) / 4] & 3;
    }
    if (addr >= 0x1C00) {
        return 0;   /* ICFGR: edge-triggered */
    }
    return 0;
}

static void r6_gic_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    R6GicState *g = opaque;
    addr &= (R6_GIC_SIZE - 1);

    if (addr == 0x100) {
        g->ctr = val & 1;
        r6_gic_update_irqs(g);
    } else if (addr == 0x104) {
        g->pmr = val & 0xFF;
        r6_gic_update_irqs(g);
    } else if (addr == 0x110) {
        r6_gic_eoir(g, (uint32_t)val);
    } else if (addr == 0x1000) {
        g->dctr = val & 1;
        r6_gic_update_irqs(g);
    } else if (addr >= 0x1080 && addr < 0x1080 + (R6_GIC_NUM_IRQ / 32) * 4) {
        g->enabled[(addr - 0x1080) / 4] |= (uint32_t)val;   /* set enable */
        r6_gic_update_irqs(g);
    } else if (addr >= 0x1180 && addr < 0x1180 + (R6_GIC_NUM_IRQ / 32) * 4) {
        g->enabled[(addr - 0x1180) / 4] &= ~(uint32_t)val;  /* clear enable */
        r6_gic_update_irqs(g);
    } else if (addr >= 0x1800 && addr < 0x1800 + (R6_GIC_NUM_IRQ / 32) * 4) {
        g->target[(addr - 0x1800) / 4] = (uint32_t)val & 3;
        r6_gic_update_irqs(g);
    } else if (addr >= 0x1C00 && addr < 0x1C00 + (R6_GIC_NUM_IRQ / 16) * 8) {
        /* ICFGR: treat everything as edge-triggered; ignore. */
    } else if (addr == 0x1F00) {
        r6_gic_sgir(g, (uint32_t)val);
    }
}

static const MemoryRegionOps r6_gic_ops = {
    .read = r6_gic_read,
    .write = r6_gic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/*
 * =========================================================================
 * Logging MMIO stubs
 * =========================================================================
 */
typedef struct R6ReadWindow {
    uint32_t base;
    uint32_t size;
    uint32_t value;
} R6ReadWindow;

typedef struct R6StubInfo {
    const char *name;
    hwaddr base;
    uint32_t count[16];   /* (offset >> 2) & 15 : access counts per slot */
    bool wake_secondary;  /* power block: 0x4FA0 releases core 2 */
    CPUState *secondary;  /* CPU to release */
    R6GicState *gic;      /* for core-1 release on mailbox write */
    R6ReadWindow win[4];  /* read windows (win[0].size == 0 => none) */
} R6StubInfo;

static void r6_stub_write(void *opaque, hwaddr addr, uint64_t val,
                          unsigned size)
{
    R6StubInfo *s = opaque;
    uint32_t slot = (addr >> 2) & 15;

    if (s->count[slot] < 8) {
        fprintf(stderr, "[r6] %s: write  0x%08x <= 0x%08x\n",
                s->name, (uint32_t)(s->base + addr), (uint32_t)val);
    }
    s->count[slot]++;

    /* Fallback core-1 release if the firmware writes the mailbox directly. */
    if (s->wake_secondary && addr == R6_SEC_CORE_POK && s->gic) {
        fprintf(stderr, "[r6] mailbox write 0x%08x -> releasing core1\n",
                (uint32_t)val);
        r6_release_core1(s->gic);
    }
}

static uint64_t r6_stub_read(void *opaque, hwaddr addr, unsigned size)
{
    R6StubInfo *s = opaque;
    uint32_t abs = (uint32_t)(s->base + addr);
    uint32_t slot = (addr >> 2) & 15;
    uint32_t val = 0;
    int i;

    for (i = 0; i < 4 && s->win[i].size; i++) {
        if (abs >= s->win[i].base &&
            abs < s->win[i].base + s->win[i].size) {
            val = s->win[i].value;
            break;
        }
    }

    if (s->count[slot] < 8) {
        fprintf(stderr, "[r6] %s: read   0x%08x => 0x%08x\n",
                s->name, abs, val);
    }
    s->count[slot]++;
    return val;
}

static const MemoryRegionOps r6_stub_ops = {
    .read = r6_stub_read,
    .write = r6_stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/*
 * Machine state (defined before the boot/reset helpers that use it).
 * R6GicState (above) and R6GfxState (top) are already defined.
 */
typedef struct R6MachineState {
    MachineState parent;

    MemoryRegion ucr;          /* 2 GB second DRAM window */
    MemoryRegion flash0;       /* gang100.bin (64 MB) */
    MemoryRegion flash1;       /* gang200.bin (32 MB) */

    MemoryRegion mmio;         /* 512 MB logging stub window */
    MemoryRegion power;        /* 0xDFFC0000 block, releases core 2 */
    MemoryRegion pwrsleep;     /* 0xDFFC7000-0xDFFFFFFFF 'bx lr' code region */
    MemoryRegion led;          /* 0xD22390C0 status LED */

    R6GicState gic;            /* DIGIC X GIC shim */
    DeviceState *uart;

    R6GfxState gfx;
} R6MachineState;

#define TYPE_CANON_R6 "canon-r6-machine"
OBJECT_DECLARE_TYPE(R6MachineState, R6MachineClass, CANON_R6)

/*
 * =========================================================================
 * Vector table install
 *
 * The firmware sets VBAR = 0xE0300980 (entry code, both cores).  Its table
 * there has a working reset vector (-> 0xE0100001) but we install a complete
 * table (IRQ -> the firmware's GIC dispatcher 0xE022D470, other vectors to a
 * safe WFE loop) at that address AND at the older 0x49234822, so whichever
 * VBAR the firmware uses, IRQs are dispatched correctly.
 * =========================================================================
 */
static void r6_install_vectors(void)
{
    uint32_t vecs[9];
    hwaddr vbar;

    vecs[0] = 0xE59FF018;                 /* ldr pc, [pc, #0x18] (reset) */
    vecs[1] = R6_VEC_SAFE_ADDR | 1;       /* undefined */
    vecs[2] = R6_VEC_SAFE_ADDR | 1;       /* swi */
    vecs[3] = R6_VEC_SAFE_ADDR | 1;       /* prefetch abort */
    vecs[4] = R6_VEC_SAFE_ADDR | 1;       /* data abort */
    vecs[5] = R6_VEC_SAFE_ADDR | 1;       /* hyp */
    vecs[6] = R6_VEC_SAFE_ADDR | 1;       /* reserved */
    vecs[7] = R6_IRQ_DISPATCHER | 1;      /* irq -> firmware GIC dispatcher */
    vecs[8] = R6_ENTRY;                   /* reset handler address (loaded by ldr pc) */

    for (vbar = R6_VBAR_VALUE; vbar <= R6_VBAR_OLD; vbar = R6_VBAR_OLD) {
        if (vbar == R6_VBAR_VALUE) {
            /* install at the firmware VBAR first */
        }
        cpu_physical_memory_write(vbar == R6_VBAR_VALUE ? R6_VBAR_VALUE
                                                        : R6_VBAR_OLD,
                                 vecs, sizeof(vecs));
    }

    /* Safe WFE/WFI loop (Thumb) for the non-IRQ vectors. */
    {
        uint8_t wfe[] = { 0x40, 0xBF, 0xFE, 0xE7 };  /* wfe ; b . */
        cpu_physical_memory_write(R6_VEC_SAFE_ADDR, wfe, sizeof(wfe));
    }

    fprintf(stderr, "[r6] vectors installed at 0x%08x and 0x%08x "
            "(irq -> 0x%08x)\n",
            (unsigned)R6_VBAR_VALUE, (unsigned)R6_VBAR_OLD,
            (unsigned)R6_IRQ_DISPATCHER);
}

/*
 * =========================================================================
 * Boot preload (emulates the SoC's internal boot ROM)
 * =========================================================================
 */
static void r6_preload_boot(R6MachineState *s)
{
    char *fn;
    uint8_t *buf;
    size_t fsz;

    fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, "boot00.bin");
    if (fn) {
        FILE *f = fopen(fn, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            fsz = ftell(f);
            fseek(f, 0, SEEK_SET);

            size_t len = fsz < R6_BOOT00_SIZE ? fsz : R6_BOOT00_SIZE;
            buf = g_malloc(len);
            size_t read_bytes = fread(buf, 1, len, f);
            fclose(f);

            cpu_physical_memory_write(R6_BOOT00_RAM_ADDR, buf, read_bytes);
            g_free(buf);
            fprintf(stderr, "[r6] boot00: %zu bytes @ 0x%08x (from %s)\n",
                    read_bytes, (unsigned)R6_BOOT00_RAM_ADDR, fn);
        } else {
            fprintf(stderr, "[r6] failed to open boot00.bin: %s\n",
                    strerror(errno));
        }
        g_free(fn);
    } else {
        buf = g_malloc(R6_BOOT00_SIZE);
        cpu_physical_memory_read(R6_FLASH_BASE + R6_BOOT00_FLASH_OFF,
                                 buf, R6_BOOT00_SIZE);
        cpu_physical_memory_write(R6_BOOT00_RAM_ADDR, buf, R6_BOOT00_SIZE);
        g_free(buf);
        fprintf(stderr, "[r6] boot00: %u bytes @ 0x%08x (from flash)\n",
                (unsigned)R6_BOOT00_SIZE, (unsigned)R6_BOOT00_RAM_ADDR);
    }

    r6_install_vectors();
}

/*
 * Reset handler.
 */
static void r6_reset(MachineState *mc)
{
    R6MachineState *s = CANON_R6(mc);
    CPUState *cs = qemu_get_cpu(0);

    qemu_devices_reset();

    r6_preload_boot(s);

    if (cs) {
        cpu_set_pc(cs, R6_ENTRY);
        fprintf(stderr, "[r6] cpu0 -> 0x%08x (Thumb)\n",
                (unsigned)R6_ENTRY);
    }
}

/*
 * =========================================================================
 * Display: MARV / XIMR framebuffer reader
 * =========================================================================
 */
#define R6_MARV_LAYER0      0x14B000
#define R6_MARV_SIGNATURE   0x5652414D
#define R6_FB_REFRESH_MS    33          /* ~30 Hz poll */

static uint32_t r6_ldl(hwaddr addr)
{
    return address_space_ldl(&address_space_memory, addr,
                             MEMTXATTRS_UNSPECIFIED, NULL);
}

static uint32_t r6_gfx_stride(uint32_t w)
{
    const char *env = g_getenv("R6_FB_STRIDE_WORDS");

    if (env) {
        return (uint32_t)atoi(env) * 4;
    }
    if (w <= 1024) {
        return 1024 * 4;
    }
    if (w <= 2048) {
        return 2048 * 4;
    }
    return w * 4;
}

static void r6_gfx_unmap_destroy(pixman_image_t *image, void *unused)
{
    void *data = pixman_image_get_data(image);
    uint64_t size = (uint64_t)pixman_image_get_stride(image) *
                    pixman_image_get_height(image);
    cpu_physical_memory_unmap(data, size, 0, 0);
}

static void r6_gfx_refresh(void *opaque)
{
    R6GfxState *s = opaque;
    uint32_t marv, bitmap, w, h;

    if (!s->con) {
        return;
    }

    marv = r6_ldl(R6_MARV_LAYER0);
    if (!marv || r6_ldl(marv) != R6_MARV_SIGNATURE) {
        return;
    }

    bitmap = r6_ldl(marv + 0x04);
    w = r6_ldl(marv + 0x1C);
    h = r6_ldl(marv + 0x20);
    if (!bitmap || w < 64 || h < 32 || w > 8192 || h > 8192) {
        return;
    }

    {
        uint32_t stride = r6_gfx_stride(w);
        uint64_t size = (uint64_t)stride * (h - 1) + (uint64_t)w * 4;
        uint64_t mapsize = size;
        void *data = cpu_physical_memory_map(bitmap, &mapsize, false);
        DisplaySurface *ds;

        if (mapsize < size) {
            cpu_physical_memory_unmap(data, mapsize, 0, 0);
            return;
        }

        ds = qemu_create_displaysurface_from(w, h, PIXMAN_a8r8g8b8,
                                             stride, data);
        if (!ds) {
            cpu_physical_memory_unmap(data, mapsize, 0, 0);
            return;
        }
        pixman_image_set_destroy_function(ds->image,
                                          r6_gfx_unmap_destroy, NULL);

        if (s->last_bitmap != bitmap || s->last_w != w || s->last_h != h) {
            fprintf(stderr,
                    "[r6] display: MARV@0x%08x bitmap=0x%08x %ux%u "
                    "stride=%u B\n",
                    (unsigned)marv, (unsigned)bitmap, w, h, stride);
            s->last_bitmap = bitmap;
            s->last_w = w;
            s->last_h = h;
        }

        dpy_gfx_replace_surface(s->con, ds);
        dpy_gfx_update_full(s->con);
    }
}

static void r6_gfx_invalidate(void *opaque)
{
}

static void r6_gfx_tick(void *opaque)
{
    r6_gfx_refresh(opaque);
}

static const GraphicHwOps r6_gfx_ops = {
    .invalidate = r6_gfx_invalidate,
    .gfx_update = r6_gfx_refresh,
};

/*
 * =========================================================================
 * Machine
 * =========================================================================
 */
static void r6_machine_init(MachineState *machine)
{
    R6MachineState *s = CANON_R6(machine);
    MemoryRegion *sysmem = get_system_memory();
    Object *cpuobj;
    CPUState *cpu0;
    CPUState *cpu1;
    Chardev *chr;
    DeviceState *uart;

    fprintf(stderr,
        "\n[r6] === Canon EOS R6 (DIGIC X, dual Cortex-A9) ===\n"
        "[r6] RAM  1 GB @ 0x%08x + 2 GB @ 0x%08x\n"
        "[r6] GIC  custom shim @ 0x%08x (DIGIC X layout)\n"
        "[r6] flash 96 MB @ 0x%08x\n"
        "[r6] entry 0x%08x\n",
        (unsigned)R6_RAM_BASE, (unsigned)R6_UCRAM_BASE,
        (unsigned)R6_GIC_BASE, (unsigned)R6_FLASH_BASE,
        (unsigned)R6_ENTRY);

    /*
     * CPUs: dual Cortex-A9.  Core 2 starts powered-off; the firmware
     * releases it by firing GIC SGI 12 (caught by the GIC shim).
     */
    cpuobj = object_new(ARM_CPU_TYPE_NAME("cortex-a9"));
    object_property_set_bool(cpuobj, "has_el3", false, &error_fatal);
    if (object_property_find(cpuobj, "has_el2")) {
        object_property_set_bool(cpuobj, "has_el2", false, &error_fatal);
    }
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);

    cpuobj = object_new(ARM_CPU_TYPE_NAME("cortex-a9"));
    object_property_set_bool(cpuobj, "has_el3", false, &error_fatal);
    if (object_property_find(cpuobj, "has_el2")) {
        object_property_set_bool(cpuobj, "has_el2", false, &error_fatal);
    }
    object_property_set_bool(cpuobj, "start-powered-off", true, &error_fatal);
    qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);

    cpu0 = qemu_get_cpu(0);
    cpu1 = qemu_get_cpu(1);

    /*
     * DIGIC X GIC shim (custom, non-standard layout).
     */
    s->gic.cpu[0] = cpu0;
    s->gic.cpu[1] = cpu1;
    s->gic.nirq[0] = qdev_get_gpio_in(DEVICE(cpu0), ARM_CPU_IRQ);
    s->gic.nirq[1] = qdev_get_gpio_in(DEVICE(cpu1), ARM_CPU_IRQ);
    s->gic.pmr = 0xFF;
    memory_region_init_io(&s->gic.mr, NULL, &r6_gic_ops, &s->gic,
                          "r6.gic", R6_GIC_SIZE);
    /* mapped after the MMIO stub (below) so it wins disambiguation */
    fprintf(stderr, "[r6] GIC shim @ 0x%08x (IAR@+0x10C EOIR@+0x110 "
            "SGIR@+0x1F00)\n", (unsigned)R6_GIC_BASE);

    /* PL011 UART @ 0xBF002000 (primary debug console, best-guess base). */
    uart = qdev_new(TYPE_PL011);
    chr = serial_hd(0);
    if (!chr) {
        chr = qemu_chr_new("r6-uart0", "pty", NULL);
    }
    if (chr) {
        qdev_prop_set_chr(uart, "chardev", chr);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(uart), &error_fatal);
    s->uart = uart;
    /* mapped after the MMIO stub (below) so it wins disambiguation */
    fprintf(stderr, "[r6] UART @ 0x%08x\n", (unsigned)R6_UART_BASE);

    /*
     * DRAM: 1 GB first window + 2 GB second window.
     */
    memory_region_add_subregion(sysmem, R6_RAM_BASE, machine->ram);

    memory_region_init_ram(&s->ucr, NULL, "r6.ucram", R6_UCRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, R6_UCRAM_BASE, &s->ucr);

    /*
     * Internal flash: gang100 (64 MB) + gang200 (32 MB).
     */
    memory_region_init_ram(&s->flash0, NULL, "r6.flash0", R6_GANG100_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, R6_FLASH_BASE, &s->flash0);

    memory_region_init_ram(&s->flash1, NULL, "r6.flash1", R6_GANG200_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, R6_GANG200_BASE, &s->flash1);

    if (!qtest_enabled()) {
        char *fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, "gang100.bin");
        if (fn) {
            int64_t sz = load_image_targphys(fn, R6_FLASH_BASE,
                                             R6_GANG100_SIZE);
            if (sz < 0) {
                fprintf(stderr, "[r6] ERROR: failed to load gang100.bin\n");
            } else {
                fprintf(stderr, "[r6] gang100: %ld bytes @ 0x%08x\n",
                        (long)sz, (unsigned)R6_FLASH_BASE);
            }
            g_free(fn);
        } else {
            fprintf(stderr, "[r6] WARNING: gang100.bin not found\n");
        }

        fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, "gang200.bin");
        if (fn) {
            int64_t sz = load_image_targphys(fn, R6_GANG200_BASE,
                                             R6_GANG200_SIZE);
            if (sz < 0) {
                fprintf(stderr, "[r6] ERROR: failed to load gang200.bin\n");
            } else {
                fprintf(stderr, "[r6] gang200: %ld bytes @ 0x%08x\n",
                        (long)sz, (unsigned)R6_GANG200_BASE);
            }
            g_free(fn);
        } else {
            fprintf(stderr, "[r6] WARNING: gang200.bin not found\n");
        }
    }

    /*
     * 512 MB MMIO window: logging stub for everything not modelled yet.
     * Lower priority than the real devices (GIC/UART/LED/power) so they win.
     */
    {
        R6StubInfo *info = g_new0(R6StubInfo, 1);
        info->name = "mmio";
        info->base = R6_MMIO_BASE;
        memory_region_init_io(&s->mmio, NULL, &r6_stub_ops, info,
                              "r6.mmio", R6_MMIO_SIZE);
    }
    memory_region_add_subregion(sysmem, R6_MMIO_BASE, &s->mmio);

    /*
     * Real MMIO devices are mapped AFTER the stub so they win QEMU's
     * disambiguation (equal priority -> newest-added wins).
     */
    memory_region_add_subregion(sysmem, R6_GIC_BASE, &s->gic.mr);
    sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0, R6_UART_BASE);

    /*
     * Power/clock block; 0x4FA0 mailbox releases core 2 (fallback path).
     */
    {
        R6StubInfo *info = g_new0(R6StubInfo, 1);
        info->name = "power";
        info->base = R6_POWER_BASE;
        info->wake_secondary = true;
        info->gic = &s->gic;
        memory_region_init_io(&s->power, NULL, &r6_stub_ops, info,
                              "r6.power", R6_POWER_SIZE);

        uint8_t *p;
        int i;

        memory_region_init_ram(&s->pwrsleep, NULL, "r6.pwrsleep",
                               0x10000, &error_fatal);
        p = memory_region_get_ram_ptr(&s->pwrsleep);
        for (i = 0; i < 0x8000; i++) {
            p[2 * i] = 0x70;
            p[2 * i + 1] = 0x47;
        }
        memory_region_add_subregion(&s->power, 0x7000, &s->pwrsleep);
    }
    memory_region_add_subregion(sysmem, R6_POWER_BASE, &s->power);

    /*
     * Status/LED GPIO (Magic Lantern verified: 0x24D0002 on, 0x24C0003 off).
     */
    {
        R6StubInfo *info = g_new0(R6StubInfo, 1);
        info->name = "led";
        info->base = R6_LED_BASE;
        memory_region_init_io(&s->led, NULL, &r6_stub_ops, info, "r6.led",
                              0x40);
    }
    memory_region_add_subregion(sysmem, R6_LED_BASE, &s->led);

    /*
     * Battery/temperature parameter block (verified hardware address):
     * 0x11D28C = battery mV, 0x11D290 = temperature (C).
     */
    {
        uint32_t mv = 7400, temp = 25;
        address_space_stl(&address_space_memory, 0x11D28C, mv,
                          MEMTXATTRS_UNSPECIFIED, NULL);
        address_space_stl(&address_space_memory, 0x11D290, temp,
                          MEMTXATTRS_UNSPECIFIED, NULL);
        fprintf(stderr, "[r6] battery: 7400 mV / 25 C @ 0x0011d28c\n");
    }

    /*
     * Display: poll the DryOS MARV descriptor (~30 Hz) and render the GUI.
     */
    s->gfx.con = graphic_console_init(NULL, 0, &r6_gfx_ops, &s->gfx);
    s->gfx.timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, r6_gfx_tick, &s->gfx);
    timer_mod(s->gfx.timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
              R6_FB_REFRESH_MS);
    fprintf(stderr, "[r6] display: MARV reader @ 0x%08x (~30Hz)\n",
            (unsigned)R6_MARV_LAYER0);
}

static void r6_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Canon EOS R6 (DIGIC X, dual Cortex-A9)";
    mc->init = r6_machine_init;
    mc->reset = r6_reset;
    mc->max_cpus = 2;
    mc->default_cpus = 2;
    mc->ignore_memory_transaction_failures = true;
    mc->default_ram_size = R6_RAM_SIZE;
    mc->default_ram_id = "r6.ram";
}

static const TypeInfo r6_machine_type = {
    .name = TYPE_CANON_R6,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(R6MachineState),
    .class_init = r6_machine_class_init,
};

static void r6_machine_register_types(void)
{
    type_register_static(&r6_machine_type);
}

type_init(r6_machine_register_types)

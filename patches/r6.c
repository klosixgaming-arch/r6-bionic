/*
 * QEMU model of the Canon EOS R6 (DIGIC X / ARM Cortex-A9)
 *
 * Copyright (C) 2026 R6-Bionic Project
 *
 * Based on reverse engineering of the Canon EOS R6 firmware 1.7.0
 * ROM dumps and Magic Lantern R6.150 platform definitions.
 *
 * Memory Map (from ROM analysis):
 *   0x00002000 - 0x3FFFFFFF : 1 GB cacheable RAM
 *   0x40000000 - 0xBEFFFFFF : 2 GB uncacheable RAM
 *   0xBF000000 - 0xDEFFFFFF : MMIO area (peripherals)
 *   0xE0000000 - 0xE7FFFFFF : Main ROM (128 MB space)
 *
 * CPU: ARM Cortex-A9 (ARMv7-A, Thumb-2)
 * ROM Base: 0xE0000000
 * Bootable Base: 0xE0100000
 *
 * This is an initial implementation. Many peripherals are stubbed.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "qemu/datadir.h"
#include "hw/boards.h"
#include "qemu/error-report.h"
#include "hw/loader.h"
#include "sysemu/cpus.h"
#include "sysemu/qtest.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "hw/arm/arm.h"
#include "hw/core/qdev.h"
#include "hw/char/pl011.h"
#include "hw/intc/arm_gic_common.h"
#include "hw/timer/digic-timer.h"
#include "hw/char/digic-uart.h"
#include "hw/gpio/gpio.h"
#include "hw/nvram/eeprom93xx.h"
#include "hw/block/flash.h"

/*
 * Canon EOS R6 Memory Map
 */
#define R6_RAM_BASE           0x00002000
#define R6_RAM_SIZE           (1 * GiB)  /* 0x00002000 - 0x3FFFFFFF */

#define R6_UNCACHABLE_BASE    0x40000000
#define R6_UNCACHABLE_SIZE    (2 * GiB)  /* 0x40000000 - 0xBEFFFFFF */

#define R6_MMIO_BASE          0xBF000000
#define R6_MMIO_SIZE          0x20000000 /* 0xBF000000 - 0xDEFFFFFF */

#define R6_ROM_BASE           0xE0000000
#define R6_ROM_SIZE           (128 * MiB) /* 0xE0000000 - 0xE7FFFFFF */

#define R6_BOOTABLE_BASE      0xE0100000  /* boot00.bin base address */
#define R6_BOOTABLE_SIZE      (36 * MiB)  /* boot00.bin is ~36.75 MB */

/*
 * R6 LED GPIO (verified working via CBasic)
 */
#define R6_LED_GPIO_BASE      0xD22390C0
#define R6_LED_ON             0x24D0002
#define R6_LED_OFF            0x24C0003

/*
 * R6 Machine State
 */
typedef struct R6MachineState {
    /*< private >*/
    MachineState parent;

    /*< public >*/
    ARMCPU *cpu;
    MemoryRegion ram;
    MemoryRegion rom;
    MemoryRegion mmio;
    MemoryRegion uncachable_ram;
    
    /* Peripherals */
    DigicUartState *uart;
    DigicTimerState *timers[3];
    
    /* LED GPIO */
    qemu_irq led_irq;
    uint32_t led_state;
} R6MachineState;

#define TYPE_R6_MACHINE "canon-r6"
OBJECT_DECLARE_TYPE(R6MachineState, R6MachineClass,
                     R6_MACHINE, machine)

/*
 * LED GPIO Device Model
 */
static const MemoryRegionOps r6_led_ops = {
    .read = {
        .impl = {
            .u32 = (memreadfn)(void *)0, /* stub */
        },
    },
    .write = {
        .impl = {
            .u32 = (memwritefn)(void *)0, /* stub */
        },
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/*
 * Load ROM image into memory
 */
static void r6_load_rom(R6MachineState *s, hwaddr addr,
                        hwaddr max_size, const char *filename)
{
    target_long rom_size;

    if (qtest_enabled()) {
        return;
    }

    if (filename) {
        char *fn = qemu_find_file(QEMU_FILE_TYPE_BIOS, filename);

        if (!fn) {
            fprintf(stderr, "Warning: ROM image '%s' not found, using zeros\n",
                    filename);
            return;
        }

        rom_size = load_image_targphys(fn, addr, max_size);
        if (rom_size < 0 || rom_size > max_size) {
            fprintf(stderr, "Warning: Couldn't load ROM image '%s'\n",
                    filename);
            g_free(fn);
            return;
        }
        fprintf(stderr, "Loaded ROM '%s' (%ld bytes) at 0x%lx\n",
                filename, (long)rom_size, (unsigned long)addr);
        g_free(fn);
    }
}

/*
 * R6 Machine Initialization
 */
static void r6_machine_init(MachineState *machine)
{
    R6MachineState *s = R6_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    ARM_CPU *cpu;
    CPUState *cs;
    int i;
    
    /*
     * Create ARM Cortex-A9 CPU
     */
    s->cpu = arm_cpu_create("cortex-a9");
    if (!s->cpu) {
        error_report("Failed to create Cortex-A9 CPU");
        exit(1);
    }

    cs = CPU(s->cpu);
    cs->as = &s->cpu->as;
    
    /*
     * Set up cacheable RAM (1 GB)
     */
    memory_region_init_ram(&s->ram, NULL, "r6.ram", R6_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, R6_RAM_BASE, &s->ram);
    
    /*
     * Set up uncacheable RAM (2 GB)
     */
    memory_region_init_ram(&s->uncachable_ram, NULL, "r6.uncachable_ram",
                           R6_UNCACHABLE_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, R6_UNCACHABLE_BASE,
                                &s->uncachable_ram);
    
    /*
     * Set up MMIO region (peripherals)
     */
    memory_region_init(&s->mmio, NULL, "r6.mmio", R6_MMIO_SIZE);
    memory_region_add_subregion(system_memory, R6_MMIO_BASE, &s->mmio);
    
    /*
     * Set up ROM region (128 MB)
     */
    memory_region_init_rom(&s->rom, NULL, "r6.rom", R6_ROM_SIZE, &error_fatal);
    memory_region_add_subregion(system_memory, R6_ROM_BASE, &s->rom);
    
    /*
     * Load ROM images
     * gang100.bin = primary ROM (64 MB) at 0xE0000000
     * boot00.bin = bootable firmware (36.75 MB) at 0xE0100000
     */
    const char *rom0 = machine->firmware;
    if (!rom0) {
        rom0 = "r6-gang100.bin";
    }
    r6_load_rom(s, R6_ROM_BASE, 64 * MiB, rom0);
    
    /* Load bootable firmware */
    r6_load_rom(s, R6_BOOTABLE_BASE, 36 * MiB, "r6-boot00.bin");
    
    /*
     * Add basic UART (for debugging)
     * Note: The actual R6 UART location is not yet known.
     * Using a placeholder at 0xBF000000 for now.
     */
    if (!qtest_enabled()) {
        s->uart = DIGIC_UART(qdev_new(TYPE_DIGIC_UART));
        qdev_realize_and_plug(DEVICE(s->uart), NULL);
        memory_region_add_subregion(&s->mmio, 0, 
                                    memory_region_find(s->uart->mem, 0, 0, &error_fatal));
    }
    
    /*
     * Set initial PC to boot vector
     * The R6 boots from 0xE0100000 (start of boot00.bin)
     */
    cpu_set_pc(cs, R6_BOOTABLE_BASE);
    
    fprintf(stderr, "Canon EOS R6 (DIGIC X) machine initialized\n");
    fprintf(stderr, "  CPU: ARM Cortex-A9\n");
    fprintf(stderr, "  RAM: 1 GB cacheable + 2 GB uncacheable\n");
    fprintf(stderr, "  ROM: 128 MB at 0xE0000000\n");
    fprintf(stderr, "  Boot: 0xE0100000\n");
}

/*
 * R6 Machine Class Initialization
 */
static void r6_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    
    mc->desc = "Canon EOS R6 (DIGIC X, ARM Cortex-A9)";
    mc->init = r6_machine_init;
    mc->ignore_memory_transaction_failures = true;
    mc->default_ram_size = R6_RAM_SIZE;
    mc->default_ram_id = "r6.ram";
    mc->valid_gsi_mods = 1;
}

/*
 * Register the R6 machine
 */
static const TypeInfo r6_machine_type = {
    .name = TYPE_R6_MACHINE,
    .parent = TYPE_ARM_MACHINE,
    .instance_size = sizeof(R6MachineState),
    .class_init = r6_machine_class_init,
};

/*
 * Machine registration
 */
static void r6_machine_register(void)
{
    type_register_static(&r6_machine_type);
    DEFINE_MACHINE("canon-r6", r6_machine_class_init);
}

type_init(r6_machine_register)

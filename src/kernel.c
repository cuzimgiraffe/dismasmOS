#include "types.h"
#include "io.h"
#include "vga.h"
#include "idt.h"
#include "pic.h"
#include "kbd.h"
#include "fs.h"
#include "proc.h"
#include "shell.h"

static void boot_delay(uint32_t count) {
    for (volatile uint32_t i = 0; i < count; i++) {
        io_wait();
    }
}

static void log_kmsg(const char *ts, const char *msg) {
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("[");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts(ts);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("] ");
    vga_puts(msg);
    vga_putchar('\n');
    boot_delay(15000);
}

static void log_ok(const char *msg) {
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("[  ");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("OK");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("  ] ");
    vga_puts(msg);
    vga_putchar('\n');
    boot_delay(20000);
}

void kmain(void) {
    vga_init();

    log_kmsg("    0.000000", "Linux version 2.0.0-dismasmOS (x86_64-pc-none) #1 PREEMPT");
    log_kmsg("    0.000004", "BIOS-provided physical RAM map:");
    log_kmsg("    0.000008", "  BIOS-e820: [mem 0x0000000000000000-0x000000000009ffff] usable");
    log_kmsg("    0.000012", "  BIOS-e820: [mem 0x0000000000100000-0x000000003fffffff] usable");
    log_kmsg("    0.000018", "CPU: Intel/AMD x86_64 64-Bit Processor (Long Mode Active)");
    log_kmsg("    0.000025", "console [vga0] enabled, 80x25 text mode at 0xB8000");
    log_kmsg("    0.000032", "GDT: 64-Bit Global Descriptor Table installed (CS=0x08, DS=0x10)");

    idt_init();
    log_kmsg("    0.000040", "IDT: 64-Bit Interrupt Descriptor Table loaded (256 gates registered)");

    pic_remap(0x20, 0x28);
    log_kmsg("    0.000048", "i8259A: Master/Slave PIC remapped to vectors 0x20-0x2F");

    kbd_init();
    log_kmsg("    0.000055", "i8042: PS/2 keyboard controller ready, IRQ1 unmasked");
    log_kmsg("    0.000062", "input: German QWERTZ keymap and AltGr mapping active");

    __asm__ volatile("sti");
    log_kmsg("    0.000070", "system: CPU hardware interrupts enabled (RFLAGS.IF=1)");

    fs_init();
    log_kmsg("    0.000085", "vfs: In-memory filesystem mounted at / (type ramfs)");

    proc_init();
    log_kmsg("    0.000095", "proc: Process management active, agent service.prf loaded");

    log_ok("Mounted In-Memory Root Filesystem.");
    log_ok("Started Process Subsystem (service.prf).");
    log_ok("Started German Keyboard Layout Mapping Service.");
    log_ok("Started Console Terminal Driver.");
    log_ok("Reached target 64-Bit System Initialization.");
    log_ok("Started dismasmOS Command Line Shell.");

    boot_delay(600000);

    shell_init();
    shell_run();

    while (1) {
        __asm__ volatile("hlt");
    }
}

const char *SYSTEM_VERSION = "1.2";
const char *SYSTEM_BUILD   = "VF001.02.0.2026";

#include "shell.h"
#include "vga.h"
#include "kbd.h"
#include "string.h"
#include "fs.h"
#include "io.h"
#include "proc.h"
#include "em.h"
#include "heap.h"


#define CMD_MAX_LEN 128
#define MAX_ARGS 16

extern uint8_t _kernel_start, _text_end, _rodata_end, _data_end, _kernel_end;

static char shell_cwd[FS_MAX_PATH] = "/home";

static void sys_shutdown(void) {
    vga_clear();
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("dismasmOS ");
    vga_puts(SYSTEM_VERSION);
    vga_puts(" is shutting down...\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("Flushing filesystem buffers... [  OK  ]\n");
    vga_puts("Stopping background agents... [  OK  ]\n");
    vga_puts("Preparing hardware power-off... [  OK  ]\n\n");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("System halted. It is now safe to power off your machine.\n");

    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    outb(0xF4, 0x00);

    __asm__ volatile (
        "cli\n\t"
        "1:\n\t"
        "hlt\n\t"
        "jmp 1b\n\t"
        : : : "memory"
    );
}

static void sys_reboot(void) {
    uint8_t temp;
    __asm__ volatile("cli");
    do {
        temp = inb(0x64);
        if (temp & 1) {
            inb(0x60);
        }
    } while (temp & 2);
    outb(0x64, 0xFE);
    while (1) {
        __asm__ volatile("hlt");
    }
}

static void print_hex(uint64_t val) {
    char hex[17];
    utoa_hex(val, hex);
    vga_puts("0x");
    int len = (int)strlen(hex);
    for (int i = 0; i < 8 - len; i++) {
        vga_putchar('0');
    }
    vga_puts(hex);
}

static void print_str_pad(const char *s, int pad) {
    int len = (int)strlen(s);
    vga_puts(s);
    while (len < pad) {
        vga_putchar(' ');
        len++;
    }
}

static void print_num_kib(uint64_t bytes, int pad) {
    char num[16];
    char out[24];
    uint64_t kib = (bytes + 1023) / 1024;
    itoa((int)kib, num);
    strcpy(out, num);
    strcat(out, " KiB");
    print_str_pad(out, pad);
}

static void cmd_memrep(void) {
    uint64_t k_start = (uint64_t)(uintptr_t)&_kernel_start;
    uint64_t t_end = (uint64_t)(uintptr_t)&_text_end;
    uint64_t ro_end = (uint64_t)(uintptr_t)&_rodata_end;
    uint64_t d_end = (uint64_t)(uintptr_t)&_data_end;
    uint64_t k_end = (uint64_t)(uintptr_t)&_kernel_end;

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("Memory Allocation Report (MemRep) - 64-Bit x86_64:\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("Address Range           Size       Region         Attribute\n");
    vga_puts("-------------------------------------------------------------\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    print_hex(0);
    vga_puts("-");
    print_hex(0x0007FFFF);
    vga_puts("  ");
    print_num_kib(0x00080000, 11);
    print_str_pad("Low Memory", 15);
    vga_puts("Reserved\n");

    print_hex(0x00080000);
    vga_puts("-");
    print_hex(0x0009FFFF);
    vga_puts("  ");
    print_num_kib(0x00020000, 11);
    print_str_pad("EBDA/BIOS", 15);
    vga_puts("Reserved\n");

    print_hex(0x000A0000);
    vga_puts("-");
    print_hex(0x000BFFFF);
    vga_puts("  ");
    print_num_kib(0x00020000, 11);
    print_str_pad("VGA VRAM", 15);
    vga_puts("MMIO (0xB8000)\n");

    print_hex(0x000C0000);
    vga_puts("-");
    print_hex(0x000FFFFF);
    vga_puts("  ");
    print_num_kib(0x00040000, 11);
    print_str_pad("Firmware ROM", 15);
    vga_puts("Read-Only\n");

    print_hex(k_start);
    vga_puts("-");
    print_hex(t_end);
    vga_puts("  ");
    print_num_kib(t_end - k_start, 11);
    print_str_pad("Kernel .text", 15);
    vga_puts("Executable\n");

    print_hex(t_end);
    vga_puts("-");
    print_hex(ro_end);
    vga_puts("  ");
    print_num_kib(ro_end - t_end, 11);
    print_str_pad("Kernel .rodata", 15);
    vga_puts("Read-Only\n");

    print_hex(ro_end);
    vga_puts("-");
    print_hex(d_end);
    vga_puts("  ");
    print_num_kib(d_end - ro_end, 11);
    print_str_pad("Kernel .data", 15);
    vga_puts("Read/Write\n");

    print_hex(d_end);
    vga_puts("-");
    print_hex(k_end);
    vga_puts("  ");
    print_num_kib(k_end - d_end, 11);
    print_str_pad("Kernel .bss", 15);
    vga_puts("Zero-Init\n");

    uint64_t heap_s = (uint64_t)heap_get_start_addr();
    uint64_t heap_e = (uint64_t)heap_get_end_addr();

    print_hex(heap_s);
    vga_puts("-");
    print_hex(heap_e);
    vga_puts("  ");
    print_num_kib(heap_e - heap_s, 11);
    print_str_pad("Kernel Heap", 15);
    vga_puts("Dynamic malloc/free\n");

    print_hex(heap_e);
    vga_puts("-");
    print_hex(0x3FFFFFFF);
    vga_puts("  ");
    print_num_kib(0x40000000 - heap_e, 11);
    print_str_pad("Identity RAM", 15);
    vga_puts("1 GiB Long Mode\n");

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("-------------------------------------------------------------\n");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);

    char num[16];
    vga_puts("RAMFS Payload: ");
    itoa((int)((fs_used_bytes() + 1023) / 1024), num);
    vga_puts(num);
    vga_puts(" KiB / ");
    itoa((int)((FS_MAX_FILES * FS_MAX_FILESIZE) / 1024), num);
    vga_puts(num);
    vga_puts(" KiB (");
    itoa((int)fs_file_count(), num);
    vga_puts(num);
    vga_puts(" active files)\n");

    vga_puts("Process Memory: ");
    itoa((int)proc_total_mem_kib(), num);
    vga_puts(num);
    vga_puts(" KiB (");
    itoa((int)proc_active_count(), num);
    vga_puts(num);
    vga_puts(" active processes)\n");

    vga_puts("Kernel Heap: ");
    itoa((int)((heap_get_used() + 1023) / 1024), num);
    vga_puts(num);
    vga_puts(" KiB / ");
    itoa((int)(heap_get_total() / 1024), num);
    vga_puts(num);
    vga_puts(" KiB (");
    itoa((int)heap_get_alloc_count(), num);
    vga_puts(" active allocs)\n");

    vga_puts("Kernel Footprint: ");
    uint64_t k_kib = (k_end - k_start + 1023) / 1024;
    itoa((int)k_kib, num);
    vga_puts(num);
    vga_puts(" KiB\n");

    uint64_t total_used_kib = k_kib + proc_total_mem_kib() + ((fs_used_bytes() + 1023) / 1024) + ((heap_get_used() + 1023) / 1024) + 32;
    vga_puts("Total System Used: ");
    itoa((int)total_used_kib, num);
    vga_puts(num);
    vga_puts(" KiB\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

static void cmd_lang(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("current layout: ");
        if (kbd_get_layout() == KBD_LAYOUT_DE) {
            vga_puts("de (German QWERTZ)\n");
        } else {
            vga_puts("en (US QWERTY)\n");
        }
        vga_puts("usage: lang <de|en>\n");
        return;
    }
    if (strcmp(argv[1], "de") == 0) {
        kbd_set_layout(KBD_LAYOUT_DE);
        vga_puts("switched to: de\n");
    } else if (strcmp(argv[1], "en") == 0) {
        kbd_set_layout(KBD_LAYOUT_EN);
        vga_puts("switched to: en\n");
    } else {
        vga_puts("- unknown - '");
        vga_puts(argv[1]);
        vga_puts("'. use 'de' or 'en'\n");
    }
}

static void sound_error(void) {
    uint32_t div = 1193180 / 3000;
    outb(0x43, 0xB6);
    outb(0x42, (uint8_t)(div & 0xFF));
    outb(0x42, (uint8_t)((div >> 8) & 0xFF));
    uint8_t tmp = inb(0x61);
    if ((tmp & 3) != 3) {
        outb(0x61, (uint8_t)(tmp | 3));
    }
    for (volatile int i = 0; i < 1000; i++) {
        io_wait();
    }
    outb(0x61, (uint8_t)(inb(0x61) & ~3));
}

static uint64_t parse_num(const char *s) {
    uint64_t val = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (*s) {
            char c = *s++;
            val <<= 4;
            if (c >= '0' && c <= '9') val |= (c - '0');
            else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
            else break;
        }
    } else {
        while (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s++ - '0');
        }
    }
    return val;
}

static int cmd_reg(void) {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t cr0, cr2, cr3, cr4, rflags;

    __asm__ volatile("mov %%rax, %0" : "=r"(rax));
    __asm__ volatile("mov %%rbx, %0" : "=r"(rbx));
    __asm__ volatile("mov %%rcx, %0" : "=r"(rcx));
    __asm__ volatile("mov %%rdx, %0" : "=r"(rdx));
    __asm__ volatile("mov %%rsi, %0" : "=r"(rsi));
    __asm__ volatile("mov %%rdi, %0" : "=r"(rdi));
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile("mov %%r8, %0" : "=r"(r8));
    __asm__ volatile("mov %%r9, %0" : "=r"(r9));
    __asm__ volatile("mov %%r10, %0" : "=r"(r10));
    __asm__ volatile("mov %%r11, %0" : "=r"(r11));
    __asm__ volatile("mov %%r12, %0" : "=r"(r12));
    __asm__ volatile("mov %%r13, %0" : "=r"(r13));
    __asm__ volatile("mov %%r14, %0" : "=r"(r14));
    __asm__ volatile("mov %%r15, %0" : "=r"(r15));

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("CPU Registers (x86_64 Long Mode):\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("-------------------------------------------------------------\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    vga_puts("RAX: "); print_hex(rax); vga_puts("  RBX: "); print_hex(rbx);
    vga_puts("  RCX: "); print_hex(rcx); vga_puts("  RDX: "); print_hex(rdx); vga_putchar('\n');

    vga_puts("RSI: "); print_hex(rsi); vga_puts("  RDI: "); print_hex(rdi);
    vga_puts("  RBP: "); print_hex(rbp); vga_puts("  RSP: "); print_hex(rsp); vga_putchar('\n');

    vga_puts("R8 : "); print_hex(r8);  vga_puts("  R9 : "); print_hex(r9);
    vga_puts("  R10: "); print_hex(r10); vga_puts("  R11: "); print_hex(r11); vga_putchar('\n');

    vga_puts("R12: "); print_hex(r12); vga_puts("  R13: "); print_hex(r13);
    vga_puts("  R14: "); print_hex(r14); vga_puts("  R15: "); print_hex(r15); vga_putchar('\n');

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("CR0: "); print_hex(cr0); vga_puts("  CR2: "); print_hex(cr2);
    vga_puts("  CR3: "); print_hex(cr3); vga_puts("  CR4: "); print_hex(cr4); vga_putchar('\n');

    vga_puts("RFLAGS: "); print_hex(rflags);
    vga_puts(" [ ");
    if (rflags & 0x0001) vga_puts("CF ");
    if (rflags & 0x0040) vga_puts("ZF ");
    if (rflags & 0x0080) vga_puts("SF ");
    if (rflags & 0x0200) vga_puts("IF ");
    if (rflags & 0x0400) vga_puts("DF ");
    if (rflags & 0x0800) vga_puts("OF ");
    vga_puts("]\n");

    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    return 0;
}

static int cmd_dump(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("usage: dump <addr> [length]\n");
        sound_error();
        return 1;
    }
    uint64_t addr = parse_num(argv[1]);
    uint64_t len = 64;
    if (argc >= 3) {
        len = parse_num(argv[2]);
        if (len == 0) len = 64;
        if (len > 512) len = 512;
    }

    uint8_t *ptr = (uint8_t *)(uintptr_t)addr;
    char hex_byte[17];

    for (uint64_t i = 0; i < len; i += 16) {
        print_hex(addr + i);
        vga_puts(": ");
        for (uint64_t j = 0; j < 16; j++) {
            if (i + j < len) {
                uint8_t b = ptr[i + j];
                utoa_hex(b, hex_byte);
                if (b < 16) vga_putchar('0');
                vga_puts(hex_byte);
                vga_putchar(' ');
            } else {
                vga_puts("   ");
            }
            if (j == 7) vga_putchar(' ');
        }
        vga_puts(" |");
        for (uint64_t j = 0; j < 16; j++) {
            if (i + j < len) {
                char c = (char)ptr[i + j];
                if (c >= 32 && c <= 126) {
                    vga_putchar(c);
                } else {
                    vga_putchar('.');
                }
            } else {
                vga_putchar(' ');
            }
        }
        vga_puts("|\n");
    }
    return 0;
}

static const char *calc_ptr;
static int calc_err;

static int64_t calc_parse_expr(void);

static void calc_skip(void) {
    while (*calc_ptr == ' ' || *calc_ptr == '\t') calc_ptr++;
}

static int64_t calc_parse_factor(void) {
    calc_skip();
    if (*calc_ptr == '+') {
        calc_ptr++;
        return calc_parse_factor();
    }
    if (*calc_ptr == '-') {
        calc_ptr++;
        return -calc_parse_factor();
    }
    if (*calc_ptr == '(') {
        calc_ptr++;
        int64_t v = calc_parse_expr();
        calc_skip();
        if (*calc_ptr == ')') {
            calc_ptr++;
        } else {
            calc_err = 1;
        }
        return v;
    }
    if ((*calc_ptr >= '0' && *calc_ptr <= '9') ||
        (*calc_ptr == '0' && (*(calc_ptr + 1) == 'x' || *(calc_ptr + 1) == 'X'))) {
        const char *s = calc_ptr;
        if (*calc_ptr == '0' && (*(calc_ptr + 1) == 'x' || *(calc_ptr + 1) == 'X')) {
            calc_ptr += 2;
            while ((*calc_ptr >= '0' && *calc_ptr <= '9') ||
                   (*calc_ptr >= 'a' && *calc_ptr <= 'f') ||
                   (*calc_ptr >= 'A' && *calc_ptr <= 'F')) {
                calc_ptr++;
            }
        } else {
            while (*calc_ptr >= '0' && *calc_ptr <= '9') calc_ptr++;
        }
        return (int64_t)parse_num(s);
    }
    calc_err = 1;
    return 0;
}

static int64_t calc_parse_term(void) {
    int64_t v = calc_parse_factor();
    while (!calc_err) {
        calc_skip();
        char op = *calc_ptr;
        if (op == '*' || op == '/' || op == '%') {
            calc_ptr++;
            int64_t rhs = calc_parse_factor();
            if (calc_err) break;
            if (op == '*') v *= rhs;
            else if (op == '/') {
                if (rhs == 0) {
                    calc_err = 2;
                    break;
                }
                v /= rhs;
            } else if (op == '%') {
                if (rhs == 0) {
                    calc_err = 2;
                    break;
                }
                v %= rhs;
            }
        } else {
            break;
        }
    }
    return v;
}

static int64_t calc_parse_expr(void) {
    int64_t v = calc_parse_term();
    while (!calc_err) {
        calc_skip();
        char op = *calc_ptr;
        if (op == '+' || op == '-') {
            calc_ptr++;
            int64_t rhs = calc_parse_term();
            if (calc_err) break;
            if (op == '+') v += rhs;
            else v -= rhs;
        } else {
            break;
        }
    }
    return v;
}

static int cmd_calc(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("usage: calc <expression>\n");
        sound_error();
        return 1;
    }
    char buf[128];
    buf[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1) strcat(buf, " ");
        strcat(buf, argv[i]);
    }
    calc_ptr = buf;
    calc_err = 0;
    int64_t res = calc_parse_expr();
    calc_skip();
    if (*calc_ptr != '\0' && !calc_err) {
        calc_err = 1;
    }
    if (calc_err == 1) {
        vga_puts("calc error: invalid syntax\n");
        sound_error();
        return 1;
    }
    if (calc_err == 2) {
        vga_puts("calc error: division by zero\n");
        sound_error();
        return 1;
    }

    char num[32];
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("Result: ");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    if (res < 0) {
        vga_putchar('-');
        itoa((int)(-res), num);
    } else {
        itoa((int)res, num);
    }
    vga_puts(num);
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts(" (");
    print_hex((uint64_t)res);
    vga_puts(")\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    return 0;
}

static uint8_t asm_payload[256];

static int cmd_asm(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("usage: asm <bytecode|nop|cli|sti|rdtsc|cpuid>\n");
        sound_error();
        return 1;
    }

    if (strcmp(argv[1], "nop") == 0) {
        __asm__ volatile("nop");
        vga_puts("ASM: nop executed\n");
        return 0;
    }
    if (strcmp(argv[1], "cli") == 0) {
        __asm__ volatile("cli");
        vga_puts("ASM: cli executed\n");
        return 0;
    }
    if (strcmp(argv[1], "sti") == 0) {
        __asm__ volatile("sti");
        vga_puts("ASM: sti executed\n");
        return 0;
    }
    if (strcmp(argv[1], "rdtsc") == 0) {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        uint64_t tsc = ((uint64_t)hi << 32) | lo;
        vga_puts("TSC: ");
        print_hex(tsc);
        vga_putchar('\n');
        return 0;
    }
    if (strcmp(argv[1], "cpuid") == 0) {
        uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
        char vendor[13];
        *(uint32_t *)&vendor[0] = ebx;
        *(uint32_t *)&vendor[4] = edx;
        *(uint32_t *)&vendor[8] = ecx;
        vendor[12] = '\0';
        vga_puts("CPUID: ");
        vga_puts(vendor);
        vga_putchar('\n');
        return 0;
    }

    size_t count = 0;
    for (int i = 1; i < argc; i++) {
        char *tok = argv[i];
        if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) tok += 2;
        size_t tlen = strlen(tok);
        for (size_t k = 0; k < tlen; k += 2) {
            if (count >= sizeof(asm_payload) - 2) break;
            char sub[3];
            sub[0] = tok[k];
            sub[1] = (k + 1 < tlen) ? tok[k + 1] : '\0';
            sub[2] = '\0';
            asm_payload[count++] = (uint8_t)parse_num(sub);
        }
    }

    if (count == 0) {
        vga_puts("asm: invalid bytecode\n");
        sound_error();
        return 1;
    }

    if (asm_payload[count - 1] != 0xC3) {
        asm_payload[count++] = 0xC3;
    }

    typedef uint64_t (*asm_call_t)(void);
    asm_call_t code = (asm_call_t)(uintptr_t)asm_payload;
    uint64_t out_rax = code();

    char num[32];
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("ASM Return RAX: ");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    print_hex(out_rax);
    vga_puts(" (");
    itoa((int)out_rax, num);
    vga_puts(num);
    vga_puts(")\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    return 0;
}

static int cmd_colr(int argc, char **argv) {
    if (argc < 2) {
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        vga_puts("16 Available Colors (0-15):\n");
        for (int i = 0; i < 16; i++) {
            char num[8];
            itoa(i, num);
            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            vga_puts("  ");
            if (i < 10) {
                vga_putchar(' ');
            }
            vga_puts(num);
            vga_puts(": ");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            print_str_pad(vga_get_color_name((uint8_t)i), 15);
            vga_set_color((uint8_t)i, VGA_COLOR_BLACK);
            vga_putchar((char)0xDB);
            vga_putchar('\n');
        }
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_puts("usage: colr <farbe>  (e.g. colr red, colr 4, colr (blue))\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return 0;
    }

    char arg_buf[64];
    arg_buf[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            strcat(arg_buf, " ");
        }
        strcat(arg_buf, argv[i]);
    }

    int color = vga_parse_color(arg_buf);
    if (color < 0 || color > 15) {
        vga_puts("colr: unknown color '");
        vga_puts(arg_buf);
        vga_puts("'. (type 'colr' for list of 16 colors)\n");
        sound_error();
        return 1;
    }

    vga_set_color((uint8_t)color, VGA_COLOR_BLACK);
    vga_putchar((char)0xDB);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_putchar('\n');
    return 0;
}

static int cmd_heap(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        vga_puts("Running Kernel Heap Allocator Self-Test (malloc, calloc, realloc, free)...\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

        int res = heap_self_test();
        if (res == 0) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            vga_puts("[  OK  ] All heap allocator tests passed successfully!\n");
        } else {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_puts("[ FAIL ] Heap self-test failed with code: ");
            char num[8];
            itoa(res, num);
            vga_puts(num);
            vga_putchar('\n');
            sound_error();
        }
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return res;
    }

    char num[32];
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("Kernel Heap Allocator Status (malloc / free):\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("-------------------------------------------------------------\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    vga_puts("  Total Heap Size:    ");
    itoa((int)(heap_get_total() / 1024), num);
    vga_puts(num);
    vga_puts(" KiB (16 MiB)\n");

    vga_puts("  Used Heap Memory:   ");
    itoa((int)heap_get_used(), num);
    vga_puts(num);
    vga_puts(" Bytes\n");

    vga_puts("  Free Heap Memory:   ");
    itoa((int)(heap_get_free() / 1024), num);
    vga_puts(num);
    vga_puts(" KiB\n");

    vga_puts("  Active Allocations: ");
    itoa((int)heap_get_alloc_count(), num);
    vga_puts(num);
    vga_puts("\n");

    vga_puts("  Heap Address Range: ");
    print_hex(heap_get_start_addr());
    vga_puts(" - ");
    print_hex(heap_get_end_addr());
    vga_puts("\n");

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("-------------------------------------------------------------\n");
    vga_puts("Type 'heap test' to run full diagnostic validation suite.\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    return 0;
}

static void cmd_help(void) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("dismasmOS 1.2 Available Commands (23):\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("  help      - display available commands\n");
    vga_puts("  clear     - clear console screen\n");
    vga_puts("  colr      - display 16-color 1-bit spot for color (colr (farbe))\n");
    vga_puts("  heap      - kernel heap allocator status & self-test (heap test)\n");
    vga_puts("  calc      - integer arithmetic calculator (calc 10 + 5 * 2)\n");
    vga_puts("  dump      - memory hex and ascii dump (dump 0x1000 64)\n");
    vga_puts("  reg       - display cpu 64-bit and control registers\n");
    vga_puts("  asm       - execute raw machine code or opcode mnemonic\n");
    vga_puts("  lsf       - list files and dirs (flags: -s size in KiB & hex, -p perms)\n");
    vga_puts("  cd        - change working directory\n");
    vga_puts("  makedir   - create a new directory\n");
    vga_puts("  changes   - view filesystem access and modification audit log\n");
    vga_puts("  em        - vi-style text editor (Alt for commands: ;wsc, ;q, ;s, ;-m)\n");
    vga_puts("  cat       - display file content\n");
    vga_puts("  echo      - print or redirect text into file (> file)\n");
    vga_puts("  touch     - create empty file\n");
    vga_puts("  rm        - remove file or directory\n");
    vga_puts("  grep      - search pattern in file\n");
    vga_puts("  pr        - process management (start, kill, -c, status)\n");
    vga_puts("  uname     - system information\n");
    vga_puts("  MemRep    - memory allocation report\n");
    vga_puts("  lang      - switch keyboard layout (de / en)\n");
    vga_puts("  shutdown  - power off / halt system via hardware assembly\n");
    vga_puts("  reboot    - reboot machine\n");
    vga_puts("  &&        - chain multiple commands sequentially\n");
}


static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        vga_puts(argv[i]);
        if (i < argc - 1) {
            vga_putchar(' ');
        }
    }
    vga_putchar('\n');
}

static int cmd_cd(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "~") == 0) {
        strcpy(shell_cwd, "/home");
        return 0;
    }
    if (strcmp(argv[1], "/") == 0) {
        strcpy(shell_cwd, "/");
        return 0;
    }
    char resolved[FS_MAX_PATH];
    fs_resolve_path(shell_cwd, argv[1], resolved);

    if (fs_is_dir(resolved)) {
        strncpy(shell_cwd, resolved, FS_MAX_PATH - 1);
        shell_cwd[FS_MAX_PATH - 1] = '\0';
        return 0;
    } else {
        vga_puts("cd: no such directory: ");
        vga_puts(argv[1]);
        vga_putchar('\n');
        sound_error();
        return 1;
    }
}

static int cmd_makedir(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("usage: makedir <directory>\n");
        sound_error();
        return 1;
    }
    char resolved[FS_MAX_PATH];
    fs_resolve_path(shell_cwd, argv[1], resolved);

    if (fs_find(resolved) != NULL || strcmp(resolved, "/") == 0) {
        vga_puts("makedir: directory already exists: ");
        vga_puts(resolved);
        vga_putchar('\n');
        sound_error();
        return 1;
    }

    if (fs_mkdir(resolved) == 0) {
        fs_log_change("CREATED", "root", resolved);
        vga_puts("created directory: ");
        vga_puts(resolved);
        vga_putchar('\n');
        return 0;
    } else {
        vga_puts("makedir: cannot create directory\n");
        sound_error();
        return 1;
    }
}

static int cmd_lsf(int argc, char **argv) {
    int show_size = 0;
    int show_perm = 0;
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (size_t k = 1; argv[i][k] != '\0'; k++) {
                if (argv[i][k] == 's') {
                    show_size = 1;
                } else if (argv[i][k] == 'p') {
                    show_perm = 1;
                }
            }
        } else {
            target = argv[i];
        }
    }

    char resolved[FS_MAX_PATH];
    if (target) {
        fs_resolve_path(shell_cwd, target, resolved);
    } else {
        strncpy(resolved, shell_cwd, FS_MAX_PATH - 1);
        resolved[FS_MAX_PATH - 1] = '\0';
    }

    if (!fs_is_dir(resolved)) {
        vga_puts("lsf: not a directory: ");
        vga_puts(resolved);
        vga_putchar('\n');
        sound_error();
        return 1;
    }

    fs_list(resolved, show_size, show_perm);
    return 0;
}

static void cmd_changes(void) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("Filesystem Audit Log (changes):\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    fs_show_changes();
}

static int cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("usage: cat <file>\n");
        sound_error();
        return 1;
    }
    char resolved[FS_MAX_PATH];
    fs_resolve_path(shell_cwd, argv[1], resolved);

    struct fs_file *file = fs_find(resolved);
    if (!file || file->is_dir) {
        vga_puts("cat: file not found: ");
        vga_puts(argv[1]);
        vga_putchar('\n');
        sound_error();
        return 1;
    }

    fs_log_change("VIEW", "root", resolved);
    vga_puts(file->data);
    if (file->size > 0 && file->data[file->size - 1] != '\n') {
        vga_putchar('\n');
    }
    return 0;
}

static int cmd_grep(int argc, char **argv) {
    if (argc < 3) {
        vga_puts("usage: grep <pattern> <file>\n");
        sound_error();
        return 1;
    }
    char resolved[FS_MAX_PATH];
    fs_resolve_path(shell_cwd, argv[2], resolved);

    struct fs_file *file = fs_find(resolved);
    if (!file || file->is_dir) {
        vga_puts("grep: file not found: ");
        vga_puts(argv[2]);
        vga_putchar('\n');
        sound_error();
        return 1;
    }

    fs_log_change("VIEW", "root", resolved);

    char line[256];
    size_t line_idx = 0;
    for (size_t i = 0; i <= file->size; i++) {
        char c = file->data[i];
        if (c == '\n' || c == '\0') {
            line[line_idx] = '\0';
            if (line_idx > 0 && strstr(line, argv[1])) {
                vga_puts(line);
                vga_putchar('\n');
            }
            line_idx = 0;
            if (c == '\0') {
                break;
            }
        } else {
            if (line_idx < sizeof(line) - 1) {
                line[line_idx++] = c;
            }
        }
    }
    return 0;
}

static int cmd_touch(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("usage: touch <file>\n");
        sound_error();
        return 1;
    }
    char resolved[FS_MAX_PATH];
    fs_resolve_path(shell_cwd, argv[1], resolved);

    if (!fs_find(resolved)) {
        if (fs_create(resolved) == 0) {
            fs_log_change("CREATED", "root", resolved);
            return 0;
        } else {
            vga_puts("touch: cannot create file\n");
            sound_error();
            return 1;
        }
    }
    return 0;
}

static int cmd_rm(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("usage: rm <file>\n");
        sound_error();
        return 1;
    }
    char resolved[FS_MAX_PATH];
    fs_resolve_path(shell_cwd, argv[1], resolved);

    if (fs_delete(resolved) == 0) {
        fs_log_change("DELETED", "root", resolved);
        vga_puts("removed: ");
        vga_puts(resolved);
        vga_putchar('\n');
        return 0;
    } else {
        vga_puts("rm: file not found: ");
        vga_puts(argv[1]);
        vga_putchar('\n');
        sound_error();
        return 1;
    }
}

static void cmd_uname(void) {
    vga_puts("dismasmOS 1.2 (x86_64 Long Mode 64-Bit)\nCreation of the Saviour\nMay God lead this Creation\n");
}

static void cmd_pr(int argc, char **argv) {
    if (argc < 2) {
        proc_status_all();
        return;
    }

    if (strcmp(argv[1], "start") == 0) {
        if (argc < 3) {
            vga_puts("usage: pr start <prozess>\n");
            return;
        }
        proc_start(argv[2]);
    } else if (strcmp(argv[1], "kill") == 0) {
        if (argc < 3) {
            vga_puts("usage: pr kill <prozess>\n");
            return;
        }
        proc_kill(argv[2]);
    } else if (strcmp(argv[1], "status") == 0) {
        if (argc >= 3) {
            proc_status(argv[2]);
        } else {
            proc_status_all();
        }
    } else if (strcmp(argv[1], "-c") == 0) {
        if (argc < 4) {
            vga_puts("usage: pr -c <comment> <prozess>\n");
            return;
        }
        char comment_buf[PROC_COMMENT_MAX];
        comment_buf[0] = '\0';
        for (int i = 2; i < argc - 1; i++) {
            if (i > 2) {
                strcat(comment_buf, " ");
            }
            strcat(comment_buf, argv[i]);
        }
        proc_set_comment(argv[argc - 1], comment_buf);
    } else {
        proc_start(argv[1]);
    }
}

static void cmd_em(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("usage: em <file>\n");
        return;
    }
    char resolved[FS_MAX_PATH];
    fs_resolve_path(shell_cwd, argv[1], resolved);

    em_run(resolved);
    vga_clear();
}

static int tokenize(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args) {
        while (*p == ' ' || *p == '\t') {
            *p++ = '\0';
        }
        if (*p == '\0') {
            break;
        }
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
    }
    return argc;
}

static int execute_command(int argc, char **argv) {
    if (argc == 0) {
        return 0;
    }
    if (strcmp(argv[0], "help") == 0) {
        cmd_help();
        return 0;
    } else if (strcmp(argv[0], "clear") == 0) {
        vga_clear();
        return 0;
    } else if (strcmp(argv[0], "colr") == 0) {
        return cmd_colr(argc, argv);
    } else if (strcmp(argv[0], "heap") == 0) {
        return cmd_heap(argc, argv);
    } else if (strcmp(argv[0], "calc") == 0) {
        return cmd_calc(argc, argv);
    } else if (strcmp(argv[0], "dump") == 0) {
        return cmd_dump(argc, argv);
    } else if (strcmp(argv[0], "reg") == 0) {
        return cmd_reg();
    } else if (strcmp(argv[0], "asm") == 0) {
        return cmd_asm(argc, argv);
    } else if (strcmp(argv[0], "echo") == 0) {
        cmd_echo(argc, argv);
        return 0;
    } else if (strcmp(argv[0], "lsf") == 0) {
        return cmd_lsf(argc, argv);
    } else if (strcmp(argv[0], "cd") == 0) {
        return cmd_cd(argc, argv);
    } else if (strcmp(argv[0], "makedir") == 0) {
        return cmd_makedir(argc, argv);
    } else if (strcmp(argv[0], "changes") == 0) {
        cmd_changes();
        return 0;
    } else if (strcmp(argv[0], "rm") == 0) {
        return cmd_rm(argc, argv);
    } else if (strcmp(argv[0], "cat") == 0) {
        return cmd_cat(argc, argv);
    } else if (strcmp(argv[0], "grep") == 0) {
        return cmd_grep(argc, argv);
    } else if (strcmp(argv[0], "touch") == 0) {
        return cmd_touch(argc, argv);
    } else if (strcmp(argv[0], "uname") == 0) {
        cmd_uname();
        return 0;
    } else if (strcmp(argv[0], "MemRep") == 0 || strcmp(argv[0], "memrep") == 0) {
        cmd_memrep();
        return 0;
    } else if (strcmp(argv[0], "lang") == 0) {
        cmd_lang(argc, argv);
        return 0;
    } else if (strcmp(argv[0], "pr") == 0) {
        cmd_pr(argc, argv);
        return 0;
    } else if (strcmp(argv[0], "em") == 0) {
        cmd_em(argc, argv);
        return 0;
    } else if (strcmp(argv[0], "shutdown") == 0) {
        sys_shutdown();
        return 0;
    } else if (strcmp(argv[0], "reboot") == 0) {
        sys_reboot();
        return 0;
    } else {
        vga_puts("unknown command: ");
        vga_puts(argv[0]);
        vga_puts(" (type 'help' for list)\n");
        sound_error();
        return 1;
    }
}

static void print_prompt(void) {
    vga_set_color(VGA_COLOR_RED, VGA_COLOR_BLACK);
    vga_puts("root@admin@dismasmOS");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_putchar(':');
    vga_puts(shell_cwd);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts(" | >>> ");
}

static int handle_single_command(char *cmd_buf) {
    char *redir = strchr(cmd_buf, '>');
    if (redir != NULL) {
        *redir = '\0';
        char *target = redir + 1;
        while (*target == ' ' || *target == '\t') {
            target++;
        }
        char *t_end = target + strlen(target);
        while (t_end > target && (*(t_end - 1) == ' ' || *(t_end - 1) == '\t')) {
            t_end--;
        }
        *t_end = '\0';

        if (*target == '\0') {
            vga_puts("syntax error: no file after '>'\n");
            sound_error();
            return 1;
        }

        char *cmd_left = cmd_buf;
        while (*cmd_left == ' ' || *cmd_left == '\t') {
            cmd_left++;
        }
        char *c_end = cmd_left + strlen(cmd_left);
        while (c_end > cmd_left && (*(c_end - 1) == ' ' || *(c_end - 1) == '\t')) {
            c_end--;
        }
        *c_end = '\0';

        if (strncmp(cmd_left, "echo", 4) == 0 && (cmd_left[4] == ' ' || cmd_left[4] == '\t' || cmd_left[4] == '\0')) {
            char *text = cmd_left + 4;
            while (*text == ' ' || *text == '\t') {
                text++;
            }
            if (*text == '"' || *text == '\'') {
                char q = *text;
                text++;
                size_t tlen = strlen(text);
                if (tlen > 0 && text[tlen - 1] == q) {
                    text[tlen - 1] = '\0';
                }
            }

            char resolved_target[FS_MAX_PATH];
            fs_resolve_path(shell_cwd, target, resolved_target);

            int exists = (fs_find(resolved_target) != NULL);
            if (!exists) {
                if (fs_create(resolved_target) != 0) {
                    vga_puts("error: cannot create file: ");
                    vga_puts(resolved_target);
                    vga_putchar('\n');
                    sound_error();
                    return 1;
                }
                fs_log_change("CREATED", "root", resolved_target);
            }
            fs_write(resolved_target, text, strlen(text));
            if (exists) {
                fs_log_change("EDITED", "root", resolved_target);
            }
            return 0;
        }
    }

    char *argv[MAX_ARGS];
    int argc = tokenize(cmd_buf, argv, MAX_ARGS);
    return execute_command(argc, argv);
}

static void handle_line(char *cmd_buf) {
    char *p = cmd_buf;
    while (*p != '\0') {
        char *sep = strstr(p, "&&");
        if (sep != NULL) {
            *sep = '\0';
            char *sub = p;
            p = sep + 2;
            while (*sub == ' ' || *sub == '\t') sub++;
            char *end = sub + strlen(sub);
            while (end > sub && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
            *end = '\0';
            if (*sub != '\0') {
                int res = handle_single_command(sub);
                if (res != 0) {
                    return;
                }
            }
        } else {
            while (*p == ' ' || *p == '\t') p++;
            char *end = p + strlen(p);
            while (end > p && (*(end - 1) == ' ' || *(end - 1) == '\t')) end--;
            *end = '\0';
            if (*p != '\0') {
                handle_single_command(p);
            }
            break;
        }
    }
}

#define HIST_MAX 32
static char hist_entries[HIST_MAX][CMD_MAX_LEN];
static int hist_total = 0;
static int hist_index = -1;

static void hist_add(const char *cmd) {
    if (cmd == NULL || cmd[0] == '\0') {
        return;
    }
    if (hist_total > 0 && strcmp(hist_entries[(hist_total - 1) % HIST_MAX], cmd) == 0) {
        return;
    }
    strncpy(hist_entries[hist_total % HIST_MAX], cmd, CMD_MAX_LEN - 1);
    hist_entries[hist_total % HIST_MAX][CMD_MAX_LEN - 1] = '\0';
    hist_total++;
}

void shell_init(void) {
    vga_clear();
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("dismasmOS ");
    vga_puts(SYSTEM_VERSION);
    vga_puts(" (x86_64 Long Mode 64-Bit) - Microkernel\n");
    vga_puts("Type 'help' for help.\n\n");
    print_prompt();
}

void shell_run(void) {
    char cmd_buf[CMD_MAX_LEN];
    size_t cmd_len = 0;

    while (1) {
        int c = kbd_getchar();
        if (c == '\n') {
            vga_putchar('\n');
            cmd_buf[cmd_len] = '\0';
            if (cmd_len > 0) {
                hist_add(cmd_buf);
            }
            hist_index = -1;
            handle_line(cmd_buf);
            cmd_len = 0;
            print_prompt();
        } else if (c == '\b') {
            if (cmd_len > 0) {
                cmd_len--;
                vga_backspace();
            }
        } else if (c == KEY_UP) {
            int count = (hist_total > HIST_MAX) ? HIST_MAX : hist_total;
            if (count > 0) {
                if (hist_index == -1) {
                    hist_index = hist_total - 1;
                } else if (hist_index > hist_total - count) {
                    hist_index--;
                }
                while (cmd_len > 0) {
                    vga_backspace();
                    cmd_len--;
                }
                int idx = hist_index % HIST_MAX;
                strcpy(cmd_buf, hist_entries[idx]);
                cmd_len = strlen(cmd_buf);
                vga_puts(cmd_buf);
            }
        } else if (c == KEY_DOWN) {
            if (hist_index != -1) {
                while (cmd_len > 0) {
                    vga_backspace();
                    cmd_len--;
                }
                if (hist_index < hist_total - 1) {
                    hist_index++;
                    int idx = hist_index % HIST_MAX;
                    strcpy(cmd_buf, hist_entries[idx]);
                    cmd_len = strlen(cmd_buf);
                    vga_puts(cmd_buf);
                } else {
                    hist_index = -1;
                    cmd_buf[0] = '\0';
                    cmd_len = 0;
                }
            }
        } else if ((c >= 32 && c <= 255) && c != 127) {
            if (cmd_len < CMD_MAX_LEN - 1) {
                cmd_buf[cmd_len++] = (char)c;
                vga_putchar((char)c);
            }
        }
    }
}

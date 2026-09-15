#include "shell.h"
#include "vga.h"
#include "kbd.h"
#include "string.h"
#include "fs.h"
#include "io.h"
#include "proc.h"
#include "em.h"

#define CMD_MAX_LEN 128
#define MAX_ARGS 16

extern uint8_t _kernel_start, _text_end, _rodata_end, _data_end, _kernel_end;

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

    print_hex(k_end);
    vga_puts("-");
    print_hex(0x3FFFFFFF);
    vga_puts("  ");
    print_num_kib(0x40000000 - k_end, 11);
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

    vga_puts("Kernel Footprint: ");
    uint64_t k_kib = (k_end - k_start + 1023) / 1024;
    itoa((int)k_kib, num);
    vga_puts(num);
    vga_puts(" KiB\n");

    uint64_t total_used_kib = k_kib + proc_total_mem_kib() + ((fs_used_bytes() + 1023) / 1024) + 32;
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
        vga_puts("- unknown -'");
        vga_puts(argv[1]);
        vga_puts("'. use 'de' or 'en'\n");
    }
}

static void cmd_help(void) {
    vga_puts("Available commands (13):\n");
    vga_puts("  help    - get help\n");
    vga_puts("  clear   - clearence\n");
    vga_puts("  echo    - print or redirect into file\n");
    vga_puts("  ls      - list files\n");
    vga_puts("  cat     - display file content\n");
    vga_puts("  grep    - search pattern in file\n");
    vga_puts("  touch   - empty file creation\n");
    vga_puts("  em      - vi-style text editor\n");
    vga_puts("  pr      - process management (start, kill, -c (comment), status)\n");
    vga_puts("  uname   - get system info\n");
    vga_puts("  MemRep  - memory allocation report in KiB\n");
    vga_puts("  lang    - switch keyboard layout\n");
    vga_puts("  reboot  - reboot machine\n");
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

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("usage: cat <file>\n");
        return;
    }
    struct fs_file *file = fs_find(argv[1]);
    if (!file) {
        vga_puts("cat: file not found: ");
        vga_puts(argv[1]);
        vga_putchar('\n');
        return;
    }
    vga_puts(file->data);
    if (file->size > 0 && file->data[file->size - 1] != '\n') {
        vga_putchar('\n');
    }
}

static void cmd_grep(int argc, char **argv) {
    if (argc < 3) {
        vga_puts("usage: grep <pattern> <file>\n");
        return;
    }
    struct fs_file *file = fs_find(argv[2]);
    if (!file) {
        vga_puts("grep: file not found: ");
        vga_puts(argv[2]);
        vga_putchar('\n');
        return;
    }

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
}

static void cmd_touch(int argc, char **argv) {
    if (argc < 2) {
        vga_puts("usage: touch <file>\n");
        return;
    }
    if (fs_create(argv[1]) != 0) {
        if (!fs_find(argv[1])) {
            vga_puts("touch: cannot create file\n");
        }
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
    em_run(argv[1]);
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

static void execute_command(int argc, char **argv) {
    if (argc == 0) {
        return;
    }
    if (strcmp(argv[0], "help") == 0) {
        cmd_help();
    } else if (strcmp(argv[0], "clear") == 0) {
        vga_clear();
    } else if (strcmp(argv[0], "echo") == 0) {
        cmd_echo(argc, argv);
    } else if (strcmp(argv[0], "ls") == 0) {
        fs_list();
    } else if (strcmp(argv[0], "cat") == 0) {
        cmd_cat(argc, argv);
    } else if (strcmp(argv[0], "grep") == 0) {
        cmd_grep(argc, argv);
    } else if (strcmp(argv[0], "touch") == 0) {
        cmd_touch(argc, argv);
    } else if (strcmp(argv[0], "uname") == 0) {
        cmd_uname();
    } else if (strcmp(argv[0], "MemRep") == 0 || strcmp(argv[0], "memrep") == 0) {
        cmd_memrep();
    } else if (strcmp(argv[0], "lang") == 0) {
        cmd_lang(argc, argv);
    } else if (strcmp(argv[0], "pr") == 0) {
        cmd_pr(argc, argv);
    } else if (strcmp(argv[0], "em") == 0) {
        cmd_em(argc, argv);
    } else if (strcmp(argv[0], "reboot") == 0) {
        sys_reboot();
    } else {
        vga_puts("unknown command: ");
        vga_puts(argv[0]);
        vga_puts(" (type 'help' for list)\n");
    }
}

static void print_prompt(void) {
    vga_set_color(VGA_COLOR_RED, VGA_COLOR_BLACK);
    vga_puts("root@admin@dismasmOS");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts(" | >>> ");
}

static void handle_line(char *cmd_buf) {
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
            return;
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

            if (!fs_find(target)) {
                if (fs_create(target) != 0) {
                    vga_puts("error: cannot create file: ");
                    vga_puts(target);
                    vga_putchar('\n');
                    return;
                }
            }
            fs_write(target, text, strlen(text));
            return;
        }
    }

    char *argv[MAX_ARGS];
    int argc = tokenize(cmd_buf, argv, MAX_ARGS);
    execute_command(argc, argv);
}

void shell_init(void) {
    vga_clear();
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("dismasmOS 1.2 (x86_64 Long Mode 64-Bit) - Microkernel\n");
    vga_puts("Type 'help' for help.\n\n");
    print_prompt();
}

void shell_run(void) {
    char cmd_buf[CMD_MAX_LEN];
    size_t cmd_len = 0;

    while (1) {
        char c = kbd_getchar();
        if (c == '\n') {
            vga_putchar('\n');
            cmd_buf[cmd_len] = '\0';
            handle_line(cmd_buf);
            cmd_len = 0;
            print_prompt();
        } else if (c == '\b') {
            if (cmd_len > 0) {
                cmd_len--;
                vga_backspace();
            }
        } else if ((uint8_t)c >= 32 && (uint8_t)c != 127) {
            if (cmd_len < CMD_MAX_LEN - 1) {
                cmd_buf[cmd_len++] = c;
                vga_putchar(c);
            }
        }
    }
}

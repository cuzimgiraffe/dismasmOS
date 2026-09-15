#include "proc.h"
#include "string.h"
#include "vga.h"

static struct process procs[MAX_PROCS];
static int next_pid = 2;

static void print_pad(const char *s, int width) {
    int len = (int)strlen(s);
    vga_puts(s);
    while (len < width) {
        vga_putchar(' ');
        len++;
    }
}

void proc_init(void) {
    memset(procs, 0, sizeof(procs));

    procs[0].pid = 0;
    strncpy(procs[0].name, "system", PROC_NAME_MAX - 1);
    strncpy(procs[0].agent, "service.prf", PROC_AGENT_MAX - 1);
    procs[0].state = PROC_RUNNING;
    procs[0].mem_kib = 16;
    procs[0].mem_bytes = 16384;
    strncpy(procs[0].comment, "core system kernel", PROC_COMMENT_MAX - 1);
    procs[0].used = 1;

    procs[1].pid = 1;
    strncpy(procs[1].name, "shell", PROC_NAME_MAX - 1);
    strncpy(procs[1].agent, "service.prf", PROC_AGENT_MAX - 1);
    procs[1].state = PROC_RUNNING;
    procs[1].mem_kib = 8;
    procs[1].mem_bytes = 8192;
    strncpy(procs[1].comment, "command line shell", PROC_COMMENT_MAX - 1);
    procs[1].used = 1;
}

int proc_start(const char *name) {
    for (size_t i = 0; i < MAX_PROCS; i++) {
        if (procs[i].used && strcmp(procs[i].name, name) == 0) {
            if (procs[i].state == PROC_RUNNING) {
                vga_puts("[service.prf] process '");
                vga_puts(name);
                vga_puts("' is already running\n");
                return 0;
            } else {
                procs[i].state = PROC_RUNNING;
                vga_puts("[service.prf] started process '");
                vga_puts(name);
                vga_puts("'\n");
                return 0;
            }
        }
    }

    for (size_t i = 0; i < MAX_PROCS; i++) {
        if (!procs[i].used) {
            procs[i].used = 1;
            procs[i].pid = next_pid++;
            strncpy(procs[i].name, name, PROC_NAME_MAX - 1);
            procs[i].name[PROC_NAME_MAX - 1] = '\0';
            strncpy(procs[i].agent, "service.prf", PROC_AGENT_MAX - 1);
            procs[i].agent[PROC_AGENT_MAX - 1] = '\0';
            procs[i].state = PROC_RUNNING;
            procs[i].mem_kib = 4;
            procs[i].mem_bytes = 4096;
            strncpy(procs[i].comment, "user service worker", PROC_COMMENT_MAX - 1);
            procs[i].comment[PROC_COMMENT_MAX - 1] = '\0';

            char num_buf[16];
            char hex_buf[16];
            vga_puts("[service.prf] started process '");
            vga_puts(name);
            vga_puts("' (PID ");
            itoa(procs[i].pid, num_buf);
            vga_puts(num_buf);
            vga_puts(", ");
            itoa((int)procs[i].mem_kib, num_buf);
            vga_puts(num_buf);
            vga_puts(" KiB / 0x");
            utoa_hex(procs[i].mem_bytes, hex_buf);
            vga_puts(hex_buf);
            vga_puts(")\n");
            return 0;
        }
    }

    vga_puts("error: process table full\n");
    return -1;
}

int proc_kill(const char *name) {
    if (strcmp(name, "system") == 0) {
        vga_puts("error: cannot kill system process\n");
        return -1;
    }

    for (size_t i = 0; i < MAX_PROCS; i++) {
        if (procs[i].used && strcmp(procs[i].name, name) == 0) {
            procs[i].state = PROC_STOPPED;
            char num_buf[16];
            vga_puts("[service.prf] killed process '");
            vga_puts(name);
            vga_puts("' (PID ");
            itoa(procs[i].pid, num_buf);
            vga_puts(num_buf);
            vga_puts(")\n");
            return 0;
        }
    }

    vga_puts("process not found: ");
    vga_puts(name);
    vga_putchar('\n');
    return -1;
}

int proc_set_comment(const char *name, const char *comment) {
    for (size_t i = 0; i < MAX_PROCS; i++) {
        if (procs[i].used && strcmp(procs[i].name, name) == 0) {
            strncpy(procs[i].comment, comment, PROC_COMMENT_MAX - 1);
            procs[i].comment[PROC_COMMENT_MAX - 1] = '\0';
            vga_puts("[service.prf] updated comment for '");
            vga_puts(name);
            vga_puts("' to '");
            vga_puts(comment);
            vga_puts("'\n");
            return 0;
        }
    }

    vga_puts("process not found: ");
    vga_puts(name);
    vga_putchar('\n');
    return -1;
}

void proc_status(const char *name) {
    for (size_t i = 0; i < MAX_PROCS; i++) {
        if (procs[i].used && strcmp(procs[i].name, name) == 0) {
            char num_buf[16];
            char hex_buf[16];

            vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
            vga_puts("Process Status: ");
            vga_puts(procs[i].name);
            vga_putchar('\n');
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

            vga_puts("  Name:          ");
            vga_puts(procs[i].name);
            vga_putchar('\n');

            vga_puts("  Agent:         ");
            vga_puts(procs[i].agent);
            vga_putchar('\n');

            vga_puts("  Running state: ");
            if (procs[i].state == PROC_RUNNING) {
                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                vga_puts("RUNNING\n");
            } else {
                vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                vga_puts("STOPPED\n");
            }
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

            vga_puts("  Mem usage:     ");
            itoa((int)procs[i].mem_kib, num_buf);
            vga_puts(num_buf);
            vga_puts(" KiB (0x");
            utoa_hex(procs[i].mem_bytes, hex_buf);
            vga_puts(hex_buf);
            vga_puts(")\n");

            vga_puts("  Comment:       ");
            vga_puts(procs[i].comment);
            vga_putchar('\n');
            return;
        }
    }

    vga_puts("process not found: ");
    vga_puts(name);
    vga_putchar('\n');
}

void proc_status_all(void) {
    char num_buf[16];
    char hex_buf[16];
    char mem_kib_str[24];
    char mem_hex_str[24];

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("PID  Name            Running state  Mem (KiB)  Mem (Hex)   Comment\n");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("--------------------------------------------------------------------------------\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    for (size_t i = 0; i < MAX_PROCS; i++) {
        if (procs[i].used) {
            itoa(procs[i].pid, num_buf);
            print_pad(num_buf, 5);
            print_pad(procs[i].name, 16);

            if (procs[i].state == PROC_RUNNING) {
                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                print_pad("RUNNING", 15);
            } else {
                vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                print_pad("STOPPED", 15);
            }
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

            itoa((int)procs[i].mem_kib, num_buf);
            strcpy(mem_kib_str, num_buf);
            strcat(mem_kib_str, " KiB");
            print_pad(mem_kib_str, 11);

            strcpy(mem_hex_str, "0x");
            utoa_hex(procs[i].mem_bytes, hex_buf);
            strcat(mem_hex_str, hex_buf);
            print_pad(mem_hex_str, 12);

            vga_puts(procs[i].comment);
            vga_putchar('\n');
        }
    }
}

size_t proc_total_mem_kib(void) {
    size_t total = 0;
    for (size_t i = 0; i < MAX_PROCS; i++) {
        if (procs[i].used && procs[i].state == PROC_RUNNING) {
            total += procs[i].mem_kib;
        }
    }
    return total;
}

size_t proc_active_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < MAX_PROCS; i++) {
        if (procs[i].used && procs[i].state == PROC_RUNNING) {
            count++;
        }
    }
    return count;
}

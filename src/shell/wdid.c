#include "wdid.h"
#include "vga.h"
#include "kbd.h"
#include "string.h"
#include "io.h"

#define PAGER_TEXT_ROWS 24
#define PAGER_STATUS_ROW 24

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

static void draw_cell(size_t col, size_t row, char c, uint8_t fg, uint8_t bg) {
    if (col >= VGA_WIDTH || row >= VGA_HEIGHT) {
        return;
    }
    volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
    uint8_t attr = (uint8_t)(fg | (bg << 4));
    vga[row * VGA_WIDTH + col] = (uint16_t)(uint8_t)c | ((uint16_t)attr << 8);
}

static int is_header_line(const char *s) {
    if (s[0] >= 'A' && s[0] <= 'Z') {
        int all_caps = 1;
        for (int i = 0; s[i] != '\0' && s[i] != ':'; i++) {
            if (s[i] >= 'a' && s[i] <= 'z') {
                all_caps = 0;
                break;
            }
        }
        return all_caps;
    }
    return 0;
}

static void render_status_bar(const struct wdid_entry *entry, size_t top_line) {
    char status[80];
    status[0] = '\0';

    strcat(status, " [WDID] ");
    strcat(status, entry->name);
    strcat(status, "(1) | Line ");

    char num[16];
    itoa((int)top_line + 1, num);
    strcat(status, num);
    strcat(status, "-");

    size_t bottom = top_line + PAGER_TEXT_ROWS;
    if (bottom > entry->line_count) {
        bottom = entry->line_count;
    }
    itoa((int)bottom, num);
    strcat(status, num);
    strcat(status, "/");

    itoa((int)entry->line_count, num);
    strcat(status, num);

    int pct = 0;
    if (entry->line_count > 0) {
        pct = (int)((bottom * 100) / entry->line_count);
    }
    strcat(status, " (");
    itoa(pct, num);
    strcat(status, num);
    strcat(status, "%) | Wheel/PgUp/PgDn/Arrows | 'q' Exit");

    for (size_t c = 0; c < VGA_WIDTH; c++) {
        char ch = (c < strlen(status)) ? status[c] : ' ';
        draw_cell(c, PAGER_STATUS_ROW, ch, VGA_COLOR_WHITE, VGA_COLOR_BLUE);
    }
}

static void render_page(const struct wdid_entry *entry, size_t top_line) {
    for (size_t r = 0; r < PAGER_TEXT_ROWS; r++) {
        size_t line_idx = top_line + r;

        if (line_idx < entry->line_count) {
            const char *line = entry->lines[line_idx];
            size_t len = strlen(line);

            uint8_t fg = VGA_COLOR_WHITE;
            if (is_header_line(line)) {
                fg = VGA_COLOR_LIGHT_CYAN;
            } else if (line[0] == ' ' && line[1] == ' ' && (line[2] == '-' || (line[2] >= 'a' && line[2] <= 'z' && line[3] != ' '))) {
                fg = VGA_COLOR_LIGHT_GREEN;
            } else if (strstr(line, "NOTE:") || strstr(line, "WARNING:") || strstr(line, "IMPORTANT:")) {
                fg = VGA_COLOR_YELLOW;
            }

            for (size_t c = 0; c < VGA_WIDTH; c++) {
                char ch = (c < len) ? line[c] : ' ';
                draw_cell(c, r, ch, fg, VGA_COLOR_BLACK);
            }
        } else {
            /* Blank line beyond end of document */
            draw_cell(0, r, '~', VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
            for (size_t c = 1; c < VGA_WIDTH; c++) {
                draw_cell(c, r, ' ', VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            }
        }
    }

    render_status_bar(entry, top_line);
    vga_update_cursor(0, PAGER_STATUS_ROW);
}

static void run_pager(const struct wdid_entry *entry) {
    size_t top_line = 0;
    size_t max_top = 0;
    if (entry->line_count > PAGER_TEXT_ROWS) {
        max_top = entry->line_count - PAGER_TEXT_ROWS;
    }

    render_page(entry, top_line);

    while (1) {
        int c = kbd_getchar();

        if (c == 'q' || c == 'Q' || c == 27) {
            break;
        } else if (c == KEY_SCROLL_UP) {
            /* Mouse scroll wheel UP: scroll 3 lines */
            if (top_line >= 3) {
                top_line -= 3;
            } else {
                top_line = 0;
            }
            render_page(entry, top_line);
        } else if (c == KEY_SCROLL_DOWN) {
            /* Mouse scroll wheel DOWN: scroll 3 lines */
            top_line += 3;
            if (top_line > max_top) {
                top_line = max_top;
            }
            render_page(entry, top_line);
        } else if (c == KEY_UP || c == 'k' || c == 'K') {
            /* Line scroll UP */
            if (top_line > 0) {
                top_line--;
                render_page(entry, top_line);
            }
        } else if (c == KEY_DOWN || c == 'j' || c == 'J') {
            /* Line scroll DOWN */
            if (top_line < max_top) {
                top_line++;
                render_page(entry, top_line);
            }
        } else if (c == KEY_PGUP || c == 'b' || c == 'B') {
            /* Page scroll UP: 20 lines */
            if (top_line >= 20) {
                top_line -= 20;
            } else {
                top_line = 0;
            }
            render_page(entry, top_line);
        } else if (c == KEY_PGDN || c == ' ') {
            /* Page scroll DOWN: 20 lines */
            top_line += 20;
            if (top_line > max_top) {
                top_line = max_top;
            }
            render_page(entry, top_line);
        } else if (c == KEY_HOME || c == 'g') {
            /* Jump to beginning */
            top_line = 0;
            render_page(entry, top_line);
        } else if (c == KEY_END || c == 'G') {
            /* Jump to end */
            top_line = max_top;
            render_page(entry, top_line);
        }
    }

    vga_clear();
}

const struct wdid_entry *wdid_find(const char *name) {
    if (!name || name[0] == '\0') {
        return NULL;
    }

    for (size_t i = 0; i < wdid_entry_count; i++) {
        if (strcmp(wdid_entries[i]->name, name) == 0) {
            return wdid_entries[i];
        }
        if (wdid_entries[i]->alias != NULL && strcmp(wdid_entries[i]->alias, name) == 0) {
            return wdid_entries[i];
        }
    }
    return NULL;
}

int cmd_wdid(int argc, char **argv) {
    const char *target = "wdid";
    if (argc >= 2) {
        target = argv[1];
    }

    const struct wdid_entry *entry = wdid_find(target);
    if (entry == NULL) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts("wdid: no manual entry found for '");
        vga_puts(target);
        vga_puts("'\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts("Type 'wdid' without arguments to view the complete command index.\n");
        sound_error();
        return 1;
    }

    run_pager(entry);
    return 0;
}

#include "vga.h"
#include "io.h"
#include "string.h"


static volatile uint16_t *vga_buffer = (volatile uint16_t *)0xB8000;
static size_t vga_row = 0;
static size_t vga_col = 0;
static uint8_t vga_attrib = 0x07;

void vga_update_cursor(int x, int y) {
    uint16_t pos = (uint16_t)(y * VGA_WIDTH + x);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void vga_scroll(void) {
    if (vga_row >= VGA_HEIGHT) {
        for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
            for (size_t x = 0; x < VGA_WIDTH; x++) {
                vga_buffer[y * VGA_WIDTH + x] = vga_buffer[(y + 1) * VGA_WIDTH + x];
            }
        }
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = (uint16_t)' ' | ((uint16_t)vga_attrib << 8);
        }
        vga_row = VGA_HEIGHT - 1;
    }
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    vga_attrib = (uint8_t)(fg | (bg << 4));
}

void vga_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            vga_buffer[y * VGA_WIDTH + x] = (uint16_t)' ' | ((uint16_t)vga_attrib << 8);
        }
    }
    vga_row = 0;
    vga_col = 0;
    vga_update_cursor(0, 0);
}

void vga_init(void) {
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_clear();
}

void vga_backspace(void) {
    if (vga_col > 0) {
        vga_col--;
        vga_buffer[vga_row * VGA_WIDTH + vga_col] = (uint16_t)' ' | ((uint16_t)vga_attrib << 8);
        vga_update_cursor(vga_col, vga_row);
    }
}

void vga_putchar(char c) {
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c == '\b') {
        vga_backspace();
        return;
    } else if (c == '\t') {
        vga_col = (vga_col + 4) & ~3;
    } else {
        vga_buffer[vga_row * VGA_WIDTH + vga_col] = (uint16_t)(uint8_t)c | ((uint16_t)vga_attrib << 8);
        vga_col++;
    }

    if (vga_col >= VGA_WIDTH) {
        vga_col = 0;
        vga_row++;
    }

    vga_scroll();
    vga_update_cursor(vga_col, vga_row);
}

void vga_write(const char *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        vga_putchar(data[i]);
    }
}

void vga_puts(const char *str) {
    while (*str) {
        vga_putchar(*str++);
    }
}

uint8_t vga_get_current_color(void) {
    return vga_attrib & 0x0F;
}

void vga_print_1bit_spot(uint8_t color) {
    uint8_t old_attrib = vga_attrib;
    vga_set_color(color, VGA_COLOR_BLACK);
    vga_putchar((char)0xDB);
    vga_attrib = old_attrib;
}

static const char *color_names[16] = {
    "black",
    "blue",
    "green",
    "cyan",
    "red",
    "magenta",
    "brown",
    "light_grey",
    "dark_grey",
    "light_blue",
    "light_green",
    "light_cyan",
    "light_red",
    "light_magenta",
    "yellow",
    "white"
};

const char *vga_get_color_name(uint8_t color) {
    if (color < 16) {
        return color_names[color];
    }
    return "unknown";
}

int vga_parse_color(const char *str) {
    if (str == NULL) {
        return -1;
    }

    char clean[32];
    size_t len = 0;
    while (*str && len < sizeof(clean) - 1) {
        char c = *str++;
        if (c != '(' && c != ')' && c != '[' && c != ']' && c != '<' && c != '>' && c != ' ' && c != '\t') {
            clean[len++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
    }
    clean[len] = '\0';
    if (len == 0) {
        return -1;
    }

    if (clean[0] >= '0' && clean[0] <= '9') {
        if (clean[1] == '\0') {
            return clean[0] - '0';
        } else if (clean[0] == '1' && clean[1] >= '0' && clean[1] <= '5' && clean[2] == '\0') {
            return 10 + (clean[1] - '0');
        }
    }

    if (strcmp(clean, "black") == 0 || strcmp(clean, "schwarz") == 0) return VGA_COLOR_BLACK;

    if (strcmp(clean, "blue") == 0 || strcmp(clean, "blau") == 0) return VGA_COLOR_BLUE;
    if (strcmp(clean, "green") == 0 || strcmp(clean, "gruen") == 0 || strcmp(clean, "grün") == 0) return VGA_COLOR_GREEN;
    if (strcmp(clean, "cyan") == 0 || strcmp(clean, "tuerkis") == 0 || strcmp(clean, "türkis") == 0) return VGA_COLOR_CYAN;
    if (strcmp(clean, "red") == 0 || strcmp(clean, "rot") == 0) return VGA_COLOR_RED;
    if (strcmp(clean, "magenta") == 0 || strcmp(clean, "lila") == 0 || strcmp(clean, "violett") == 0 || strcmp(clean, "purple") == 0) return VGA_COLOR_MAGENTA;
    if (strcmp(clean, "brown") == 0 || strcmp(clean, "braun") == 0) return VGA_COLOR_BROWN;
    if (strcmp(clean, "light_grey") == 0 || strcmp(clean, "lightgrey") == 0 || strcmp(clean, "grey") == 0 ||
        strcmp(clean, "grau") == 0 || strcmp(clean, "hellgrau") == 0 || strcmp(clean, "light_gray") == 0 ||
        strcmp(clean, "lightgray") == 0 || strcmp(clean, "gray") == 0) return VGA_COLOR_LIGHT_GREY;
    if (strcmp(clean, "dark_grey") == 0 || strcmp(clean, "darkgrey") == 0 || strcmp(clean, "dunkelgrau") == 0 ||
        strcmp(clean, "dark_gray") == 0 || strcmp(clean, "darkgray") == 0) return VGA_COLOR_DARK_GREY;
    if (strcmp(clean, "light_blue") == 0 || strcmp(clean, "lightblue") == 0 || strcmp(clean, "hellblau") == 0) return VGA_COLOR_LIGHT_BLUE;
    if (strcmp(clean, "light_green") == 0 || strcmp(clean, "lightgreen") == 0 || strcmp(clean, "hellgruen") == 0 || strcmp(clean, "hellgrün") == 0) return VGA_COLOR_LIGHT_GREEN;
    if (strcmp(clean, "light_cyan") == 0 || strcmp(clean, "lightcyan") == 0 || strcmp(clean, "helltuerkis") == 0 || strcmp(clean, "helltürkis") == 0) return VGA_COLOR_LIGHT_CYAN;
    if (strcmp(clean, "light_red") == 0 || strcmp(clean, "lightred") == 0 || strcmp(clean, "hellrot") == 0) return VGA_COLOR_LIGHT_RED;
    if (strcmp(clean, "light_magenta") == 0 || strcmp(clean, "lightmagenta") == 0 || strcmp(clean, "rosa") == 0 ||
        strcmp(clean, "pink") == 0 || strcmp(clean, "hellmagenta") == 0) return VGA_COLOR_LIGHT_MAGENTA;
    if (strcmp(clean, "yellow") == 0 || strcmp(clean, "gelb") == 0 || strcmp(clean, "light_brown") == 0 ||
        strcmp(clean, "lightbrown") == 0 || strcmp(clean, "hellbraun") == 0) return VGA_COLOR_YELLOW;
    if (strcmp(clean, "white") == 0 || strcmp(clean, "weiss") == 0 || strcmp(clean, "weiß") == 0) return VGA_COLOR_WHITE;

    return -1;
}


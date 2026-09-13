#include "vga.h"
#include "io.h"

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

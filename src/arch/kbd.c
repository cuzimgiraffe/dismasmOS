#include "kbd.h"
#include "io.h"
#include "pic.h"

#define KBD_BUF_SIZE 256

static volatile char kbd_buffer[KBD_BUF_SIZE];
static volatile size_t kbd_head = 0;
static volatile size_t kbd_tail = 0;
static int shift_pressed = 0;
static int altgr_pressed = 0;
static int extended = 0;
static int current_layout = KBD_LAYOUT_DE;

static const char kbd_map_de_normal[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', (char)0xE1, '`', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'z', 'u', 'i', 'o', 'p', (char)0x81, '+', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', (char)0x94, (char)0x84, '^',
    0, '#', 'y', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, '<'
};

static const char kbd_map_de_shifted[128] = {
    0, 27, '!', '"', (char)0x15, '$', '%', '&', '/', '(', ')', '=', '?', '`', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Z', 'U', 'I', 'O', 'P', (char)0x9A, '*', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', (char)0x99, (char)0x8E, (char)0xF8,
    0, '\'', 'Y', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, '>'
};

static const char kbd_map_en_normal[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' '
};

static const char kbd_map_en_shifted[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' '
};

void kbd_set_layout(int layout) {
    if (layout == KBD_LAYOUT_DE || layout == KBD_LAYOUT_EN) {
        current_layout = layout;
    }
}

int kbd_get_layout(void) {
    return current_layout;
}

void kbd_init(void) {
    kbd_head = 0;
    kbd_tail = 0;
    shift_pressed = 0;
    altgr_pressed = 0;
    extended = 0;
    current_layout = KBD_LAYOUT_DE;
    pic_unmask_irq(1);
}

void kbd_handler(void) {
    uint8_t scancode = inb(0x60);
    pic_send_eoi(1);

    if (scancode == 0xE0) {
        extended = 1;
        return;
    }

    if (extended) {
        extended = 0;
        if (scancode == 0x38) {
            altgr_pressed = 1;
            return;
        }
        if (scancode == 0xB8) {
            altgr_pressed = 0;
            return;
        }
    }

    if (scancode == 0x2A || scancode == 0x36) {
        shift_pressed = 1;
        return;
    }
    if (scancode == 0xAA || scancode == 0xB6) {
        shift_pressed = 0;
        return;
    }

    if (scancode & 0x80) {
        return;
    }

    char c = 0;

    if (current_layout == KBD_LAYOUT_DE) {
        if (altgr_pressed) {
            if (scancode == 0x10) c = '@';
            else if (scancode == 0x08) c = '{';
            else if (scancode == 0x09) c = '[';
            else if (scancode == 0x0A) c = ']';
            else if (scancode == 0x0B) c = '}';
            else if (scancode == 0x0C) c = '\\';
            else if (scancode == 0x56) c = '|';
            else if (scancode == 0x1B) c = '~';
        } else if (scancode < sizeof(kbd_map_de_normal)) {
            if (shift_pressed) {
                c = kbd_map_de_shifted[scancode];
            } else {
                c = kbd_map_de_normal[scancode];
            }
        }
    } else {
        if (scancode < sizeof(kbd_map_en_normal)) {
            if (shift_pressed) {
                c = kbd_map_en_shifted[scancode];
            } else {
                c = kbd_map_en_normal[scancode];
            }
        }
    }

    if (c) {
        size_t next = (kbd_head + 1) % KBD_BUF_SIZE;
        if (next != kbd_tail) {
            kbd_buffer[kbd_head] = c;
            kbd_head = next;
        }
    }
}

char kbd_getchar(void) {
    while (kbd_head == kbd_tail) {
        __asm__ volatile("sti; hlt");
    }
    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return c;
}

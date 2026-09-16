#include "kbd.h"
#include "io.h"
#include "pic.h"

#define KBD_BUF_SIZE 256

static volatile int kbd_buffer[KBD_BUF_SIZE];
static volatile size_t kbd_head = 0;
static volatile size_t kbd_tail = 0;
static int shift_pressed = 0;
static int altgr_pressed = 0;
static int extended = 0;
static int current_layout = KBD_LAYOUT_DE;

/* German QWERTZ layout (Set 1 scancodes) using designated initializers */
static const int kbd_map_de_normal[128] = {
    [0x01] = 27,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = (char)0xE1, [0x0D] = '`',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'z', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = (char)0x81, [0x1B] = '+',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = (char)0x94, [0x28] = (char)0x84,
    [0x29] = '^',
    [0x2B] = '#',
    [0x2C] = 'y', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '-',
    [0x37] = '*',
    [0x39] = ' ',
    [0x4A] = '-',
    [0x4E] = '+',
    [0x56] = '<',
};

static const int kbd_map_de_shifted[128] = {
    [0x01] = 27,
    [0x02] = '!', [0x03] = '"', [0x04] = (char)0x15, [0x05] = '$',
    [0x06] = '%', [0x07] = '&', [0x08] = '/', [0x09] = '(',
    [0x0A] = ')', [0x0B] = '=', [0x0C] = '?', [0x0D] = '`',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Z', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = (char)0x9A, [0x1B] = '*',
    [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = (char)0x99, [0x28] = (char)0x8E,
    [0x29] = (char)0xF8,
    [0x2B] = '\'',
    [0x2C] = 'Y', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
    [0x33] = ';', [0x34] = ':', [0x35] = '_',
    [0x37] = '*',
    [0x39] = ' ',
    [0x4A] = '-',
    [0x4E] = '+',
    [0x56] = '>',
};

/* US QWERTY layout */
static const int kbd_map_en_normal[128] = {
    [0x01] = 27,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = '-', [0x0D] = '=',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = '[', [0x1B] = ']',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x37] = '*',
    [0x39] = ' ',
    [0x4A] = '-',
    [0x4E] = '+',
};

static const int kbd_map_en_shifted[128] = {
    [0x01] = 27,
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
    [0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
    [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
    [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
    [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}',
    [0x1C] = '\n',
    [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
    [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
    [0x33] = '<', [0x34] = '>', [0x35] = '?',
    [0x37] = '*',
    [0x39] = ' ',
    [0x4A] = '-',
    [0x4E] = '+',
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
        if (scancode & 0x80) {
            return;
        }

        int key = 0;
        if (scancode == 0x48) key = KEY_UP;
        else if (scancode == 0x50) key = KEY_DOWN;
        else if (scancode == 0x4B) key = KEY_LEFT;
        else if (scancode == 0x4D) key = KEY_RIGHT;
        else if (scancode == 0x47) key = KEY_HOME;
        else if (scancode == 0x4F) key = KEY_END;
        else if (scancode == 0x49) key = KEY_PGUP;
        else if (scancode == 0x51) key = KEY_PGDN;
        else if (scancode == 0x52) key = KEY_INSERT;
        else if (scancode == 0x53) key = KEY_DELETE;

        if (key) {
            size_t next = (kbd_head + 1) % KBD_BUF_SIZE;
            if (next != kbd_tail) {
                kbd_buffer[kbd_head] = key;
                kbd_head = next;
            }
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

    /* Left Alt press / release */
    if (scancode == 0x38) {
        size_t next = (kbd_head + 1) % KBD_BUF_SIZE;
        if (next != kbd_tail) {
            kbd_buffer[kbd_head] = KEY_ALT;
            kbd_head = next;
        }
        return;
    }
    if (scancode == 0xB8) {
        return;
    }

    /* Key release */
    if (scancode & 0x80) {
        return;
    }

    int c = 0;

    /* Check numpad navigation keys when not shifted */
    if (scancode == 0x48) c = KEY_UP;
    else if (scancode == 0x50) c = KEY_DOWN;
    else if (scancode == 0x4B) c = KEY_LEFT;
    else if (scancode == 0x4D) c = KEY_RIGHT;
    else if (scancode == 0x47) c = KEY_HOME;
    else if (scancode == 0x4F) c = KEY_END;
    else if (scancode == 0x53) c = KEY_DELETE;
    else if (current_layout == KBD_LAYOUT_DE) {
        if (altgr_pressed) {
            if (scancode == 0x10) c = '@';
            else if (scancode == 0x03) c = (char)0xFD; /* ² */
            else if (scancode == 0x04) c = (char)0xFC; /* ³ */
            else if (scancode == 0x08) c = '{';
            else if (scancode == 0x09) c = '[';
            else if (scancode == 0x0A) c = ']';
            else if (scancode == 0x0B) c = '}';
            else if (scancode == 0x0C) c = '\\';
            else if (scancode == 0x12) c = 'e';
            else if (scancode == 0x1B) c = '~';
            else if (scancode == 0x32) c = (char)0xE6; /* µ */
            else if (scancode == 0x56) c = '|';
        } else if (scancode < 128) {
            if (shift_pressed) {
                c = kbd_map_de_shifted[scancode];
            } else {
                c = kbd_map_de_normal[scancode];
            }
        }
    } else {
        if (scancode < 128) {
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

int kbd_getchar(void) {
    while (kbd_head == kbd_tail) {
        __asm__ volatile("sti; hlt");
    }
    int c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return c;
}

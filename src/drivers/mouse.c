#include "mouse.h"
#include "io.h"
#include "pic.h"
#include "kbd.h"

#define MOUSE_PORT_DATA    0x60
#define MOUSE_PORT_STATUS  0x64
#define MOUSE_PORT_CMD     0x64

#define MOUSE_STATUS_OUTPUT_BUFFER_FULL 0x01
#define MOUSE_STATUS_INPUT_BUFFER_FULL  0x02
#define MOUSE_STATUS_MOUSE_DATA         0x20

static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[4];
static int has_wheel = 0;

static int mouse_wait_write(void) {
    uint32_t timeout = 100000;
    while ((inb(MOUSE_PORT_STATUS) & MOUSE_STATUS_INPUT_BUFFER_FULL) && --timeout) {
        io_wait();
    }
    return (timeout > 0) ? 0 : -1;
}

static int mouse_wait_read(void) {
    uint32_t timeout = 100000;
    while (!(inb(MOUSE_PORT_STATUS) & MOUSE_STATUS_OUTPUT_BUFFER_FULL) && --timeout) {
        io_wait();
    }
    return (timeout > 0) ? 0 : -1;
}

static void mouse_write(uint8_t val) {
    mouse_wait_write();
    outb(MOUSE_PORT_CMD, 0xD4);
    mouse_wait_write();
    outb(MOUSE_PORT_DATA, val);
}

static uint8_t mouse_read(void) {
    if (mouse_wait_read() != 0) {
        return 0;
    }
    return inb(MOUSE_PORT_DATA);
}

static void mouse_set_sample_rate(uint8_t rate) {
    mouse_write(0xF3);
    mouse_read();
    mouse_write(rate);
    mouse_read();
}

int mouse_has_wheel(void) {
    return has_wheel;
}

void mouse_init(void) {
    mouse_cycle = 0;
    has_wheel = 0;

    /* Drain any stale bytes from controller buffer */
    for (int i = 0; i < 16; i++) {
        if (inb(MOUSE_PORT_STATUS) & MOUSE_STATUS_OUTPUT_BUFFER_FULL) {
            inb(MOUSE_PORT_DATA);
        } else {
            break;
        }
    }

    /* Enable auxiliary PS/2 device (mouse) */
    mouse_wait_write();
    outb(MOUSE_PORT_CMD, 0xA8);

    /* Read controller configuration byte */
    mouse_wait_write();
    outb(MOUSE_PORT_CMD, 0x20);
    uint8_t status = mouse_read();

    /* Enable mouse interrupt (bit 1) and enable mouse clock (clear bit 5) */
    status |= 0x02;
    status &= (uint8_t)~0x20;

    /* Write back configuration byte */
    mouse_wait_write();
    outb(MOUSE_PORT_CMD, 0x60);
    mouse_wait_write();
    outb(MOUSE_PORT_DATA, status);

    /* Set default mouse settings */
    mouse_write(0xF6);
    mouse_read();

    /* IntelliMouse wheel detection magic sequence: sample rates 200, 100, 80 */
    mouse_set_sample_rate(200);
    mouse_set_sample_rate(100);
    mouse_set_sample_rate(80);

    /* Query device ID */
    mouse_write(0xF2);
    mouse_read();
    uint8_t dev_id = mouse_read();

    if (dev_id == 3 || dev_id == 4) {
        has_wheel = 1;
    } else {
        has_wheel = 0;
    }

    /* Enable packet streaming */
    mouse_write(0xF4);
    mouse_read();

    /* Unmask Cascade (IRQ 2) and PS/2 Mouse (IRQ 12) */
    pic_unmask_irq(2);
    pic_unmask_irq(12);
}

void mouse_handler(void) {
    uint8_t status = inb(MOUSE_PORT_STATUS);

    if (status & MOUSE_STATUS_OUTPUT_BUFFER_FULL) {
        uint8_t data = inb(MOUSE_PORT_DATA);

        if (status & MOUSE_STATUS_MOUSE_DATA) {
            if (mouse_cycle == 0) {
                /* Bit 3 of byte 0 must be 1 in standard PS/2 packets */
                if (data & 0x08) {
                    mouse_packet[0] = data;
                    mouse_cycle = 1;
                }
            } else if (mouse_cycle == 1) {
                mouse_packet[1] = data;
                mouse_cycle = 2;
            } else if (mouse_cycle == 2) {
                mouse_packet[2] = data;
                if (has_wheel) {
                    mouse_cycle = 3;
                } else {
                    mouse_cycle = 0;
                }
            } else if (mouse_cycle == 3) {
                mouse_packet[3] = data;
                mouse_cycle = 0;

                /* Decode Z-axis (scroll wheel) */
                int8_t z = (int8_t)mouse_packet[3];
                if (z > 0) {
                    kbd_enqueue_key(KEY_SCROLL_UP);
                } else if (z < 0) {
                    kbd_enqueue_key(KEY_SCROLL_DOWN);
                }
            }
        }
    }

    pic_send_eoi(12);
}

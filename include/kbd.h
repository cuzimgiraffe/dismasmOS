#ifndef KBD_H
#define KBD_H

#include "types.h"

#define KBD_LAYOUT_DE 0
#define KBD_LAYOUT_EN 1

/* Special key codes */
#define KEY_UP       0x8001
#define KEY_DOWN     0x8002
#define KEY_LEFT     0x8003
#define KEY_RIGHT    0x8004
#define KEY_HOME     0x8005
#define KEY_END      0x8006
#define KEY_PGUP     0x8007
#define KEY_PGDN     0x8008
#define KEY_INSERT   0x8009
#define KEY_DELETE   0x800A
#define KEY_ALT      0x800B

void kbd_init(void);
int kbd_getchar(void);
void kbd_handler(void);
void kbd_set_layout(int layout);
int kbd_get_layout(void);

#endif

#ifndef KBD_H
#define KBD_H

#include "types.h"

#define KBD_LAYOUT_DE 0
#define KBD_LAYOUT_EN 1

void kbd_init(void);
char kbd_getchar(void);
void kbd_handler(void);
void kbd_set_layout(int layout);
int kbd_get_layout(void);

#endif

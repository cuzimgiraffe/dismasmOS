#include "idt.h"
#include "string.h"

extern void idt_load(struct idt_ptr *ptr);
extern void isr_kbd_entry(void);
extern void isr_default(void);

static struct idt_entry idt[256];
static struct idt_ptr idt_p;

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = (uint16_t)(base & 0xFFFF);
    idt[num].selector = sel;
    idt[num].ist = 0;
    idt[num].flags = flags;
    idt[num].base_mid = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].base_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    idt[num].zero = 0;
}

void idt_init(void) {
    idt_p.limit = (uint16_t)(sizeof(struct idt_entry) * 256 - 1);
    idt_p.base = (uint64_t)(uintptr_t)&idt;

    memset(&idt, 0, sizeof(struct idt_entry) * 256);

    for (int i = 0; i < 256; i++) {
        idt_set_gate((uint8_t)i, (uint64_t)(uintptr_t)isr_default, 0x08, 0x8E);
    }

    idt_set_gate(33, (uint64_t)(uintptr_t)isr_kbd_entry, 0x08, 0x8E);

    idt_load(&idt_p);
}

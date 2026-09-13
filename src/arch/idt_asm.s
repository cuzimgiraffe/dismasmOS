.code32
.section .text
.global idt_load
.global isr_kbd_entry
.global isr_default
.extern kbd_handler

idt_load:
    mov 4(%esp), %eax
    lidt (%eax)
    ret

isr_default:
    pushal
    cld
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    movb $0x20, %al
    outb %al, $0xA0
    outb %al, $0x20
    popal
    iret

isr_kbd_entry:
    pushal
    cld
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    call kbd_handler
    popal
    iret

.code64
.section .text
.global idt_load
.global isr_kbd_entry
.global isr_default
.extern kbd_handler

idt_load:
    lidt (%rdi)
    ret

isr_default:
    push %rax
    movb $0x20, %al
    outb %al, $0xA0
    outb %al, $0x20
    pop %rax
    iretq

isr_kbd_entry:
    push %rax
    push %rcx
    push %rdx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    cld
    call kbd_handler
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rdx
    pop %rcx
    pop %rax
    iretq

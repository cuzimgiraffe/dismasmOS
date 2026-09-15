.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

.section .bss
.align 4096
pml4_table:
    .skip 4096
pdpt_table:
    .skip 4096
pd_table:
    .skip 4096
stack_bottom:
    .skip 32768
stack_top:

.section .data
.align 8
gdt64_start:
    .quad 0x0000000000000000
    .quad 0x00209A0000000000
    .quad 0x0000920000000000
gdt64_end:

.align 4
gdt64_ptr:
    .word gdt64_end - gdt64_start - 1
    .long gdt64_start

.section .text
.code32
.global _start
.type _start, @function
_start:
    cli

    mov $pml4_table, %edi
    xor %eax, %eax
    mov $3072, %ecx
    cld
    rep stosl

    mov $pml4_table, %edi
    mov $pdpt_table, %eax
    or $0x03, %eax
    mov %eax, (%edi)

    mov $pdpt_table, %edi
    mov $pd_table, %eax
    or $0x03, %eax
    mov %eax, (%edi)

    mov $pd_table, %edi
    mov $0x83, %ebx
    mov $512, %ecx
1:
    mov %ebx, (%edi)
    add $0x200000, %ebx
    add $8, %edi
    loop 1b

    mov %cr4, %eax
    bts $5, %eax
    mov %eax, %cr4

    mov $pml4_table, %eax
    mov %eax, %cr3

    mov $0xC0000080, %ecx
    rdmsr
    bts $8, %eax
    wrmsr

    mov %cr0, %eax
    bts $31, %eax
    mov %eax, %cr0

    lgdt gdt64_ptr
    ljmp $0x08, $long_mode_start

.code64
long_mode_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    mov $stack_top, %rsp

    push $0
    popfq

    call kmain

    cli
halt_loop:
    hlt
    jmp halt_loop

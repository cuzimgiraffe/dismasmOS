ENTRY(_start)

SECTIONS
{
    . = 1M;

    _kernel_start = .;

    .text : ALIGN(4K)
    {
        KEEP(*(.multiboot))
        *(.text)
    }

    _text_end = .;

    .rodata : ALIGN(4K)
    {
        *(.rodata*)
    }

    _rodata_end = .;

    .data : ALIGN(4K)
    {
        *(.data)
    }

    _data_end = .;

    .bss : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    _kernel_end = .;
}

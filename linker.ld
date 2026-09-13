ENTRY(_start)

SECTIONS
{
    . = 1M;

    _kernel_start = .;

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }

    _text_end = .;

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata*)
    }

    _rodata_end = .;

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    _data_end = .;

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    _kernel_end = .;
}

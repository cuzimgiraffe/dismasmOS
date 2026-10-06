CC = gcc
AS = as
LD = ld

CFLAGS = -m64 -ffreestanding -mcmodel=kernel -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -O2 -Wall -Wextra -fno-pie -fno-stack-protector -fno-builtin -nostdlib -Iinclude
ASFLAGS = --64
LDFLAGS = -m elf_x86_64 -T linker.ld -nostdlib

NASM = nasm
NASMFLAGS = -f elf64

SRC_C = $(wildcard src/*.c) $(wildcard src/**/*.c)
SRC_S = $(wildcard src/**/*.s)
SRC_ASM = $(wildcard src/**/*.asm)

OBJ = src/boot/boot.o src/arch/idt_asm.o $(SRC_ASM:.asm=.o) $(filter-out src/boot/boot.o src/arch/idt_asm.o, $(SRC_C:.c=.o))

BIN = dismasmOS.bin
ISO = dismasmOS.iso

all: $(BIN)

$(BIN): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

%.o: %.asm
	$(NASM) $(NASMFLAGS) $< -o $@

iso: $(BIN)
	mkdir -p isodir/boot/grub
	cp $(BIN) isodir/boot/$(BIN)
	cp grub.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) isodir
	rm -rf isodir

clean:
	-rm -f $(OBJ) $(BIN) $(ISO)
	-rm -rf isodir

qemu: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO)

qemu-bin: $(BIN)
	qemu-system-x86_64 -kernel $(BIN)

.PHONY: all iso clean qemu qemu-bin

CC = gcc
AS = as
LD = ld

CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -fno-pie -fno-stack-protector -fno-builtin -nostdlib -Iinclude
ASFLAGS = --32
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

SRC_C = $(wildcard src/*.c) $(wildcard src/**/*.c)
SRC_S = $(wildcard src/*.s) $(wildcard src/**/*.s)

OBJ = $(SRC_C:.c=.o) $(SRC_S:.s=.o)

BIN = dismasmOS.bin
ISO = dismasmOS.iso

all: $(BIN)

$(BIN): $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.s
	$(AS) $(ASFLAGS) $< -o $@

iso: $(BIN)
	mkdir -p isodir/boot/grub
	cp $(BIN) isodir/boot/$(BIN)
	cp grub.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) isodir
	rm -rf isodir

clean:
	rm -f $(OBJ) $(BIN) $(ISO)
	rm -rf isodir

qemu: $(ISO)
	qemu-system-i386 -cdrom $(ISO)

qemu-bin: $(BIN)
	qemu-system-i386 -kernel $(BIN)

.PHONY: all iso clean qemu qemu-bin

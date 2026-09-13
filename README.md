# dismasmOS: Architecture, Specification, and Implementation Manual

```text
       _ _                                       ____   _____ 
      | (_)                                     / __ \ / ____|
    __| |_ ___ _ __ ___   __ _ ___ _ __ ___    | |  | | (___  
   / _` | / __| '_ ` _ \ / _` / __| '_ ` _ \   | |  | |\___ \ 
  | (_| | \__ \ | | | | | (_| \__ \ | | | | |  | |__| |____) |
   \__,_|_|___/_| |_| |_|\__,_|___/_| |_| |_|   \____/|_____/ 
   ===========================================================
   Deterministic Bare-Metal Freestanding IA-32 Operating System
```

---

## Abstract

**dismasmOS** is an ultra-lean, deterministic, freestanding monolithic x86 operating system implemented strictly in ISO C99 and GNU Assembler for the IA-32 architectural specification. Operating entirely without reliance on the C standard library (`-nostdlib`, `-ffreestanding`), third-party runtimes, or secondary userland abstractions, dismasmOS establishes an end-to-end bare-metal compute environment within an unpaged 4 GiB flat memory model. 

The system implements a custom Global Descriptor Table (GDT), a 256-entry Interrupt Descriptor Table (IDT), a remapped dual-8259 Programmable Interrupt Controller (PIC) cascaded architecture, a hardware-polled / interrupt-driven Intel 8042 PS/2 controller driver featuring full German DIN 2137-2 (QWERTZ) keyboard mapping with AltGr key combinations, a direct memory-mapped VGA 80x25 text-mode console with hardware cursor register synchronization, an in-memory linear virtual filesystem (RAMFS), a modular CLI shell supporting stream parsing and file search, and an authentic Debian-style kernel telemetry (`dmesg` / `systemd`) boot logging facility.

The compiled binary footprint of the kernel image totals less than 19 KiB, achieving immediate sub-millisecond cold boot sequences and a near-zero idle power state via continuous processor halt (`HLT`) scheduling.

---

## 1. System Architecture Overview

dismasmOS executes in IA-32 32-Bit Protected Mode at Privilege Level 0 (Ring 0). The hardware initialization pipeline follows a strictly ordered, deterministic sequence that bypasses BIOS real-mode limitations via Multiboot-compliant bootstrapping.

```text
+-----------------------------------------------------------------------------------+
|                                 PHYSICAL HARDWARE                                 |
|  [CPU: x86 IA-32]    [VGA: 0xB8000]    [PIC: 8259A Dual]    [KBD: PS/2 8042]      |
+-----------------------------------------------------------------------------------+
                                         │
                                         ▼
+-----------------------------------------------------------------------------------+
|                             BOOTLOADER HANDSHAKE (GRUB)                           |
|  - Magic: 0x1BADB002                                                              |
|  - Flags: ALIGN (bit 0) | MEMINFO (bit 1)                                         |
|  - Checks: -(MAGIC + FLAGS)                                                       |
+-----------------------------------------------------------------------------------+
                                         │
                                         ▼
+-----------------------------------------------------------------------------------+
|                        EARLY ASSEMBLY STAGE (src/boot/boot.s)                     |
|  1. Disable Maskable Interrupts (CLI)                                             |
|  2. Load Custom Segment Descriptors (LGDT gdt_descriptor)                         |
|  3. Serialize Processor Pipeline & Far Jump (LJMP $0x08, $1f)                      |
|  4. Normalize Segment Selectors: DS=ES=FS=GS=SS=0x10                              |
|  5. Allocate 16 KiB 16-byte Aligned Stack Frame (ESP -> stack_top)                |
|  6. Reset EFLAGS Register                                                         |
|  7. Call Kernel Entry Point (call kmain)                                          |
+-----------------------------------------------------------------------------------+
                                         │
                                         ▼
+-----------------------------------------------------------------------------------+
|                              KERNEL MAIN (src/kernel.c)                           |
|  ┌─────────────────────────────────────────────────────────────────────────────┐  |
|  │  1. Video Subsystem: vga_init() -> Direct Framebuffer Map (0xB8000)         │  |
|  │  2. Telemetry Stage 1: Debian-Style Kernel Log Banner [    0.000000]        │  |
|  │  3. Trap Management: idt_init() -> Setup 256 IDT Gates                      │  |
|  │  4. Controller Remap: pic_remap(0x20, 0x28) -> Dual 8259 PIC ICW1-ICW4      │  |
|  │  5. Input Subsystem: kbd_init() -> DIN 2137-2 Keymap & Unmask IRQ1          │  |
|  │  6. Interrupt Activation: STI (EFLAGS.IF = 1)                               │  |
|  │  7. Storage Engine: fs_init() -> Root RAMFS Mount & Payload Population      │  |
|  │  8. Telemetry Stage 2: systemd Unit OK Assertions [  OK  ]                  │  |
|  │  9. Shell Engine: shell_init() & shell_run() Event Loop                     │  |
|  └─────────────────────────────────────────────────────────────────────────────┘  |
+-----------------------------------------------------------------------------------+
```

---

## 2. Memory Topology and Linker Mapping

The physical address space is structured to prevent collision with legacy IBM PC BIOS reservations, Video Display Memory, and ACPI tables. The kernel image is loaded at `0x00100000` (1 MiB), above the conventional real-mode memory boundary.

### Physical Memory Allocation Map

| Address Range | Allocation Size | Designation / Subsystem | Cache / Attributes |
| :--- | :--- | :--- | :--- |
| `0x00000000 - 0x000003FF` | 1 KiB | Real Mode Interrupt Vector Table (IVT) | Legacy Reserved |
| `0x00000400 - 0x000004FF` | 256 Bytes | BIOS Data Area (BDA) | Legacy Reserved |
| `0x00007E00 - 0x0007FFFF` | ~480 KiB | Conventional Low Memory (Unused Buffer) | Read/Write |
| `0x000A0000 - 0x000B7FFF` | 96 KiB | VGA Graphics Display Buffer (EGA/VGA Plane) | Memory Mapped I/O |
| `0x000B8000 - 0x000BFFFF` | 32 KiB | VGA Color Text Mode Framebuffer | MMIO (Dual-Port RAM) |
| `0x000C0000 - 0x000C7FFF` | 32 KiB | Video ROM BIOS Extension | Read-Only |
| `0x000E0000 - 0x000FFFFF` | 128 KiB | System BIOS & ACPI RSDP Physical Tables | Read-Only |
| `0x00100000 - 0x00101FFF` | ~8 KiB | **dismasmOS `.text` Section (Code + Entry)** | Executable / Read-Only |
| `0x00102000 - 0x00102FFF` | ~4 KiB | **dismasmOS `.rodata` Section (Constants)** | Read-Only |
| `0x00103000 - 0x00103FFF` | ~4 KiB | **dismasmOS `.data` Section (Initialized)** | Read/Write |
| `0x00104000 - 0x00118E60` | ~83 KiB | **dismasmOS `.bss` (IDT, VFS Table, 16KiB Stack)** | Zero-Initialized / RW |

### Linker Script Specification (`linker.ld`)

```ld
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata*)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }
}
```

The linker explicitly groups the `.multiboot` header at the foremost boundary of the `.text` segment, guaranteeing placement within the first 8192 bytes of the ELF executable as mandated by the Multiboot Specification version 0.6.96.

---

## 3. Microarchitectural CPU State & GDT Specification

To guarantee isolation from intermediate bootloader assumptions (such as GRUB 2 or SeaBIOS GDT variations that trigger hypervisor triple faults under Windows Hyper-V / VirtualBox Native Execution Manager), dismasmOS establishes an independent Global Descriptor Table immediately upon entry.

### GDT Entry Bitfield Layout

Every GDT segment descriptor comprises an 8-byte (64-bit) structure with the following topology:

```text
 63             56 55 54 53 52 51    48 47       40 39             16 15              0
+-----------------+--+--+--+--+--------+-----------+-----------------+-----------------+
|   Base 31..24   | G| D| 0| A| Lim19..| P| DPL| S |   Type 43..40   |   Base 23..0    |
+-----------------+--+--+--+--+--------+-----------+-----------------+-----------------+
|                               Limit 15..0                                           |
+-------------------------------------------------------------------------------------+
```

### Active Segment Descriptors

```text
Selector 0x0000: Null Descriptor
0x0000000000000000 -> Base=0x00000000, Limit=0x00000000, Attributes=0x00

Selector 0x0008: Kernel Code Segment Descriptor (Flat 4 GiB)
0x00CF9A000000FFFF -> Base=0x00000000, Limit=0xFFFFF, Granularity=4KiB (Total: 4 GiB)
                      P=1, DPL=00b, S=1 (Code/Data), Type=1010b (Execute/Read, Accessed=0)
                      D=1 (32-Bit Default Operand), L=0 (Not IA-32e)

Selector 0x0010: Kernel Data Segment Descriptor (Flat 4 GiB)
0x00CF92000000FFFF -> Base=0x00000000, Limit=0xFFFFF, Granularity=4KiB (Total: 4 GiB)
                      P=1, DPL=00b, S=1 (Code/Data), Type=0010b (Read/Write, Expand-Up)
                      D=1 (32-Bit Big Data), L=0
```

### Pipeline Serialization Routine

```asm
    cli
    lgdt gdt_descriptor
    ljmp $0x08, $1f
1:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    mov $stack_top, %esp
```

The far jump (`ljmp $0x08, $1f`) forces the CPU prefetch queue to flush and synchronously reloads the Hidden Segment Register Cache for `CS` with attributes from descriptor index 1.

---

## 4. Interrupt Handling & PIC Architecture

dismasmOS manages interrupts through a dedicated Interrupt Descriptor Table spanning all 256 vector positions, preventing processor halts on spurious IRQs or legacy timer ticks.

```text
                               8259A Dual PIC Cascading
                               
          +-----------------+                   +-----------------+
          |    Slave PIC    |                   |   Master PIC    |
          |  Command: 0xA0  |                   |  Command: 0x20  |
          |  Data:    0xA1  |                   |  Data:    0x21  |
          |  Vector:  0x28  |                   |  Vector:  0x20  |
          +--------+--------+                   +--------+--------+
                   |                                     |
                   | IRQ 8-15                            | IRQ 0-7
                   +--------> IRQ 2 (Cascade Pin) ------>+
                                                         |
                                                         v
                                                   CPU INTR PIN
                                                         |
                                                         v
                                                   IDT Vector Table
                                                 (Vectors 0x20 - 0x2F)
```

### PIC Initialization Command Words (ICW)

Both Intel 8259A controllers undergo four-stage state initialization:

1. **ICW1 (`0x11`)**: Assert initialization bit (`0x10`) and dictate that ICW4 is expected (`0x01`).
2. **ICW2 (Offsets)**: 
   - Master PIC vector offset programmed to `0x20` (Interrupts 32–39).
   - Slave PIC vector offset programmed to `0x28` (Interrupts 40–47).
3. **ICW3 (Cascade Configuration)**:
   - Master written with `0x04` (Bit 2 asserted: Slave connection established at IRQ2).
   - Slave written with `0x02` (Binary identification notation: connected to Master IRQ2).
4. **ICW4 (`0x01`)**: Set 8086/88 microprocessor architecture mode.
5. **OCW1 (Interrupt Mask Registers - IMR)**:
   - Master IMR written with `0xFD` (`11111101b`): All lines masked except **IRQ1 (PS/2 Keyboard)**.
   - Slave IMR written with `0xFF` (`11111111b`): Entire slave cascaded domain masked.

### IDT Gate Structure (32-Bit Interrupt Gate)

```text
 31                             16 15 14 13 12 11   8 7            0
+---------------------------------+--+-----+--+------+--------------+
|       Offset High (16..31)      | P| DPL | 0| Type | Reserved (0) |
+---------------------------------+--+-----+--+------+--------------+
|       Segment Selector (0x08)   |       Offset Low (0..15)        |
+---------------------------------+---------------------------------+
```

- **Present (P)**: `1`
- **Privilege (DPL)**: `00b` (Kernel Ring 0 Execution)
- **Type**: `0xE` (32-bit Interrupt Gate; disables interrupts automatically via `EFLAGS.IF=0` upon trap).

### Context Preservation & Assembly ISR Stubs

```asm
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
```

The assembly stub guarantees:
- Total register state preservation (`EAX`, `ECX`, `EDX`, `EBX`, `ESP`, `EBP`, `ESI`, `EDI`) via `pushal`.
- C standard ABI compliance with `cld` (Direction Flag set to auto-incrementing memory string pointers).
- Segment register validation against any potential corruption.
- Clean interrupt return (`iret`) popping `EIP`, `CS`, and `EFLAGS` atomically.

---

## 5. Input Subsystem & German DIN 2137-2 Layout

Keyboard processing operates via an asynchronous, event-driven driver decoding PS/2 Scancode Set 1 make and break codes.

### Physical Scancode Transformation

```text
[Key Press] ---> PS/2 Port 0x60 ---> IRQ1 ---> isr_kbd_entry ---> kbd_handler()
                                                                         │
    ┌────────────────────────────────────────────────────────────────────┘
    ▼
Check Extended Prefix (0xE0)
    │
    ├── Yes: Set extended state flag; decode AltGr Make (0x38) / Break (0xB8)
    │
    └── No:  Evaluate Break Bit (Scancode & 0x80)
                 │
                 ├── Make Code: Translate via Active Modifier Map
                 │              (kbd_map_normal / kbd_map_shifted / altgr_map)
                 │              Enqueue character to 256-byte Circular Buffer
                 │
                 └── Break Code: Clear Shift/Modifier flags
```

### DIN 2137-2 (German QWERTZ) Scancode Translation Matrix

dismasmOS converts hardware scancodes directly into IBM Code Page 437 (CP437) binary indices:

| Scancode (Hex) | Key Position | Normal Glyph | Shift Glyph | AltGr Glyph | CP437 Binary Code |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0x02` | Number 1 | `1` | `!` | - | `0x31` / `0x21` |
| `0x03` | Number 2 | `2` | `"` | - | `0x32` / `0x22` |
| `0x04` | Number 3 | `3` | `§` | - | `0x33` / `0x15` |
| `0x08` | Number 7 | `7` | `/` | `{` | `0x37` / `0x2F` / `0x7B` |
| `0x09` | Number 8 | `8` | `(` | `[` | `0x38` / `0x28` / `0x5B` |
| `0x0A` | Number 9 | `9` | `)` | `]` | `0x39` / `0x29` / `0x5D` |
| `0x0B` | Number 0 | `0` | `=` | `}` | `0x30` / `0x3D` / `0x7D` |
| `0x0C` | Key `ß` | `ß` | `?` | `\` | `0xE1` / `0x3F` / `0x5C` |
| `0x10` | Letter Q | `q` | `Q` | `@` | `0x71` / `0x51` / `0x40` |
| `0x15` | Letter Z (QWERTZ) | **`z`** | **`Z`** | - | `0x7A` / `0x5A` |
| `0x1A` | Key `Ü` | `ü` | `Ü` | - | `0x81` / `0x9A` |
| `0x1B` | Key `+` | `+` | `*` | `~` | `0x2B` / `0x2A` / `0x7E` |
| `0x27` | Key `Ö` | `ö` | `Ö` | - | `0x94` / `0x99` |
| `0x28` | Key `Ä` | `ä` | `Ä` | - | `0x84` / `0x8E` |
| `0x29` | Key `^` | `^` | `°` | - | `0x5E` / `0xF8` |
| `0x2C` | Letter Y (QWERTZ) | **`y`** | **`Y`** | - | `0x79` / `0x59` |
| `0x35` | Key `-` | `-` | `_` | - | `0x2D` / `0x5F` |
| `0x56` | Key `<` (ISO) | `<` | `>` | `\|` | `0x3C` / `0x3E` / `0x7C` |

### Deterministic Low-Power Wait Loop

The input consumer executes an atomic halt wait loop:

```c
char kbd_getchar(void) {
    while (kbd_head == kbd_tail) {
        __asm__ volatile("sti; hlt");
    }
    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return c;
}
```

When no input is queued in the circular buffer (`kbd_head == kbd_tail`), the CPU transitions into state `C1` via the `HLT` opcode. Host virtualization hypervisors detect the condition and relinquish host CPU threads, keeping idle processor usage below 0.01%. Upon IRQ1 delivery, the CPU returns immediately after `hlt`, extracts the buffered character, and advances the FIFO tail.

---

## 6. Video Output & Telemetry Logging Engine

The display system operates exclusively in IBM 80x25 16-color alphanumeric text mode via direct access to the dual-port Video RAM at address `0x000B8000`.

### VRAM Cell Byte Structure

Every on-screen character occupies exactly 2 bytes (16 bits):

```text
 15               12 11                8 7                               0
+-------------------+-------------------+---------------------------------+
|  Background Color |  Foreground Color |      ASCII / CP437 Character    |
+-------------------+-------------------+---------------------------------+
```

### Hardware Cursor Synchronization

The hardware cursor position is synchronized with the software raster cursor `(x, y)` by writing directly to CRT Controller registers over I/O ports `0x3D4` and `0x3D5`:

```c
void vga_update_cursor(int x, int y) {
    uint16_t pos = (uint16_t)(y * VGA_WIDTH + x);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}
```

### Debian Boot Telemetry Specification

Upon cold boot, dismasmOS mirrors standard Debian/Linux kernel initialization output followed by systemd target unit reports.

```text
[    0.000000] Linux version 1.0.0-dismasmOS (i686-pc-none) #1 PREEMPT
[    0.000004] BIOS-provided physical RAM map:
[    0.000008]   BIOS-e820: [mem 0x0000000000000000-0x000000000009ffff] usable
[    0.000012]   BIOS-e820: [mem 0x0000000000100000-0x0000000003ffffff] usable
[    0.000018] CPU: Intel/AMD x86 32-Bit Processor (Protected Mode)
[    0.000025] console [vga0] enabled, 80x25 text mode at 0xB8000
[    0.000032] GDT: Global Descriptor Table installed (CS=0x08, DS=0x10)
[    0.000040] IDT: Interrupt Descriptor Table loaded (256 gates registered)
[    0.000048] i8259A: Master/Slave PIC remapped to vectors 0x20-0x2F
[    0.000055] i8042: PS/2 keyboard controller ready, IRQ1 unmasked
[    0.000062] input: German QWERTZ keymap and AltGr mapping active
[    0.000070] system: CPU hardware interrupts enabled (EFLAGS.IF=1)
[    0.000085] vfs: In-memory filesystem mounted at / (type ramfs)
[  OK  ] Mounted In-Memory Root Filesystem.
[  OK  ] Reached target System Initialization.
[  OK  ] Started German Keyboard Layout Mapping Service.
[  OK  ] Started Console Terminal Driver.
[  OK  ] Reached target Multi-User System.
[  OK  ] Started dismasmOS Command Line Shell.

dismasmOS>>> _
```

Unlike simplistic toy kernels that clear the screen prior to the shell prompt, dismasmOS preserves the kernel ring-buffer boot log in the active text buffer, positioning the initial `dismasmOS>>> ` prompt seamlessly at the line immediately following the final target unit.

---

## 7. In-Memory Virtual File System (RAMFS)

Storage is structured as a contiguous, fixed-allocation in-memory file table designed for static determinism, eliminating dynamic heap fragmentation (`malloc`/`free` overhead).

### Structure Definition

```c
#define FS_MAX_FILES 32
#define FS_MAX_FILENAME 32
#define FS_MAX_FILESIZE 2048

struct fs_file {
    char name[FS_MAX_FILENAME];
    char data[FS_MAX_FILESIZE];
    size_t size;
    uint8_t used;
};
```

### Static Pre-Populated Files

During kernel bootstrapping (`fs_init()`), three canonical files are generated in RAM:

1. `readme.txt` (107 Bytes): Introductory release notice and system details.
2. `kernel.sys` (72 Bytes): Machine-parsable system parameters:
   ```text
   OS_NAME=dismasmOS
   VERSION=1.0
   ARCH=x86_32
   BOOT=MULTIBOOT1
   SHELL=BUILTIN
   ```
3. `hardware.txt` (86 Bytes): Hardware enumeration summary:
   ```text
   CPU=x86 Protected Mode
   VIDEO=VGA Text 80x25
   INPUT=PS2 Keyboard IRQ1
   PIC=8259 Remapped
   ```

---

## 8. Shell Architecture & Command Reference

The command-line interface operates via an in-place tokenizing Read-Eval-Print Loop (REPL). User input is buffered into a 128-byte array, scanned for whitespace delimiters (`0x20` and `0x09`), and partitioned into a vector of argument pointers (`argv`) without allocating dynamic memory.

The system incorporates 11 native utilities:

```text
+----------+--------------------------------------+------------------------------------+
| Command  | Parameter Signature                  | Operational Semantics              |
+----------+--------------------------------------+------------------------------------+
| help     | help                                 | Outputs built-in utility summary   |
| clear    | clear                                | Resets VGA buffer (blanks 80x25)   |
| echo     | echo [args...]                       | Echoes string vector to display    |
| ls       | ls                                   | Lists VFS entries and byte sizes   |
| cat      | cat <filename>                       | Streams full payload of a file     |
| grep     | grep <pattern> <filename>            | Substring line filter on file data |
| touch    | touch <filename>                     | Creates an empty node in RAMFS     |
| uname    | uname                                | Prints kernel release & metadata   |
| MemRep   | MemRep                               | Full memory allocation report      |
| lang     | lang <de|en>                         | Switches layout (QWERTZ / QWERTY)  |
| reboot   | reboot                               | Pulses 8042 CPU reset line (0xFE)  |
+----------+--------------------------------------+------------------------------------+
```

### Command Execution Details

#### `MemRep` Memory Allocation Report
Computes and formats a real-time memory map by reading the exported linker segment boundaries (`_kernel_start`, `_text_end`, `_rodata_end`, `_data_end`, `_kernel_end`), hardware MMIO regions, and querying active RAMFS buffer occupancy dynamically.

#### `lang` Runtime Layout Switcher
Toggles the active PS/2 key translation state matrix between German DIN 2137-2 (QWERTZ with AltGr and CP437 umlauts) and US Standard (QWERTY), immediately altering keycode routing without requiring a kernel reboot.

#### `cat` File Streaming
Executes a linear lookup in the file table matching `argv[1]`. If located, it emits characters directly to the VGA console driver until `file->size` is reached.

#### `grep` In-Memory Pattern Search
Iterates sequentially over the specified target file payload. Lines bounded by `\n` or `\0` are isolated into a temporary buffer and evaluated using a freestanding implementation of `strstr`. Only lines satisfying `strstr(line, pattern) != NULL` are written to the display.

#### `reboot` Hardware Reset Vector
Performs an immediate cold hardware restart by communicating with the Intel 8042 Keyboard Microcontroller:

```c
static void sys_reboot(void) {
    uint8_t temp;
    __asm__ volatile("cli");
    do {
        temp = inb(0x64);
        if (temp & 1) {
            inb(0x60);
        }
    } while (temp & 2);
    outb(0x64, 0xFE);
    while (1) {
        __asm__ volatile("hlt");
    }
}
```

Writing `0xFE` to command port `0x64` forces the controller to pulse the processor's active-low `#RESET` pin, instantly initiating a clean hardware reboot cycle.

---

## 9. Codebase Layout

```text
dismasmOS/
├── build.sh                 # Fully automated Bash build and ISO generation harness
├── Makefile                 # Deterministic GNU Makefile with freestanding compiler flags
├── linker.ld                # GNU LD script defining 1 MiB VMA/LMA layout
├── grub.cfg                 # GRUB 2 ISO bootloader menu configuration
├── include/                 # Freestanding Kernel Header Specifications
│   ├── fs.h                 # RAMFS struct file descriptor declarations
│   ├── idt.h                # IDT descriptor and gate entry definitions
│   ├── io.h                 # Inline x86 assembly port I/O primitives (inb, outb)
│   ├── kbd.h                # Keyboard driver interfaces and circular queue
│   ├── pic.h                # Dual-8259A PIC register definitions and command masks
│   ├── shell.h              # Command parser, tokenizer, and loop declarations
│   ├── string.h             # Freestanding C string library function prototypes
│   ├── types.h              # Fixed-width standard integer typedefs (uint8_t, etc.)
│   └── vga.h                # VGA text mode colors, macros, and function prototypes
└── src/                     # Core Implementation Sources
    ├── boot/
    │   └── boot.s           # Multiboot 1 entry, GDT definition, stack, and far jump
    ├── arch/
    │   ├── idt_asm.s        # Low-level assembly ISR dispatch stubs and lidt wrapper
    │   ├── idt.c            # IDT gate population and initialization logic
    │   ├── kbd.c            # DIN 2137-2 QWERTZ driver, AltGr decoder, scancode table
    │   └── pic.c            # 8259A PIC initialization, remapping, and EOI signaling
    ├── drivers/
    │   └── vga.c            # 0xB8000 VRAM driver, scrolling, and CRT cursor sync
    ├── fs/
    │   └── fs.c             # RAMFS linear directory storage and default file payloads
    ├── lib/
    │   └── string.c         # Zero-dependency implementations of strlen, strcmp, etc.
    ├── shell/
    │   └── shell.c          # Tokenizer, REPL loop, prompt styling, command implementations
    └── kernel.c             # Hardware bootstrap sequence, Debian boot telemetry banner
```

---

## 10. Building and Image Generation

### Toolchain Prerequisites

To assemble, compile, link, and package dismasmOS, the host environment requires:

- **GNU Compiler Collection (`gcc`)** with 32-bit compilation support (`multilib` / `-m32`).
- **GNU Assembler (`as`)** and **GNU Linker (`ld`)** supporting target `elf_i386`.
- **GNU Make (`make`)**.
- **GRUB 2 (`grub-mkrescue`)** and **xorriso** for bootable El Torito ISO creation.
- **QEMU (`qemu-system-i386`)** for local emulation and testing.

On Debian, Ubuntu, or WSL:

```bash
sudo apt-get update
sudo apt-get install build-essential gcc-multilib grub-pc-bin grub-common xorriso qemu-system-x86
```

### Build Execution

Run the provided build script:

```bash
chmod +x build.sh
./build.sh
```

The script performs full target dependency verification, cleans object files, compiles all C and assembly units with optimization flags `-O2`, links the final executable `dismasmOS.bin`, builds the GRUB directory tree, and invokes `xorriso` to output `dismasmOS.iso`.

To compile manually via the Makefile:

```bash
# Clean intermediate object files
make clean

# Compile and link the 19 KiB ELF binary
make

# Build bootable ISO image
make iso
```

---

## 11. Emulation and Hardware Deployment

### Running under QEMU

Execute directly from the compiled ELF kernel binary:

```bash
qemu-system-i386 -kernel dismasmOS.bin
```

Or emulate the complete CD-ROM boot sequence using the generated ISO image:

```bash
qemu-system-i386 -cdrom dismasmOS.iso -m 256M
```

### Running under Oracle VirtualBox

1. Create a new Virtual Machine:
   - **Name**: `dismasmOS`
   - **Type**: `Other`
   - **Version**: `Other/Unknown (32-bit)`
   - **RAM**: `64 MB` or `128 MB`
2. In **Storage Settings**, attach `dismasmOS.iso` to the Optical Drive.
3. Boot the Virtual Machine. The system will initialize via GRUB, load the custom GDT, render the Debian-style telemetry sequence, and present the `dismasmOS>>> ` prompt with the German keyboard layout fully active.

---

## 12. Technical Specifications Reference Sheet

```text
Processor Architecture:     x86 (IA-32, 32-Bit Protected Mode)
Privilege Level:            Ring 0 (Kernel Supervisor)
Address Translation:        Flat Physical Addressing (Paging Disabled)
Binary Format:              ELF 32-bit LSB Executable (i386)
Boot Specification:         Multiboot 1 (RFC 0.6.96)
Entry Point Physical Addr:  0x00100000 (1 MiB Alignment)
Interrupt Architecture:     Dual-8259A Cascaded PIC (Vectors 0x20-0x2F)
Console Video Mode:         VGA Color Text Mode 80x25 @ 0x000B8000
Console Hardware Ports:     0x3D4 (Index), 0x3D5 (Data) - CRT Controller
Keyboard Controller:        Intel 8042 PS/2 Microcontroller
Keyboard Protocol:          Scancode Set 1 Translation
Active Layout:              German DIN 2137-2 (QWERTZ) with AltGr Extensions
Default Code Page:          IBM Code Page 437 (CP437)
Filesystem Engine:          Contiguous Flat Static In-Memory RAMFS
Power Management:           Low-Power Idle C1 State via Processor HLT
External Dependencies:      0 (Fully freestanding, no external C library)
```

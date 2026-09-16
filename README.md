# dismasmOS: Architecture, Specification, and Implementation Manual

```text
       _ _                                       ____   _____ 
      | (_)                                     / __ \ / ____|
    __| |_ ___ _ __ ___   __ _ ___ _ __ ___    | |  | | (___  
   / _` | / __| '_ ` _ \ / _` / __| '_ ` _ \   | |  | |\___ \ 
  | (_| | \__ \ | | | | | (_| \__ \ | | | | |  | |__| |____) |
   \__,_|_|___/_| |_| |_|\__,_|___/_| |_| |_|   \____/|_____/ 
   ===========================================================
   Deterministic Bare-Metal Freestanding x86_64 64-Bit Kernel
```

---

## Abstract

**dismasmOS 1.2** is an ultra-lean, deterministic, freestanding x86_64 Long Mode operating system implemented strictly in ISO C99 and GNU Assembler for the AMD64/Intel 64 architectural specification. Operating entirely without reliance on the C standard library (`-nostdlib`, `-ffreestanding`), third-party runtimes, or secondary userland abstractions, dismasmOS establishes an end-to-end bare-metal compute environment featuring:

* **64-Bit Long Mode Microkernel**: 4-level paging (PML4, PDPT, PD) with a 1 GiB identity-mapped address space using 2 MiB huge pages, custom 64-bit Global Descriptor Table (GDT64), and a 256-gate 64-bit Interrupt Descriptor Table (IDT).
* **Deterministic Input Engine**: Fully compliant German DIN 2137-2 (QWERTZ) and US (QWERTY) keyboard mapping using C99 designated initializers, complete AltGr decoding, ISO-key `< > |` support, and extended PS/2 scancode decoding (`0xE0`) for dedicated hardware Arrow Keys and Alt navigation.
* **Modern In-Memory Virtual Filesystem (RAMFS)**: Hierarchical directory tree featuring `/home` (user working directory), `/boot` (critical bootloader assets), `/krnl` (kernel core), `/shell` (userland scripts and configurations), and `/etc` (system configuration tables).
* **Comprehensive Filesystem Audit Logging (`changes`)**: Integrated kernel-level change and access telemetry tracking all `VIEW`, `EDITED`, `CREATED`, and `DELETED` file operations.
* **Full-Featured Text Editor ("em")**: Direct text writing by default, hardware arrow-key cursor navigation, Alt-key modal command execution (`;wsc`, `;s`, `;q`, `;-m`), integrated spellchecking with typo highlighting, and an interactive graphical system warning modal protecting critical files in `/boot` and `/krnl`.
* **Hardware Shutdown & Power Management**: Active inline assembly CPU shutdown (`cli; hlt`) coupled with automated ACPI power-off sequences for QEMU, Bochs, and VirtualBox.
* **Strict Command Suite**: Full deprecation and removal of `ls` in favor of **`lsf`** (featuring `-s` for KiB/Bytes and Hex output and `-p` for permission/rights analysis), alongside `cd`, `makedir`, `rm`, `pr`, `MemRep`, and stream redirection.

---

## 1. System Architecture & Boot Sequence

dismasmOS initializes from a Multiboot 1 compliant bootloader (GRUB 2) and transitions deterministically from 32-bit protected mode into 64-bit Long Mode at Privilege Level 0 (Ring 0).

```text
+-----------------------------------------------------------------------------------+
|                                 PHYSICAL HARDWARE                                 |
|  [CPU: x86_64 Long Mode] [VGA: 0xB8000] [PIC: Dual 8259A] [KBD: PS/2 8042]       |
+-----------------------------------------------------------------------------------+
                                         │
                                         ▼
+-----------------------------------------------------------------------------------+
|                             BOOTLOADER HANDSHAKE (GRUB)                           |
|  - Multiboot 1 Header Magic: 0x1BADB002                                           |
|  - Flags: ALIGN (bit 0) | MEMINFO (bit 1)                                         |
+-----------------------------------------------------------------------------------+
                                         │
                                         ▼
+-----------------------------------------------------------------------------------+
|                     EARLY ASSEMBLY BOOTSTRAP (src/boot/boot.s)                    |
|  1. Clear EFLAGS & Disable Interrupts (CLI)                                       |
|  2. Zero Page Tables (PML4, PDPT, PD) in .bss section                             |
|  3. Build 4-Level Paging: PML4[0] -> PDPT, PDPT[0] -> PD                         |
|  4. Identity-map 1 GiB using 512 x 2 MiB Large Page Entries (0x83 Flag)           |
|  5. Enable Physical Address Extension (CR4.PAE = 1)                              |
|  6. Enable Long Mode in EFER MSR (MSR 0xC0000080, Bit 8 LME = 1)                  |
|  7. Activate Paging and Protected Subsystems (CR0.PG = 1, CR0.PE = 1)             |
|  8. Load 64-Bit GDT (LGDT gdt64_ptr)                                              |
|  9. Far Jump to 64-Bit Long Mode Code Segment (LJMP $0x08, $long_mode_start)      |
| 10. Setup 32 KiB 16-byte Aligned 64-Bit Stack (RSP -> stack_top)                  |
| 11. Call Kernel Entry Point: call kmain                                           |
+-----------------------------------------------------------------------------------+
                                         │
                                         ▼
+-----------------------------------------------------------------------------------+
|                              KERNEL MAIN (src/kernel.c)                           |
|  ┌─────────────────────────────────────────────────────────────────────────────┐  |
|  │  1. Video Subsystem: vga_init() -> Direct Framebuffer Map (0xB8000)         │  |
|  │  2. Telemetry Stage 1: Debian-Style Kernel Log Banner [    0.000000]        │  |
|  │  3. Trap Management: idt_init() -> Setup 256 64-Bit IDT Gates               │  |
|  │  4. Controller Remap: pic_remap(0x20, 0x28) -> Dual 8259 PIC ICW1-ICW4      │  |
|  │  5. Input Subsystem: kbd_init() -> DIN 2137-2 Keymap & Unmask IRQ1          │  |
|  │  6. Interrupt Activation: STI (RFLAGS.IF = 1)                               │  |
|  │  7. Storage Engine: fs_init() -> Mount /home, /boot, /krnl, /shell, /etc    │  |
|  │  8. Process Subsystem: proc_init() -> Initialize process table & service.prf│  |
|  │  9. Telemetry Stage 2: systemd Unit OK Assertions [  OK  ]                  │  |
|  │ 10. Shell Engine: shell_init() & shell_run() Event Loop                     │  |
|  └─────────────────────────────────────────────────────────────────────────────┘  |
+-----------------------------------------------------------------------------------+
```

### Centralized System Version & Build Configuration

To enable rapid and effortless version increments without having to search across the codebase, both `SYSTEM_VERSION` and `SYSTEM_BUILD` are declared as clean global variables directly at the very top of [`src/shell/shell.c`](file:///C:/Users/spemg/OneDrive/Desktop/dismasmOS/src/shell/shell.c#L1-L2):

```c
const char *SYSTEM_VERSION = "1.2";
const char *SYSTEM_BUILD   = "2026";
```

These variables are exported via [`include/shell.h`](file:///C:/Users/spemg/OneDrive/Desktop/dismasmOS/include/shell.h) (`extern const char *SYSTEM_VERSION;`) and consumed across kernel telemetry, shell startup banners, shutdown status screens, and system information utilities.

---

## 2. 64-Bit Paging & Memory Topology

### 4-Level Paging Architecture

dismasmOS implements hardware-enforced 4-level paging:
* **PML4 (Page Map Level 4)**: Located at `pml4_table`, entry `[0]` points to the Page Directory Pointer Table (`pdpt_table`) with flags `0x03` (Present | Writable).
* **PDPT (Page Directory Pointer Table)**: Entry `[0]` points to the Page Directory (`pd_table`) with flags `0x03` (Present | Writable).
* **PD (Page Directory)**: Contains 512 contiguous entries of 2 MiB each, identity-mapping physical addresses `0x0000000000000000` through `0x000000003FFFFFFF` (first 1 GiB of RAM) using flag `0x83` (Present | Writable | Page Size 2 MiB).

### Physical Memory Allocation Map

| Address Range | Size | Subsystem / Component | Attributes |
| :--- | :--- | :--- | :--- |
| `0x00000000 - 0x0007FFFF` | 512 KiB | Low Memory (IVT, BDA, Unused Buffer) | Reserved / Identity Mapped |
| `0x00080000 - 0x0009FFFF` | 128 KiB | EBDA / BIOS Reserved Area | Reserved |
| `0x000A0000 - 0x000BFFFF` | 128 KiB | VGA Framebuffer (`0xB8000` Text Mode) | MMIO (Dual-Port RAM) |
| `0x000C0000 - 0x000FFFFF` | 256 KiB | Video ROM & System BIOS Firmware | Read-Only |
| `0x00100000 - 0x00101FFF` | ~8 KiB | **dismasmOS `.text` (64-Bit Code)** | Executable / Read-Only |
| `0x00102000 - 0x00102FFF` | ~4 KiB | **dismasmOS `.rodata` (Constants & Strings)** | Read-Only |
| `0x00103000 - 0x00103FFF` | ~4 KiB | **dismasmOS `.data` (Initialized Variables)**| Read/Write |
| `0x00104000 - 0x00118000` | ~80 KiB | **dismasmOS `.bss` (Page Tables, RAMFS, 32KiB Stack)**| Zero-Initialized / RW |
| `0x00118000 - 0x3FFFFFFF` | ~1022 MiB | **Identity-Mapped Physical Free Memory** | Available RAM |

---

## 3. Microarchitectural CPU State, GDT64 & IDT64

### 64-Bit Global Descriptor Table (GDT64)

dismasmOS establishes a flat 64-bit Long Mode descriptor table with three 8-byte descriptors:

1. **Null Descriptor (`0x00`)**: `0x0000000000000000`
2. **Kernel 64-Bit Code Descriptor (`0x08`)**: `0x00209A0000000000`
   * Base = 0, Limit = 0 (Ignored in 64-bit mode)
   * Access: Present (`P=1`), Ring 0 (`DPL=00`), Code Segment (`S=1`), Executable/Readable (`Type=1010b`)
   * Flags: Long Mode Code (`L=1`), Default Size (`D=0`)
3. **Kernel 64-Bit Data Descriptor (`0x10`)**: `0x0000920000000000`
   * Base = 0, Limit = 0
   * Access: Present (`P=1`), Ring 0 (`DPL=00`), Data Segment (`S=1`), Read/Write (`Type=0010b`)

### 64-Bit Interrupt Descriptor Table (IDT64)

The 64-bit IDT consists of 256 16-byte gate descriptors:

```text
 127                                           96 95                                           64
+------------------------------------------------+-----------------------------------------------+
|                    Reserved                    |                 Offset 63..32                 |
+------------------------------------------------+-----------------------------------------------+
 63                                            48 47             40 39       35 34  32 31       0
+------------------------------------------------+-----------------+-----------+------+----------+
|                 Offset 31..16                  | P | DPL | 0 |Type| Reserved  | IST  | Selector |
+------------------------------------------------+-----------------+-----------+------+----------+
|                                  Offset 15..0                                                  |
+------------------------------------------------------------------------------------------------+
```

Every exception and hardware IRQ (0x00 - 0xFF) is routed through common assembly ISR wrappers in `src/arch/idt_asm.s` that save the 64-bit register context (`rax` through `r15`) and execute `iretq`.

---

## 4. Input Subsystem & Dual Keyboard Layout Engine

### German DIN 2137-2 (QWERTZ) & Scancode Mapping

The keyboard driver in `src/arch/kbd.c` interfaces directly with the Intel 8042 PS/2 microcontroller on I/O ports `0x60` (Data) and `0x64` (Status/Command).

To prevent index drift or off-by-one errors common in legacy array tables, all scancodes are mapped using **C99 Designated Initializers**:

```c
static const int kbd_map_de_normal[128] = {
    [0x01] = 27,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
    [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
    [0x0A] = '9', [0x0B] = '0', [0x0C] = (char)0xE1, [0x0D] = '`',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
    [0x14] = 't', [0x15] = 'z', [0x16] = 'u', [0x17] = 'i',
    [0x18] = 'o', [0x19] = 'p', [0x1A] = (char)0x81, [0x1B] = '+',
    [0x1C] = '\n',
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
    [0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
    [0x26] = 'l', [0x27] = (char)0x94, [0x28] = (char)0x84,
    [0x29] = '^',
    [0x2B] = '#',
    [0x2C] = 'y', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
    [0x33] = ',', [0x34] = '.', [0x35] = '-',
    [0x37] = '*', [0x39] = ' ',
    [0x4A] = '-', [0x4E] = '+',
    [0x56] = '<',  /* ISO 105-key specific keycode */
};
```

### Special Character & AltGr Mapping Reference

| Scancode | Key Position | Normal | Shift | AltGr | CP437 Character / Hex Code |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0x03` | Number 2 | `2` | `"` | `²` | `0xFD` (Superscript 2) |
| `0x04` | Number 3 | `3` | `§` | `³` | `0x15` (Section §) / `0xFC` (Superscript 3) |
| `0x08` | Number 7 | `7` | `/` | `{` | `0x7B` |
| `0x09` | Number 8 | `8` | `(` | `[` | `0x5B` |
| `0x0A` | Number 9 | `9` | `)` | `]` | `0x5D` |
| `0x0B` | Number 0 | `0` | `=` | `}` | `0x7D` |
| `0x0C` | Key `ß` | `ß` | `?` | `\` | `0xE1` (Sharp s ß) / `0x5C` |
| `0x10` | Letter Q | `q` | `Q` | `@` | `0x40` |
| `0x15` | Letter Z | **`z`** | **`Z`** | - | QWERTZ swapped with Y |
| `0x1A` | Key `Ü` | `ü` | `Ü` | - | `0x81` / `0x9A` |
| `0x1B` | Key `+` | `+` | `*` | `~` | `0x7E` |
| `0x27` | Key `Ö` | `ö` | `Ö` | - | `0x94` / `0x99` |
| `0x28` | Key `Ä` | `ä` | `Ä` | - | `0x84` / `0x8E` |
| `0x29` | Key `^` | `^` | `°` | - | `0x5E` / `0xF8` (Degree °) |
| `0x2B` | Key `#` | `#` | `'` | - | `0x23` / `0x27` |
| `0x2C` | Letter Y | **`y`** | **`Y`** | - | QWERTZ swapped with Z |
| `0x32` | Letter M | `m` | `M` | `µ` | `0xE6` (Micro µ) |
| `0x35` | Key `-` | `-` | `_` | - | `0x2D` / `0x5F` |
| `0x56` | Key `<` | `<` | `>` | `\|` | `0x3C` / `0x3E` / `0x7C` |

### Extended Scancode Decoding (`0xE0`) & Arrow Keys

The driver uses a two-stage state machine to decode two-byte escape sequences prefixed by `0xE0`:

```c
#define KEY_UP       0x8001
#define KEY_DOWN     0x8002
#define KEY_LEFT     0x8003
#define KEY_RIGHT    0x8004
#define KEY_HOME     0x8005
#define KEY_END      0x8006
#define KEY_PGUP     0x8007
#define KEY_PGDN     0x8008
#define KEY_INSERT   0x8009
#define KEY_DELETE   0x800A
#define KEY_ALT      0x800B
```

* **Arrow Keys**: Up (`0xE0 0x48`), Down (`0xE0 0x50`), Left (`0xE0 0x4B`), Right (`0xE0 0x4D`).
* **Navigation**: Delete (`0xE0 0x53`), Home (`0xE0 0x47`), End (`0xE0 0x4F`), PageUp (`0xE0 0x49`), PageDown (`0xE0 0x51`).
* **Alt Key Isolation**: Left Alt (scancode `0x38` non-extended) immediately triggers `KEY_ALT`, while Right Alt (`0xE0 0x38`) sets the `altgr_pressed` flag, ensuring zero conflict between Alt command hotkeys and AltGr symbol typing.

---

## 5. Hierarchical In-Memory Filesystem (RAMFS)

Storage is structured as a contiguous, fixed-allocation in-memory file table designed for static determinism, eliminating dynamic heap fragmentation (`malloc`/`free` overhead).

### Structure Definition

```c
#define FS_MAX_FILES 64
#define FS_MAX_PATH 64
#define FS_MAX_FILESIZE 2048
#define FS_MAX_CHANGES 64

struct fs_file {
    char path[FS_MAX_PATH];
    char data[FS_MAX_FILESIZE];
    size_t size;
    uint8_t is_dir;
    uint8_t is_system;
    uint8_t used;
};
```

### Directory Hierarchy Specifications

During kernel bootstrapping (`fs_init()`), the system creates a full directory structure and removes all legacy flat files (`hardware.txt` and `readme.txt` have been completely eliminated):

```text
/
├── home/                <- Primary User Workspace (Default Shell CWD, Clean Sandbox)
├── boot/                <- Protected Bootloader Subsystem
│   ├── grub.cfg         <- GRUB2 multiboot configuration (Critical System File)
│   └── boot.s          <- 64-bit bootstrap assembly (Critical System File)
├── krnl/                <- Protected Kernel Subsystem
│   ├── kernel.sys       <- Kernel build parameters and paging info (Critical System File)
│   └── system.map       <- Kernel symbol address mapping table (Critical System File)
├── shell/               <- Userland Shell Environment
│   ├── sh.cfg           <- Shell prompt and history configuration
│   ├── motd             <- Message of the Day banner
│   └── aliases          <- Custom command aliases (dir=lsf, cls=clear)
└── etc/                 <- System Configuration
    ├── os-release       <- Standard OS metadata
    ├── hostname         <- Machine network hostname ("dismasm-box")
    └── fstab            <- Filesystem mount table
```

### Path Resolution & Canonicalization Engine

The kernel provides `fs_resolve_path(const char *cwd, const char *path, char *out_path)`:
* Handles both **absolute paths** (starting with `/`) and **relative paths** (evaluated against `shell_cwd`).
* Resolves `.` (current directory) and `..` (parent directory) components iteratively.
* Clamps root traversal at `/` so that `cd /home/..` resolves cleanly to `/`.

---

## 6. Filesystem Audit & Change Logging Engine (`changes`)

dismasmOS maintains a continuous ring-buffer log of all file operations in kernel memory.

### Change Log Schema

```c
struct fs_change {
    char type[10];     /* VIEW, EDITED, DELETED, CREATED */
    char user[16];     /* Default: root */
    char filename[FS_MAX_PATH];
};
```

### Operation Logging Triggers

| Operation Type | Action Verb | Triggering System Events | Output Format |
| :--- | :--- | :--- | :--- |
| `VIEW` | Viewed | File opened in `em`, file displayed via `cat`, search via `grep` | `[VIEW]-[root] - Viewed <path>` |
| `EDITED` | Edited | File saved in `em` (`;wsc` or `;s`), file overwritten via `echo > file` | `[EDITED]-[root] - Edited <path>` |
| `CREATED` | Created | File created via `touch`, directory created via `makedir`, new file via `echo > file` | `[CREATED]-[root] - Created <path>` |
| `DELETED` | Deleted | File removed via `rm <path>` | `[DELETED]-[root] - Deleted <path>` |

The audit log is displayed in real-time by executing the **`changes`** shell command.

---

## 7. Text Editor "em" Deep Specification

**em** is a full-screen, high-performance in-memory text editor operating at rows 0 through 24 of the VGA display.

```text
Row  0 - 22: Document Editing Area (with real-time dictionary typo highlighting)
Row 23     : Status Bar: [EDIT:BASH] /home/main.sh | 1:1 | 98 B | Alt: cmd (;wsc ;q ;s ;-m)
Row 24     : Interactive Command / Message Bar (Prompts ';' when Alt is pressed)
```

### Operational Modes

1. **Direct Writing Mode (`MODE_EDIT`)**:
   * The editor opens directly in typing mode. Characters, numbers, spaces, umlauts, and semicolons (`;`) are inserted directly into the document buffer at `cursor_pos`.
   * No `i` or `a` key is required to begin typing.
2. **Command Mode (`MODE_COMMAND`)**:
   * Triggered exclusively by pressing the **`Alt`** key.
   * Row 24 displays the cyan `;` prompt.
   * Typing commands:
     * `;wsc` (or `wsc`): Write, Save, and Close (saves changes to RAMFS, logs `[EDITED]`, and returns to shell).
     * `;s` (or `s`): Save document to RAMFS without closing.
     * `;q` (or `q`): Quit without saving (exits to shell).
     * `;-m` (or `-m`): Hop cursor to the next detected typographical error.
   * Pressing `Alt` again or `Esc` immediately cancels command mode and returns to direct editing.

### Multi-Grammar Syntax & Typo Checking Engine (Bash, Python, Markdown)

**em** integrates an intelligent, multi-language real-time spellchecking and keyword validation engine tailored specifically for system administrators and developers:

1. **Automatic Grammar & File-Type Detection**:
   The editor analyzes the filename extension and buffer shebang on load:
   * **Bash (`[EDIT:BASH]`)**: Triggered by `.sh`, `.bash`, `sh.cfg`, `aliases`, `motd`, or `#!/bin/bash`, `#!/bin/sh`.
   * **Python (`[EDIT:PYTHON]`)**: Triggered by `.py`, `.pyw`, or `#!/usr/bin/python`.
   * **Markdown (`[EDIT:MARKDOWN]`)**: Triggered by `.md`, `.markdown`, `README`, `CHANGES`.
   * **Generic (`[EDIT]`)**: Standard fallback cross-matching all dictionaries.

2. **Grammar Dictionaries**:
   * **Bash Dictionary (`dict_bash`)**:
     * Builtins & Keywords: `if`, `then`, `else`, `elif`, `fi`, `case`, `esac`, `for`, `while`, `until`, `do`, `done`, `echo`, `printf`, `read`, `export`, `alias`, `source`, `exec`, `trap`, `eval`, `declare`, `local`, `shopt`, etc.
     * Coreutils & CLI: `cat`, `grep`, `sed`, `awk`, `find`, `xargs`, `chmod`, `chown`, `mkdir`, `cp`, `mv`, `rm`, `touch`, `lsf`, `ps`, `top`, `curl`, `wget`, `tar`, `gzip`, `sudo`, `systemctl`, `shutdown`, `reboot`, etc.
     * Scripting terms: `stdin`, `stdout`, `stderr`, `null`, `pipe`, `stream`, `status`, `args`, `path`, `home`, etc.
   * **Python Dictionary (`dict_python`)**:
     * Keywords: `def`, `class`, `import`, `from`, `return`, `yield`, `lambda`, `try`, `except`, `finally`, `raise`, `async`, `await`, `assert`, `pass`, `with`, `while`, `for`, `in`, `is`, `not`, `and`, `or`, etc.
     * Built-in functions & types: `print`, `len`, `range`, `str`, `int`, `float`, `list`, `dict`, `set`, `tuple`, `bool`, `bytes`, `open`, `read`, `write`, `append`, `extend`, `pop`, `keys`, `values`, `items`, `enumerate`, `zip`, `map`, `filter`, `sorted`, `isinstance`, `super`, `self`, `cls`, `init`, `repr`, etc.
     * Exceptions & standard libraries: `valueerror`, `typeerror`, `indexerror`, `keyerror`, `ioerror`, `oserror`, `sys`, `os`, `math`, `re`, `json`, `time`, `datetime`, `random`, `subprocess`, `argparse`, `logging`, etc.
   * **Markdown Dictionary (`dict_markdown`)**:
     * Document structure: `heading`, `header`, `title`, `summary`, `abstract`, `overview`, `architecture`, `specification`, `implementation`, `manual`, `reference`, `guide`, `changelog`, `features`, `usage`, `installation`, `requirements`, `prerequisites`, `setup`, `configuration`, `build`, `compile`, `running`, `test`, `tests`, `benchmark`, `license`, `author`, `version`, `release`, `notes`, `description`, `details`, etc.
     * Markdown elements: `example`, `sample`, `code`, `syntax`, `table`, `list`, `link`, `image`, `badge`, `quote`, `blockquote`, `block`, `codeblock`, `inline`, `bold`, `italic`, `strikethrough`, `section`, `bullet`, `todo`, `warning`, `note`, `tip`, `important`, `caution`, `parameter`, `argument`, `status`, `terminal`, `console`, `commit`, `push`, `pull`, `branch`, `repo`, `github`, `git`, `doc`, `docs`, `api`, `cli`, `sdk`, etc.

3. **Visual Highlighting & Hopping**:
   * Recognized keywords and vocabulary are rendered in clean `VGA_COLOR_WHITE`.
   * Unrecognized tokens and typos are highlighted in real-time in **`VGA_COLOR_LIGHT_RED`**.
   * In Command Mode (`Alt`), typing **`;-m`** (or `-m`) automatically hops the cursor sequentially to the next detected typographical error or misspelled keyword, wrapping around the document.

### Cursor & Document Navigation

* **`KEY_UP` / `KEY_DOWN`**: Moves cursor vertically between lines, preserving horizontal column offset or clamping to the destination line boundary.
* **`KEY_LEFT` / `KEY_RIGHT`**: Decrements or increments `cursor_pos` within the active buffer.
* **`KEY_HOME` / `KEY_END`**: Jumps cursor to the line's start or end.
* **`KEY_DELETE`**: Deletes the character directly under the cursor.
* **`Backspace`**: Deletes the character immediately preceding the cursor.

### Critical System File Protection Modal

When attempting to open a file marked as a critical system asset (any file located within `/boot` or `/krnl`), `em` intercepts execution and presents a warning dialog:

```text
+------------------------------------------------------------------+
|                    *** SYSTEM WARNING ***                        |
|                                                                  |
|  WARNING: CRITICAL SYSTEM FILE!                                  |
|  File: /boot/grub.cfg                                            |
|                                                                  |
|  You are attempting to open a vital system file.                 |
|  Modifying this file may cause severe system instability,        |
|  kernel panics, or prevent dismasmOS from booting!               |
|  Please think twice and ensure you understand the risks.         |
|  Are you sure you want to proceed?                               |
|                                                                  |
|               [   OK   ]               [  EXIT  ]                |
+------------------------------------------------------------------+
```

* **Interactive Controls**: Navigable via the `Left` and `Right` arrow keys (as well as `Tab`, `Up`, and `Down`). The active button is visually highlighted in green (`[ OK ]`) or red (`[ EXIT ]`).
* **Confirmation**:
  * Selecting `[ OK ]` and pressing `Enter` bypasses the warning, logs `[VIEW]`, and opens the editor.
  * Selecting `[ EXIT ]` or pressing `Escape` aborts the operation and returns to the shell without modifying the screen or opening the file.

---

## 8. Shell Architecture & Command Reference

The command-line interface operates via an in-place tokenizing Read-Eval-Print Loop (REPL). The working directory (`shell_cwd`) defaults to `/home` upon cold boot.

**Important**: The legacy `ls` command has been completely removed from the system. All file listing operations are handled exclusively by **`lsf`**.

```text
+----------+--------------------------------------+------------------------------------+
| Command  | Parameter Signature                  | Operational Semantics              |
+----------+--------------------------------------+------------------------------------+
| help     | help                                 | Outputs built-in utility summary   |
| clear    | clear                                | Resets VGA buffer (blanks 80x25)   |
| echo     | echo [args...]                       | Echoes string vector or redirects  |
| lsf      | lsf [-s] [-p] [dir]                  | Lists files, sizes (KiB+hex), perms|
| cd       | cd <directory>                       | Changes working directory (/home)  |
| makedir  | makedir <directory>                  | Creates a new directory            |
| changes  | changes                              | Shows audit history log            |
| em       | em <filename>                        | Full text editor with Alt command  |
| cat      | cat <filename>                       | Streams full payload of a file     |
| grep     | grep <pattern> <filename>            | Substring line filter on file data |
| touch    | touch <filename>                     | Creates an empty node in RAMFS     |
| rm       | rm <filename>                        | Deletes file / node from RAMFS     |
| pr       | pr [start|kill|-c|status]            | Process management subsystem       |
| uname    | uname                                | Prints kernel release & metadata   |
| MemRep   | MemRep                               | Full memory allocation report      |
| lang     | lang <de|en>                         | Switches layout (QWERTZ / QWERTY)  |
| shutdown | shutdown                             | ACPI + Assembly hardware halt      |
| reboot   | reboot                               | Pulses 8042 CPU reset line (0xFE)  |
+----------+--------------------------------------+------------------------------------+
```

### Detailed Command Specifications

#### `lsf` (List Files)
* **`lsf`**: Clean listing of files and directories in the target path (directories rendered in cyan with trailing `/`).
* **`lsf -s`**: Displays file sizes in both human-readable KiB/Bytes and 32-bit hexadecimal notation (`0x0000004E`).
* **`lsf -p`**: Displays permission strings (`drwxr-xr-x`, `-rwxr-xr-x`, `-rw-r--r--`) and user permissions (`[root:admin rwx]`).
* **Flags can be combined**: `lsf -sp`, `lsf -s -p`, `lsf -ps`.

#### `cd` (Change Directory)
* `cd`: Jumps to default workspace `/home`.
* `cd ~`: Jumps to default workspace `/home`.
* `cd /`: Jumps to root filesystem `/`.
* `cd ..`: Moves to parent directory.
* `cd <rel_path>`: Resolves relative path against current `shell_cwd`.

#### `makedir` (Make Directory)
* Creates a directory node in the filesystem table.
* Automatically records a `[CREATED]` event in the `changes` audit log.

#### `shutdown` (System Poweroff & Assembly Halt)
* Performs an active, safe system shutdown:
  1. Flushes in-memory filesystem buffers.
  2. Disables interrupts via `cli`.
  3. Sends power-off signals to standard virtual machine ACPI ports:
     * Port `0x604` (QEMU ACPI poweroff: `outw(0x604, 0x2000)`)
     * Port `0xB004` (Bochs ACPI poweroff: `outw(0xB004, 0x2000)`)
     * Port `0x4004` (VirtualBox ACPI poweroff: `outw(0x4004, 0x3400)`)
     * Port `0xF4` (QEMU debug exit: `outb(0xF4, 0x00)`)
  4. Enters an infinite processor halt loop:
     ```asm
     cli
     1: hlt
     jmp 1b
     ```

---

## 9. Codebase Layout

```text
dismasmOS/
├── build.sh                 # Fully automated build and ISO generation harness
├── Makefile                 # GNU Makefile with freestanding x86_64 compiler flags
├── linker.ld                # GNU LD script defining 1 MiB VMA/LMA layout
├── grub.cfg                 # GRUB 2 ISO multiboot menu configuration
├── changes.txt              # Plain-text commit and release changelog (no Markdown syntax)
├── include/                 # Freestanding Kernel Header Specifications
│   ├── em.h                 # "em" editor prototypes and modal dialog interfaces
│   ├── fs.h                 # RAMFS struct file, path resolver, audit log declarations
│   ├── idt.h                # 64-Bit IDT descriptor and gate entry definitions
│   ├── io.h                 # Inline assembly port I/O primitives (inb, outb, outw)
│   ├── kbd.h                # Keyboard driver interfaces, special keycodes (KEY_UP, etc.)
│   ├── pic.h                # Dual-8259A PIC register definitions and command masks
│   ├── proc.h               # Process table structures and management interfaces
│   ├── shell.h              # Command parser, tokenizer, and loop declarations
│   ├── string.h             # Freestanding C string library (strlen, strcmp, strncat, etc.)
│   ├── types.h              # Standard integer typedefs (uint8_t, size_t, etc.)
│   └── vga.h                # VGA text mode colors, macros, and function prototypes
└── src/                     # Core Implementation Sources
    ├── boot/
    │   └── boot.s           # Multiboot 1 entry, 4-level paging setup, 64-bit transition
    ├── arch/
    │   ├── idt_asm.s        # 64-Bit assembly ISR dispatch stubs and lidt wrapper
    │   ├── idt.c            # 256-entry 64-bit IDT gate population and setup
    │   ├── kbd.c            # DIN 2137-2 QWERTZ driver, AltGr decoder, designated init
    │   └── pic.c            # Dual-8259A PIC initialization, remapping, and EOI signaling
    ├── drivers/
    │   └── vga.c            # 0xB8000 VRAM driver, scrolling, and CRT cursor sync
    ├── em/
    │   └── em.c             # Full "em" text editor, Alt-command engine, warning dialog
    ├── fs/
    │   └── fs.c             # Hierarchical RAMFS, path canonicalization, changes audit log
    ├── lib/
    │   └── string.c         # Freestanding implementations of strlen, strcmp, strncat, etc.
    ├── proc/
    │   └── proc.c           # Process control block, process table, service.prf agent
    ├── shell/
    │   └── shell.c          # Tokenizer, REPL loop, prompt styling, lsf, cd, shutdown
    └── kernel.c             # Hardware bootstrap sequence, Debian boot telemetry banner
```

---

## 10. Building and Image Generation

### Toolchain Prerequisites

To build dismasmOS, the host environment requires:
* **GNU Compiler Collection (`gcc`)** with 64-bit freestanding support.
* **GNU Assembler (`as`)** and **GNU Linker (`ld`)** supporting target `elf_x86_64`.
* **GNU Make (`make`)**.
* **GRUB 2 (`grub-mkrescue`)** and **xorriso** for bootable ISO creation.
* **QEMU (`qemu-system-x86_64`)** for local emulation.

### Build Commands

```bash
# Clean intermediate object files and build artifacts
make clean

# Compile and link the 64-bit ELF binary (dismasmOS.bin)
make

# Build bootable hybrid El Torito ISO image (dismasmOS.iso)
make iso
```

---

## 11. Emulation and Testing

### Running under QEMU

Execute directly using the compiled ELF kernel binary:

```bash
qemu-system-x86_64 -kernel dismasmOS.bin
```

Or boot the complete CD-ROM ISO image:

```bash
qemu-system-x86_64 -cdrom dismasmOS.iso -m 256M
```

To test the German keyboard layout directly within QEMU:

```bash
qemu-system-x86_64 -cdrom dismasmOS.iso -m 256M -k de
```

### Running under Oracle VirtualBox

1. Create a new Virtual Machine:
   * **Type**: `Other`
   * **Version**: `Other/Unknown (64-bit)`
   * **RAM**: `128 MB`
2. Under **Storage**, attach `dismasmOS.iso` to the IDE/SATA Optical Drive.
3. Start the VM. GRUB 2 will load the kernel, establish 64-bit Long Mode, and drop directly into `/home` at the shell prompt.

---

## 12. Technical Specifications Reference Sheet

```text
Processor Architecture:     x86_64 (AMD64 / Intel 64 Long Mode)
Privilege Level:            Ring 0 (Kernel Supervisor)
Address Translation:        4-Level Paging (PML4, PDPT, PD, 1 GiB Identity Mapped)
Page Size:                  2 MiB Huge Pages (0x83 Flag)
Binary Format:              ELF 64-bit LSB Executable (x86-64)
Boot Specification:         Multiboot 1 (RFC 0.6.96)
Entry Point Physical Addr:  0x00100000 (1 MiB Alignment)
Interrupt Architecture:     Dual-8259A Cascaded PIC (Vectors 0x20-0x2F)
Interrupt Descriptor Gate:  64-Bit Gate Descriptors (16 Bytes / Descriptor)
Console Video Mode:         VGA Color Text Mode 80x25 @ 0x000B8000
Console Hardware Ports:     0x3D4 (Index), 0x3D5 (Data) - CRT Controller
System Config Variables:    SYSTEM_VERSION & SYSTEM_BUILD defined at top of src/shell/shell.c
Keyboard Controller:        Intel 8042 PS/2 Microcontroller (IRQ1)
Keyboard Driver Mapping:    German DIN 2137-2 (QWERTZ) & US (QWERTY)
Hardware Navigation:        Full Arrow Keys, Home, End, Delete, Alt-Command Trigger
Text Editor:                "em" with Spellchecking, Modal Alt-Commands, Warning Dialog
Filesystem Engine:          Hierarchical RAMFS (/home, /boot, /krnl, /shell, /etc)
Default Working Directory:  /home
Filesystem Audit Log:       Kernel-level Ring-Buffer Log (VIEW, EDITED, CREATED, DELETED)
Power Management:           ACPI VM Poweroff (0x604, 0xB004, 0x4004) + Assembly HLT Loop
External Dependencies:      0 (Fully freestanding, zero external C library linkage)
```

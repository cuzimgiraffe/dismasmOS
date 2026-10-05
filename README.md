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

## Executive Summary

dismasmOS is an ultra-lean, deterministic, freestanding x86_64 Long Mode operating system implemented strictly in ISO C99 and GNU Assembler for the AMD64 and Intel 64 architectural specification. Operating entirely without reliance on the C standard library, third-party runtimes, or secondary userland abstractions, dismasmOS establishes an end-to-end bare-metal compute environment featuring:

* **64-Bit Long Mode Microkernel**: 4-level paging (PML4, PDPT, PD) with a 1 GiB identity-mapped address space using 2 MiB huge pages, custom 64-bit Global Descriptor Table (GDT64), and a 256-gate 64-bit Interrupt Descriptor Table (IDT).
* **Freestanding Heap Memory Allocator**: Custom 16 MiB dynamic memory allocator implementing malloc, free, calloc, and realloc from scratch with 16-byte alignment, 48-byte headers, double-free detection, and bidirectional coalescing.
* **Deterministic Input Engine**: Fully compliant German DIN 2137-2 (QWERTZ) and US (QWERTY) keyboard mapping using C99 designated initializers, complete AltGr decoding, ISO-key support, and extended PS/2 scancode decoding for dedicated hardware Arrow Keys and Alt navigation.
* **Interactive CLI Shell & Command Chaining**: Robust Read-Eval-Print Loop supporting sequential command chaining via the && operator, a 32-entry command history with hardware Arrow Key recall, and 23 native commands.
* **Direct Assembly & Machine Code Execution**: Integrated dynamic execution engine allowing raw machine bytecode or processor mnemonics (nop, rdtsc, cpuid, cli, sti) to be executed directly in memory with RAX and RDX return register inspection.
* **System & Register Diagnostics**: Live inspection of all 64-bit general-purpose registers (RAX through R15), control registers (CR0, CR2, CR3, CR4), and RFLAGS status flags, paired with formatted hexadecimal and ASCII memory dumping.
* **Integrated CLI Calculator**: Recursive-descent integer and hexadecimal calculator supporting addition, subtraction, multiplication, division, modulo, and nested parenthetical expressions.
* **16-Color VGA Palette & Solid Terminal Spot**: Full 16-color video support with color selection in English, German, or numeric values, featuring a true 1-bit solid terminal block spot in the selected color.
* **Acoustic Feedback Engine**: Hardware PC Speaker integration generating a high-pitch 3000 Hz acoustic error beep lasting exactly 1 millisecond upon invalid syntax or command failure.
* **Hierarchical In-Memory Filesystem (RAMFS)**: Fixed-structure file tables featuring root, home (user workspace), boot (bootloader files), krnl (kernel assets), shell (scripts and configurations), and etc (system parameters) directories with full audit logging.
* **Full-Featured Text Editor ("em")**: Direct text writing by default, hardware arrow-key cursor navigation, Alt-key modal command execution, multi-grammar syntax typo highlighting (Bash, Python, Markdown), and interactive warning modals protecting critical system files.
* **Hardware Shutdown & Power Management**: Active inline assembly CPU shutdown combined with automated ACPI power-off sequences for virtualization platforms.
* **Operating System Architecture Mapping**: Comprehensive architectural blueprint and hardware mapping workbook formatted as a multi-sheet spreadsheet.

---

## 1. System Architecture & Boot Sequence

dismasmOS initializes from a Multiboot 1 compliant bootloader (such as GRUB 2) and transitions deterministically from 32-bit protected mode into 64-bit Long Mode at Privilege Level 0 (Ring 0).

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
|                           EARLY ASSEMBLY BOOTSTRAP                                |
|  1. Clear EFLAGS & Disable Interrupts (CLI)                                       |
|  2. Zero Page Tables (PML4, PDPT, PD) in BSS section                              |
|  3. Build 4-Level Paging: PML4[0] -> PDPT, PDPT[0] -> PD                          |
|  4. Identity-map 1 GiB using 512 x 2 MiB Large Page Entries (0x83 Flag)           |
|  5. Enable Physical Address Extension (CR4.PAE = 1)                               |
|  6. Enable Long Mode in EFER MSR (MSR 0xC0000080, Bit 8 LME = 1)                   |
|  7. Activate Paging and Protected Subsystems (CR0.PG = 1, CR0.PE = 1)              |
|  8. Load 64-Bit GDT (LGDT gdt64_ptr)                                               |
|  9. Far Jump to 64-Bit Long Mode Code Segment                                      |
| 10. Setup 32 KiB 16-byte Aligned 64-Bit Stack                                      |
| 11. Call Kernel Entry Point (kmain)                                                |
+-----------------------------------------------------------------------------------+
                                         │
                                         ▼
+-----------------------------------------------------------------------------------+
|                                   KERNEL MAIN                                     |
|  ┌─────────────────────────────────────────────────────────────────────────────┐  |
|  │  1. Video Subsystem: Initialize 80x25 VGA Text Buffer (0xB8000)             │  |
|  │  2. Telemetry Stage 1: Debian-Style Kernel Log Banner [    0.000000]        │  |
|  │  3. Trap Management: Setup 256 64-Bit IDT Gates                             │  |
|  │  4. Controller Remap: Remap Dual 8259 PIC to IRQ 0x20 and 0x28              │  |
|  │  5. Input Subsystem: DIN 2137-2 QWERTZ Keymap & Unmask IRQ1 Keyboard        │  |
|  │  6. Memory Manager: Initialize 16 MiB Freestanding Heap Allocator           │  |
|  │  7. Interrupt Activation: Enable Hardware Interrupts (STI)                  │  |
|  │  8. Storage Engine: Mount In-Memory Hierarchical Filesystem                 │  |
|  │  9. Process Subsystem: Initialize Process Control Blocks & Daemon Tasks     │  |
|  │ 10. Telemetry Stage 2: System Service Assertions [  OK  ]                   │  |
|  │ 11. Shell Engine: Initialize Command REPL and Launch Shell Event Loop       │  |
|  └─────────────────────────────────────────────────────────────────────────────┘  |
+-----------------------------------------------------------------------------------+
```

### Centralized System Version & Configuration

System identity and build metadata are centralized in dedicated global descriptors:
* System Version: "1.4"
* System Build: "2026"

These values are consumed uniformly across kernel boot telemetry banners, shell prompts, diagnostic reports, and shutdown status screens.

---

## 2. 64-Bit Paging & Memory Topology

### 4-Level Paging Architecture

dismasmOS implements hardware-enforced 4-level paging:
* **PML4 (Page Map Level 4)**: Entry 0 references the Page Directory Pointer Table (PDPT) with flags Present and Writable.
* **PDPT (Page Directory Pointer Table)**: Entry 0 references the Page Directory (PD) with flags Present and Writable.
* **PD (Page Directory)**: Contains 512 contiguous entries of 2 MiB each, identity-mapping physical addresses 0x0000000000000000 through 0x000000003FFFFFFF (first 1 GiB of RAM) using flag 0x83 (Present, Writable, Page Size 2 MiB, Supervisor). The absence of the No-Execute bit allows dynamic runtime execution of machine code.

### Physical Memory Allocation Map

| Address Range | Size | Subsystem / Component | Attributes |
| :--- | :--- | :--- | :--- |
| `0x00000000 - 0x0007FFFF` | 512 KiB | Low Memory (IVT, BDA, Boot Buffers) | Reserved / Identity Mapped |
| `0x00080000 - 0x0009FFFF` | 128 KiB | EBDA / BIOS Reserved Area | Reserved |
| `0x000A0000 - 0x000BFFFF` | 128 KiB | VGA Framebuffer (0xB8000 Text Mode) | MMIO Dual-Port Video RAM |
| `0x000C0000 - 0x000FFFFF` | 256 KiB | Video ROM & System BIOS Firmware | Read-Only Firmware |
| `0x00100000 - 0x00101FFF` | ~8 KiB | dismasmOS Text Section (64-Bit Code) | Executable / Read-Only |
| `0x00102000 - 0x00102FFF` | ~4 KiB | dismasmOS Rodata (Constants & Strings) | Read-Only |
| `0x00103000 - 0x00103FFF` | ~4 KiB | dismasmOS Data (Initialized Globals) | Read/Write |
| `0x00104000 - 0x00118000` | ~80 KiB | dismasmOS BSS (Paging, RAMFS, Stack) | Zero-Initialized / Read-Write |
| `0x01000000 - 0x01FFFFFF` | 16 MiB | Freestanding Kernel Heap Memory Pool | Dynamic Allocation Pool |
| `0x02000000 - 0x3FFFFFFF` | ~992 MiB | Free Identity-Mapped Physical Memory | Available Memory |

---

## 3. Freestanding Dynamic Heap Allocator

dismasmOS incorporates a fully custom, independent kernel heap memory manager created specifically for bare-metal x86_64 systems without standard C library support.

### Heap Architectural Specifications
* **Pool Size**: Dedicated 16 MiB contiguous memory region located at physical offset 0x01000000.
* **Alignment**: All memory requests are strictly aligned to 16-byte boundaries, satisfying all AMD64 and Intel 64 SSE and general alignment requirements.
* **Block Header (48 Bytes)**:
  * Magic Verification Word: 64-bit signature (0xDEADBEEFCAFE0001) used to validate heap consistency and prevent corruption.
  * Size Field: 64-bit representation of the usable payload size.
  * Allocation Status: 8-bit flag distinguishing allocated blocks from free blocks.
  * Bidirectional Pointers: 64-bit next and previous block pointers enabling fast traversal in both directions.
* **Allocation Strategy**: First-Fit search traversing the contiguous block list with dynamic block splitting whenever a free block exceeds the requested size by more than the header overhead plus minimum payload.
* **Deallocation & Coalescing**: Double-free detection guarding against memory bugs, paired with immediate bidirectional coalescing that merges newly freed memory with adjacent preceding and succeeding free blocks to eliminate external fragmentation.

### Dynamic Memory Interface
* **malloc(size)**: Allocates an uninitialized contiguous buffer of the specified size.
* **free(ptr)**: Validates header magic, marks the memory block as free, and performs immediate bidirectional coalescing.
* **calloc(num, size)**: Allocates contiguous memory for an array and zeroes out all allocated bytes.
* **realloc(ptr, new_size)**: Dynamically resizes an existing allocation, reusing the block in place when possible, or allocating a new chunk, copying the data, and freeing the original block.

---

## 4. CPU Architecture, GDT64, and Trap Management

### 64-Bit Global Descriptor Table (GDT64)

dismasmOS establishes a flat 64-bit Long Mode descriptor table with three primary descriptors:
1. **Null Descriptor (Selector 0x00)**: Reserved 64-bit zero descriptor.
2. **Kernel 64-Bit Code Descriptor (Selector 0x08)**: Present, Ring 0, Code Segment, Executable and Readable, Long Mode 64-Bit Flag enabled.
3. **Kernel 64-Bit Data Descriptor (Selector 0x10)**: Present, Ring 0, Data Segment, Read and Write enabled.

### 64-Bit Interrupt Descriptor Table (IDT64)

The 64-bit IDT consists of 256 16-byte gate descriptors:
* Gate Format: 64-bit target offset split across lower, middle, and upper fields, 16-bit code segment selector (0x08), Interrupt Gate Type (0x8E for 64-bit Ring 0 interrupt gate), and Interrupt Stack Table index.
* Exception Handling: Dedicated assembly ISR dispatch stubs save all 64-bit general-purpose registers (RAX through R15) onto the stack before routing execution to high-level C exception handlers.
* Programmable Interrupt Controllers: Dual 8259A PICs remapped to interrupt vectors 0x20 through 0x2F to prevent collisions with CPU-reserved architecture exceptions.

---

## 5. Input Subsystem & Dual Keyboard Layout Engine

### Hardware Interface & Scancode Decoding

The keyboard driver interfaces directly with the Intel 8042 PS/2 controller on I/O ports 0x60 (Data Port) and 0x64 (Status and Command Port). It supports dual international layouts:
* **German DIN 2137-2 (QWERTZ)**: Default layout upon system boot.
* **US Standard (QWERTY)**: Selectable at runtime via the `lang en` command.

All scancode translation tables are implemented using C99 designated initializers to ensure exact index matching and eliminate array drift.

### Special Character and AltGr Mapping

| Scancode | Key Position | Normal | Shift | AltGr | Character Description |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0x03` | Number 2 | `2` | `"` | `²` | Superscript 2 |
| `0x04` | Number 3 | `3` | `§` | `³` | Section sign / Superscript 3 |
| `0x08` | Number 7 | `7` | `/` | `{` | Left curly brace |
| `0x09` | Number 8 | `8` | `(` | `[` | Left square bracket |
| `0x0A` | Number 9 | `9` | `)` | `]` | Right square bracket |
| `0x0B` | Number 0 | `0` | `=` | `}` | Right curly brace |
| `0x0C` | Key ß | `ß` | `?` | `\` | Backslash |
| `0x10` | Letter Q | `q` | `Q` | `@` | At symbol |
| `0x15` | Letter Z | `z` | `Z` | - | German QWERTZ swapped with Y |
| `0x1A` | Key Ü | `ü` | `Ü` | - | German umlaut U |
| `0x1B` | Key + | `+` | `*` | `~` | Tilde |
| `0x27` | Key Ö | `ö` | `Ö` | - | German umlaut O |
| `0x28` | Key Ä | `ä` | `Ä` | - | German umlaut A |
| `0x29` | Key ^ | `^` | `°` | - | Degree sign |
| `0x2B` | Key # | `#` | `'` | - | Hash and apostrophe |
| `0x2C` | Letter Y | `y` | `Y` | - | German QWERTZ swapped with Z |
| `0x32` | Letter M | `m` | `M` | `µ` | Micro symbol |
| `0x35` | Key - | `-` | `_` | - | Hyphen and underscore |
| `0x56` | Key < | `<` | `>` | `\|` | ISO 105-key angle brackets and pipe |

### Extended Scancodes & Dedicated Hardware Keys

The keyboard driver implements a state machine decoding extended two-byte sequences prefixed by scancode 0xE0:
* Up Arrow: Scancode 0xE0 0x48
* Down Arrow: Scancode 0xE0 0x50
* Left Arrow: Scancode 0xE0 0x4B
* Right Arrow: Scancode 0xE0 0x4D
* Home: Scancode 0xE0 0x47
* End: Scancode 0xE0 0x4F
* Delete: Scancode 0xE0 0x53
* Left Alt: Scancode 0x38 (Triggers command mode in text editor)
* Right Alt: Scancode 0xE0 0x38 (Activates AltGr state for third-level symbols)

---

## 6. 16-Color Video Subsystem & Acoustic Engine

### VGA Text Mode Display
* **Memory Address**: 0x000B8000 mapped as 80 columns by 25 rows with 2 bytes per character cell (ASCII character byte and color attribute byte).
* **Color Palette**: Supports all 16 standard VGA text colors:
  0: Black, 1: Blue, 2: Green, 3: Cyan, 4: Red, 5: Magenta, 6: Brown, 7: Light Gray, 8: Dark Gray, 9: Light Blue, 10: Light Green, 11: Light Cyan, 12: Light Red, 13: Light Magenta, 14: Yellow, 15: White.

### Color Configuration & 1-Bit Spot (`colr`)
* The `colr` command allows inspecting and setting the active terminal foreground color.
* Accepts color numbers (0 through 15), English color names, German color names, and optional bracketed expressions (such as `colr (gruen)`).
* Outputs an immediate visual confirmation featuring a solid 1-bit terminal spot (VGA character code 219, rendering a 100% filled block) in the newly selected color.

### Hardware PC Speaker Error Beep
* Dedicated acoustic feedback triggered whenever a command fails or syntax is invalid.
* Controls the Programmable Interval Timer (PIT) Channel 2 connected to the PC Speaker via I/O Port 0x61.
* Frequency: 3000 Hz for a clear, high-pitched acoustic warning.
* Duration: Exactly 1 millisecond generated via calibrated PIT channel cycles.

---

## 7. Interactive CLI Shell Architecture

The command-line interface operates via an in-place tokenizing Read-Eval-Print Loop (REPL). Upon system boot, the shell initializes in the default user workspace (the home directory).

```text
+-----------------------------------------------------------------------------------+
|                            SHELL REPL PROCESSING FLOW                             |
|                                                                                   |
|  [Keyboard Input] ──► [Line Buffer] ──► [Up/Down History Recall (32 Slots)]       |
|                               │                                                   |
|                               ▼                                                   |
|                [Chaining Tokenizer: "&&" Splitter]                                |
|                               │                                                   |
|                               ▼                                                   |
|        ┌──────────────────────────────────────────────┐                           |
|        │ Execute Subcommand (Arguments & Redirection) │                           |
|        └──────────────────────┬───────────────────────┘                           |
|                               │                                                   |
|                ┌──────────────┴──────────────┐                                    |
|                ▼                             ▼                                    |
|         [Return Code == 0]            [Return Code != 0]                          |
|                │                             │                                    |
|                ▼                             ▼                                    |
|       Advance to Next Subcmd        Acoustic Error Beep (1 ms)                    |
|       in the "&&" Pipeline          HALT Pipeline Execution                       |
+-----------------------------------------------------------------------------------+
```

### Command Chaining with `&&`
* Multiple commands can be chained on a single command line separated by the `&&` operator.
* Commands are evaluated strictly from left to right.
* If any subcommand returns an error status, execution of subsequent commands in the chain is immediately aborted, and the 1 ms PC Speaker acoustic error beep is sounded.

### Command History Navigation
* A dedicated 32-entry circular history buffer stores executed commands.
* Pressing the Up Arrow recalls earlier commands in chronological order.
* Pressing the Down Arrow steps forward toward newer commands or returns to a blank input line.
* The terminal display is dynamically cleared and updated to match the recalled line buffer.

---

## 8. Complete Command Reference Suite

dismasmOS provides 23 native commands:

| Command | Syntax Signature | Operational Semantics |
| :--- | :--- | :--- |
| `help` | `help` | Displays command summary and usage |
| `clear` | `clear` | Blanks the 80x25 VGA screen and resets cursor |
| `echo` | `echo [text] [> file]` | Prints text or redirects output into a file |
| `lsf` | `lsf [-s] [-p] [dir]` | Lists files with optional size (KiB and hex) and permissions |
| `cd` | `cd [directory]` | Changes current working directory (defaults to home directory) |
| `makedir` | `makedir <directory>` | Creates a new directory node in the filesystem |
| `changes` | `changes` | Displays real-time audit log of file access events |
| `em` | `em <filename>` | Full-screen text editor with real-time grammar checks |
| `cat` | `cat <filename>` | Streams the raw contents of a file to stdout |
| `grep` | `grep <pattern> <file>` | Searches for text patterns within a file |
| `touch` | `touch <filename>` | Creates an empty file node in the filesystem |
| `rm` | `rm <filename>` | Removes a file or directory node from the filesystem |
| `pr` | `pr [start\|kill\|status]` | Process management subsystem and daemon status |
| `uname` | `uname` | Prints operating system name, release, and machine target |
| `MemRep` | `MemRep` | Comprehensive memory report including kernel and heap stats |
| `lang` | `lang <de\|en>` | Switches active keyboard layout between QWERTZ and QWERTY |
| `shutdown`| `shutdown` | Executes ACPI power-off and assembly CPU halt loop |
| `reboot` | `reboot` | Triggers hardware CPU reset via 8042 keyboard controller |
| `colr` | `colr [name\|0-15]` | Displays palette or changes color and outputs 1-bit spot |
| `heap` | `heap [test]` | Displays heap statistics or runs automated allocator self-tests |
| `calc` | `calc <expression>` | Evaluates integer and hexadecimal mathematical expressions |
| `dump` | `dump <address> [len]` | Displays formatted hexadecimal and ASCII memory dump |
| `reg` | `reg` | Displays 64-bit general-purpose, control, and flag registers |
| `asm` | `asm <hex\|mnemonic>` | Executes raw machine code or processor instructions dynamically |

---

## 9. Diagnostic & Computation Subsystems

### System & Register Diagnostics (`reg`)

The `reg` command captures the current CPU execution context directly from the running 64-bit kernel using inline assembly:
* **General-Purpose 64-Bit Registers**: Displays RAX, RBX, RCX, RDX, RSI, RDI, RBP, RSP, and R8 through R15 in full 16-character hexadecimal format.
* **Control Registers**:
  * CR0: Paging, Protection, and Numeric Error flags.
  * CR2: Page Fault Linear Address.
  * CR3: Page Directory Base Register (PML4 physical address).
  * CR4: Physical Address Extension (PAE) and OS support flags.
* **RFLAGS Register**: Displays raw 64-bit flag register and individual status indicators:
  * CF (Carry Flag)
  * ZF (Zero Flag)
  * SF (Sign Flag)
  * IF (Interrupt Enable Flag)
  * DF (Direction Flag)
  * OF (Overflow Flag)

### Memory Inspection (`dump`)

The `dump` command enables direct inspection of physical and virtual memory in kernel space:
* Syntax: `dump <address> [length]` (address formatted in decimal or hexadecimal with prefix 0x).
* Output format: 16 bytes per line with address offset, individual hexadecimal byte values, and a printable ASCII character pane on the right (unprintable characters rendered as dots).
* Safety bounding: Default display length of 64 bytes, capped at a maximum of 512 bytes per invocation.

### Dynamic Assembly & Machine Code Execution (`asm`)

The `asm` command provides an interactive runtime environment for executing raw machine code:
* **Bytecode Execution**: Enter raw hexadecimal byte sequences (such as `asm 90 90 c3`).
* **RAM Execution Buffer**: The kernel copies bytecode into an executable memory buffer, validates or appends a trailing return instruction (opcode 0xC3), and calls the buffer directly as a 64-bit function.
* **Return Value Inspection**: Captures the state of the RAX register upon function return and displays its contents in both hexadecimal and decimal notation.
* **Mnemonic Quick Instructions**:
  * `asm nop`: Executes a hardware No-Operation instruction.
  * `asm cli`: Disables maskable hardware interrupts.
  * `asm sti`: Enables maskable hardware interrupts.
  * `asm rdtsc`: Reads the hardware Time-Stamp Counter into RAX and RDX and prints the tick count.
  * `asm cpuid`: Queries processor identification data via CPUID instruction.

### Integrated CLI Calculator (`calc`)

The `calc` command implements a recursive-descent mathematical parser designed for system developers:
* **Supported Operators**: Addition (+), Subtraction (-), Multiplication (*), Division (/), Modulo (%).
* **Precedence Rules**: Correct mathematical operator precedence with support for nested parentheses.
* **Numerical Formats**: Evaluates standard decimal numbers and hexadecimal values prefixed with 0x.
* **Fault Handling**: Validates syntax, guards against division by zero, cleanly aborts calculation, and sounds the 1 ms PC Speaker error tone upon encountering invalid expressions.
* **Result Display**: Outputs calculated values simultaneously in decimal and hexadecimal notation.

---

## 10. Hierarchical In-Memory Filesystem (RAMFS) & Audit Logging

### Storage Architecture

Storage is structured as a contiguous, fixed-allocation in-memory file table designed for static determinism, eliminating dynamic heap fragmentation during standard filesystem calls:
* Maximum registered nodes: 64 filesystem entries.
* Maximum path length: 64 characters.
* Maximum file payload: 2048 bytes per node.

### Filesystem Hierarchy

During bootstrapping, the kernel establishes a clean standard filesystem tree:
* **Root Directory**: Base of the hierarchical filesystem.
* **home**: Primary user workspace and default working directory upon shell initialization.
* **boot**: Protected bootloader directory containing bootloader configuration (grub.cfg) and bootstrap assembly (boot.s).
* **krnl**: Protected kernel directory containing kernel build parameters (kernel.sys) and symbol mapping tables (system.map).
* **shell**: Userland shell environment containing shell configuration (sh.cfg), message of the day (motd), and alias tables (aliases).
* **etc**: System configuration directory containing release metadata (os-release), machine hostname (hostname), and filesystem mount parameters (fstab).

### Path Resolution Engine

The kernel canonicalization algorithm:
* Supports both absolute paths (starting with root) and relative paths evaluated against the current working directory.
* Resolves dot (current directory) and dot-dot (parent directory) components iteratively.
* Clamps root traversal to prevent navigating above the filesystem root.

### Filesystem Audit Telemetry (`changes`)

All file operations are recorded in an internal circular audit log:
* `VIEW`: Triggered when viewing files in the text editor, displaying files via `cat`, or searching via `grep`.
* `EDITED`: Triggered when saving files in the text editor or overwriting files via output redirection.
* `CREATED`: Triggered when creating files via `touch`, directories via `makedir`, or redirecting to new nodes.
* `DELETED`: Triggered when removing files or directories via `rm`.

The complete history is displayed by issuing the `changes` command.

---

## 11. Full-Screen Text Editor "em" Deep Specification

"em" is a high-performance, full-screen text editor operating directly across the 80x25 VGA buffer:
* Rows 0 through 22: Document editing canvas with real-time vocabulary syntax validation.
* Row 23: Inverted status bar showing file mode, filename, cursor coordinates, and file size.
* Row 24: Command line bar activated when pressing the Alt key.

### Editor Modes
1. **Direct Writing Mode**:
   * Editor opens directly in text input mode. Characters, spaces, punctuation, and semicolons are inserted immediately into the buffer at the active cursor position.
2. **Command Mode**:
   * Activated exclusively by pressing the Left Alt key.
   * Row 24 displays the semicolon command prompt.
   * Command options:
     * `;wsc`: Write, save to RAMFS, log audit event, and close editor.
     * `;s`: Save document to RAMFS without closing.
     * `;q`: Quit editor without saving changes.
     * `;-m`: Hop cursor to the next detected typographical error.
   * Pressing Alt or Escape returns immediately to direct writing mode.

### Multi-Grammar Typo and Keyword Validation

The editor incorporates an integrated grammar detection and vocabulary validation engine:
* **Automatic Grammar Detection**: Analyzes file extensions and shebang lines to activate specialized dictionaries for Bash scripting, Python development, or Markdown documentation.
* **Visual Highlighting**: Valid keywords and recognized terms are rendered in clean white text, while unrecognized tokens and typos are highlighted in bright red.
* **Typo Hopping**: Typing `;-m` in command mode automatically advances the cursor to the next misspelled word or unknown keyword in the document buffer.

### Critical System File Protection Dialog

Attempting to open critical operating system assets (files residing within the boot or kernel directories) triggers an interactive security dialog:
* Prompts the user with a high-visibility warning detailing the risks of modifying vital system files.
* Navigable buttons: `[ OK ]` highlighted in green and `[ EXIT ]` highlighted in red.
* Controllable using Arrow Keys, Tab, Enter, or Escape.

---

## 12. Operating System Mapping Workbook

A comprehensive hardware, memory, and architecture mapping workbook has been constructed as an Excel spreadsheet:

* **Sheet 1 - System & Architecture**: Documents CPU mode, privilege rings, stack boundaries, compiler flags, and kernel entry parameters.
* **Sheet 2 - Memory Map & Paging**: Documents physical memory allocation, 4-level paging hierarchies, 2 MiB huge page mappings, and heap boundaries.
* **Sheet 3 - I/O Port Map**: Comprehensive table of x86 I/O ports including PIC controllers, PIT timers, keyboard controllers, VGA CRT registers, and PC Speaker ports.
* **Sheet 4 - IDT & Interrupts**: Documents all 256 interrupt gates, CPU architecture exceptions, and hardware IRQ routing.
* **Sheet 5 - CLI & Shell Commands**: Complete command matrix documenting all 23 native commands, signatures, and operational semantics.

---

## 13. Building, Toolchain Prerequisites & Emulation

### Host Toolchain Prerequisites
* **GNU Compiler Collection (gcc)** with x86_64 freestanding target support.
* **GNU Assembler (as)** and **GNU Linker (ld)** supporting elf_x86_64 output.
* **GNU Make (make)**.
* **GRUB 2 (grub-mkrescue)** and **xorriso** for bootable ISO creation.
* **QEMU (qemu-system-x86_64)** for hardware emulation.

### Build Commands

```bash
# Clean intermediate object files and build binaries
make clean

# Compile kernel source files and link 64-bit ELF kernel binary
make

# Build bootable hybrid El Torito ISO image
make iso
```

### Running under QEMU

Execute the compiled ELF kernel binary directly:

```bash
qemu-system-x86_64 -kernel dismasmOS.bin
```

Or boot the complete hybrid ISO image with 256 MiB of emulated physical RAM:

```bash
qemu-system-x86_64 -cdrom dismasmOS.iso -m 256M
```

To test the German keyboard layout with full AltGr decoding under QEMU:

```bash
qemu-system-x86_64 -cdrom dismasmOS.iso -m 256M -k de
```

### Running under VirtualBox or Physical Hardware

1. Create a 64-bit virtual machine (type Other 64-bit, 128 MiB RAM or higher).
2. Attach the generated ISO image to the virtual optical drive.
3. Start the system. GRUB 2 will load the kernel, establish 64-bit Long Mode, and initialize the shell in the user workspace.

---

## 14. Comprehensive Technical Specification Matrix

```text
Processor Architecture:     x86_64 (AMD64 / Intel 64 Long Mode)
Operating Privilege:        Ring 0 (Supervisor Mode)
Paging Hierarchy:           4-Level Paging (PML4, PDPT, PD)
Identity Mapped Memory:     1 GiB Contiguous (512 x 2 MiB Huge Pages)
Executable Memory Flag:     Writable & Executable (0x83 Flag, No NX-bit)
Dynamic Heap Memory:        16 MiB Contiguous Freestanding Heap (0x01000000)
Heap Allocator Features:    malloc, free, calloc, realloc, 16-byte alignment
Binary Format:              ELF 64-bit LSB Executable (x86-64)
Boot Protocol:              Multiboot 1 Compliant (Header Magic 0x1BADB002)
Kernel Entry Physical Addr: 0x00100000 (1 MiB Physical Boundary)
Stack Configuration:        32 KiB 16-byte Aligned Stack
Interrupt Architecture:     Dual 8259A Cascaded PIC (Remapped to 0x20-0x2F)
IDT Structure:              256 Gates (64-Bit Gate Descriptors, 16 Bytes each)
Video Subsystem:            VGA Color Text Mode 80x25 @ 0x000B8000
Color Palette:              16 Colors with Terminal 1-Bit Solid Block Spot
Acoustic Subsystem:         Hardware PC Speaker (PIT Ch 2 @ 3000 Hz, 1 ms)
Keyboard Hardware:          Intel 8042 PS/2 Microcontroller (IRQ 1)
Keyboard Keymaps:           German DIN 2137-2 (QWERTZ) & US (QWERTY)
Extended Input Decoding:    Hardware Arrow Keys, Home, End, Delete, AltGr, Left Alt
Shell Command Chaining:     && Operator with Fail-Fast Error Abort
Command History Buffer:     32-Entry Circular Recall via Up and Down Arrow Keys
Diagnostic Tools:           reg (CPU & Control Registers), dump (Hex/ASCII Memory)
Computation Subsystem:      calc (Integer & Hexadecimal Arithmetic)
Dynamic Execution Engine:   asm (Raw Bytecode & CPU Instructions in RAM)
Text Editor:                "em" with Spellchecking, Alt-Commands, Warning Modal
Filesystem Engine:          Hierarchical RAMFS (root, home, boot, krnl, shell, etc)
Filesystem Audit Logging:   changes Command (VIEW, EDITED, CREATED, DELETED)
Power Management:           ACPI VM Shutdown (0x604, 0xB004, 0x4004) & CLI HLT Loop
Standard Library Bindings:  0 (Strictly freestanding, -nostdlib, -ffreestanding)
```

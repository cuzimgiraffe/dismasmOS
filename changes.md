# Project Changes and Progress Log

## Summary of Planned Changes
- Implement the wdid command to function as a comprehensive manual viewer similar to Linux man.
- Provide extensive and detailed documentation in simple English for every available system command.
- Implement interactive document viewing and scrolling supporting:
  - Mouse scroll wheel navigation
  - Page Up and Page Down keyboard keys
  - Arrow keys, Home, End, Space, and exit controls
- Implement PS/2 mouse and scroll wheel driver support with IntelliMouse wheel packet decoding.
- Integrate the wdid command into the operating system shell and command dispatcher.
- Update help diagnostics to include wdid.

## Progress and Completed Milestones

### 1. Hardware Input Subsystems and PS/2 Mouse Driver
- Analyzed operating system architecture, build pipeline, input handlers, and VGA display routines.
- Verified x86_64 Long Mode kernel compilation environment in WSL.
- Implemented dedicated PS/2 mouse driver with auxiliary port enablement, controller configuration, and 8042 communication.
- Implemented IntelliMouse sample rate negotiation sequence to detect hardware scroll wheel capabilities.
- Added mouse interrupt service routine handling IRQ 12 across cascaded dual 8259A interrupt controllers.
- Integrated mouse packet stream decoding and translation of scroll wheel movements into input events.
- Extended keyboard input system with dedicated queue support for mouse scroll events and numpad navigation.

### 2. Interactive Document Pager Engine
- Designed full-screen manual pager for the 80x25 VGA text console.
- Allocated top 24 screen rows for document content with syntax-highlighted headers, keywords, and callouts.
- Built persistent status bar on row 24 displaying active command name, line number range, total line count, read percentage, and navigation controls.
- Integrated multi-modal scrolling:
  - Mouse Scroll Wheel Up scrolls 3 lines upward.
  - Mouse Scroll Wheel Down scrolls 3 lines downward.
  - Page Up and 'b' keys jump 20 lines upward.
  - Page Down and Space bar jump 20 lines downward.
  - Up Arrow and 'k' scroll 1 line upward.
  - Down Arrow and 'j' scroll 1 line downward.
  - Home and 'g' jump directly to document top.
  - End and 'G' jump directly to document bottom.
  - 'q', 'Q', and Escape exit the manual reader and restore the clean shell prompt.

### 3. Comprehensive English Manual Pages for All Commands
- Written extensive, detailed manual documentation in simple English for all 25 native commands and operators:
  - wdid: Manual pager and documentation engine.
  - help: Summary reference list of available commands.
  - clear: Screen buffer wiping and cursor reset utility.
  - colr: 16-color VGA palette inspector and 1-bit solid terminal spot tester.
  - heap: Kernel 16 MiB dynamic heap allocator status and diagnostic self-test suite.
  - calc: Recursive-descent integer and hexadecimal arithmetic expression evaluator.
  - dump: Memory hexadecimal and ASCII inspector.
  - reg: 64-bit general registers, control registers (CR0-CR4), and status flags inspector.
  - asm: Dynamic in-memory machine bytecode and x86 instruction execution engine.
  - lsf: RAM filesystem directory listing with sizes and permissions.
  - cd: Working directory navigator.
  - makedir: Directory node creator with audit logging.
  - changes: Real-time filesystem access and modification audit trail viewer.
  - em: Full-screen modal text editor with syntax grammar dictionaries and warning modals.
  - cat: Raw file text content streaming utility.
  - echo: Text argument printer and file redirection operator.
  - touch: Empty file creator in RAM filesystem.
  - rm: File and directory deletion tool with audit tracking.
  - grep: Line-by-line pattern matching search tool.
  - pr: Process management subsystem and daemon status inspector.
  - uname: Kernel, operating system release, architecture, and dedication banner.
  - MemRep: System-wide physical memory allocation and layout report.
  - lang: Keyboard layout switcher between German QWERTZ and US QWERTY.
  - shutdown: Clean operating system halt and hardware power-off sequence.
  - reboot: Machine reset using 8042 controller reset line pulse.
  - Chaining operator (&&): Sequential command execution pipeline with fail-fast abort.

### 4. Shell Integration and Verification
- Integrated wdid and man command dispatch into shell REPL evaluation loop.
- Updated shell help command to list wdid with pagination instructions.
- Updated system documentation overview and technical specification tables.
- Built clean hybrid bootable ISO image.
- Tested interactive boot, keyboard navigation, Page Down, Page Up, and exit routines inside QEMU with automated screen verification.

### 5. ATA PIO LBA28 Extent Filesystem & Dynamic Live-Monitoring 'pr'
- Implemented high-performance, bare-metal ATA PIO LBA28 Block/Extent Filesystem in NASM assembly:
  * Raw ATA ports 0x1F0-0x1F7 communication with BSY and DRQ status polling.
  * Direct 16-byte Table of Contents (ToC) records stored in Sector 0 (8 bytes Name, 4 bytes Start-LBA, 4 bytes Size).
  * Static BSS buffers (`toc_buffer`, `last_io_lba`, `last_ata_status`) with zero dynamic heap allocation.
  * Ultra-fast string block transfers via `rep outsw` (write) and `rep insw` (read).
  * Implemented CLI ToC directory inspector (`toc` / `atals`) and extent save/load routines (`atasave`, `ataload`).
- Re-architected `pr` into a continuous, real-time bare-metal hardware monitor:
  * Zero-flicker, direct VGA video memory writes to `0xB8000` with `stosw` and `lodsb`.
  * Live non-blocking polling on PS/2 keyboard controller ports 0x64 and 0x60 (exiting on ESC or 'q').
  * CMOS RTC live time polling on ports 0x70/0x71 with UIP sync and hardware BCD decoding.
  * Cycle-accurate timing using CPU `rdtsc` delta calculations and 8254 PIT channel 0 latching.
  * Live inspection of CPU general registers (EAX, EBX, ESP), control registers (CR0, CR3), and EFLAGS.
  * Real-time ATA status register flags (BSY, DRDY, ERR) and last I/O sector tracking.
  * PS/2 raw scancode display directly from port 0x60.
- Provided dual-architecture support (x86 32-bit Protected Mode and x86_64 Long Mode).

### 6. First-Boot NTFS 3.1 Auto-Formatting, Linux-Style Persistent Disk Selection & Hardware Media Tagging

#### English-Only System Standardization
- Audited and translated all kernel console logs, shell command descriptions, usage prompts, error diagnostics, filesystem utilities, and assembly live-monitoring interfaces into clear English.
- Preserved runtime keyboard layout switching while ensuring every system message, table header, manual entry, and code comment across C and assembly modules remains strictly in English.

#### Multi-Channel ATA Bus Discovery and Hardware Media Tagging
- Extended the bare-metal ATA PIO storage subsystem to scan all four IDE controller positions across the Primary (`0x1F0` / `0x3F6`) and Secondary (`0x170` / `0x376`) buses (Master and Slave slots), automatically skipping ATAPI optical drives via device signature detection (`0xEB14` and `0x9669`).
- Added slot-aware 28-bit and 48-bit LBA sector read/write routines and hardware write-cache flushing (`0xE7` and `0xEA`) so the kernel can inspect and operate on multiple attached disks independently.
- Implemented Linux-style hardware media classification by parsing the 256-word `ATA IDENTIFY DEVICE` (`0xEC`) structure:
  - **Virtual Disks (`VHD001` to `VHD999`)**: Identified by matching hypervisor model and serial signatures (including QEMU, KVM, VMware, VirtualBox, Hyper-V, Xen, Bochs, and VirtIO).
  - **Physical Solid-State Drives (`SD001` to `SD999`)**: Identified when ATA8-ACS Word 217 (Nominal Media Rotation Rate) reports `0x0001` (non-rotating solid-state media), when Word 169 Bit 0 advertises Data Set Management TRIM support, or when solid-state model identifiers are present.
  - **Physical Hard Disk Drives (`HD001` to `HD999`)**: Identified when Word 217 reports a physical spindle speed (`0x0401` through `0xFFFE` RPM) or for standard rotational magnetic ATA drives.
- Assigned sequential three-digit tags (`001` through `999`) per media category and reconciled runtime numbering against persisted NTFS volume labels so previously formatted disks retain their exact tag across reboots and hardware topology changes.

#### Bare-Metal In-Kernel NTFS 3.1 Formatter
- Built a complete standalone NTFS 3.1 volume formatter directly inside the kernel without relying on external formatting utilities:
  - Calculates volume geometry (`512` bytes per sector, `8` sectors / `4096` bytes per cluster) and generates a unique 64-bit Volume Serial Number (`UUID`) by combining CPU `RDTSC` timestamps, CMOS Real-Time Clock registers, and FNV-1a hashes of drive identity metadata mixed through SplitMix64.
  - Writes the primary Volume Boot Record in Sector 0 (OEM signature `NTFS    `, BIOS Parameter Block, `$MFT` and `$MFTMirr` cluster locations, record size descriptors, and `0xAA55` boot signature) together with a backup Volume Boot Record in the final sector of the disk.
  - Constructs all 16 reserved system Master File Table (`$MFT`) records (`1024` bytes per record with Update Sequence Array fixups) and writes the mirrored metadata records (`$MFT`, `$MFTMirr`, `$LogFile`, `$Volume`) to `$MFTMirr`.
  - Encodes the drive's assigned hardware tag (`VHD001`, `HD001`, `SD001`, etc.) as a UTF-16LE `$VOLUME_NAME` (`0x60`) attribute inside MFT Record 3 (`$Volume`) alongside `$VOLUME_INFORMATION` (`0x70`, NTFS version `3.1`).
  - Initializes the empty root directory `$I30` index in MFT Record 5, generates the 128 KiB `$UpCase` uppercase character mapping table (`65,536` UTF-16LE entries), marks allocated metadata clusters in `$Bitmap`, clears `$LogFile`, and issues an ATA cache flush to guarantee on-disk durability.

#### First-Boot Auto-Provisioning and Linux-Style UUID Persistence
- Implemented an automated boot-time volume discovery and mount workflow modeled after Linux `blkid` probing and static filesystem table (`fstab`) `UUID` binding:
  - **Non-Destructive Device Probing**: On every boot, the kernel probes each detected disk (supporting both raw whole-disk NTFS volumes and MBR partition tables) to extract its 64-bit NTFS `UUID`, `$Volume` tag label, and stored mount binding metadata.
  - **First-Boot Auto-Formatting**: When no formatted NTFS volume is found on any connected disk during first boot, the kernel automatically selects the first writable hard disk (`1 MiB` or larger), formats it as NTFS 3.1 using its classified hardware tag, mounts it, and records a persistent `UUID` and tag binding entry on disk.
  - **Subsequent-Boot Persistent Selection**: On all following boots, the kernel reads the stored `UUID` binding, scans all attached disks for the matching 64-bit NTFS Volume Serial Number, and mounts that exact disk—even if new blank disks are added in earlier controller slots or IDE cable order is rearranged.
- Synchronized the in-memory mount configuration table with the persistent NTFS volume metadata automatically during boot and file write operations.

#### Shell Integration, Documentation, and Verification
- Added `blkid` (with alias `lsblk`) to display all detected storage devices, media types, assigned hardware tags, volume labels, `UUID` values, and active mount status.
- Added `ntfsformat` (with alias `mkfs.ntfs`) for manual NTFS 3.1 formatting of detected drives by device name, hardware tag, or slot index, and added safety guards to raw Sector 0 Table of Contents commands when an NTFS volume is mounted.
- Added comprehensive English manual pages for the new storage and formatting commands in the `wdid` documentation viewer.
- Verified end-to-end in headless QEMU across multi-boot tests covering first-boot virtual disk formatting (`VHD001`), persistent file survival across IDE slot swaps alongside secondary unformatted drives (`VHD002`), solid-state drive detection (`SD001` at non-rotational rate `1`), and rotational hard disk detection (`HD001` at `7200` RPM).




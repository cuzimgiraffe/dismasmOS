; ==============================================================================
; dismasmOS - Block / Extent-Based ATA-PIO LBA28 Filesystem
; ==============================================================================
; Specification:
; - Raw ATA-PIO ports (0x1F0-0x1F7) in LBA28 mode
; - Minimal Table of Contents (ToC) in Sector 0:
;   * 16-byte entries: 8-byte name, 4-byte start LBA, 4-byte byte size
;   * 32 entries maximum (512 bytes / 16 bytes = 32)
; - No dynamic heap usage, no complex tables
; - Static buffers in BSS
; - Optimized string instructions: 'rep insw', 'rep outsw', 'cld', 'stosw'
; ==============================================================================

default rel

; ------------------------------------------------------------------------------
; ATA I/O Ports (Primary Bus)
; ------------------------------------------------------------------------------
ATA_PORT_DATA       equ 0x1F0
ATA_PORT_ERROR      equ 0x1F1
ATA_PORT_FEATURES   equ 0x1F1
ATA_PORT_SECCOUNT   equ 0x1F2
ATA_PORT_LBA_LO     equ 0x1F3
ATA_PORT_LBA_MID    equ 0x1F4
ATA_PORT_LBA_HI     equ 0x1F5
ATA_PORT_DRV_HEAD   equ 0x1F6
ATA_PORT_STATUS     equ 0x1F7
ATA_PORT_COMMAND    equ 0x1F7

; ATA Commands
ATA_CMD_READ_PIO    equ 0x20
ATA_CMD_WRITE_PIO   equ 0x30
ATA_CMD_CACHE_FLUSH equ 0xE7

; ATA Status Bits
ATA_SR_BSY          equ 0x80    ; Drive is busy
ATA_SR_DRDY         equ 0x40    ; Drive is ready
ATA_SR_DF           equ 0x20    ; Drive write fault
ATA_SR_DRQ          equ 0x08    ; Data request ready
ATA_SR_ERR          equ 0x01    ; Error bit

; ToC Specification
TOC_ENTRY_SIZE      equ 16      ; 8-byte name, 4-byte LBA, 4-byte size
TOC_MAX_ENTRIES     equ 32      ; 512 / 16 = 32 entries per sector

section .data
align 4
str_toc_hdr:
    db "================================================================================", 10
    db " [ATA ToC] Sector 0 LBA28 Block/Extent Filesystem", 10
    db "================================================================================", 10
    db " IDX  FILENAME    START-LBA   SIZE (BYTES)    SECTORS    RANGE", 10
    db "--------------------------------------------------------------------------------", 10, 0
str_toc_empty:
    db "  (No files found. Table of Contents is empty.)", 10, 0
str_toc_foot:
    db "--------------------------------------------------------------------------------", 10, 0
str_nl:
    db 10, 0

section .bss
align 16
global toc_buffer
global last_io_lba
global last_ata_status

; Fixed static buffers in BSS - zero dynamic heap allocation!
toc_buffer:         resb 512    ; 1 sector for Table of Contents
last_io_lba:        resd 1      ; Last accessed LBA28 sector
last_ata_status:    resb 1      ; Last status byte read from port 0x1F7
str_num_buf:        resb 16     ; Scratch buffer for numeric formatting

section .text

global ata_lba28_wait_bsy
global ata_lba28_wait_drq
global ata_lba28_read_sectors
global ata_lba28_write_sectors
global fs_ata_save_file
global fs_ata_load_file
global fs_ata_list_toc

extern vga_puts
extern vga_putchar
extern vga_set_color

; ==============================================================================
; ata_400ns_delay: Reading the status port 4 times produces a standard 400ns delay
; ==============================================================================
ata_400ns_delay:
    mov dx, ATA_PORT_STATUS
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    ret

; ==============================================================================
; ata_lba28_wait_bsy: Status polling - wait until BSY (bit 7) is clear
; Return: EAX = 0 on success, 1 on timeout / floating bus (0xFF)
; ==============================================================================
ata_lba28_wait_bsy:
    mov dx, ATA_PORT_STATUS
    mov ecx, 1000000            ; Timeout guard against hangs
.poll_bsy:
    in al, dx
    mov [last_ata_status], al
    cmp al, 0xFF                ; Floating bus (no drive connected)
    je .err_nodrive
    test al, ATA_SR_BSY
    jz .bsy_clear
    dec ecx
    jnz .poll_bsy
.err_nodrive:
    mov eax, 1
    ret
.bsy_clear:
    xor eax, eax
    ret

; ==============================================================================
; ata_lba28_wait_drq: Status polling - wait until DRQ (bit 3) is set & no ERR
; Return: EAX = 0 on success, 1 on error
; ==============================================================================
ata_lba28_wait_drq:
    mov dx, ATA_PORT_STATUS
    mov ecx, 1000000
.poll_drq:
    in al, dx
    mov [last_ata_status], al
    test al, ATA_SR_ERR
    jnz .err
    test al, ATA_SR_DRQ
    jnz .drq_ready
    dec ecx
    jnz .poll_drq
.err:
    mov eax, 1
    ret
.drq_ready:
    xor eax, eax
    ret

; ==============================================================================
; ata_lba28_read_sectors:
; Reads contiguous sectors via 'rep insw' from the ATA interface
; Parameters (System V AMD64 ABI):
;   RDI = Start LBA (uint32_t)
;   RSI = Sector count (uint8_t, 1..255)
;   RDX = Destination buffer in RAM (void *)
; Return: EAX = 0 on success, -1 on error
; ==============================================================================
ata_lba28_read_sectors:
    push rbx
    push r12
    push r13
    push r14

    mov r12d, edi               ; R12D = current LBA
    mov r13d, esi               ; R13D = remaining sectors
    and r13d, 0xFF
    test r13d, r13d
    jz .read_done
    mov r14, rdx                ; R14 = RAM destination address

.read_sector_loop:
    ; 1. Record last LBA in BSS
    mov [last_io_lba], r12d

    ; 2. Wait for BSY clear
    call ata_lba28_wait_bsy
    test eax, eax
    jnz .read_fail

    ; 3. Select drive & LBA28 bits 24..27 (Master drive = 0xE0)
    mov eax, r12d
    shr eax, 24
    and al, 0x0F
    or al, 0xE0
    mov dx, ATA_PORT_DRV_HEAD
    out dx, al

    call ata_400ns_delay
    call ata_lba28_wait_bsy
    test eax, eax
    jnz .read_fail

    ; 4. Sector count (1 sector per iteration for strict polling)
    mov al, 1
    mov dx, ATA_PORT_SECCOUNT
    out dx, al

    ; 5. LBA Low (0..7)
    mov eax, r12d
    mov dx, ATA_PORT_LBA_LO
    out dx, al

    ; 6. LBA Mid (8..15)
    mov eax, r12d
    shr eax, 8
    mov dx, ATA_PORT_LBA_MID
    out dx, al

    ; 7. LBA High (16..23)
    mov eax, r12d
    shr eax, 16
    mov dx, ATA_PORT_LBA_HI
    out dx, al

    ; 8. Issue READ PIO command (0x20)
    mov al, ATA_CMD_READ_PIO
    mov dx, ATA_PORT_COMMAND
    out dx, al

    call ata_400ns_delay

    ; 9. Status polling: wait until DRQ is active
    call ata_lba28_wait_drq
    test eax, eax
    jnz .read_fail

    ; 10. Cycle-optimized read via 'rep insw' (256 words = 512 bytes)
    mov rdi, r14
    mov dx, ATA_PORT_DATA
    mov ecx, 256
    cld
    rep insw
    mov r14, rdi                ; RDI automatically incremented by 512

    ; Next sector
    inc r12d
    dec r13d
    jnz .read_sector_loop

.read_done:
    xor eax, eax
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.read_fail:
    mov eax, -1
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; ==============================================================================
; ata_lba28_write_sectors:
; Writes raw RAM data via 'rep outsw' with full status polling
; Parameters:
;   RDI = Start LBA (uint32_t)
;   RSI = Sector count (uint8_t, 1..255)
;   RDX = Source buffer in RAM (const void *)
; Return: EAX = 0 on success, -1 on error
; ==============================================================================
ata_lba28_write_sectors:
    push rbx
    push r12
    push r13
    push r14

    mov r12d, edi               ; R12D = current LBA
    mov r13d, esi               ; R13D = remaining sectors
    and r13d, 0xFF
    test r13d, r13d
    jz .write_done
    mov r14, rdx                ; R14 = RAM source address

.write_sector_loop:
    ; 1. Record last LBA in BSS
    mov [last_io_lba], r12d

    ; 2. Wait for BSY clear
    call ata_lba28_wait_bsy
    test eax, eax
    jnz .write_fail

    ; 3. Select Drive / Head
    mov eax, r12d
    shr eax, 24
    and al, 0x0F
    or al, 0xE0
    mov dx, ATA_PORT_DRV_HEAD
    out dx, al

    call ata_400ns_delay
    call ata_lba28_wait_bsy
    test eax, eax
    jnz .write_fail

    ; 4. Sector count = 1
    mov al, 1
    mov dx, ATA_PORT_SECCOUNT
    out dx, al

    ; 5. LBA Low (0..7)
    mov eax, r12d
    mov dx, ATA_PORT_LBA_LO
    out dx, al

    ; 6. LBA Mid (8..15)
    mov eax, r12d
    shr eax, 8
    mov dx, ATA_PORT_LBA_MID
    out dx, al

    ; 7. LBA High (16..23)
    mov eax, r12d
    shr eax, 16
    mov dx, ATA_PORT_LBA_HI
    out dx, al

    ; 8. Issue WRITE PIO command (0x30)
    mov al, ATA_CMD_WRITE_PIO
    mov dx, ATA_PORT_COMMAND
    out dx, al

    call ata_400ns_delay

    ; 9. Status polling: wait until DRQ is active
    call ata_lba28_wait_drq
    test eax, eax
    jnz .write_fail

    ; 10. Cycle-optimized write via 'rep outsw' (256 words = 512 bytes)
    mov rsi, r14
    mov dx, ATA_PORT_DATA
    mov ecx, 256
    cld
    rep outsw
    mov r14, rsi                ; RSI automatically incremented by 512

    ; 11. Wait for BSY clear after sector write
    call ata_lba28_wait_bsy
    test eax, eax
    jnz .write_fail

    ; Next sector
    inc r12d
    dec r13d
    jnz .write_sector_loop

    ; 12. Issue ATA Cache Flush command (0xE7) for persistence
    mov al, ATA_CMD_CACHE_FLUSH
    mov dx, ATA_PORT_COMMAND
    out dx, al
    call ata_400ns_delay
    call ata_lba28_wait_bsy

.write_done:
    xor eax, eax
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.write_fail:
    mov eax, -1
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; ==============================================================================
; fs_ata_save_file:
; Writes raw RAM data to contiguous sectors (block extent) and registers the
; file in the Table of Contents (ToC) in Sector 0.
; Parameters:
;   RDI = Filename (8-byte ASCII pointer)
;   RSI = Data pointer in RAM (const void *)
;   RDX = File size in bytes (uint32_t)
; Return: EAX = 0 on success, negative on error
; ==============================================================================
fs_ata_save_file:
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov r12, rdi                ; R12 = Name (8 bytes)
    mov r13, rsi                ; R13 = RAM data
    mov r14d, edx               ; R14D = File size in bytes

    test r14d, r14d
    jz .save_err_invalid

    ; 1. Load ToC (Sector 0, 1 sector) into static BSS buffer toc_buffer
    xor edi, edi                ; LBA = 0
    mov esi, 1                  ; 1 sector
    lea rdx, [toc_buffer]
    call ata_lba28_read_sectors
    test eax, eax
    jnz .save_err_io

    ; 2. Compute sector count for file: (size + 511) / 512
    mov eax, r14d
    add eax, 511
    shr eax, 9                  ; / 512
    mov r15d, eax               ; R15D = required sectors

    ; 3. Scan ToC for free slot or matching existing entry
    ;    Track highest occupied LBA simultaneously for extent allocation
    lea rbx, [toc_buffer]
    xor ecx, ecx                ; ECX = slot index (0..31)
    mov edx, -1                 ; EDX = first free slot (-1 = none yet)
    mov esi, 1                  ; ESI = next free LBA (starts at 1, LBA 0 is ToC)

.scan_toc_save:
    cmp byte [rbx], 0           ; Is entry empty?
    jne .entry_occupied

    ; Record free slot if none found yet
    cmp edx, -1
    jne .next_slot
    mov edx, ecx
    jmp .next_slot

.entry_occupied:
    ; Check whether 8-byte filename matches
    mov rax, [rbx]
    mov r8, [r12]
    cmp rax, r8
    je .found_matching_entry

    ; Compute highest LBA: start_lba + sectors
    mov eax, [rbx + 8]          ; start_lba
    mov r8d, [rbx + 12]         ; size
    add r8d, 511
    shr r8d, 9                  ; sectors
    add eax, r8d                ; end_lba
    cmp eax, esi
    jbe .next_slot
    mov esi, eax                ; new max_lba

.next_slot:
    add rbx, TOC_ENTRY_SIZE
    inc ecx
    cmp ecx, TOC_MAX_ENTRIES
    jb .scan_toc_save

    ; No matching entry found: use free slot
    cmp edx, -1
    je .save_err_full           ; ToC is full (all 32 entries occupied)
    mov ecx, edx                ; Index of free slot
    lea rbx, [toc_buffer]
    shl edx, 4                  ; * 16 bytes
    add rbx, rdx
    mov edi, esi                ; Start LBA = next free LBA
    jmp .write_extent

.found_matching_entry:
    ; Overwrite existing file at its current start LBA
    mov edi, [rbx + 8]          ; Existing start LBA

.write_extent:
    push rbx                    ; Save ToC entry pointer
    push rdi                    ; Save start LBA

    ; 4. Write raw data to contiguous sectors via 'rep outsw'
    ;    RDI = start_lba, RSI = sectors, RDX = ram_data
    mov esi, r15d
    mov rdx, r13
    call ata_lba28_write_sectors
    pop rdi                     ; Restore start LBA
    pop rbx                     ; Restore ToC entry pointer
    test eax, eax
    jnz .save_err_io

    ; 5. Update ToC entry
    ; Copy 8-byte name via qword mov
    mov rax, [r12]
    mov [rbx], rax
    ; Store start LBA (offset +8, 4 bytes)
    mov [rbx + 8], edi
    ; Store byte size (offset +12, 4 bytes)
    mov [rbx + 12], r14d

    ; 6. Write ToC (Sector 0) back to disk
    xor edi, edi                ; LBA 0
    mov esi, 1                  ; 1 sector
    lea rdx, [toc_buffer]
    call ata_lba28_write_sectors
    test eax, eax
    jnz .save_err_io

    xor eax, eax                ; Success!
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.save_err_invalid:
    mov eax, -1
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.save_err_full:
    mov eax, -2
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

.save_err_io:
    mov eax, -3
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; ==============================================================================
; fs_ata_load_file:
; Looks up a file by 8-byte name in the ToC and loads it into RAM via 'rep insw'.
; Parameters:
;   RDI = Filename (8-byte ASCII pointer)
;   RSI = Destination buffer in RAM (void *)
; Return: RAX = Bytes read, or -1 if not found / I/O error
; ==============================================================================
fs_ata_load_file:
    push rbx
    push r12
    push r13

    mov r12, rdi                ; R12 = search name (8 bytes)
    mov r13, rsi                ; R13 = destination buffer in RAM

    ; 1. Read ToC (Sector 0, 1 sector)
    xor edi, edi                ; LBA 0
    mov esi, 1
    lea rdx, [toc_buffer]
    call ata_lba28_read_sectors
    test eax, eax
    jnz .load_fail

    ; 2. Search ToC for filename
    lea rbx, [toc_buffer]
    xor ecx, ecx

.scan_toc_load:
    cmp byte [rbx], 0
    je .load_next_slot

    ; Compare 8-byte name
    mov rax, [rbx]
    mov rdx, [r12]
    cmp rax, rdx
    je .load_match_found

.load_next_slot:
    add rbx, TOC_ENTRY_SIZE
    inc ecx
    cmp ecx, TOC_MAX_ENTRIES
    jb .scan_toc_load

    ; Not found
    mov rax, -1
    pop r13
    pop r12
    pop rbx
    ret

.load_match_found:
    ; Read metadata from ToC entry
    mov edi, [rbx + 8]          ; EDI = start LBA
    mov ebx, [rbx + 12]         ; EBX = file size in bytes

    ; Compute required sectors: (size + 511) / 512
    mov eax, ebx
    add eax, 511
    shr eax, 9
    mov esi, eax                ; ESI = sector count
    mov rdx, r13                ; RDX = destination buffer

    push rbx                    ; Save byte size
    call ata_lba28_read_sectors
    pop rbx
    test eax, eax
    jnz .load_fail

    ; Return file size in bytes
    mov eax, ebx
    pop r13
    pop r12
    pop rbx
    ret

.load_fail:
    mov rax, -1
    pop r13
    pop r12
    pop rbx
    ret

; ==============================================================================
; fs_ata_list_toc:
; CLI command routine: Lists all files stored in the Sector 0 ToC.
; ==============================================================================
fs_ata_list_toc:
    push rbx
    push r12
    push r13

    ; 1. Read ToC from LBA 0
    xor edi, edi
    mov esi, 1
    lea rdx, [toc_buffer]
    call ata_lba28_read_sectors
    test eax, eax
    jz .toc_read_ok

    ; Print read error
    mov rdi, 0x0C               ; Red
    xor rsi, rsi
    call vga_set_color
    lea rdi, [.str_io_err]
    call vga_puts
    mov rdi, 0x0F
    xor rsi, rsi
    call vga_set_color
    pop r13
    pop r12
    pop rbx
    ret

.str_io_err: db "Error: Could not read Table of Contents from ATA LBA 0!", 10, 0

.toc_read_ok:
    ; Print header
    mov rdi, 0x0B               ; Cyan
    xor rsi, rsi
    call vga_set_color
    lea rdi, [str_toc_hdr]
    call vga_puts

    mov rdi, 0x0F               ; White
    xor rsi, rsi
    call vga_set_color

    lea rbx, [toc_buffer]
    xor r12d, r12d              ; R12D = index (0..31)
    xor r13d, r13d              ; R13D = count of found files

.toc_entry_loop:
    cmp byte [rbx], 0
    je .toc_loop_next

    ; File entry found!
    inc r13d

    ; 1. Print index: "[XX] "
    mov rdi, '['
    call vga_putchar
    mov eax, r12d
    call print_dec2
    mov rdi, ']'
    call vga_putchar
    mov rdi, ' '
    call vga_putchar
    mov rdi, ' '
    call vga_putchar

    ; 2. Print 8-byte filename (ASCII)
    mov rdi, 0x0A               ; Light green
    xor rsi, rsi
    call vga_set_color
    xor ecx, ecx
.name_loop:
    movzx rdi, byte [rbx + rcx]
    test dil, dil
    jnz .print_char
    mov dil, ' '
.print_char:
    push rcx
    call vga_putchar
    pop rcx
    inc ecx
    cmp ecx, 8
    jb .name_loop

    mov rdi, 0x0F               ; White
    xor rsi, rsi
    call vga_set_color
    mov rdi, ' '
    call vga_putchar
    mov rdi, ' '
    call vga_putchar

    ; 3. Print start LBA in hex: "0xXXXXXXXX"
    lea rdi, [.str_hex_prefix]
    call vga_puts
    mov eax, [rbx + 8]
    call print_hex32
    mov rdi, ' '
    call vga_putchar
    mov rdi, ' '
    call vga_putchar

    ; 4. Print size in bytes
    mov eax, [rbx + 12]
    call print_dec_padded
    mov rdi, ' '
    call vga_putchar
    mov rdi, ' '
    call vga_putchar

    ; 5. Compute and print sector count
    mov eax, [rbx + 12]
    add eax, 511
    shr eax, 9
    push rax
    call print_dec_padded
    mov rdi, ' '
    call vga_putchar
    mov rdi, ' '
    call vga_putchar
    pop rcx                     ; RCX = sectors

    ; 6. Print LBA range: LBA .. LBA+sectors-1
    mov eax, [rbx + 8]
    call print_dec
    lea rdi, [.str_range_sep]
    call vga_puts
    mov eax, [rbx + 8]
    add eax, ecx
    dec eax
    call print_dec
    lea rdi, [str_nl]
    call vga_puts

.toc_loop_next:
    add rbx, TOC_ENTRY_SIZE
    inc r12d
    cmp r12d, TOC_MAX_ENTRIES
    jb .toc_entry_loop

    ; Check whether any files were found
    test r13d, r13d
    jnz .toc_show_footer

    mov rdi, 0x0E               ; Yellow
    xor rsi, rsi
    call vga_set_color
    lea rdi, [str_toc_empty]
    call vga_puts

.toc_show_footer:
    mov rdi, 0x0B               ; Cyan
    xor rsi, rsi
    call vga_set_color
    lea rdi, [str_toc_foot]
    call vga_puts
    mov rdi, 0x0F
    xor rsi, rsi
    call vga_set_color

    pop r13
    pop r12
    pop rbx
    ret

.str_hex_prefix: db "0x", 0
.str_range_sep:  db "..", 0

; ------------------------------------------------------------------------------
; Internal compact formatting helpers
; ------------------------------------------------------------------------------

; print_hex32: Prints 8-digit hex value from EAX
print_hex32:
    push rbx
    mov edx, eax
    mov ecx, 8
.h_loop:
    rol edx, 4
    mov al, dl
    and al, 0x0F
    cmp al, 9
    jbe .d_digit
    add al, 'A' - 10 - '0'
.d_digit:
    add al, '0'
    movzx rdi, al
    push rcx
    push rdx
    call vga_putchar
    pop rdx
    pop rcx
    dec ecx
    jnz .h_loop
    pop rbx
    ret

; print_dec2: Prints 2-digit number from EAX (with leading zero)
print_dec2:
    xor edx, edx
    mov ecx, 10
    div ecx
    push rdx
    add al, '0'
    movzx rdi, al
    call vga_putchar
    pop rdx
    add dl, '0'
    movzx rdi, dl
    call vga_putchar
    ret

; print_dec: Prints decimal integer from EAX
print_dec:
    push rbx
    lea rbx, [str_num_buf + 15]
    mov byte [rbx], 0
    mov ecx, 10
.dec_loop:
    xor edx, edx
    div ecx
    add dl, '0'
    dec rbx
    mov [rbx], dl
    test eax, eax
    jnz .dec_loop
    mov rdi, rbx
    call vga_puts
    pop rbx
    ret

; print_dec_padded: Prints right-aligned decimal integer (width 8)
print_dec_padded:
    push rbx
    push r12
    lea rbx, [str_num_buf + 15]
    mov byte [rbx], 0
    mov ecx, 10
    xor r12d, r12d
.pdec_loop:
    xor edx, edx
    div ecx
    add dl, '0'
    dec rbx
    mov [rbx], dl
    inc r12d
    test eax, eax
    jnz .pdec_loop

    ; Print padding spaces (8 - length)
    mov eax, 8
    sub eax, r12d
    jbe .pdec_out
    mov ecx, eax
.pad_sp:
    push rcx
    mov rdi, ' '
    call vga_putchar
    pop rcx
    dec ecx
    jnz .pad_sp

.pdec_out:
    mov rdi, rbx
    call vga_puts
    pop r12
    pop rbx
    ret

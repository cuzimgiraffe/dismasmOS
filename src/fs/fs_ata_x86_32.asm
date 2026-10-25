; ==============================================================================
; dismasmOS / Bare-Metal x86 - Pure 32-Bit Protected Mode Filesystem
; ==============================================================================
; Specification:
; - Architecture: x86 (IA-32, 32-Bit Protected Mode)
; - Raw ATA-PIO ports (0x1F0-0x1F7) in LBA28 mode
; - Minimal Table of Contents (ToC) in Sector 0:
;   * Fixed 16-byte entries: 8-byte name, 4-byte start LBA, 4-byte byte size
;   * 32 entries maximum (512 bytes / 16 bytes = 32)
; - No dynamic heap usage, fixed static buffers in BSS
; - Optimized string instructions: 'rep insw', 'rep outsw', 'cld', 'stosw'
; ==============================================================================

[BITS 32]

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
ATA_SR_BSY          equ 0x80
ATA_SR_DRDY         equ 0x40
ATA_SR_DF           equ 0x20
ATA_SR_DRQ          equ 0x08
ATA_SR_ERR          equ 0x01

; ToC Specification
TOC_ENTRY_SIZE      equ 16
TOC_MAX_ENTRIES     equ 32

section .data
align 4
str_toc_hdr_32:
    db "================================================================================", 10
    db " [ATA ToC] Sector 0 LBA28 Block/Extent Filesystem (x86 32-Bit PM)", 10
    db "================================================================================", 10
    db " IDX  FILENAME    START-LBA   SIZE (BYTES)    SECTORS    RANGE", 10
    db "--------------------------------------------------------------------------------", 10, 0
str_toc_empty_32:
    db "  (No files found. Table of Contents is empty.)", 10, 0
str_toc_foot_32:
    db "--------------------------------------------------------------------------------", 10, 0
str_nl_32:
    db 10, 0

section .bss
align 16
global toc_buffer_32
global last_io_lba_32
global last_ata_status_32

; Fixed static buffers in BSS - zero dynamic heap allocation!
toc_buffer_32:      resb 512    ; 1 sector for Table of Contents
last_io_lba_32:     resd 1      ; Last accessed LBA28 sector
last_ata_status_32: resb 1      ; Last status byte read from port 0x1F7
num_buf_32:         resb 16     ; Scratch buffer for numeric output

section .text

global ata_wait_bsy_32
global ata_wait_drq_32
global ata_read_sectors_32
global ata_write_sectors_32
global fs_ata_save_file_32
global fs_ata_load_file_32
global fs_ata_list_toc_32

extern vga_puts
extern vga_putchar
extern vga_set_color

; ==============================================================================
; ata_400ns_delay_32: Reading status port 4 times produces a standard 400ns delay
; ==============================================================================
ata_400ns_delay_32:
    mov dx, ATA_PORT_STATUS
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    ret

; ==============================================================================
; ata_wait_bsy_32: Status polling - wait until BSY (bit 7) is clear
; ==============================================================================
ata_wait_bsy_32:
    mov dx, ATA_PORT_STATUS
    mov ecx, 1000000
.poll_bsy:
    in al, dx
    mov [last_ata_status_32], al
    cmp al, 0xFF
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
; ata_wait_drq_32: Status polling - wait until DRQ (bit 3) is set & no ERR
; ==============================================================================
ata_wait_drq_32:
    mov dx, ATA_PORT_STATUS
    mov ecx, 1000000
.poll_drq:
    in al, dx
    mov [last_ata_status_32], al
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
; ata_read_sectors_32:
; Reads contiguous sectors via 'rep insw' using x86 32-bit cdecl calling model
; Stack: [ebp+8]  = uint32_t lba
;        [ebp+12] = uint8_t  count
;        [ebp+16] = void    *buffer
; ==============================================================================
ata_read_sectors_32:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi

    mov ebx, [ebp + 8]          ; EBX = start LBA
    movzx ecx, byte [ebp + 12]  ; ECX = sector count
    test ecx, ecx
    jz .read_done_32
    mov edi, [ebp + 16]         ; EDI = RAM destination buffer

.sector_loop_rd:
    mov [last_io_lba_32], ebx

    call ata_wait_bsy_32
    test eax, eax
    jnz .read_fail_32

    ; Master Drive | LBA bits 24..27
    mov eax, ebx
    shr eax, 24
    and al, 0x0F
    or al, 0xE0
    mov dx, ATA_PORT_DRV_HEAD
    out dx, al

    call ata_400ns_delay_32
    call ata_wait_bsy_32
    test eax, eax
    jnz .read_fail_32

    ; Sector count = 1
    mov al, 1
    mov dx, ATA_PORT_SECCOUNT
    out dx, al

    ; LBA Low
    mov eax, ebx
    mov dx, ATA_PORT_LBA_LO
    out dx, al

    ; LBA Mid
    mov eax, ebx
    shr eax, 8
    mov dx, ATA_PORT_LBA_MID
    out dx, al

    ; LBA High
    mov eax, ebx
    shr eax, 16
    mov dx, ATA_PORT_LBA_HI
    out dx, al

    ; Command: 0x20
    mov al, ATA_CMD_READ_PIO
    mov dx, ATA_PORT_COMMAND
    out dx, al

    call ata_400ns_delay_32
    call ata_wait_drq_32
    test eax, eax
    jnz .read_fail_32

    ; Read 256 words via 'rep insw'
    push ecx
    mov dx, ATA_PORT_DATA
    mov ecx, 256
    cld
    rep insw                    ; EDI automatically incremented by 512 bytes
    pop ecx

    inc ebx
    dec ecx
    jnz .sector_loop_rd

.read_done_32:
    xor eax, eax
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

.read_fail_32:
    mov eax, -1
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; ==============================================================================
; ata_write_sectors_32:
; Writes contiguous sectors via 'rep outsw' using x86 32-bit cdecl calling model
; Stack: [ebp+8]  = uint32_t lba
;        [ebp+12] = uint8_t  count
;        [ebp+16] = const void *buffer
; ==============================================================================
ata_write_sectors_32:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi

    mov ebx, [ebp + 8]          ; EBX = start LBA
    movzx ecx, byte [ebp + 12]  ; ECX = sector count
    test ecx, ecx
    jz .write_done_32
    mov esi, [ebp + 16]         ; ESI = RAM source buffer

.sector_loop_wr:
    mov [last_io_lba_32], ebx

    call ata_wait_bsy_32
    test eax, eax
    jnz .write_fail_32

    ; Master Drive | LBA bits 24..27
    mov eax, ebx
    shr eax, 24
    and al, 0x0F
    or al, 0xE0
    mov dx, ATA_PORT_DRV_HEAD
    out dx, al

    call ata_400ns_delay_32
    call ata_wait_bsy_32
    test eax, eax
    jnz .write_fail_32

    ; Sector count = 1
    mov al, 1
    mov dx, ATA_PORT_SECCOUNT
    out dx, al

    ; LBA Low
    mov eax, ebx
    mov dx, ATA_PORT_LBA_LO
    out dx, al

    ; LBA Mid
    mov eax, ebx
    shr eax, 8
    mov dx, ATA_PORT_LBA_MID
    out dx, al

    ; LBA High
    mov eax, ebx
    shr eax, 16
    mov dx, ATA_PORT_LBA_HI
    out dx, al

    ; Command: 0x30
    mov al, ATA_CMD_WRITE_PIO
    mov dx, ATA_PORT_COMMAND
    out dx, al

    call ata_400ns_delay_32
    call ata_wait_drq_32
    test eax, eax
    jnz .write_fail_32

    ; Write 256 words via 'rep outsw'
    push ecx
    mov dx, ATA_PORT_DATA
    mov ecx, 256
    cld
    rep outsw                   ; ESI automatically incremented by 512 bytes
    pop ecx

    call ata_wait_bsy_32
    test eax, eax
    jnz .write_fail_32

    inc ebx
    dec ecx
    jnz .sector_loop_wr

    ; Flush cache
    mov al, ATA_CMD_CACHE_FLUSH
    mov dx, ATA_PORT_COMMAND
    out dx, al
    call ata_400ns_delay_32
    call ata_wait_bsy_32

.write_done_32:
    xor eax, eax
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

.write_fail_32:
    mov eax, -1
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; ==============================================================================
; fs_ata_save_file_32:
; Saves file to ATA sectors and updates ToC in Sector 0.
; Stack: [ebp+8]  = const char *name8
;        [ebp+12] = const void *data
;        [ebp+16] = uint32_t size
; ==============================================================================
fs_ata_save_file_32:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi

    mov edx, [ebp + 16]         ; EDX = byte size
    test edx, edx
    jz .save_err_32

    ; 1. Read ToC (Sector 0)
    push toc_buffer_32
    push 1
    push 0
    call ata_read_sectors_32
    add esp, 12
    test eax, eax
    jnz .save_err_32

    ; 2. Compute sector count: (size + 511) / 512
    mov eax, [ebp + 16]
    add eax, 511
    shr eax, 9
    mov edi, eax                ; EDI = sector count

    ; 3. Find free slot or matching entry
    mov esi, toc_buffer_32
    xor ecx, ecx
    mov edx, -1                 ; Free slot index
    mov ebx, 1                  ; Next free LBA

.scan_save_32:
    cmp byte [esi], 0
    jne .occ_32
    cmp edx, -1
    jne .next_s_32
    mov edx, ecx
    jmp .next_s_32

.occ_32:
    ; Compare 8-byte name (2x dwords in 32-bit mode)
    mov eax, [esi]
    mov edi, [ebp + 8]
    cmp eax, [edi]
    jne .chk_max_32
    mov eax, [esi + 4]
    cmp eax, [edi + 4]
    je .found_match_32

.chk_max_32:
    mov eax, [esi + 8]
    mov edi, [esi + 12]
    add edi, 511
    shr edi, 9
    add eax, edi
    cmp eax, ebx
    jbe .next_s_32
    mov ebx, eax

.next_s_32:
    add esi, TOC_ENTRY_SIZE
    inc ecx
    cmp ecx, TOC_MAX_ENTRIES
    jb .scan_save_32

    cmp edx, -1
    je .save_err_32
    ; Configure free slot
    mov esi, toc_buffer_32
    shl edx, 4
    add esi, edx
    jmp .do_write_32

.found_match_32:
    mov ebx, [esi + 8]          ; Reuse existing LBA

.do_write_32:
    ; Compute sector count
    mov eax, [ebp + 16]
    add eax, 511
    shr eax, 9

    ; Write data sectors
    push dword [ebp + 12]       ; Buffer
    push eax                    ; Sectors
    push ebx                    ; Start LBA
    call ata_write_sectors_32
    add esp, 12
    test eax, eax
    jnz .save_err_32

    ; Populate ToC entry
    mov edi, [ebp + 8]
    mov eax, [edi]
    mov [esi], eax
    mov eax, [edi + 4]
    mov [esi + 4], eax
    mov [esi + 8], ebx          ; Start LBA
    mov eax, [ebp + 16]
    mov [esi + 12], eax         ; Byte size

    ; Write ToC (Sector 0) back to disk
    push toc_buffer_32
    push 1
    push 0
    call ata_write_sectors_32
    add esp, 12
    test eax, eax
    jnz .save_err_32

    xor eax, eax
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

.save_err_32:
    mov eax, -1
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; ==============================================================================
; fs_ata_load_file_32:
; Loads file by 8-byte name into RAM via 'rep insw'.
; Stack: [ebp+8]  = const char *name8
;        [ebp+12] = void *dest
; ==============================================================================
fs_ata_load_file_32:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi

    ; 1. Read ToC
    push toc_buffer_32
    push 1
    push 0
    call ata_read_sectors_32
    add esp, 12
    test eax, eax
    jnz .load_err_32

    mov esi, toc_buffer_32
    xor ecx, ecx

.scan_load_32:
    cmp byte [esi], 0
    je .next_ld_32

    mov edi, [ebp + 8]
    mov eax, [esi]
    cmp eax, [edi]
    jne .next_ld_32
    mov eax, [esi + 4]
    cmp eax, [edi + 4]
    je .match_load_32

.next_ld_32:
    add esi, TOC_ENTRY_SIZE
    inc ecx
    cmp ecx, TOC_MAX_ENTRIES
    jb .scan_load_32

.load_err_32:
    mov eax, -1
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

.match_load_32:
    mov ebx, [esi + 8]          ; LBA
    mov edi, [esi + 12]         ; Byte size

    mov eax, edi
    add eax, 511
    shr eax, 9                  ; Sectors

    push edi                    ; Save size
    push dword [ebp + 12]       ; Dest
    push eax                    ; Sector count
    push ebx                    ; Start LBA
    call ata_read_sectors_32
    add esp, 12
    pop edi
    test eax, eax
    jnz .load_err_32

    mov eax, edi                ; Return file size in bytes
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; ==============================================================================
; fs_ata_list_toc_32:
; CLI routine for x86 32-bit mode to list all files in ToC
; ==============================================================================
fs_ata_list_toc_32:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi

    push toc_buffer_32
    push 1
    push 0
    call ata_read_sectors_32
    add esp, 12
    test eax, eax
    jz .list_ok_32
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

.list_ok_32:
    push 0
    push 0x0B
    call vga_set_color
    add esp, 8

    push str_toc_hdr_32
    call vga_puts
    add esp, 4

    mov esi, toc_buffer_32
    xor ebx, ebx                ; Index
    xor edx, edx                ; Count

.entry_loop_32:
    cmp byte [esi], 0
    je .skip_ent_32

    inc edx
    push edx

    push '['
    call vga_putchar
    add esp, 4

    mov eax, ebx
    call print_dec2_32

    push ']'
    call vga_putchar
    add esp, 4

    push ' '
    call vga_putchar
    call vga_putchar
    add esp, 8

    ; Print 8-character name
    xor ecx, ecx
.pr_name_32:
    movzx eax, byte [esi + ecx]
    test al, al
    jnz .ch_ok_32
    mov al, ' '
.ch_ok_32:
    push ecx
    push eax
    call vga_putchar
    add esp, 4
    pop ecx
    inc ecx
    cmp ecx, 8
    jb .pr_name_32

    push ' '
    call vga_putchar
    call vga_putchar
    add esp, 8

    ; Start LBA
    mov eax, [esi + 8]
    call print_hex32_32

    push ' '
    call vga_putchar
    call vga_putchar
    add esp, 8

    ; Byte size
    mov eax, [esi + 12]
    call print_dec_32

    push str_nl_32
    call vga_puts
    add esp, 4

    pop edx

.skip_ent_32:
    add esi, TOC_ENTRY_SIZE
    inc ebx
    cmp ebx, TOC_MAX_ENTRIES
    jb .entry_loop_32

    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; 32-bit helper functions
print_hex32_32:
    push ebx
    mov edx, eax
    mov ecx, 8
.h_lp:
    rol edx, 4
    mov al, dl
    and al, 0x0F
    cmp al, 9
    jbe .d_dg
    add al, 'A' - 10 - '0'
.d_dg:
    add al, '0'
    movzx eax, al
    push ecx
    push edx
    push eax
    call vga_putchar
    add esp, 4
    pop edx
    pop ecx
    dec ecx
    jnz .h_lp
    pop ebx
    ret

print_dec2_32:
    xor edx, edx
    mov ecx, 10
    div ecx
    push edx
    add al, '0'
    movzx eax, al
    push eax
    call vga_putchar
    add esp, 4
    pop edx
    add dl, '0'
    movzx edx, dl
    push edx
    call vga_putchar
    add esp, 4
    ret

print_dec_32:
    push ebx
    lea ebx, [num_buf_32 + 15]
    mov byte [ebx], 0
    mov ecx, 10
.dc_lp:
    xor edx, edx
    div ecx
    add dl, '0'
    dec ebx
    mov [ebx], dl
    test eax, eax
    jnz .dc_lp
    push ebx
    call vga_puts
    add esp, 4
    pop ebx
    ret

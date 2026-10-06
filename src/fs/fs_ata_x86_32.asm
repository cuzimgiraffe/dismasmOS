; ==============================================================================
; dismasmOS / Bare-Metal x86 - Reines 32-Bit Protected Mode Dateisystem
; ==============================================================================
; Spezifikation:
; - Architektur: x86 (IA-32, 32-Bit Protected Mode)
; - Rohe ATA-PIO-Ports (0x1F0-0x1F7) im LBA28-Modus
; - Minimale Table of Contents (ToC) in Sektor 0:
;   * Feste 16-Byte-Eintraege: 8 Bytes Name, 4 Bytes Start-LBA, 4 Bytes Groesse
;   * 32 Eintraege max (512 Bytes / 16 Bytes = 32)
; - Keine dynamische Heap-Nutzung, feste statische Puffer im BSS
; - Optimierte String-Instruktionen: 'rep insw', 'rep outsw', 'cld', 'stosw'
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

; ToC Spezifikation
TOC_ENTRY_SIZE      equ 16
TOC_MAX_ENTRIES     equ 32

section .data
align 4
str_toc_hdr_32:
    db "================================================================================", 10
    db " [ATA ToC] Sektor 0 LBA28 Block/Extent Dateisystem (x86 32-Bit PM)", 10
    db "================================================================================", 10
    db " IDX  DATEINAME   START-LBA   GROESSE (BYTE)  SEKTOREN   BEREICH", 10
    db "--------------------------------------------------------------------------------", 10, 0
str_toc_empty_32:
    db "  (Keine Dateien vorhanden. Table of Contents ist leer.)", 10, 0
str_toc_foot_32:
    db "--------------------------------------------------------------------------------", 10, 0
str_nl_32:
    db 10, 0

section .bss
align 16
global toc_buffer_32
global last_io_lba_32
global last_ata_status_32

; Feste statische Puffer im BSS - Keine dynamische Heap-Allozierung!
toc_buffer_32:      resb 512    ; 1 Sektor fuer Table of Contents
last_io_lba_32:     resd 1      ; Letzter zugegriffener LBA28-Sektor
last_ata_status_32: resb 1      ; Zuletzt von Port 0x1F7 gelesenes Status-Byte
num_buf_32:         resb 16     ; Scratch-Puffer fuer Zahlenausgabe

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
; ata_400ns_delay_32: 4x Lesen des Statusports erzeugt standardkonforme 400ns Pause
; ==============================================================================
ata_400ns_delay_32:
    mov dx, ATA_PORT_STATUS
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    ret

; ==============================================================================
; ata_wait_bsy_32: Status-Polling - Warte bis BSY (Bit 7) geloescht ist
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
; ata_wait_drq_32: Status-Polling - Warte bis DRQ (Bit 3) gesetzt und kein ERR
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
; Liest fortlaufende Sektoren via 'rep insw' im x86 32-Bit cdecl Aufrufmodell
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

    mov ebx, [ebp + 8]          ; EBX = Start-LBA
    movzx ecx, byte [ebp + 12]  ; ECX = Sektoranzahl
    test ecx, ecx
    jz .read_done_32
    mov edi, [ebp + 16]         ; EDI = RAM-Zielpuffer

.sector_loop_rd:
    mov [last_io_lba_32], ebx

    call ata_wait_bsy_32
    test eax, eax
    jnz .read_fail_32

    ; Master Drive | LBA Bits 24..27
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

    ; Sektoranzahl = 1
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

    ; 256 Words via 'rep insw' einlesen
    push ecx
    mov dx, ATA_PORT_DATA
    mov ecx, 256
    cld
    rep insw                    ; EDI wird automatisch um 512 Bytes inkrementiert
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
; Schreibt fortlaufende Sektoren via 'rep outsw' im x86 32-Bit cdecl Aufrufmodell
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

    mov ebx, [ebp + 8]          ; EBX = Start-LBA
    movzx ecx, byte [ebp + 12]  ; ECX = Sektoranzahl
    test ecx, ecx
    jz .write_done_32
    mov esi, [ebp + 16]         ; ESI = RAM-Quellpuffer

.sector_loop_wr:
    mov [last_io_lba_32], ebx

    call ata_wait_bsy_32
    test eax, eax
    jnz .write_fail_32

    ; Master Drive | LBA Bits 24..27
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

    ; Sektoranzahl = 1
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

    ; 256 Words via 'rep outsw' schreiben
    push ecx
    mov dx, ATA_PORT_DATA
    mov ecx, 256
    cld
    rep outsw                   ; ESI wird automatisch um 512 Bytes inkrementiert
    pop ecx

    call ata_wait_bsy_32
    test eax, eax
    jnz .write_fail_32

    inc ebx
    dec ecx
    jnz .sector_loop_wr

    ; Flush Cache
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
; Speichert Datei auf ATA-Sektoren und aktualisiert ToC in Sektor 0.
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

    mov edx, [ebp + 16]         ; EDX = Groesse
    test edx, edx
    jz .save_err_32

    ; 1. ToC (Sektor 0) einlesen
    push toc_buffer_32
    push 1
    push 0
    call ata_read_sectors_32
    add esp, 12
    test eax, eax
    jnz .save_err_32

    ; 2. Sektoren berechnen: (size + 511) / 512
    mov eax, [ebp + 16]
    add eax, 511
    shr eax, 9
    mov edi, eax                ; EDI = Sektoranzahl

    ; 3. Freien Slot oder passenden Eintrag suchen
    mov esi, toc_buffer_32
    xor ecx, ecx
    mov edx, -1                 ; Freier Slot Index
    mov ebx, 1                  ; Naechste freie LBA

.scan_save_32:
    cmp byte [esi], 0
    jne .occ_32
    cmp edx, -1
    jne .next_s_32
    mov edx, ecx
    jmp .next_s_32

.occ_32:
    ; 8-Byte Namen vergleichen (2x Dwords in 32-Bit)
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
    ; Freien Slot konfigurieren
    mov esi, toc_buffer_32
    shl edx, 4
    add esi, edx
    jmp .do_write_32

.found_match_32:
    mov ebx, [esi + 8]          ; Alte LBA weiterverwenden

.do_write_32:
    ; Sektoren berechnen
    mov eax, [ebp + 16]
    add eax, 511
    shr eax, 9

    ; Daten schreiben
    push dword [ebp + 12]       ; Puffer
    push eax                    ; Sektoren
    push ebx                    ; Start-LBA
    call ata_write_sectors_32
    add esp, 12
    test eax, eax
    jnz .save_err_32

    ; ToC-Eintrag fuellen
    mov edi, [ebp + 8]
    mov eax, [edi]
    mov [esi], eax
    mov eax, [edi + 4]
    mov [esi + 4], eax
    mov [esi + 8], ebx          ; Start-LBA
    mov eax, [ebp + 16]
    mov [esi + 12], eax         ; Groesse

    ; ToC (Sektor 0) zurueckschreiben
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
; Laedt Datei anhand des Namens via 'rep insw' in den RAM.
; Stack: [ebp+8]  = const char *name8
;        [ebp+12] = void *dest
; ==============================================================================
fs_ata_load_file_32:
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi

    ; 1. ToC einlesen
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
    mov edi, [esi + 12]         ; Groesse

    mov eax, edi
    add eax, 511
    shr eax, 9                  ; Sektoren

    push edi                    ; Groesse sichern
    push dword [ebp + 12]       ; Dest
    push eax                    ; Sektoranzahl
    push ebx                    ; Start-LBA
    call ata_read_sectors_32
    add esp, 12
    pop edi
    test eax, eax
    jnz .load_err_32

    mov eax, edi                ; Dateigroesse zurueckgeben
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret

; ==============================================================================
; fs_ata_list_toc_32:
; CLI-Befehl fuer x86 32-Bit zum Auflisten aller Dateien im ToC
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

    ; Name ausgeben (8 Zeichen)
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

    ; Groesse
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

; Hilfsfunktionen fuer 32-Bit
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

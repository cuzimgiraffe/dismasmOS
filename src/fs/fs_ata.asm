; ==============================================================================
; dismasmOS - Block-/Extent-basiertes ATA-PIO LBA28 Dateisystem
; ==============================================================================
; Spezifikation:
; - Rohe ATA-PIO-Ports (0x1F0-0x1F7) im LBA28-Modus
; - Minimale Table of Contents (ToC) in Sektor 0:
;   * 16-Byte Eintraege: 8 Bytes Name, 4 Bytes Start-LBA, 4 Bytes Groesse
;   * 32 Eintraege max (512 Bytes / 16 Bytes = 32)
; - Keine dynamische Heap-Nutzung, keine komplexen Tabellen
; - Statische Puffer im BSS
; - Optimierte String-Instruktionen: 'rep insw', 'rep outsw', 'cld', 'stosw'
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

; ToC Spezifikation
TOC_ENTRY_SIZE      equ 16      ; 8 Bytes Name, 4 Bytes LBA, 4 Bytes Size
TOC_MAX_ENTRIES     equ 32      ; 512 / 16 = 32 Eintraege pro Sektor

section .data
align 4
str_toc_hdr:
    db "================================================================================", 10
    db " [ATA ToC] Sektor 0 LBA28 Block/Extent Dateisystem", 10
    db "================================================================================", 10
    db " IDX  DATEINAME   START-LBA   GROESSE (BYTE)  SEKTOREN   BEREICH", 10
    db "--------------------------------------------------------------------------------", 10, 0
str_toc_empty:
    db "  (Keine Dateien vorhanden. Table of Contents ist leer.)", 10, 0
str_toc_foot:
    db "--------------------------------------------------------------------------------", 10, 0
str_nl:
    db 10, 0

section .bss
align 16
global toc_buffer
global last_io_lba
global last_ata_status

; Feste statische Puffer im BSS - Keine dynamische Heap-Allozierung!
toc_buffer:         resb 512    ; 1 Sektor fuer Table of Contents
last_io_lba:        resd 1      ; Letzter zugegriffener LBA28-Sektor
last_ata_status:    resb 1      ; Zuletzt von Port 0x1F7 gelesenes Status-Byte
str_num_buf:        resb 16     ; Scratch-Puffer fuer Zahlenausgabe

section .text

global ata_wait_bsy
global ata_wait_drq
global ata_read_sectors
global ata_write_sectors
global fs_ata_save_file
global fs_ata_load_file
global fs_ata_list_toc

extern vga_puts
extern vga_putchar
extern vga_set_color

; ==============================================================================
; ata_400ns_delay: 4x Lesen des Statusports erzeugt standardkonforme 400ns Pause
; ==============================================================================
ata_400ns_delay:
    mov dx, ATA_PORT_STATUS
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    ret

; ==============================================================================
; ata_wait_bsy: Status-Polling - Warte bis BSY (Bit 7) geloescht ist
; Rueckgabe: EAX = 0 bei Erfolg, 1 bei Timeout/Floating Bus (0xFF)
; ==============================================================================
ata_wait_bsy:
    mov dx, ATA_PORT_STATUS
    mov ecx, 1000000            ; Timeout-Schutz gegen Haengen
.poll_bsy:
    in al, dx
    mov [last_ata_status], al
    cmp al, 0xFF                ; Floating bus (kein Laufwerk verbunden)
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
; ata_wait_drq: Status-Polling - Warte bis DRQ (Bit 3) gesetzt und kein ERR (Bit 0)
; Rueckgabe: EAX = 0 bei Erfolg, 1 bei Fehler
; ==============================================================================
ata_wait_drq:
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
; ata_read_sectors:
; Liest fortlaufende Sektoren via 'rep insw' von der ATA-Schnittstelle
; Parameter (System V AMD64 ABI):
;   RDI = Start-LBA (uint32_t)
;   RSI = Sektoranzahl (uint8_t, 1..255)
;   RDX = Zielpuffer im RAM (void *)
; Rueckgabe: EAX = 0 bei Erfolg, -1 bei Fehler
; ==============================================================================
ata_read_sectors:
    push rbx
    push r12
    push r13
    push r14

    mov r12d, edi               ; R12D = aktuelle LBA
    mov r13d, esi               ; R13D = verbleibende Sektoren
    and r13d, 0xFF
    test r13d, r13d
    jz .read_done
    mov r14, rdx                ; R14 = RAM-Zieladresse

.read_sector_loop:
    ; 1. Letzten LBA im BSS protokollieren
    mov [last_io_lba], r12d

    ; 2. BSY abwarten
    call ata_wait_bsy
    test eax, eax
    jnz .read_fail

    ; 3. Laufwerk & LBA28 Bits 24..27 waehlen (Master-Laufwerk = 0xE0)
    mov eax, r12d
    shr eax, 24
    and al, 0x0F
    or al, 0xE0
    mov dx, ATA_PORT_DRV_HEAD
    out dx, al

    call ata_400ns_delay
    call ata_wait_bsy
    test eax, eax
    jnz .read_fail

    ; 4. Sektoranzahl (1 Sektor pro Iteration fuer exaktes Polling)
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

    ; 8. READ PIO Befehl (0x20) senden
    mov al, ATA_CMD_READ_PIO
    mov dx, ATA_PORT_COMMAND
    out dx, al

    call ata_400ns_delay

    ; 9. Status-Polling: Warten bis DRQ aktiv ist
    call ata_wait_drq
    test eax, eax
    jnz .read_fail

    ; 10. Zyklenoptimiertes Einlesen via 'rep insw' (256 Words = 512 Bytes)
    mov rdi, r14
    mov dx, ATA_PORT_DATA
    mov ecx, 256
    cld
    rep insw
    mov r14, rdi                ; RDI wurde automatisch um 512 erhoeht

    ; Naechster Sektor
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
; ata_write_sectors:
; Schreibt Rohdaten aus dem RAM via 'rep outsw' inklusive Status-Polling
; Parameter:
;   RDI = Start-LBA (uint32_t)
;   RSI = Sektoranzahl (uint8_t, 1..255)
;   RDX = Quellpuffer im RAM (const void *)
; Rueckgabe: EAX = 0 bei Erfolg, -1 bei Fehler
; ==============================================================================
ata_write_sectors:
    push rbx
    push r12
    push r13
    push r14

    mov r12d, edi               ; R12D = aktuelle LBA
    mov r13d, esi               ; R13D = verbleibende Sektoren
    and r13d, 0xFF
    test r13d, r13d
    jz .write_done
    mov r14, rdx                ; R14 = RAM-Quelladresse

.write_sector_loop:
    ; 1. Letzten LBA im BSS protokollieren
    mov [last_io_lba], r12d

    ; 2. BSY abwarten
    call ata_wait_bsy
    test eax, eax
    jnz .write_fail

    ; 3. Drive / Head waehlen
    mov eax, r12d
    shr eax, 24
    and al, 0x0F
    or al, 0xE0
    mov dx, ATA_PORT_DRV_HEAD
    out dx, al

    call ata_400ns_delay
    call ata_wait_bsy
    test eax, eax
    jnz .write_fail

    ; 4. Sektoranzahl = 1
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

    ; 8. WRITE PIO Befehl (0x30) senden
    mov al, ATA_CMD_WRITE_PIO
    mov dx, ATA_PORT_COMMAND
    out dx, al

    call ata_400ns_delay

    ; 9. Status-Polling: Warten bis DRQ aktiv ist
    call ata_wait_drq
    test eax, eax
    jnz .write_fail

    ; 10. Zyklenoptimiertes Schreiben via 'rep outsw' (256 Words = 512 Bytes)
    mov rsi, r14
    mov dx, ATA_PORT_DATA
    mov ecx, 256
    cld
    rep outsw
    mov r14, rsi                ; RSI wurde automatisch um 512 erhoeht

    ; 11. BSY nach Schreibvorgang abwarten
    call ata_wait_bsy
    test eax, eax
    jnz .write_fail

    ; Naechster Sektor
    inc r12d
    dec r13d
    jnz .write_sector_loop

    ; 12. ATA Cache Flush Befehl (0xE7) zur Persistierung
    mov al, ATA_CMD_CACHE_FLUSH
    mov dx, ATA_PORT_COMMAND
    out dx, al
    call ata_400ns_delay
    call ata_wait_bsy

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
; Speichert Rohdaten aus dem RAM auf fortlaufende Sektoren (Block-Extent)
; und traegt die Datei in die Table of Contents (ToC) in Sektor 0 ein.
; Parameter:
;   RDI = Dateiname (8-Byte ASCII Pointer)
;   RSI = Datenzeiger im RAM (const void *)
;   RDX = Dateigroesse in Bytes (uint32_t)
; Rueckgabe: EAX = 0 bei Erfolg, negativ bei Fehler
; ==============================================================================
fs_ata_save_file:
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov r12, rdi                ; R12 = Name (8 Bytes)
    mov r13, rsi                ; R13 = RAM-Daten
    mov r14d, edx               ; R14D = Dateigroesse in Bytes

    test r14d, r14d
    jz .save_err_invalid

    ; 1. ToC (Sektor 0, 1 Sektor) in statischen BSS-Puffer toc_buffer laden
    xor edi, edi                ; LBA = 0
    mov esi, 1                  ; 1 Sektor
    lea rdx, [toc_buffer]
    call ata_read_sectors
    test eax, eax
    jnz .save_err_io

    ; 2. Sektoranzahl fuer Datei berechnen: (size + 511) / 512
    mov eax, r14d
    add eax, 511
    shr eax, 9                  ; / 512
    mov r15d, eax               ; R15D = Benoetigte Sektoren

    ; 3. ToC nach freiem Slot oder bestehendem Eintrag durchsuchen
    ;    Gleichzeitig hoechste belegte LBA zur Allokation ermitteln
    lea rbx, [toc_buffer]
    xor ecx, ecx                ; ECX = Slot-Index (0..31)
    mov edx, -1                 ; EDX = Gefundener freier Slot (-1 = noch keiner)
    mov esi, 1                  ; ESI = Naechste freie LBA (Start bei 1, LBA 0 ist ToC)

.scan_toc_save:
    cmp byte [rbx], 0           ; Ist Eintrag leer?
    jne .entry_occupied

    ; Freier Slot gemerkt, falls noch keiner
    cmp edx, -1
    jne .next_slot
    mov edx, ecx
    jmp .next_slot

.entry_occupied:
    ; Pruefen, ob Dateiname uebereinstimmt (8 Bytes Vergleich)
    mov rax, [rbx]
    mov r8, [r12]
    cmp rax, r8
    je .found_matching_entry

    ; Hoechste LBA ermitteln: start_lba + sectors
    mov eax, [rbx + 8]          ; start_lba
    mov r8d, [rbx + 12]         ; size
    add r8d, 511
    shr r8d, 9                  ; sectors
    add eax, r8d                ; end_lba
    cmp eax, esi
    jbe .next_slot
    mov esi, eax                ; neue max_lba

.next_slot:
    add rbx, TOC_ENTRY_SIZE
    inc ecx
    cmp ecx, TOC_MAX_ENTRIES
    jb .scan_toc_save

    ; Kein uebereinstimmender Eintrag gefunden: Freien Slot nutzen
    cmp edx, -1
    je .save_err_full           ; ToC ist voll (32 Eintraege belegt)
    mov ecx, edx                ; Index des freien Slots
    lea rbx, [toc_buffer]
    shl edx, 4                  ; * 16 Bytes
    add rbx, rdx
    mov edi, esi                ; Start-LBA = naechste freie LBA
    jmp .write_extent

.found_matching_entry:
    ; Existierende Datei ueberschreiben: Wenn Platz reicht, alte LBA behalten
    mov edi, [rbx + 8]          ; Vorhandene Start-LBA

.write_extent:
    push rbx                    ; ToC-Eintragsadresse sichern
    push rdi                    ; Start-LBA sichern

    ; 4. Rohdaten auf fortlaufende Sektoren via 'rep outsw' schreiben
    ;    RDI = start_lba, RSI = sectors, RDX = ram_data
    mov esi, r15d
    mov rdx, r13
    call ata_write_sectors
    pop rdi                     ; Start-LBA wiederherstellen
    pop rbx                     ; ToC-Eintrag wiederherstellen
    test eax, eax
    jnz .save_err_io

    ; 5. ToC-Eintrag aktualisieren
    ; Name kopieren (8 Bytes via qword mov)
    mov rax, [r12]
    mov [rbx], rax
    ; Start-LBA eintragen (Offset +8, 4 Bytes)
    mov [rbx + 8], edi
    ; Groesse eintragen (Offset +12, 4 Bytes)
    mov [rbx + 12], r14d

    ; 6. ToC (Sektor 0) zurueck auf die Festplatte schreiben
    xor edi, edi                ; LBA 0
    mov esi, 1                  ; 1 Sektor
    lea rdx, [toc_buffer]
    call ata_write_sectors
    test eax, eax
    jnz .save_err_io

    xor eax, eax                ; Erfolg!
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
; Sucht Datei nach 8-Byte Namen in der ToC und laedt sie via 'rep insw' in den RAM.
; Parameter:
;   RDI = Dateiname (8-Byte ASCII Pointer)
;   RSI = Zielpuffer im RAM (void *)
; Rueckgabe: RAX = Gelesene Dateigroesse in Bytes, oder -1 wenn nicht gefunden/Fehler
; ==============================================================================
fs_ata_load_file:
    push rbx
    push r12
    push r13

    mov r12, rdi                ; R12 = Suchname (8 Bytes)
    mov r13, rsi                ; R13 = Zielpuffer im RAM

    ; 1. ToC (Sektor 0, 1 Sektor) einlesen
    xor edi, edi                ; LBA 0
    mov esi, 1
    lea rdx, [toc_buffer]
    call ata_read_sectors
    test eax, eax
    jnz .load_fail

    ; 2. ToC nach Dateinamen durchsuchen
    lea rbx, [toc_buffer]
    xor ecx, ecx

.scan_toc_load:
    cmp byte [rbx], 0
    je .load_next_slot

    ; 8-Byte Namen vergleichen
    mov rax, [rbx]
    mov rdx, [r12]
    cmp rax, rdx
    je .load_match_found

.load_next_slot:
    add rbx, TOC_ENTRY_SIZE
    inc ecx
    cmp ecx, TOC_MAX_ENTRIES
    jb .scan_toc_load

    ; Nicht gefunden
    mov rax, -1
    pop r13
    pop r12
    pop rbx
    ret

.load_match_found:
    ; Metadaten aus ToC-Eintrag auslesen
    mov edi, [rbx + 8]          ; EDI = Start-LBA
    mov ebx, [rbx + 12]         ; EBX = Dateigroesse in Bytes

    ; Benoetigte Sektoren berechnen: (size + 511) / 512
    mov eax, ebx
    add eax, 511
    shr eax, 9
    mov esi, eax                ; ESI = Sektoranzahl
    mov rdx, r13                ; RDX = Zielpuffer

    push rbx                    ; Groesse sichern
    call ata_read_sectors
    pop rbx
    test eax, eax
    jnz .load_fail

    ; Rueckgabe: Dateigroesse in Bytes
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
; CLI-Befehlsroutine: Listet alle vorhandenen Dateien in der ToC auf.
; ==============================================================================
fs_ata_list_toc:
    push rbx
    push r12
    push r13

    ; 1. ToC von LBA 0 einlesen
    xor edi, edi
    mov esi, 1
    lea rdx, [toc_buffer]
    call ata_read_sectors
    test eax, eax
    jz .toc_read_ok

    ; Lesefehler ausgeben
    mov rdi, 0x0C               ; Rot
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

.str_io_err: db "Fehler: Table of Contents konnte nicht von ATA LBA 0 gelesen werden!", 10, 0

.toc_read_ok:
    ; Header ausgeben
    mov rdi, 0x0B               ; Cyan
    xor rsi, rsi
    call vga_set_color
    lea rdi, [str_toc_hdr]
    call vga_puts

    mov rdi, 0x0F               ; Weiss
    xor rsi, rsi
    call vga_set_color

    lea rbx, [toc_buffer]
    xor r12d, r12d              ; R12D = Index (0..31)
    xor r13d, r13d              ; R13D = Anzahl gefundener Dateien

.toc_entry_loop:
    cmp byte [rbx], 0
    je .toc_loop_next

    ; Datei gefunden!
    inc r13d

    ; 1. Index ausgeben: "[XX] "
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

    ; 2. 8-Byte Dateinamen ausgeben (ASCII)
    mov rdi, 0x0A               ; Gruen
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

    mov rdi, 0x0F               ; Weiss
    xor rsi, rsi
    call vga_set_color
    mov rdi, ' '
    call vga_putchar
    mov rdi, ' '
    call vga_putchar

    ; 3. Start-LBA als Hex ausgeben: "0xXXXXXXXX"
    lea rdi, [.str_hex_prefix]
    call vga_puts
    mov eax, [rbx + 8]
    call print_hex32
    mov rdi, ' '
    call vga_putchar
    mov rdi, ' '
    call vga_putchar

    ; 4. Groesse in Bytes ausgeben
    mov eax, [rbx + 12]
    call print_dec_padded
    mov rdi, ' '
    call vga_putchar
    mov rdi, ' '
    call vga_putchar

    ; 5. Sektoren berechnen und ausgeben
    mov eax, [rbx + 12]
    add eax, 511
    shr eax, 9
    push rax
    call print_dec_padded
    mov rdi, ' '
    call vga_putchar
    mov rdi, ' '
    call vga_putchar
    pop rcx                     ; RCX = Sektoren

    ; 6. LBA-Bereich ausgeben: LBA .. LBA+sectors-1
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

    ; Pruefen, ob Dateien gefunden wurden
    test r13d, r13d
    jnz .toc_show_footer

    mov rdi, 0x0E               ; Gelb
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
; Interne Hilfsroutinen zur kompakten formatierten Ausgabe
; ------------------------------------------------------------------------------

; print_hex32: Schreibt 8-stelligen Hexwert aus EAX
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

; print_dec2: Schreibt 2-stellige Zahl aus EAX (fuehrende Null)
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

; print_dec: Schreibt Dezimalzahl aus EAX
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

; print_dec_padded: Schreibt Dezimalzahl rechtsbuendig formatiert (8 Zeichen)
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

    ; Padding-Leerzeichen ausgeben (8 - laenge)
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

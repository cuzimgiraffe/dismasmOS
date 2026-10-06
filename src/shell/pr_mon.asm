; ==============================================================================
; dismasmOS - Dynamischer Live-Monitoring-Befehl ('pr')
; ==============================================================================
; Spezifikation:
; - Kontinuierliche Aktualisierung in einer Schleife bis ESC (0x01) oder 'q' (0x10)
;   Abfrage ueber Tastatur-Ports 0x60 (Daten) und 0x64 (Status)
; - Direktes Schreiben in den VGA-Textspeicher (0xB8000) -> Flackerfrei, kein String-Overhead
; - Echte dynamische Hardware-Werte:
;   * RTC-Uhrzeit live aus CMOS-Ports 0x70/0x71 mit BCD-Konvertierung
;   * PIT-Ticks und TSC-Differenz zur Takt-/Laufzeitmessung (rdtsc, PIT 0x40)
;   * Registerwerte (EAX, EBX, ESP, CR0, CR3, EFLAGS)
;   * ATA-Status-Register (Port 0x1F7: BSY, DRDY, ERR) und letzter I/O-LBA-Sektor
;   * Letzter roher Tastatur-Scancode (Port 0x60)
; - Maximal zyklen- und speicheroptimiert (stosw, lodsb, rep stosw, minimale Spruenge)
; - Automatische Unterstuetzung fuer ELF32 (x86 32-Bit) und ELF64 (x86_64)
; ==============================================================================

%ifidn __OUTPUT_FORMAT__, elf32
    [BITS 32]
    %define REG_DI edi
    %define REG_SI esi
    %define REG_AX eax
    %define REG_BX ebx
    %define REG_CX ecx
    %define REG_DX edx
    %define REG_SP esp
    %define REG_BP ebp
%else
    [BITS 64]
    default rel
    %define REG_DI rdi
    %define REG_SI rsi
    %define REG_AX rax
    %define REG_BX rbx
    %define REG_CX rcx
    %define REG_DX rdx
    %define REG_SP rsp
    %define REG_BP rbp
%endif

VGA_BASE            equ 0xB8000
VGA_COLS            equ 80
VGA_ROWS            equ 25

; Farb-Attribute (Hintergrund | Vordergrund)
ATTR_HEADER         equ 0x1F    ; Weiss auf Blau
ATTR_DIVIDER        equ 0x03    ; Dunkelcyan auf Schwarz
ATTR_LABEL          equ 0x0B    ; Hellcyan auf Schwarz
ATTR_TEXT           equ 0x07    ; Hellgrau auf Schwarz
ATTR_VAL_WHITE      equ 0x0F    ; Weiss (intensiv)
ATTR_VAL_YELLOW     equ 0x0E    ; Gelb
ATTR_VAL_GREEN      equ 0x0A    ; Hellgruen
ATTR_VAL_RED        equ 0x0C    ; Hellrot
ATTR_FOOTER         equ 0x2F    ; Weiss auf Gruen

; Hardware-Ports
PORT_KBD_DATA       equ 0x60
PORT_KBD_STATUS     equ 0x64
PORT_RTC_INDEX      equ 0x70
PORT_RTC_DATA       equ 0x71
PORT_ATA_STATUS     equ 0x1F7
PORT_PIT_CH0        equ 0x40
PORT_PIT_CMD        equ 0x43

section .bss
align 16
global prev_tsc_low

; Statische BSS-Variablen (kein Heap!)
prev_tsc_low:       resd 1
snap_eax:           resd 1
snap_ebx:           resd 1
snap_esp:           resd 1
snap_cr0:           resd 1
snap_cr3:           resd 1
snap_eflags:        resd 1
snap_pit:           resw 1
last_scancode_pr:   resb 1
status_64_pr:       resb 1

; Externe BSS-Variablen aus dem ATA-Dateisystem
extern last_io_lba
extern last_ata_status

section .data
align 4
str_banner:
    db " [dismasmOS] BARE-METAL LIVE HARDWARE MONITOR ('pr') | [ESC] / [q] Beenden ", 0
str_div:
    times 80 db 0xCD
    db 0
str_sec_rtc:
    db " [CMOS RTC]      Uhrzeit:       ", 0
str_rtc_note:
    db " (Ports 0x70/0x71, BCD-dekodiert)", 0
str_sec_cpu:
    db " [CPU TIMING]    TSC-Zaehler:   0x", 0
str_sec_delta:
    db "  Delta/Frame: 0x", 0
str_sec_pit:
    db "  PIT-Ch0: 0x", 0
str_sec_reg1:
    db " [CPU REGISTERS] EAX: 0x", 0
str_sec_reg2:
    db "  EBX: 0x", 0
str_sec_reg3:
    db "  ESP: 0x", 0
str_sec_ctrl1:
    db " [CTRL & FLAGS]  CR0: 0x", 0
str_sec_ctrl2:
    db "  CR3: 0x", 0
str_sec_ctrl3:
    db "  FLG: 0x", 0
str_sec_ata1:
    db " [ATA DISK I/O]  Status (0x1F7): 0x", 0
str_sec_ata_bsy:
    db " [BSY:", 0
str_sec_ata_rdy:
    db " DRDY:", 0
str_sec_ata_err:
    db " ERR:", 0
str_bracket_close:
    db "]", 0
str_sec_lba:
    db "  Letzter I/O Sektor (LBA): 0x", 0
str_sec_kbd1:
    db " [KEYBOARD I/O]  Port 0x60 Scancode: 0x", 0
str_sec_kbd2:
    db "  Status Port 0x64: 0x", 0
str_info_title:
    db " [ARCHITEKTUR & LEISTUNGSMERKMALE]", 0
str_info_1:
    db " * Direkter VGA-Schreibzugriff auf 0x000B8000 (Vollstaendig flackerfrei)", 0
str_info_2:
    db " * Maximal optimierte String-Befehle: stosw, lodsb, rep stosw", 0
str_info_3:
    db " * RTC-Uhrzeit mit UIP-Synchronisation und hardwarenaher BCD-Wandlung", 0
str_info_4:
    db " * Hochpraezise Taktmessung via CPU RDTSC und PIT-Zaehler Ch0", 0
str_info_5:
    db " * Rohe Abfrage der Controller-Ports 0x1F7 (ATA) und 0x60/0x64 (PS/2)", 0
str_footer:
    db " LIVE REFRESH AKTIV | Zyklenoptimierte Endlosschleife | Druecke 'q' oder ESC zum Beenden", 0

section .text
global pr_monitor_loop

extern vga_clear
extern vga_update_cursor

; ==============================================================================
; pr_monitor_loop:
; Haupteinsprungpunkt des dynamischen 'pr'-Live-Monitors.
; Fuehrt eine kontinuierliche Aktualisierungsschleife aus, bis ESC oder 'q' gedrueckt wird.
; ==============================================================================
pr_monitor_loop:
%ifidn __OUTPUT_FORMAT__, elf32
    push ebx
    push ebp
    push esi
    push edi
%else
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
%endif

    ; 1. Initiale Register-Snapshots sichern (EAX, EBX, ESP)
    mov [snap_eax], eax
    mov [snap_ebx], ebx
    mov [snap_esp], esp

    ; Cursor in Ecke setzen
%ifidn __OUTPUT_FORMAT__, elf32
    push 0
    push 0
    call vga_update_cursor
    add esp, 8
%else
    xor edi, edi
    xor esi, esi
    call vga_update_cursor
%endif

    ; 2. Statisches Layout direkt in den VGA-Puffer (0xB8000) schreiben
    call render_static_layout

    ; 3. Initialer TSC-Wert
    rdtsc
    mov [prev_tsc_low], eax

    ; 4. Tastaturpuffer leeren vor Eintritt in die Live-Schleife
.drain_kbd_init:
    in al, PORT_KBD_STATUS
    test al, 0x01
    jz .kbd_ready
    in al, PORT_KBD_DATA
    jmp .drain_kbd_init
.kbd_ready:

    ; Interrupts temporaer maskieren waehrend des Live-Hardware-Pollings
    cli

; ==============================================================================
; LIVE-SCHLEIFE (Kontinuierliche Aktualisierung ohne Flackern)
; ==============================================================================
.live_loop:

    ; --------------------------------------------------------------------------
    ; A. Tastatur-Polling ueber Port 0x64 und Port 0x60
    ; --------------------------------------------------------------------------
    in al, PORT_KBD_STATUS
    mov [status_64_pr], al
    test al, 0x01               ; Bit 0 = Output Buffer Full?
    jz .no_key_pressed

    ; Scancode von Port 0x60 lesen
    in al, PORT_KBD_DATA
    mov [last_scancode_pr], al

    ; Abbruchbedingung pruefen:
    ; 0x01 = ESC (Make Code)
    cmp al, 0x01
    je .exit_monitor
    ; 0x10 = 'q' (Make Code im Set 1)
    cmp al, 0x10
    je .exit_monitor

.no_key_pressed:

    ; --------------------------------------------------------------------------
    ; B. RTC-Uhrzeit live aus CMOS-Ports 0x70/0x71 (mit BCD-Konvertierung)
    ; --------------------------------------------------------------------------
    ; UIP (Update In Progress, Bit 7 von Register 0x0A) abwarten
.wait_uip:
    mov al, 0x0A
    out PORT_RTC_INDEX, al
    in al, PORT_RTC_DATA
    test al, 0x80
    jnz .wait_uip

    ; Stunden lesen (Register 0x04)
    mov al, 0x04
    out PORT_RTC_INDEX, al
    in al, PORT_RTC_DATA
    mov cl, al                  ; CL = Stunden

    ; Minuten lesen (Register 0x02)
    mov al, 0x02
    out PORT_RTC_INDEX, al
    in al, PORT_RTC_DATA
    mov ch, al                  ; CH = Minuten

    ; Sekunden lesen (Register 0x00)
    mov al, 0x00
    out PORT_RTC_INDEX, al
    in al, PORT_RTC_DATA
    mov dl, al                  ; DL = Sekunden

    ; RTC live in VGA-Textspeicher an Zeile 2, Spalte 33 schreiben
    mov REG_DI, VGA_BASE + (2 * 80 + 33) * 2
    mov ah, ATTR_VAL_YELLOW

    ; Stunden BCD -> ASCII
    mov al, cl
    shr al, 4
    add al, '0'
    stosw
    mov al, cl
    and al, 0x0F
    add al, '0'
    stosw

    ; Doppelpunkt
    mov al, ':'
    stosw

    ; Minuten BCD -> ASCII
    mov al, ch
    shr al, 4
    add al, '0'
    stosw
    mov al, ch
    and al, 0x0F
    add al, '0'
    stosw

    ; Doppelpunkt
    mov al, ':'
    stosw

    ; Sekunden BCD -> ASCII
    mov al, dl
    shr al, 4
    add al, '0'
    stosw
    mov al, dl
    and al, 0x0F
    add al, '0'
    stosw

    ; --------------------------------------------------------------------------
    ; C. PIT-Ticks und TSC-Differenz zur Takt-/Laufzeitmessung
    ; --------------------------------------------------------------------------
    ; PIT-Zaehler Channel 0 live latchen und auslesen
    mov al, 0x00                ; Latch Counter 0
    out PORT_PIT_CMD, al
    in al, PORT_PIT_CH0         ; Low Byte
    mov bl, al
    in al, PORT_PIT_CH0         ; High Byte
    mov bh, al
    mov [snap_pit], bx

    ; RDTSC ausfuehren: Liefert 64-Bit Taktzyklen in EDX:EAX
    rdtsc
    mov ecx, eax                ; Aktueller TSC Low
    mov ebx, eax
    sub ebx, [prev_tsc_low]     ; Delta = Aktuell - Vorherig
    mov [prev_tsc_low], ecx

    ; TSC 64-Bit in VGA schreiben: Zeile 3, Spalte 35
    mov REG_DI, VGA_BASE + (3 * 80 + 35) * 2
    push REG_CX
    mov ch, ATTR_VAL_WHITE
    mov eax, edx                ; TSC High 32-Bit
    call write_hex32_vga
    pop REG_AX                  ; TSC Low 32-Bit
    mov ch, ATTR_VAL_WHITE
    call write_hex32_vga

    ; TSC Delta in VGA schreiben: Zeile 3, Spalte 68
    mov REG_DI, VGA_BASE + (3 * 80 + 68) * 2
    mov ch, ATTR_VAL_GREEN
    mov eax, ebx                ; Delta
    call write_hex32_vga

    ; --------------------------------------------------------------------------
    ; D. Aktuelle Registerwerte (EAX, EBX, ESP, CR0, CR3, EFLAGS)
    ; --------------------------------------------------------------------------
    ; Live Control Register und Flags abfragen
%ifidn __OUTPUT_FORMAT__, elf32
    mov eax, cr0
    mov [snap_cr0], eax
    mov eax, cr3
    mov [snap_cr3], eax
    pushfd
    pop eax
    mov [snap_eflags], eax
%else
    mov rax, cr0
    mov [snap_cr0], eax
    mov rax, cr3
    mov [snap_cr3], eax
    pushfq
    pop rax
    mov [snap_eflags], eax
%endif

    ; Zeile 5: EAX (Spalte 26), EBX (Spalte 45), ESP (Spalte 64)
    mov REG_DI, VGA_BASE + (5 * 80 + 26) * 2
    mov ch, ATTR_VAL_WHITE
    mov eax, [snap_eax]
    call write_hex32_vga

    mov REG_DI, VGA_BASE + (5 * 80 + 45) * 2
    mov ch, ATTR_VAL_WHITE
    mov eax, [snap_ebx]
    call write_hex32_vga

    mov REG_DI, VGA_BASE + (5 * 80 + 64) * 2
    mov ch, ATTR_VAL_WHITE
    mov eax, [snap_esp]
    call write_hex32_vga

    ; Zeile 6: CR0 (Spalte 26), CR3 (Spalte 45), EFLAGS (Spalte 64)
    mov REG_DI, VGA_BASE + (6 * 80 + 26) * 2
    mov ch, ATTR_VAL_YELLOW
    mov eax, [snap_cr0]
    call write_hex32_vga

    mov REG_DI, VGA_BASE + (6 * 80 + 45) * 2
    mov ch, ATTR_VAL_YELLOW
    mov eax, [snap_cr3]
    call write_hex32_vga

    mov REG_DI, VGA_BASE + (6 * 80 + 64) * 2
    mov ch, ATTR_VAL_YELLOW
    mov eax, [snap_eflags]
    call write_hex32_vga

    ; --------------------------------------------------------------------------
    ; E. ATA-Status-Register (BSY, DRDY, ERR) und letzter I/O-Sektor
    ; --------------------------------------------------------------------------
    ; Port 0x1F7 live lesen
    mov dx, PORT_ATA_STATUS
    in al, dx
    mov [last_ata_status], al
    mov cl, al                  ; CL = ATA Status Byte

    ; Status Hex-Wert an Zeile 8, Spalte 35 ausgeben
    mov REG_DI, VGA_BASE + (8 * 80 + 35) * 2
    mov ch, ATTR_VAL_WHITE
    mov al, cl
    call write_hex8_vga

    ; BSY (Bit 7): Zeile 8, Spalte 44
    mov REG_DI, VGA_BASE + (8 * 80 + 44) * 2
    test cl, 0x80
    jz .bsy_0
    mov ax, (ATTR_VAL_RED << 8) | '1'
    jmp .bsy_put
.bsy_0:
    mov ax, (ATTR_VAL_GREEN << 8) | '0'
.bsy_put:
    stosw

    ; DRDY (Bit 6): Zeile 8, Spalte 52
    mov REG_DI, VGA_BASE + (8 * 80 + 52) * 2
    test cl, 0x40
    jz .drdy_0
    mov ax, (ATTR_VAL_GREEN << 8) | '1'
    jmp .drdy_put
.drdy_0:
    mov ax, (ATTR_VAL_RED << 8) | '0'
.drdy_put:
    stosw

    ; ERR (Bit 0): Zeile 8, Spalte 59
    mov REG_DI, VGA_BASE + (8 * 80 + 59) * 2
    test cl, 0x01
    jz .err_0
    mov ax, (ATTR_VAL_RED << 8) | '1'
    jmp .err_put
.err_0:
    mov ax, (ATTR_VAL_GREEN << 8) | '0'
.err_put:
    stosw

    ; Letzter LBA28-Sektor: Zeile 9, Spalte 45
    mov REG_DI, VGA_BASE + (9 * 80 + 45) * 2
    mov ch, ATTR_VAL_WHITE
    mov eax, [last_io_lba]
    call write_hex32_vga

    ; --------------------------------------------------------------------------
    ; F. Letzter roher Tastatur-Scancode (Port 0x60) und Status (Port 0x64)
    ; --------------------------------------------------------------------------
    ; Scancode: Zeile 11, Spalte 41
    mov REG_DI, VGA_BASE + (11 * 80 + 41) * 2
    mov ch, ATTR_VAL_YELLOW
    mov al, [last_scancode_pr]
    call write_hex8_vga

    ; Status 0x64: Zeile 11, Spalte 65
    mov REG_DI, VGA_BASE + (11 * 80 + 65) * 2
    mov ch, ATTR_VAL_WHITE
    mov al, [status_64_pr]
    call write_hex8_vga

    ; --------------------------------------------------------------------------
    ; G. Kurze Verzoegerungsschleife fuer geschmeidige 30-60 Hz Bildwiederholung
    ; --------------------------------------------------------------------------
    mov ecx, 150000
.delay_spin:
    pause
    dec ecx
    jnz .delay_spin

    ; Naechster Schleifendurchlauf
    jmp .live_loop

; ==============================================================================
; Beenden des Live-Monitors bei ESC oder 'q'
; ==============================================================================
.exit_monitor:
    ; Interrupts wieder freigeben
    sti

    ; Bildschirm saeubern und Cursor zuruecksetzen
    call vga_clear
%ifidn __OUTPUT_FORMAT__, elf32
    push 0
    push 0
    call vga_update_cursor
    add esp, 8
    pop edi
    pop esi
    pop ebp
    pop ebx
%else
    xor edi, edi
    xor esi, esi
    call vga_update_cursor
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
%endif
    ret

; ==============================================================================
; render_static_layout:
; Schreibt das statische GUI-Gitter einmalig direkt in den VGA-Speicher (0xB8000)
; Nutzt rep stosw und lodsb fuer hoechste Performance.
; ==============================================================================
render_static_layout:
    ; 1. Ganzen Bildschirm auf Schwarz leeren
    mov REG_DI, VGA_BASE
    mov ax, (ATTR_TEXT << 8) | ' '
    mov ecx, VGA_COLS * VGA_ROWS
    cld
    rep stosw

    ; 2. Zeile 0: Titel-Banner (Weiss auf Blau)
    mov REG_DI, VGA_BASE
    mov ax, (ATTR_HEADER << 8) | ' '
    mov ecx, VGA_COLS
    rep stosw
    mov REG_DI, VGA_BASE + 2
    mov ah, ATTR_HEADER
    lea REG_SI, [str_banner]
    call blit_str_vga

    ; 3. Trennlinien: Zeile 1, 4, 7, 10, 12
    mov REG_DI, VGA_BASE + (1 * 80 * 2)
    call blit_divider_line
    mov REG_DI, VGA_BASE + (4 * 80 * 2)
    call blit_divider_line
    mov REG_DI, VGA_BASE + (7 * 80 * 2)
    call blit_divider_line
    mov REG_DI, VGA_BASE + (10 * 80 * 2)
    call blit_divider_line
    mov REG_DI, VGA_BASE + (12 * 80 * 2)
    call blit_divider_line

    ; 4. Zeile 2: RTC
    mov REG_DI, VGA_BASE + (2 * 80 + 2) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_rtc]
    call blit_str_vga
    mov REG_DI, VGA_BASE + (2 * 80 + 44) * 2
    mov ah, ATTR_TEXT
    lea REG_SI, [str_rtc_note]
    call blit_str_vga

    ; 5. Zeile 3: CPU Timing & TSC
    mov REG_DI, VGA_BASE + (3 * 80 + 2) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_cpu]
    call blit_str_vga
    mov REG_DI, VGA_BASE + (3 * 80 + 53) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_delta]
    call blit_str_vga

    ; 6. Zeile 5: CPU Register
    mov REG_DI, VGA_BASE + (5 * 80 + 2) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_reg1]
    call blit_str_vga
    mov REG_DI, VGA_BASE + (5 * 80 + 36) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_reg2]
    call blit_str_vga
    mov REG_DI, VGA_BASE + (5 * 80 + 55) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_reg3]
    call blit_str_vga

    ; 7. Zeile 6: Control Register & Flags
    mov REG_DI, VGA_BASE + (6 * 80 + 2) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_ctrl1]
    call blit_str_vga
    mov REG_DI, VGA_BASE + (6 * 80 + 36) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_ctrl2]
    call blit_str_vga
    mov REG_DI, VGA_BASE + (6 * 80 + 55) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_ctrl3]
    call blit_str_vga

    ; 8. Zeile 8: ATA Status
    mov REG_DI, VGA_BASE + (8 * 80 + 2) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_ata1]
    call blit_str_vga
    mov REG_DI, VGA_BASE + (8 * 80 + 39) * 2
    mov ah, ATTR_TEXT
    lea REG_SI, [str_sec_ata_bsy]
    call blit_str_vga
    mov REG_DI, VGA_BASE + (8 * 80 + 46) * 2
    mov ah, ATTR_TEXT
    lea REG_SI, [str_sec_ata_rdy]
    call blit_str_vga
    mov REG_DI, VGA_BASE + (8 * 80 + 54) * 2
    mov ah, ATTR_TEXT
    lea REG_SI, [str_sec_ata_err]
    call blit_str_vga
    mov REG_DI, VGA_BASE + (8 * 80 + 61) * 2
    mov ah, ATTR_TEXT
    lea REG_SI, [str_bracket_close]
    call blit_str_vga

    ; 9. Zeile 9: ATA Letzter I/O Sektor
    mov REG_DI, VGA_BASE + (9 * 80 + 2) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_lba]
    call blit_str_vga

    ; 10. Zeile 11: Keyboard I/O
    mov REG_DI, VGA_BASE + (11 * 80 + 2) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_kbd1]
    call blit_str_vga
    mov REG_DI, VGA_BASE + (11 * 80 + 45) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_kbd2]
    call blit_str_vga

    ; 11. Zeilen 14..20: Architektur-Infobox
    mov REG_DI, VGA_BASE + (14 * 80 + 2) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_info_title]
    call blit_str_vga

    mov REG_DI, VGA_BASE + (15 * 80 + 2) * 2
    mov ah, ATTR_TEXT
    lea REG_SI, [str_info_1]
    call blit_str_vga

    mov REG_DI, VGA_BASE + (16 * 80 + 2) * 2
    mov ah, ATTR_TEXT
    lea REG_SI, [str_info_2]
    call blit_str_vga

    mov REG_DI, VGA_BASE + (17 * 80 + 2) * 2
    mov ah, ATTR_TEXT
    lea REG_SI, [str_info_3]
    call blit_str_vga

    mov REG_DI, VGA_BASE + (18 * 80 + 2) * 2
    mov ah, ATTR_TEXT
    lea REG_SI, [str_info_4]
    call blit_str_vga

    mov REG_DI, VGA_BASE + (19 * 80 + 2) * 2
    mov ah, ATTR_TEXT
    lea REG_SI, [str_info_5]
    call blit_str_vga

    ; 12. Zeile 24: Status Footer (Weiss auf Gruen)
    mov REG_DI, VGA_BASE + (24 * 80 * 2)
    mov ax, (ATTR_FOOTER << 8) | ' '
    mov ecx, VGA_COLS
    rep stosw
    mov REG_DI, VGA_BASE + (24 * 80 + 2) * 2
    mov ah, ATTR_FOOTER
    lea REG_SI, [str_footer]
    call blit_str_vga

    ret

; ==============================================================================
; Hilfsfunktionen fuer VGA Direct-Rendering via stosw / lodsb
; ==============================================================================

; blit_divider_line: Zeichnet eine 80-Zeichen Trennlinie
; REG_DI = Zeilenanfang im VGA-Puffer
blit_divider_line:
    mov ax, (ATTR_DIVIDER << 8) | 0xC4 ; Einfache horizontale Linie
    mov ecx, VGA_COLS
    cld
    rep stosw
    ret

; blit_str_vga:
; Schreibt nullterminierten String aus REG_SI mit Farb-Attribut AH an VGA-Adresse REG_DI
blit_str_vga:
    cld
.loop:
    lodsb
    test al, al
    jz .done
    stosw
    jmp .loop
.done:
    ret

; write_hex32_vga:
; Schreibt 8-stelligen 32-Bit Hexwert aus EAX mit Attribut CH direkt nach REG_DI
write_hex32_vga:
    push REG_DX
    push REG_CX
    mov edx, eax
    mov ecx, 8
.h32_loop:
    rol edx, 4
    mov al, dl
    and al, 0x0F
    cmp al, 9
    jbe .d_digit
    add al, 'A' - 10 - '0'
.d_digit:
    add al, '0'
    mov ah, ch                  ; Farb-Attribut
    stosw
    dec ecx
    jnz .h32_loop
    pop REG_CX
    pop REG_DX
    ret

; write_hex8_vga:
; Schreibt 2-stelligen 8-Bit Hexwert aus AL mit Attribut CH direkt nach REG_DI
write_hex8_vga:
    push REG_DX
    mov dl, al
    rol dl, 4
    mov al, dl
    and al, 0x0F
    cmp al, 9
    jbe .d1
    add al, 'A' - 10 - '0'
.d1:
    add al, '0'
    mov ah, ch
    stosw

    rol dl, 4
    mov al, dl
    and al, 0x0F
    cmp al, 9
    jbe .d2
    add al, 'A' - 10 - '0'
.d2:
    add al, '0'
    mov ah, ch
    stosw

    pop REG_DX
    ret

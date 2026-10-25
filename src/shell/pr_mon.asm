; ==============================================================================
; dismasmOS - Dynamic Live Hardware Monitoring Command ('pr')
; ==============================================================================
; Specification:
; - Continuous refresh loop until ESC (0x01) or 'q' (0x10)
;   polled directly via PS/2 ports 0x60 (Data) and 0x64 (Status)
; - Direct writes to VGA text buffer (0xB8000) -> flicker-free, zero string overhead
; - Live dynamic hardware telemetry:
;   * RTC clock read live from CMOS ports 0x70/0x71 with BCD conversion
;   * PIT ticks and TSC delta for cycle/runtime measurement (rdtsc, PIT 0x40)
;   * Register snapshots & live control registers (EAX, EBX, ESP, CR0, CR3, EFLAGS)
;   * ATA status register (port 0x1F7: BSY, DRDY, ERR) and last I/O LBA sector
;   * Last raw keyboard scancode (port 0x60)
; - Cycle- and memory-optimized (stosw, lodsb, rep stosw, minimal branches)
; - Automatic support for ELF32 (x86 32-bit) and ELF64 (x86_64)
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

; Color attributes (background | foreground)
ATTR_HEADER         equ 0x1F    ; White on Blue
ATTR_DIVIDER        equ 0x03    ; Cyan on Black
ATTR_LABEL          equ 0x0B    ; Light Cyan on Black
ATTR_TEXT           equ 0x07    ; Light Grey on Black
ATTR_VAL_WHITE      equ 0x0F    ; Bright White
ATTR_VAL_YELLOW     equ 0x0E    ; Yellow
ATTR_VAL_GREEN      equ 0x0A    ; Light Green
ATTR_VAL_RED        equ 0x0C    ; Light Red
ATTR_FOOTER         equ 0x2F    ; White on Green

; Hardware ports
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

; Static BSS variables (zero heap usage!)
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

; External BSS variables from ATA filesystem module
extern last_io_lba
extern last_ata_status

section .data
align 4
str_banner:
    db " [dismasmOS] BARE-METAL LIVE HARDWARE MONITOR ('pr') | [ESC] / [q] Quit ", 0
str_div:
    times 80 db 0xCD
    db 0
str_sec_rtc:
    db " [CMOS RTC]      Current Time: ", 0
str_rtc_note:
    db " (Ports 0x70/0x71, BCD-decoded)", 0
str_sec_cpu:
    db " [CPU TIMING]    TSC Counter:  0x", 0
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
    db " [ATA LAST LBA]  Last I/O Sector (LBA):  0x", 0
str_sec_kbd1:
    db " [KEYBOARD I/O]  Port 0x60 Scancode: 0x", 0
str_sec_kbd2:
    db "  Status Port 0x64: 0x", 0
str_info_title:
    db " [ARCHITECTURE & PERFORMANCE FEATURES]", 0
str_info_1:
    db " * Direct VGA VRAM writes to 0x000B8000 (completely flicker-free)", 0
str_info_2:
    db " * Cycle-optimized string instructions: stosw, lodsb, rep stosw", 0
str_info_3:
    db " * Live CMOS RTC clock with UIP synchronization and hardware BCD decode", 0
str_info_4:
    db " * High-precision cycle timing via CPU RDTSC and PIT Counter Ch0", 0
str_info_5:
    db " * Raw hardware polling on ports 0x1F7 (ATA) and 0x60/0x64 (PS/2)", 0
str_footer:
    db " LIVE REFRESH ACTIVE | Cycle-Optimized Loop | Press 'q' or ESC to exit ", 0

section .text
global pr_monitor_loop

extern vga_clear
extern vga_update_cursor

; ==============================================================================
; pr_monitor_loop:
; Main entry point of the dynamic 'pr' live hardware monitor.
; Runs a continuous refresh loop until ESC or 'q' is pressed.
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

    ; 1. Save initial register snapshots (EAX, EBX, ESP)
    mov [snap_eax], eax
    mov [snap_ebx], ebx
    mov [snap_esp], esp

    ; Move cursor to top-left corner
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

    ; 2. Render static layout directly into VGA buffer (0xB8000)
    call render_static_layout

    ; 3. Capture initial TSC baseline
    rdtsc
    mov [prev_tsc_low], eax

    ; 4. Drain PS/2 keyboard output buffer before entering live loop
.drain_kbd_init:
    in al, PORT_KBD_STATUS
    test al, 0x01
    jz .kbd_ready
    in al, PORT_KBD_DATA
    jmp .drain_kbd_init
.kbd_ready:

    ; Temporarily mask interrupts during bare-metal live hardware polling
    cli

; ==============================================================================
; LIVE LOOP (Continuous flicker-free refresh)
; ==============================================================================
.live_loop:

    ; --------------------------------------------------------------------------
    ; A. Keyboard polling via port 0x64 and port 0x60
    ; --------------------------------------------------------------------------
    in al, PORT_KBD_STATUS
    mov [status_64_pr], al
    test al, 0x01               ; Bit 0 = Output Buffer Full?
    jz .no_key_pressed

    ; Read scancode from port 0x60
    in al, PORT_KBD_DATA
    mov [last_scancode_pr], al

    ; Check exit keys:
    ; 0x01 = ESC (Make Code)
    cmp al, 0x01
    je .exit_monitor
    ; 0x10 = 'q' (Make Code in Set 1)
    cmp al, 0x10
    je .exit_monitor

.no_key_pressed:

    ; --------------------------------------------------------------------------
    ; B. Live RTC clock from CMOS ports 0x70/0x71 (with BCD conversion)
    ; --------------------------------------------------------------------------
    ; Wait while UIP (Update In Progress, bit 7 of register 0x0A) is set
.wait_uip:
    mov al, 0x0A
    out PORT_RTC_INDEX, al
    in al, PORT_RTC_DATA
    test al, 0x80
    jnz .wait_uip

    ; Read Hours (register 0x04)
    mov al, 0x04
    out PORT_RTC_INDEX, al
    in al, PORT_RTC_DATA
    mov cl, al                  ; CL = Hours

    ; Read Minutes (register 0x02)
    mov al, 0x02
    out PORT_RTC_INDEX, al
    in al, PORT_RTC_DATA
    mov ch, al                  ; CH = Minutes

    ; Read Seconds (register 0x00)
    mov al, 0x00
    out PORT_RTC_INDEX, al
    in al, PORT_RTC_DATA
    mov dl, al                  ; DL = Seconds

    ; Write live RTC to VGA text buffer at Row 2, Column 33
    mov REG_DI, VGA_BASE + (2 * 80 + 33) * 2
    mov ah, ATTR_VAL_YELLOW

    ; Hours BCD -> ASCII
    mov al, cl
    shr al, 4
    add al, '0'
    stosw
    mov al, cl
    and al, 0x0F
    add al, '0'
    stosw

    ; Colon
    mov al, ':'
    stosw

    ; Minutes BCD -> ASCII
    mov al, ch
    shr al, 4
    add al, '0'
    stosw
    mov al, ch
    and al, 0x0F
    add al, '0'
    stosw

    ; Colon
    mov al, ':'
    stosw

    ; Seconds BCD -> ASCII
    mov al, dl
    shr al, 4
    add al, '0'
    stosw
    mov al, dl
    and al, 0x0F
    add al, '0'
    stosw

    ; --------------------------------------------------------------------------
    ; C. PIT ticks and TSC delta for cycle/runtime measurement
    ; --------------------------------------------------------------------------
    ; Latch and read PIT Channel 0 counter live
    mov al, 0x00                ; Latch Counter 0
    out PORT_PIT_CMD, al
    in al, PORT_PIT_CH0         ; Low byte
    mov bl, al
    in al, PORT_PIT_CH0         ; High byte
    mov bh, al
    mov [snap_pit], bx

    ; Execute RDTSC: returns 64-bit cycle counter in EDX:EAX
    rdtsc
    mov ecx, eax                ; Current TSC low
    mov ebx, eax
    sub ebx, [prev_tsc_low]     ; Delta = Current - Previous
    mov [prev_tsc_low], ecx

    ; Write 64-bit TSC to VGA: Row 3, Column 35
    mov REG_DI, VGA_BASE + (3 * 80 + 35) * 2
    push REG_CX
    mov ch, ATTR_VAL_WHITE
    mov eax, edx                ; TSC high 32 bits
    call write_hex32_vga
    pop REG_AX                  ; TSC low 32 bits
    mov ch, ATTR_VAL_WHITE
    call write_hex32_vga

    ; Write TSC delta to VGA: Row 3, Column 68
    mov REG_DI, VGA_BASE + (3 * 80 + 68) * 2
    mov ch, ATTR_VAL_GREEN
    mov eax, ebx                ; Delta
    call write_hex32_vga

    ; --------------------------------------------------------------------------
    ; D. Current register values (EAX, EBX, ESP, CR0, CR3, EFLAGS)
    ; --------------------------------------------------------------------------
    ; Read live control registers and flags
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

    ; Row 5: EAX (Col 26), EBX (Col 45), ESP (Col 64)
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

    ; Row 6: CR0 (Col 26), CR3 (Col 45), EFLAGS (Col 64)
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
    ; E. ATA status register (BSY, DRDY, ERR) and last I/O sector
    ; --------------------------------------------------------------------------
    ; Read port 0x1F7 live
    mov dx, PORT_ATA_STATUS
    in al, dx
    mov [last_ata_status], al
    mov cl, al                  ; CL = ATA status byte

    ; Write status hex byte at Row 8, Column 35
    mov REG_DI, VGA_BASE + (8 * 80 + 35) * 2
    mov ch, ATTR_VAL_WHITE
    mov al, cl
    call write_hex8_vga

    ; BSY (bit 7): Row 8, Column 44
    mov REG_DI, VGA_BASE + (8 * 80 + 44) * 2
    test cl, 0x80
    jz .bsy_0
    mov ax, (ATTR_VAL_RED << 8) | '1'
    jmp .bsy_put
.bsy_0:
    mov ax, (ATTR_VAL_GREEN << 8) | '0'
.bsy_put:
    stosw

    ; DRDY (bit 6): Row 8, Column 52
    mov REG_DI, VGA_BASE + (8 * 80 + 52) * 2
    test cl, 0x40
    jz .drdy_0
    mov ax, (ATTR_VAL_GREEN << 8) | '1'
    jmp .drdy_put
.drdy_0:
    mov ax, (ATTR_VAL_RED << 8) | '0'
.drdy_put:
    stosw

    ; ERR (bit 0): Row 8, Column 59
    mov REG_DI, VGA_BASE + (8 * 80 + 59) * 2
    test cl, 0x01
    jz .err_0
    mov ax, (ATTR_VAL_RED << 8) | '1'
    jmp .err_put
.err_0:
    mov ax, (ATTR_VAL_GREEN << 8) | '0'
.err_put:
    stosw

    ; Last LBA28 sector: Row 9, Column 45
    mov REG_DI, VGA_BASE + (9 * 80 + 45) * 2
    mov ch, ATTR_VAL_WHITE
    mov eax, [last_io_lba]
    call write_hex32_vga

    ; --------------------------------------------------------------------------
    ; F. Last raw keyboard scancode (port 0x60) and status (port 0x64)
    ; --------------------------------------------------------------------------
    ; Scancode: Row 11, Column 41
    mov REG_DI, VGA_BASE + (11 * 80 + 41) * 2
    mov ch, ATTR_VAL_YELLOW
    mov al, [last_scancode_pr]
    call write_hex8_vga

    ; Status 0x64: Row 11, Column 65
    mov REG_DI, VGA_BASE + (11 * 80 + 65) * 2
    mov ch, ATTR_VAL_WHITE
    mov al, [status_64_pr]
    call write_hex8_vga

    ; --------------------------------------------------------------------------
    ; G. Short spin delay for smooth 30-60 Hz refresh cadence
    ; --------------------------------------------------------------------------
    mov ecx, 150000
.delay_spin:
    pause
    dec ecx
    jnz .delay_spin

    ; Next iteration
    jmp .live_loop

; ==============================================================================
; Exit live monitor on ESC or 'q'
; ==============================================================================
.exit_monitor:
    ; Re-enable interrupts
    sti

    ; Clear screen and reset cursor
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
; Writes the static UI grid once directly into VGA memory (0xB8000).
; Uses rep stosw and lodsb for peak throughput.
; ==============================================================================
render_static_layout:
    ; 1. Clear entire screen to black
    mov REG_DI, VGA_BASE
    mov ax, (ATTR_TEXT << 8) | ' '
    mov ecx, VGA_COLS * VGA_ROWS
    cld
    rep stosw

    ; 2. Row 0: Title banner (White on Blue)
    mov REG_DI, VGA_BASE
    mov ax, (ATTR_HEADER << 8) | ' '
    mov ecx, VGA_COLS
    rep stosw
    mov REG_DI, VGA_BASE + 2
    mov ah, ATTR_HEADER
    lea REG_SI, [str_banner]
    call blit_str_vga

    ; 3. Divider lines: Rows 1, 4, 7, 10, 12
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

    ; 4. Row 2: RTC
    mov REG_DI, VGA_BASE + (2 * 80 + 2) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_rtc]
    call blit_str_vga
    mov REG_DI, VGA_BASE + (2 * 80 + 44) * 2
    mov ah, ATTR_TEXT
    lea REG_SI, [str_rtc_note]
    call blit_str_vga

    ; 5. Row 3: CPU Timing & TSC
    mov REG_DI, VGA_BASE + (3 * 80 + 2) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_cpu]
    call blit_str_vga
    mov REG_DI, VGA_BASE + (3 * 80 + 53) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_delta]
    call blit_str_vga

    ; 6. Row 5: CPU Registers
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

    ; 7. Row 6: Control Registers & Flags
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

    ; 8. Row 8: ATA Status
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

    ; 9. Row 9: ATA Last I/O Sector
    mov REG_DI, VGA_BASE + (9 * 80 + 2) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_lba]
    call blit_str_vga

    ; 10. Row 11: Keyboard I/O
    mov REG_DI, VGA_BASE + (11 * 80 + 2) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_kbd1]
    call blit_str_vga
    mov REG_DI, VGA_BASE + (11 * 80 + 45) * 2
    mov ah, ATTR_LABEL
    lea REG_SI, [str_sec_kbd2]
    call blit_str_vga

    ; 11. Rows 14..19: Architecture Info Box
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

    ; 12. Row 24: Status Footer (White on Green)
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
; Direct VGA rendering helpers via stosw / lodsb
; ==============================================================================

; blit_divider_line: Draws an 80-column horizontal divider line
; REG_DI = row start address in VGA buffer
blit_divider_line:
    mov ax, (ATTR_DIVIDER << 8) | 0xC4 ; Single horizontal box-drawing line
    mov ecx, VGA_COLS
    cld
    rep stosw
    ret

; blit_str_vga:
; Writes null-terminated string from REG_SI with attribute AH to VGA address REG_DI
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
; Writes 8-digit 32-bit hex value from EAX with attribute CH directly to REG_DI
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
    mov ah, ch                  ; Color attribute
    stosw
    dec ecx
    jnz .h32_loop
    pop REG_CX
    pop REG_DX
    ret

; write_hex8_vga:
; Writes 2-digit 8-bit hex value from AL with attribute CH directly to REG_DI
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

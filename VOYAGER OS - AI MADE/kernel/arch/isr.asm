; x86-64 interrupt stubs (NASM).
; Each stub pushes its vector number and jumps to isr_common,
; which saves all GP regs, calls the C handler, restores, iretq.

global isr_common
extern irq_handlers

section .text
bits 64
default rel

global isr_0
isr_0:
    push 0
    jmp isr_common

global isr_1
isr_1:
    push 1
    jmp isr_common

global isr_2
isr_2:
    push 2
    jmp isr_common

global isr_3
isr_3:
    push 3
    jmp isr_common

global isr_4
isr_4:
    push 4
    jmp isr_common

global isr_5
isr_5:
    push 5
    jmp isr_common

global isr_6
isr_6:
    push 6
    jmp isr_common

global isr_7
isr_7:
    push 7
    jmp isr_common

global isr_8
isr_8:
    push 8
    jmp isr_common

global isr_9
isr_9:
    push 9
    jmp isr_common

global isr_10
isr_10:
    push 10
    jmp isr_common

global isr_11
isr_11:
    push 11
    jmp isr_common

global isr_12
isr_12:
    push 12
    jmp isr_common

global isr_13
isr_13:
    push 13
    jmp isr_common

global isr_14
isr_14:
    push 14
    jmp isr_common

global isr_15
isr_15:
    push 15
    jmp isr_common

global isr_16
isr_16:
    push 16
    jmp isr_common

global isr_17
isr_17:
    push 17
    jmp isr_common

global isr_18
isr_18:
    push 18
    jmp isr_common

global isr_19
isr_19:
    push 19
    jmp isr_common

global isr_20
isr_20:
    push 20
    jmp isr_common

global isr_21
isr_21:
    push 21
    jmp isr_common

global isr_22
isr_22:
    push 22
    jmp isr_common

global isr_23
isr_23:
    push 23
    jmp isr_common

global isr_24
isr_24:
    push 24
    jmp isr_common

global isr_25
isr_25:
    push 25
    jmp isr_common

global isr_26
isr_26:
    push 26
    jmp isr_common

global isr_27
isr_27:
    push 27
    jmp isr_common

global isr_28
isr_28:
    push 28
    jmp isr_common

global isr_29
isr_29:
    push 29
    jmp isr_common

global isr_30
isr_30:
    push 30
    jmp isr_common

global isr_31
isr_31:
    push 31
    jmp isr_common

global isr_32
isr_32:
    push 32
    jmp isr_common

global isr_33
isr_33:
    push 33
    jmp isr_common

global isr_34
isr_34:
    push 34
    jmp isr_common

global isr_35
isr_35:
    push 35
    jmp isr_common

global isr_36
isr_36:
    push 36
    jmp isr_common

global isr_37
isr_37:
    push 37
    jmp isr_common

global isr_38
isr_38:
    push 38
    jmp isr_common

global isr_39
isr_39:
    push 39
    jmp isr_common

global isr_40
isr_40:
    push 40
    jmp isr_common

global isr_41
isr_41:
    push 41
    jmp isr_common

global isr_42
isr_42:
    push 42
    jmp isr_common

global isr_43
isr_43:
    push 43
    jmp isr_common

global isr_44
isr_44:
    push 44
    jmp isr_common

global isr_45
isr_45:
    push 45
    jmp isr_common

global isr_46
isr_46:
    push 46
    jmp isr_common

global isr_47
isr_47:
    push 47
    jmp isr_common

; Common entry. Stack: [vector][rip][cs][rflags] (+errcode for some vecs).
isr_common:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; rdi = pointer to saved-regs frame (rsp -> r15)
    mov rdi, rsp
    mov rax, [rdi + 15*8]        ; vector
    cmp rax, 256
    jae .Ldone
    lea rdx, [rel irq_handlers]
    mov rax, [rdx + rax*8]
    test rax, rax
    jz .Ldone
    call rax                    ; handler(struct regs*) - rdi already set
.Ldone:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax                     ; rax = interrupted value (must survive iretq)

    ; rsp -> [vector][err?][rip][cs][rflags]
    ; Stash real rax so we can use rax as scratch to read/classify the
    ; vector without corrupting the interrupted context. Previously this
    ; code clobbered rax with the vector number, so every IRQ returned
    ; rax=32 and silently corrupted caller state.
    push rax
    mov rax, [rsp + 8]          ; vector (saved_rax occupies [rsp])
    cmp rax, 32
    jae .Lnoerr                 ; IRQs (32-47) never carry an error code
    ; exception vectors: check the CPU error-code list
    cmp rax, 8
    je .Lerr
    cmp rax, 10
    je .Lerr
    cmp rax, 11
    je .Lerr
    cmp rax, 12
    je .Lerr
    cmp rax, 13
    je .Lerr
    cmp rax, 14
    je .Lerr
    cmp rax, 17
    je .Lerr
    cmp rax, 21
    je .Lerr
    cmp rax, 29
    je .Lerr
    cmp rax, 30
    je .Lerr
    cmp rax, 31
    je .Lerr
.Lnoerr:
    pop rax                     ; restore real rax
    add rsp, 8                  ; drop vector only
    iretq
.Lerr:
    pop rax                     ; restore real rax
    add rsp, 16                 ; drop vector + error code
    iretq

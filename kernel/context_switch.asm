;
; Anykernel OS — Context Switch (x86_64)
;
; void context_switch(struct task *old, struct task *new);
;
; RDI = old task (save current state here)
; RSI = new task (restore state from here)
;
; Only callee-saved registers need saving:
;   rbp, rbx, r12, r13, r14, r15
; The C calling convention guarantees all others are scratch.
;
; task.rsp is the FIRST field of struct task (offset 0).
;

global context_switch

section .text

context_switch:
    ; Save callee-saved registers on OLD stack
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Save old RSP into old->rsp (offset 0 of struct task)
    mov [rdi], rsp

    ; Load new RSP from new->rsp (offset 0 of struct task)
    mov rsp, [rsi]

    ; Restore callee-saved registers from NEW stack
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; Return to wherever the new task was suspended
    ; (either back into schedule() or into task_entry_trampoline)
    ret

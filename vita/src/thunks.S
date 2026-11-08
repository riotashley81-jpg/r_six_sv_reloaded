@ Import thunks for the natively running game library.
@
@ Each table entry loads its index into r12 and jumps to the common
@ trampoline, which saves the caller's registers as a ThunkFrame (see cpu.h),
@ calls r6_thunk_dispatch(frame, index), then restores everything from the
@ frame. Handlers see the game's softfp arguments exactly as passed (core
@ registers + stack) and can change r4-r11, sp, lr and d8-d15 on return.

    .syntax unified
    .arm
    .text

    .equ FRAME_SIZE, 136            @ r0-r15, s16-s31, fpscr, pad

    .global r6_thunk_common
    .type   r6_thunk_common, %function
r6_thunk_common:
    sub     sp, sp, #FRAME_SIZE
    stmia   sp, {r0-r12}            @ r[0..12], r12 = thunk index
    add     r0, sp, #FRAME_SIZE
    str     r0, [sp, #52]           @ r[13] = sp at the call (stacked args)
    str     lr, [sp, #56]           @ r[14]
    add     r0, sp, #64
    vstmia  r0, {d8-d15}
    vmrs    r0, fpscr
    str     r0, [sp, #128]
    mov     r0, sp
    mov     r1, r12
    bl      r6_thunk_dispatch
    add     r0, sp, #64
    vldmia  r0, {d8-d15}
    ldr     r0, [sp, #128]
    vmsr    fpscr, r0
    ldr     lr, [sp, #56]
    ldr     r12, [sp, #52]
    ldmia   sp, {r0-r11}
    mov     sp, r12
    bx      lr
    .size   r6_thunk_common, . - r6_thunk_common

    .global r6_thunk_table
    .type   r6_thunk_table, %function
    .align  3
r6_thunk_table:
    .set    index, 0
    .rept   1024                    @ keep in sync with MAX_THUNKS in cpu_vita.cpp
    movw    r12, #index
    b       r6_thunk_common
    .set    index, index + 1
    .endr
    .size   r6_thunk_table, . - r6_thunk_table

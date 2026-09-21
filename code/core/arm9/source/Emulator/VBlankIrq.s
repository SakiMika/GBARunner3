.section ".itcm", "ax"
.altmacro

#include "AsmMacros.inc"

arm_func emu_vblankIrq
    // For center and mask display capture has to be enabled every frame
    // and the buffers need to be swapped
jumpToCaptureUpdate:
    nop

updateDisplayCaptureVramC:
    ldr lr,= 0x84808036
    mov r13, #0x04000000
    strh lr, [r13, #0x66]! // REG_DISPCAPCNT
    mov lr, lr, lsr #16
    add r13, r13, #(0x242 - 0x66)
    strh lr, [r13]
    ldr r13, jumpToUpdateDisplayCaptureVramDInstruction
    b checkSaveWrite

updateDisplayCaptureVramD:
    ldr lr,= 0x80848037
    mov r13, #0x04000000
    strh lr, [r13, #0x66]! // REG_DISPCAPCNT
    mov lr, lr, lsr #16
    add r13, r13, #(0x242 - 0x66)
    strh lr, [r13], -r13 // r13 becomes 0

checkSaveWrite:
    str r13, jumpToCaptureUpdate

    // This is replaced by a nop when no vblank dma is in use
.global emu_vblankDmaJumpInstruction
emu_vblankDmaJumpInstruction:
    b vblankDma

    // This is replaced by a nop when save needs to be checked
.global emu_vblankIrqSkipSaveCheckInstruction
emu_vblankIrqSkipSaveCheckInstruction:
    b cheat_vblankTail
#ifndef GBAR3_TEST
    ldr r13,= gGbaSaveShared
    mcr p15, 0, r13, c7, c6, 1 // invalidate range
    ldrb lr, [r13]
    cmp lr, #3 // GBA_SAVE_STATE_WRITE
    bne cheat_vblankTail

    ldr sp,= dtcmIrqStackEnd
    push {r0-r3,r12}
    bl sav_writeSaveToFile
    pop {r0-r3,r12}
    b cheat_vblankTail
#endif


#ifndef GBAR3_TEST
// Cheat work runs only after the stock VBlank capture/DMA/save path has
// completed.  At this point r13 is disposable: vm_irq reloads it immediately
// at emu_vblankIrqReturn.  This avoids perturbing the timing/state expected by
// the stock VBlank prologue before display capture and save handling.
cheat_vblankTail:
    ldr lr,= gCheatVBlankEnabled
    ldr lr, [lr]
    cmp lr, #0
    beq emu_vblankIrqReturn

    // r13 from the VM is not a C stack. Switch first, then preserve every
    // caller-clobbered guest register that the C ABI may change.
    ldr sp,= gCheatIrqStack + 4096
    push {r0-r3,r12,lr}
    ldr r12,= cheat_onVBlank
    blx r12
    pop {r0-r3,r12,lr}
    b emu_vblankIrqReturn
#else
cheat_vblankTail:
    b emu_vblankIrqReturn
#endif

jumpToUpdateDisplayCaptureVramDInstruction:
    add pc, pc, #(updateDisplayCaptureVramD - jumpToCaptureUpdate - 8)

vblankDma:
    ldr sp,= dtcmIrqStackEnd
    push {r0-r3,r12}
#ifndef GBAR3_TEST
    // These are replaced by nops when not active
.global emu_vblankDmaJumpInstructions
emu_vblankDmaJumpInstructions:
    bl dma_dma0Transfer
    bl dma_dma1Transfer
    bl dma_dma2Transfer
    bl dma_dma3Transfer
#endif
    pop {r0-r3,r12}
    b emu_vblankIrqSkipSaveCheckInstruction

.pool
.end

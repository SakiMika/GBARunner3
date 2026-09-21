.section ".itcm", "ax"
.altmacro

#include "AsmMacros.inc"

.extern dma_state

/// @brief Performs hblank irq tasks for the emulator.
///        - Emulates HDMA
/// @param r0-r12 Preserved
/// @param r13 Trashed.
/// @param lr Return address.
arm_func emu_hblankIrq
    // Don't tigger hblank irq on scanlines beyond the gba screen
    mov r13, #0x04000000
    ldrh lr, [r13, #6]

#ifndef GBAR3_TEST
    // Closed cheat button overlay.  Center-and-mask needs only captured source
    // lines 0..159.  Stop capture after line 159 so VRAM C rows 176..191 keep
    // the pre-rendered button, then show those rows on the physical lower LCD
    // for the final 16 scanlines.  The stock VBlank path restores normal
    // capture/display state before the next frame.
    ldr r13,= gCheatOverlayEnabled
    ldr r13, [r13]
    cmp r13, #0
    beq cheat_hblankOverlayDone

    cmp lr, #159
    beq cheat_hblankStopCapture
    cmp lr, #175
    beq cheat_hblankShowClosedButton
    b cheat_hblankOverlayDone

cheat_hblankStopCapture:
    mov r13, #0x04000000
    mov lr, #0
    str lr, [r13, #0x64]        // REG_DISPCAPCNT = 0
    ldrh lr, [r13, #6]          // restore VCOUNT in lr
    b cheat_hblankOverlayDone

cheat_hblankShowClosedButton:
    ldr r13,= gCheatOverlayActive
    mov lr, #1
    str lr, [r13]

    mov r13, #0x04000000
    mov lr, #0x80
    strb lr, [r13, #0x242]      // VRAM C -> LCDC
    mov lr, #0
    strh lr, [r13, #0x6C]       // main master brightness -> normal
    ldr lr,= 0x000A0000         // MODE_FB2: VRAM C direct display
    str lr, [r13]
    ldrh lr, [r13, #6]          // restore VCOUNT in lr

cheat_hblankOverlayDone:
#endif

    cmp lr, #260
    sublo r13, lr, #160
    rsblos r13, r13, #31
    bichs r4, r4, #2 // HBLANK IRQ

    cmp lr, #160
    // This is replaced by a b instruction when no hblank dma is in use
.global emu_hblankDmaSkipInstruction
emu_hblankDmaSkipInstruction:
    bge emu_hblankIrqReturn

    ldr sp,= dtcmIrqStackEnd
    push {r0-r3,r12}
#ifndef GBAR3_TEST
    // These are replaced by nops when not active
.global emu_hblankDmaJumpInstructions
emu_hblankDmaJumpInstructions:
    bl dma_dma0Transfer
    bl dma_dma1Transfer
    bl dma_dma2Transfer
    bl dma_dma3Transfer
#endif
    pop {r0-r3,r12}
    b emu_hblankIrqReturn

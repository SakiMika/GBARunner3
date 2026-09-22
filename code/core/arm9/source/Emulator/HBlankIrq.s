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
    // Closed cheat button overlay. Center-and-mask capture keeps using VRAM
    // C/D untouched. For only the final 16 scanlines of the physical lower
    // LCD, expose the pre-rendered strip in otherwise-unused VRAM B. The top
    // GBA image consumes the normal captured 240x160 area and is unaffected.
    ldr r13,= gCheatOverlayEnabled
    ldr r13, [r13]
    cmp r13, #0
    beq cheat_hblankOverlayDone

    cmp lr, #175
    bne cheat_hblankOverlayDone

    ldr r13,= gCheatOverlayActive
    mov lr, #1
    str lr, [r13]

    mov r13, #0x04000000
    mov lr, #0x80
    strb lr, [r13, #0x241]      // VRAM B -> LCDC
    mov lr, #0
    strh lr, [r13, #0x6C]       // main master brightness -> normal
    ldr lr,= 0x00060000         // MODE_FB2, direct display VRAM B
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

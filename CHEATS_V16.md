# Cheats v16

- Restores VRAM H/I to LCDC. GBARunner3 stores the GBA BIOS and ROM-cache lookup table in `.vramhi.bss`; remapping H to the sub-screen was the cause of the white/stalled game screen.
- Uses VRAM C for the lower-screen cheat UI. When cheats are present, center-and-mask capture is disabled, so VRAM C is available for Sub BG.
- Removes the old boot/VBlank debug text. The closed lower screen now shows only `[CHEAT]`.
- Keeps the existing ROM-ID JSON loader, multi-line codes, touch pause/resume, D-pad navigation, and A toggle.

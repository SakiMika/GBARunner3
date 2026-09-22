# Cheat support v19

## Runtime display fix
- Closed `[CHEAT]` strip moved from VRAM C to VRAM B.
- VRAM C/D are no longer remapped/stopped by the closed overlay.
- HBlank exposes VRAM B only for scanlines 176..191.
- VBlank restores VRAM B to MAIN_BG +0x40000 before the next emulated frame.
- Full cheat menu still uses VRAM C only while the VM is paused.

## Drill Dozer test cheats
- Unlimited Energy: single-line CodeBreaker write.
- Multi Jump: four-line CodeBreaker sequence using two D-button conditionals and two writes.

Multi Jump test code:
```
D0000020 0001
33000E3C 0004
D0000020 0001
83000EA6 FFFC
```

The current executor interprets `D0000020 0001` as: execute the next line while GBA KEYINPUT bit 0 (A) is held. Each D line gates exactly the following line, which matches this four-line sequence.

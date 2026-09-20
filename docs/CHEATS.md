# Cheat support

Cheat files live on the SD card at:

`/_gba/cheats/<ROM-ID>.json`

The primary ROM-ID follows the same convention as `/_gba/configs`: four-character GBA game code plus the two-digit software version. A secondary alias using the two-character maker code is also accepted for compatibility with ROM databases that call that combination the ROM ID.

Example for Drill Dozer:

```json
{
  "format": "codebreaker",
  "cheats": [
    {
      "name": "Drill Dozer Master Code",
      "codes": [
        "0000B9D7 2C97",
        "00007292 000A",
        "100F68A4 0007"
      ]
    },
    {
      "name": "Unlimited Energy",
      "codes": ["820346BC 02BC"]
    }
  ]
}
```

The supplied 8+4 hexadecimal examples are CodeBreaker Advance syntax (often loosely grouped with Action Replay cheats on cheat sites). Multi-line entries stay grouped as one menu item and are executed in sequence. Master-code Game ID/hook lines are accepted but do not install hardware hooks; GBARunner3 does not need the external cheat cartridge handler.

When at least one cheat is loaded, the lower LCD is reserved for the cheat UI and GBA output is placed on the upper LCD. Tap `[CHEAT]` to pause and open the list. Use D-pad Up/Down to select, Left/Right to page, A to toggle. Enabled cheats show `X` before the name. Tap `[CHEAT]` again to resume.

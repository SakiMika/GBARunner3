#pragma once

#include "common.h"
#include "GbaHeader.h"

class CheatService
{
public:
    static constexpr u32 MaxCheats = 64;
    static constexpr u32 MaxCodeLines = 24;
    static constexpr u32 MaxNameLength = 48;

    bool LoadForRom(const GbaHeader& header);
    void InitializeUi() __attribute__((long_call));
    bool HasCheats() const { return _cheatCount != 0; }
    void OnVBlank();

private:
    struct CodeLine
    {
        u32 op1;
        u32 op2;
        u8 operandDigits;
    };

    struct Cheat
    {
        char name[MaxNameLength];
        CodeLine lines[MaxCodeLines];
        u8 lineCount;
        bool enabled;
    };

    // Deliberately no C++ default member initializers here. This object lives
    // in .ewram.bss and is cleared by crt0. Keeping it trivially initialized
    // avoids an early .init_array constructor that would execute EWRAM code
    // before gbaRunnerMain. LoadForRom() resets the runtime state explicitly.
    Cheat _cheats[MaxCheats];
    u32 _cheatCount;
    u32 _selected;
    u32 _scroll;
    bool _menuOpen;
    bool _uiInitialized;
    bool _touchWasDown;
    bool _hotkeyWasDown;

    bool TryLoadFile(const char* path);
    static bool ParseCodeLine(const char* text, CodeLine& line);
    void ApplyEnabledCheats();
    void ApplyCodeBreakerCheat(const Cheat& cheat);
    void SetPaused(bool paused);
    bool ReadCheatButtonPressed();
    void RunMenuLoop();
    void RenderClosed();
    void RenderMenu();
};

extern CheatService gCheatService;

#ifdef __cplusplus
extern "C" {
#endif
void cheat_onVBlank(void);
#ifdef __cplusplus
}
#endif

#pragma once

#include "common.h"
#include "GbaHeader.h"

class CheatService
{
public:
    static constexpr u32 MaxCheats = 64;
    static constexpr u32 MaxCodeLines = 24;
    static constexpr u32 MaxNameLength = 48;

    [[gnu::long_call]] bool LoadForRom(const GbaHeader& header);
    [[gnu::long_call]] void InitializeUi();
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

    Cheat _cheats[MaxCheats] {};
    u32 _cheatCount = 0;
    u32 _selected = 0;
    u32 _scroll = 0;
    bool _menuOpen = false;
    bool _uiInitialized = false;
    bool _touchWasDown = false;

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

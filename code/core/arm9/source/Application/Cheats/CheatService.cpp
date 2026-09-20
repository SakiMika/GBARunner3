#include "CheatService.h"
#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <memory>
#include <string.h>
#include <nds.h>
#include <mini-printf.h>
#include <libtwl/mem/memVram.h>
#include <libtwl/gfx/gfx.h>
#include <libtwl/sys/sysPower.h>
#include "Application/Settings/Json/ArduinoJson.h"
#include "Fat/ff.h"
#include "MemoryEmulator/MemoryLoadStore.h"
#include "Peripherals/Sound/GbaSound9.h"
#include "SystemIpc.h"
#include "cp15.h"

#define CHEAT_PATH_VERSION_FORMAT "/_gba/cheats/%c%c%c%c%02X.json"
#define CHEAT_PATH_MAKER_FORMAT   "/_gba/cheats/%c%c%c%c%c%c.json"
#define CHEAT_FILE_MAX_SIZE       (24 * 1024)
#define CHEAT_JSON_CAPACITY       (32 * 1024)
#define CHEAT_VISIBLE_ROWS        18
#define CHEAT_BUTTON_X0           200
#define CHEAT_BUTTON_Y0           168

[[gnu::section(".ewram.bss"), gnu::aligned(4)]]
CheatService gCheatService;
static PrintConsole sCheatConsole;

// The normal emulator IRQ stack is only 288 bytes. The cheat menu calls the
// console renderer while the GBA VM is paused inside VBlank, so give it a
// dedicated stack instead of risking DTCM IRQ stack corruption.
extern "C" {
[[gnu::section(".ewram.bss"), gnu::aligned(8)]]
u8 gCheatIrqStack[4096];
}

static bool parseHex(const char*& p, u32& value, u32 minDigits, u32 maxDigits, u32& digits)
{
    value = 0;
    digits = 0;
    while (*p == ' ' || *p == '\t') ++p;
    while (digits < maxDigits)
    {
        char c = *p;
        u32 nibble;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else break;
        value = (value << 4) | nibble;
        ++digits;
        ++p;
    }
    return digits >= minDigits;
}

bool CheatService::ParseCodeLine(const char* text, CodeLine& line)
{
    if (!text) return false;
    const char* p = text;
    u32 d1 = 0, d2 = 0;
    if (!parseHex(p, line.op1, 8, 8, d1)) return false;
    if (!parseHex(p, line.op2, 4, 8, d2)) return false;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != '\0') return false;
    line.operandDigits = static_cast<u8>(d2);
    return true;
}

bool CheatService::TryLoadFile(const char* path)
{
    FIL file {};
    if (f_open(&file, path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return false;

    const u32 fileSize = f_size(&file);
    if (fileSize == 0 || fileSize > CHEAT_FILE_MAX_SIZE)
    {
        f_close(&file);
        return false;
    }

    auto buffer = std::make_unique<char[]>(fileSize + 1);
    UINT bytesRead = 0;
    const FRESULT readResult = f_read(&file, buffer.get(), fileSize, &bytesRead);
    f_close(&file);
    if (readResult != FR_OK || bytesRead != fileSize)
        return false;
    buffer[fileSize] = '\0';

    DynamicJsonDocument json(CHEAT_JSON_CAPACITY);
    const auto result = deserializeJson(json, buffer.get(), fileSize);
    if (result != DeserializationError::Ok)
    {
        gLogger->Log(LogLevel::Debug, "Cheat JSON parse error: %d (%s)\n", result, path);
        return false;
    }

    JsonArrayConst cheats = json["cheats"].as<JsonArrayConst>();
    if (cheats.isNull())
        return false;

    _cheatCount = 0;
    for (JsonObjectConst object : cheats)
    {
        if (_cheatCount >= MaxCheats)
            break;

        const char* name = object["name"] | "Unnamed cheat";
        JsonArrayConst codes = object["codes"].as<JsonArrayConst>();
        if (codes.isNull())
            continue;

        Cheat& cheat = _cheats[_cheatCount];
        memset(&cheat, 0, sizeof(cheat));
        strncpy(cheat.name, name, MaxNameLength - 1);
        cheat.name[MaxNameLength - 1] = '\0';

        for (JsonVariantConst value : codes)
        {
            if (cheat.lineCount >= MaxCodeLines)
                break;
            if (!value.is<const char*>())
                continue;
            CodeLine line {};
            if (ParseCodeLine(value.as<const char*>(), line))
                cheat.lines[cheat.lineCount++] = line;
        }

        if (cheat.lineCount != 0)
            ++_cheatCount;
    }

    if (_cheatCount != 0)
        gLogger->Log(LogLevel::Debug, "Loaded %u cheats from %s\n", _cheatCount, path);
    return _cheatCount != 0;
}

bool CheatService::LoadForRom(const GbaHeader& header)
{
    _cheatCount = 0;
    char path[96];
    const char c0 = header.gameCode & 0xFF;
    const char c1 = (header.gameCode >> 8) & 0xFF;
    const char c2 = (header.gameCode >> 16) & 0xFF;
    const char c3 = (header.gameCode >> 24) & 0xFF;

    mini_snprintf(path, sizeof(path), CHEAT_PATH_VERSION_FORMAT,
        c0, c1, c2, c3, header.softwareVersion);
    if (TryLoadFile(path))
        return true;

    // Compatibility alias: some ROM databases/users call GAMECODE+makerCode the ROM ID.
    // Example: Drill Dozer has gameCode V49E and makerCode "01" => V49E01.json.
    const char maker0 = header.makerCode & 0xFF;
    const char maker1 = (header.makerCode >> 8) & 0xFF;
    mini_snprintf(path, sizeof(path), CHEAT_PATH_MAKER_FORMAT,
        c0, c1, c2, c3, maker0, maker1);
    return TryLoadFile(path);
}

void CheatService::InitializeUi()
{
    if (!HasCheats())
        return;

    // A persistent touch UI needs the lower LCD. Keep GBA rendering on the main engine/top LCD.
    sys_setMainEngineToTopScreen();
    sysipc_setTopBacklight(true);
    sysipc_setBottomBacklight(true);

    mem_setVramHMapping(MEM_VRAM_H_SUB_BG_00000);
    videoSetModeSub(MODE_0_2D);
    consoleInit(&sCheatConsole, 0, BgType_Text4bpp, BgSize_T_256x256, 8, 0, false, true);
    consoleSelect(&sCheatConsole);
    _uiInitialized = true;
    RenderClosed();
}

void CheatService::RenderClosed()
{
    if (!_uiInitialized) return;
    consoleSelect(&sCheatConsole);
    consoleClear();
    iprintf("\x1b[22;26H[CHEAT]");
}

void CheatService::RenderMenu()
{
    if (!_uiInitialized) return;
    consoleSelect(&sCheatConsole);
    consoleClear();
    iprintf("CHEATS  A: toggle  D-Pad: move\n");
    iprintf("Tap CHEAT again to resume\n\n");

    if (_selected < _scroll) _scroll = _selected;
    if (_selected >= _scroll + CHEAT_VISIBLE_ROWS)
        _scroll = _selected - CHEAT_VISIBLE_ROWS + 1;

    const u32 end = std::min(_cheatCount, _scroll + CHEAT_VISIBLE_ROWS);
    for (u32 i = _scroll; i < end; ++i)
    {
        const char cursor = i == _selected ? '>' : ' ';
        const char enabled = _cheats[i].enabled ? 'X' : ' ';
        iprintf("%c%c %-28.28s\n", cursor, enabled, _cheats[i].name);
    }
    iprintf("\x1b[22;26H[CHEAT]");
}

void CheatService::SetPaused(bool paused)
{
    gGbaSoundShared.cheatControl.pauseAudio = paused ? 1 : 0;
    dc_flushRange((void*)&gGbaSoundShared.cheatControl, sizeof(gGbaSoundShared.cheatControl));
}

bool CheatService::ReadCheatButtonPressed()
{
    dc_invalidateRange((void*)&gGbaSoundShared.cheatInput, sizeof(gGbaSoundShared.cheatInput));
    const bool down = gGbaSoundShared.cheatInput.touchDown != 0;
    bool pressed = false;
    if (down && !_touchWasDown)
    {
        const int x = gGbaSoundShared.cheatInput.touchX;
        const int y = gGbaSoundShared.cheatInput.touchY;
        pressed = x >= CHEAT_BUTTON_X0 && y >= CHEAT_BUTTON_Y0;
    }
    _touchWasDown = down;
    return pressed;
}

static inline u16 readKeysHeld()
{
    return static_cast<u16>((~REG_KEYINPUT) & 0x03FF);
}

static void waitNextFramePolling()
{
    while (REG_VCOUNT >= 192) { }
    while (REG_VCOUNT < 192) { }
}

void CheatService::RunMenuLoop()
{
    _menuOpen = true;
    SetPaused(true);
    RenderMenu();

    u16 previousKeys = readKeysHeld();
    while (_menuOpen)
    {
        waitNextFramePolling();
        const u16 keys = readKeysHeld();
        const u16 down = keys & ~previousKeys;
        previousKeys = keys;

        bool redraw = false;
        if ((down & KEY_UP) && _selected > 0)
        {
            --_selected;
            redraw = true;
        }
        if ((down & KEY_DOWN) && _selected + 1 < _cheatCount)
        {
            ++_selected;
            redraw = true;
        }
        if ((down & KEY_LEFT) && _selected > 0)
        {
            _selected = _selected > CHEAT_VISIBLE_ROWS ? _selected - CHEAT_VISIBLE_ROWS : 0;
            redraw = true;
        }
        if ((down & KEY_RIGHT) && _selected + 1 < _cheatCount)
        {
            _selected = std::min(_cheatCount - 1, _selected + CHEAT_VISIBLE_ROWS);
            redraw = true;
        }
        if ((down & KEY_A) && _cheatCount != 0)
        {
            _cheats[_selected].enabled = !_cheats[_selected].enabled;
            redraw = true;
        }
        if (ReadCheatButtonPressed())
        {
            _menuOpen = false;
            break;
        }
        if (redraw)
            RenderMenu();
    }

    // Do not leak the menu A/D-pad press into the game.
    while (readKeysHeld() != 0)
        waitNextFramePolling();

    SetPaused(false);
    RenderClosed();
}

void CheatService::ApplyCodeBreakerCheat(const Cheat& cheat)
{
    bool executeNext = true;
    for (u32 i = 0; i < cheat.lineCount; ++i)
    {
        const CodeLine& line = cheat.lines[i];
        // The supplied GBA codes use CodeBreaker Advance's 8+4 hex format.
        if (line.operandDigits > 4)
            continue; // AR/GS encrypted 8+8 codes are intentionally not mis-decoded.

        const u32 type = line.op1 >> 28;
        const u32 address = line.op1 & 0x0FFFFFFF;
        const u16 operand = static_cast<u16>(line.op2);

        if (!executeNext)
        {
            executeNext = true;
            continue;
        }

        switch (type)
        {
            case 0x0: // Game ID / master-code metadata: no hardware hook needed in an emulator.
            case 0x1: // Master-code hook: no-op for the emulator.
                break;
            case 0x2: // 16-bit OR
                memu_store16FromC(address, memu_load16FromC(address) | operand);
                break;
            case 0x3: // 8-bit write
                memu_store8FromC(address, operand & 0xFF);
                break;
            case 0x6: // 16-bit AND
                memu_store16FromC(address, memu_load16FromC(address) & operand);
                break;
            case 0x7: // execute following line if equal
                executeNext = memu_load16FromC(address) == operand;
                break;
            case 0x8: // 16-bit write
                memu_store16FromC(address, operand);
                break;
            case 0xA: // execute following line if not equal
                executeNext = memu_load16FromC(address) != operand;
                break;
            case 0xB: // execute following line if greater than
                executeNext = memu_load16FromC(address) > operand;
                break;
            case 0xC: // execute following line if less than
                executeNext = memu_load16FromC(address) < operand;
                break;
            case 0xD:
                // Common CodeBreaker key conditional: D0000020 XXXX checks GBA KEYINPUT.
                if (address == 0x20)
                    executeNext = (memu_load16FromC(0x04000130) & operand) == 0;
                break;
            case 0xE: // 16-bit add
                memu_store16FromC(address, memu_load16FromC(address) + operand);
                break;
            case 0xF: // execute following line if any masked bit is set
                executeNext = (memu_load16FromC(address) & operand) != 0;
                break;
            default:
                break;
        }
    }
}

void CheatService::ApplyEnabledCheats()
{
    for (u32 i = 0; i < _cheatCount; ++i)
    {
        if (_cheats[i].enabled)
            ApplyCodeBreakerCheat(_cheats[i]);
    }
}

void CheatService::OnVBlank()
{
    if (!HasCheats())
        return;

    ApplyEnabledCheats();
    if (ReadCheatButtonPressed())
        RunMenuLoop();
}

extern "C" void cheat_onVBlank(void)
{
    gCheatService.OnVBlank();
}

#include "CheatService.h"
#include <nds.h>
#include <libtwl/sys/sysPower.h>
#include "MemoryEmulator/MemoryLoadStore.h"
#include "Peripherals/Sound/GbaSound9.h"
#include "SystemIpc.h"
#include "cp15.h"
#include "Application/BootDebug.h"
#include "Application/GbaDisplayConfigurationService.h"
#include "Application/Settings/AppSettingsService.h"

#pragma GCC optimize("Os")

#define CHEAT_VISIBLE_ROWS        18
#define CHEAT_BUTTON_X0           184
#define CHEAT_BUTTON_Y0           172
#define CHEAT_BUTTON_ROW          22
#define CHEAT_BUTTON_COL          25
#define SUB_BG_TILE_BASE          ((volatile u8*)0x06200000)
#define SUB_BG_MAP_BASE           ((volatile u16*)0x06204000)
#define SUB_BG_PALETTE            ((volatile u16*)0x05000400)

extern "C" {
extern volatile u32 gCheatVBlankEnabled;
extern volatile u32 gCheatOverlayEnabled;
extern volatile u32 gCheatOverlayActive;
extern volatile u32 gCheatPendingDispCnt;
}

static void uiClear()
{
    for (u32 i = 0; i < 32 * 32; ++i)
        SUB_BG_MAP_BASE[i] = 0;
}

static void uiPutChar(u32 x, u32 y, char c)
{
    if (x >= 32 || y >= 32) return;
    u8 uc = static_cast<u8>(c);
    if (uc < 32 || uc > 126) uc = '?';
    SUB_BG_MAP_BASE[y * 32 + x] = static_cast<u16>(uc - 32);
}

static void uiPrint(u32 x, u32 y, const char* text, u32 maxChars = 32)
{
    while (*text && x < 32 && maxChars--)
        uiPutChar(x++, y, *text++);
}

void CheatService::InitializeUi()
{
    // The assembly hooks remain inert throughout boot and are enabled only
    // after the shared touch state and the closed-button framebuffer exist.
    gCheatVBlankEnabled = 0;
    gCheatOverlayEnabled = 0;
    gCheatOverlayActive = 0;
    if (!HasCheats())
        return;

    _uiInitialized = true;

    // Center-and-mask keeps the GBA image at the stock 240x160 centered
    // position on the top LCD.  The lower LCD stays black except for a small
    // 16-scanline framebuffer strip shown by the HBlank overlay.
    BootDebug_PrepareClosedButton();
    BootDebug_EnableBottomBacklight();

    dc_invalidateRange((void*)&gGbaSoundShared.cheatInput, sizeof(gGbaSoundShared.cheatInput));
    _touchWasDown = gGbaSoundShared.cheatInput.touchDown != 0;
    _hotkeyWasDown = false;

    gCheatPendingDispCnt = REG_DISPCNT;
    dc_flushRange((void*)&gGbaSoundShared, sizeof(gGbaSoundShared));
    dc_flushRange((void*)&gCheatPendingDispCnt, sizeof(gCheatPendingDispCnt));

    gCheatOverlayEnabled = 1;
    gCheatVBlankEnabled = 1;
    dc_flushRange((void*)&gCheatOverlayEnabled, sizeof(gCheatOverlayEnabled));
    dc_flushRange((void*)&gCheatVBlankEnabled, sizeof(gCheatVBlankEnabled));
}

void CheatService::RenderClosed()
{
    if (!_uiInitialized) return;
    BootDebug_PrepareClosedButton();
}

void CheatService::RenderMenu()
{
    if (!_uiInitialized) return;
    uiClear();
    uiPrint(0, 0, "CHEATS   A: TOGGLE   D-PAD: MOVE");
    uiPrint(0, 1, "B/TAP [CHEAT] TO RESUME");

    if (_selected < _scroll) _scroll = _selected;
    if (_selected >= _scroll + CHEAT_VISIBLE_ROWS)
        _scroll = _selected - CHEAT_VISIBLE_ROWS + 1;

    const u32 end = (_scroll + CHEAT_VISIBLE_ROWS < _cheatCount)
        ? (_scroll + CHEAT_VISIBLE_ROWS) : _cheatCount;
    for (u32 i = _scroll; i < end; ++i)
    {
        const u32 row = 3 + i - _scroll;
        uiPutChar(0, row, i == _selected ? '>' : ' ');
        uiPutChar(1, row, _cheats[i].enabled ? 'X' : ' ');
        uiPutChar(2, row, ' ');
        uiPrint(3, row, _cheats[i].name, 29);
    }
    uiPrint(CHEAT_BUTTON_COL, CHEAT_BUTTON_ROW, "[CHEAT]", 7);
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
    gCheatOverlayEnabled = 0;
    gCheatOverlayActive = 0;
    dc_flushRange((void*)&gCheatOverlayEnabled, sizeof(gCheatOverlayEnabled));
    dc_flushRange((void*)&gCheatOverlayActive, sizeof(gCheatOverlayActive));

    SetPaused(true);

    // While the VM is paused, move the raw main engine to the top LCD and use
    // the sub engine/VRAM C for the full cheat list on the touch screen.
    // Normal centered capture is restored before the VM resumes.
    sys_setMainEngineToTopScreen();
    sysipc_setTopBacklight(true);
    sysipc_setBottomBacklight(true);
    REG_MASTER_BRIGHT = 0;
    BootDebug_RestoreVideo();
    BootDebug_EnableBottomBacklight();
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
            const u32 next = _selected + CHEAT_VISIBLE_ROWS;
            _selected = next < _cheatCount ? next : _cheatCount - 1;
            redraw = true;
        }
        if ((down & KEY_A) && _cheatCount)
        {
            _cheats[_selected].enabled = !_cheats[_selected].enabled;
            redraw = true;
        }
        const u16 closeHotkey = KEY_L | KEY_R | KEY_SELECT;
        const bool closeHotkeyPressed =
            ((keys & closeHotkey) == closeHotkey) && ((down & closeHotkey) != 0);
        if ((down & KEY_B) || ReadCheatButtonPressed() || closeHotkeyPressed)
        {
            _menuOpen = false;
            break;
        }
        if (redraw) RenderMenu();
    }

    while (readKeysHeld() != 0)
        waitNextFramePolling();

    BootDebug_Hide();

    // Restore the exact normal GBARunner3 display path: centered 240x160 game
    // on the top LCD, hidden/raw main engine on the lower LCD.  Then re-create
    // the closed button strip and light the lower LCD back up.
    auto displaySettings = gAppSettingsService.GetAppSettings().displaySettings;
    displaySettings.gbaScreen = GbaScreen::Top;
    displaySettings.enableCenterAndMask = true;
    gGbaDisplayConfigurationService.ApplyDisplaySettings(displaySettings);
    RenderClosed();
    BootDebug_EnableBottomBacklight();

    gCheatPendingDispCnt = REG_DISPCNT;
    gCheatOverlayActive = 0;
    gCheatOverlayEnabled = 1;
    dc_flushRange((void*)&gCheatPendingDispCnt, sizeof(gCheatPendingDispCnt));
    dc_flushRange((void*)&gCheatOverlayActive, sizeof(gCheatOverlayActive));
    dc_flushRange((void*)&gCheatOverlayEnabled, sizeof(gCheatOverlayEnabled));

    SetPaused(false);
}

// Reuse the existing 32-bit C bridge instead of adding 8/16-bit wrappers to
// ITCM. GBARunner3's 32 KiB ITCM layout is extremely tight and is arranged at
// fixed offsets by gbarunner9.ld. Read-modify-write preserves neighbouring
// bytes/halfwords while keeping the original ITCM image unchanged.
static inline u8 cheatRead8(u32 address)
{
    const u32 aligned = address & ~3u;
    const u32 word = memu_load32FromC(aligned);
    return static_cast<u8>(word >> ((address & 3u) * 8));
}

static inline u16 cheatRead16(u32 address)
{
    const u32 byte = address & 3u;
    if (byte != 3u)
    {
        const u32 word = memu_load32FromC(address & ~3u);
        return static_cast<u16>(word >> (byte * 8));
    }
    // Rare unaligned halfword crossing a 32-bit boundary.
    return static_cast<u16>(cheatRead8(address) |
        (static_cast<u16>(cheatRead8(address + 1)) << 8));
}

static inline void cheatWrite8(u32 address, u8 value)
{
    const u32 aligned = address & ~3u;
    const u32 shift = (address & 3u) * 8;
    const u32 mask = 0xFFu << shift;
    const u32 oldWord = memu_load32FromC(aligned);
    memu_store32FromC(aligned, (oldWord & ~mask) | (static_cast<u32>(value) << shift));
}

static inline void cheatWrite16(u32 address, u16 value)
{
    const u32 byte = address & 3u;
    if (byte != 3u)
    {
        const u32 aligned = address & ~3u;
        const u32 shift = byte * 8;
        const u32 mask = 0xFFFFu << shift;
        const u32 oldWord = memu_load32FromC(aligned);
        memu_store32FromC(aligned,
            (oldWord & ~mask) | (static_cast<u32>(value) << shift));
        return;
    }
    cheatWrite8(address, static_cast<u8>(value));
    cheatWrite8(address + 1, static_cast<u8>(value >> 8));
}

void CheatService::ApplyCodeBreakerCheat(const Cheat& cheat)
{
    bool executeNext = true;
    for (u32 i = 0; i < cheat.lineCount; ++i)
    {
        const CodeLine& line = cheat.lines[i];
        if (line.operandDigits > 4)
            continue;

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
            case 0x0:
            case 0x1:
                break;
            case 0x2:
                cheatWrite16(address, static_cast<u16>(cheatRead16(address) | operand));
                break;
            case 0x3:
                cheatWrite8(address, static_cast<u8>(operand));
                break;
            case 0x6:
                cheatWrite16(address, static_cast<u16>(cheatRead16(address) & operand));
                break;
            case 0x7:
                executeNext = cheatRead16(address) == operand;
                break;
            case 0x8:
                cheatWrite16(address, operand);
                break;
            case 0xA:
                executeNext = cheatRead16(address) != operand;
                break;
            case 0xB:
                executeNext = cheatRead16(address) > operand;
                break;
            case 0xC:
                executeNext = cheatRead16(address) < operand;
                break;
            case 0xD:
                if (address == 0x20)
                    executeNext = (cheatRead16(0x04000130) & operand) == 0;
                break;
            case 0xE:
                cheatWrite16(address, static_cast<u16>(cheatRead16(address) + operand));
                break;
            case 0xF:
                executeNext = (cheatRead16(address) & operand) != 0;
                break;
            default:
                break;
        }
    }
}

void CheatService::ApplyEnabledCheats()
{
    for (u32 i = 0; i < _cheatCount; ++i)
        if (_cheats[i].enabled)
            ApplyCodeBreakerCheat(_cheats[i]);
}

void CheatService::OnVBlank()
{
    if (!HasCheats()) return;

    ApplyEnabledCheats();

    // Touch is the primary control. L+R+SELECT is a hardware fallback so the
    // menu remains reachable even if a particular touch-panel calibration or
    // ARM7 touch path misbehaves.
    const u16 keys = readKeysHeld();
    const u16 hotkey = KEY_L | KEY_R | KEY_SELECT;
    const bool hotkeyDown = (keys & hotkey) == hotkey;
    const bool hotkeyPressed = hotkeyDown && !_hotkeyWasDown;
    _hotkeyWasDown = hotkeyDown;

    if (ReadCheatButtonPressed() || hotkeyPressed)
        RunMenuLoop();
}

extern "C" void cheat_onVBlank(void)
{
    gCheatService.OnVBlank();
}

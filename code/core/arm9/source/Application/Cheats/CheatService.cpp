#include "CheatService.h"
#include <nds.h>
#include <string.h>
#include <libtwl/mem/memVram.h>
#include <libtwl/sys/sysPower.h>
#include "Fat/ff.h"
#include "MemoryEmulator/MemoryLoadStore.h"
#include "Peripherals/Sound/GbaSound9.h"
#include "SystemIpc.h"
#include "cp15.h"

#pragma GCC optimize("Os")

#define CHEAT_FILE_MAX_SIZE       (24 * 1024)
#define CHEAT_VISIBLE_ROWS        18
#define CHEAT_BUTTON_X0           192
#define CHEAT_BUTTON_Y0           160
#define CHEAT_BUTTON_ROW          22
#define CHEAT_BUTTON_COL          25
#define SUB_BG_TILE_BASE          ((volatile u8*)0x06200000)
#define SUB_BG_MAP_BASE           ((volatile u16*)0x06204000)
#define SUB_BG_PALETTE            ((volatile u16*)0x05000400)

[[gnu::section(".ewram.bss"), gnu::aligned(4)]]
CheatService gCheatService;

// Dedicated storage avoids heap pressure while a ROM is being initialized.
[[gnu::section(".ewram.bss"), gnu::aligned(32)]]
static char sCheatFileBuffer[CHEAT_FILE_MAX_SIZE + 1];

// GBARunner3's normal ARM9 stack is only ~992 bytes. FatFs FIL contains a
// sector-sized private buffer and a temporary Cheat is several hundred bytes,
// so keeping either as a local variable can overflow the boot stack while the
// splash screen is still visible. Keep all cheat-loader workspace in EWRAM.
[[gnu::section(".ewram.bss"), gnu::aligned(32)]]
static FIL sCheatFile;
[[gnu::section(".ewram.bss"), gnu::aligned(4)]]
static char sCheatPath[64];
[[gnu::section(".ewram.bss"), gnu::aligned(4)]]
static char sJsonKey[16];
[[gnu::section(".ewram.bss"), gnu::aligned(4)]]
static char sCodeText[32];

// The emulator IRQ stack is deliberately tiny. Menu rendering/polling uses its
// own stack so opening the menu cannot corrupt the VM's normal IRQ stack.
extern "C" {
[[gnu::section(".ewram.bss"), gnu::aligned(8)]]
u8 gCheatIrqStack[4096];

// VBlank assembly checks this before it ever switches stacks or enters C++.
// Keep it zero throughout boot; InitializeUi() enables it only after all cheat
// state, shared IPC data and display resources are ready.
[[gnu::section(".ewram.bss"), gnu::aligned(4)]]
volatile u32 gCheatVBlankEnabled;

// Closed-button scanline overlay state. The HBlank/VBlank assembly only
// touches these simple words; no C/C++ call is made from the timing-critical
// path.
[[gnu::section(".ewram.bss"), gnu::aligned(4)]]
volatile u32 gCheatOverlayEnabled;
[[gnu::section(".ewram.bss"), gnu::aligned(4)]]
volatile u32 gCheatOverlayActive;
[[gnu::section(".ewram.bss"), gnu::aligned(4)]]
volatile u32 gCheatPendingDispCnt;
}


static const char* findCharLocal(const char* p, char needle)
{
    while (*p)
    {
        if (*p == needle) return p;
        ++p;
    }
    return nullptr;
}

static bool textEqualsLocal(const char* a, const char* b)
{
    while (*a && *b)
    {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

static void copyLiteralLocal(char* dst, u32 dstSize, const char* src)
{
    if (!dstSize) return;
    u32 i = 0;
    while (src[i] && i + 1 < dstSize)
    {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

static const char* findLiteralLocal(const char* haystack, const char* needle)
{
    if (!*needle) return haystack;
    for (const char* h = haystack; *h; ++h)
    {
        const char* a = h;
        const char* b = needle;
        while (*a && *b && *a == *b)
        {
            ++a;
            ++b;
        }
        if (!*b) return h;
    }
    return nullptr;
}

static inline const char* skipWs(const char* p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

static bool parseJsonString(const char*& p, char* out, u32 outSize)
{
    p = skipWs(p);
    if (*p != '"') return false;
    ++p;
    u32 n = 0;
    while (*p && *p != '"')
    {
        char c = *p++;
        if (c == '\\')
        {
            const char e = *p++;
            if (!e) return false;
            switch (e)
            {
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'u':
                    // Cheat names/codes are expected to be ASCII. Consume the
                    // four hex digits and show '?' instead of expanding UTF-16.
                    for (int i = 0; i < 4 && *p; ++i) ++p;
                    c = '?';
                    break;
                default: c = e; break;
            }
        }
        if (out && outSize && n + 1 < outSize)
            out[n++] = c;
    }
    if (*p != '"') return false;
    ++p;
    if (out && outSize) out[n] = 0;
    return true;
}

static void skipJsonValue(const char*& p)
{
    p = skipWs(p);
    if (*p == '"')
    {
        parseJsonString(p, nullptr, 0);
        return;
    }
    if (*p == '{' || *p == '[')
    {
        const char open = *p++;
        const char close = open == '{' ? '}' : ']';
        int depth = 1;
        while (*p && depth > 0)
        {
            if (*p == '"')
            {
                parseJsonString(p, nullptr, 0);
                continue;
            }
            if (*p == open) ++depth;
            else if (*p == close) --depth;
            ++p;
        }
        return;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') ++p;
}

static bool parseHex(const char*& p, u32& value, u32 minDigits, u32 maxDigits, u32& digits)
{
    value = 0;
    digits = 0;
    while (*p == ' ' || *p == '\t') ++p;
    while (digits < maxDigits)
    {
        const char c = *p;
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

static char hexDigit(u8 value)
{
    value &= 0xF;
    return value < 10 ? ('0' + value) : ('A' + value - 10);
}

static void buildVersionPath(char* out, const GbaHeader& header)
{
    static const char prefix[] = "/_gba/cheats/";
    char* d = out;
    for (const char* s = prefix; *s; ++s) *d++ = *s;
    *d++ = header.gameCode & 0xFF;
    *d++ = (header.gameCode >> 8) & 0xFF;
    *d++ = (header.gameCode >> 16) & 0xFF;
    *d++ = (header.gameCode >> 24) & 0xFF;
    *d++ = hexDigit(header.softwareVersion >> 4);
    *d++ = hexDigit(header.softwareVersion);
    *d++ = '.'; *d++ = 'j'; *d++ = 's'; *d++ = 'o'; *d++ = 'n'; *d = 0;
}

static void buildMakerPath(char* out, const GbaHeader& header)
{
    static const char prefix[] = "/_gba/cheats/";
    char* d = out;
    for (const char* s = prefix; *s; ++s) *d++ = *s;
    *d++ = header.gameCode & 0xFF;
    *d++ = (header.gameCode >> 8) & 0xFF;
    *d++ = (header.gameCode >> 16) & 0xFF;
    *d++ = (header.gameCode >> 24) & 0xFF;
    *d++ = header.makerCode & 0xFF;
    *d++ = (header.makerCode >> 8) & 0xFF;
    *d++ = '.'; *d++ = 'j'; *d++ = 's'; *d++ = 'o'; *d++ = 'n'; *d = 0;
}

bool CheatService::ParseCodeLine(const char* text, CodeLine& line)
{
    if (!text) return false;
    const char* p = text;
    u32 d1 = 0, d2 = 0;
    if (!parseHex(p, line.op1, 8, 8, d1)) return false;
    if (!parseHex(p, line.op2, 4, 8, d2)) return false;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != 0) return false;
    line.operandDigits = static_cast<u8>(d2);
    return true;
}

bool CheatService::TryLoadFile(const char* path)
{
    // Never put FIL on the normal ARM9 stack: FF_FS_TINY=0 gives FIL its own
    // sector buffer, which alone consumes more than half of that stack.
    memset(&sCheatFile, 0, sizeof(sCheatFile));
    if (f_open(&sCheatFile, path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
        return false;

    const u32 fileSize = f_size(&sCheatFile);
    if (fileSize == 0 || fileSize > CHEAT_FILE_MAX_SIZE)
    {
        f_close(&sCheatFile);
        return false;
    }

    UINT bytesRead = 0;
    const FRESULT result = f_read(&sCheatFile, sCheatFileBuffer, fileSize, &bytesRead);
    f_close(&sCheatFile);
    if (result != FR_OK || bytesRead != fileSize)
        return false;
    sCheatFileBuffer[fileSize] = 0;

    const char* p = findLiteralLocal(sCheatFileBuffer, "\"cheats\"");
    if (!p) return false;
    p = findCharLocal(p, '[');
    if (!p) return false;
    ++p;

    _cheatCount = 0;
    while (*p && _cheatCount < MaxCheats)
    {
        p = skipWs(p);
        if (*p == ']') break;
        if (*p == ',') { ++p; continue; }
        if (*p != '{') { ++p; continue; }
        ++p;

        // Parse directly into the final EWRAM slot. This avoids a ~340-byte
        // temporary Cheat object on the boot stack. _cheatCount is advanced
        // only after at least one valid code line was parsed.
        Cheat& cheat = _cheats[_cheatCount];
        memset(&cheat, 0, sizeof(cheat));
        copyLiteralLocal(cheat.name, MaxNameLength, "Unnamed cheat");

        while (*p)
        {
            p = skipWs(p);
            if (*p == '}') { ++p; break; }
            if (*p == ',') { ++p; continue; }

            if (!parseJsonString(p, sJsonKey, sizeof(sJsonKey)))
            {
                ++p;
                continue;
            }
            p = skipWs(p);
            if (*p != ':') return false;
            ++p;
            p = skipWs(p);

            if (textEqualsLocal(sJsonKey, "name"))
            {
                if (!parseJsonString(p, cheat.name, MaxNameLength)) return false;
            }
            else if (textEqualsLocal(sJsonKey, "codes"))
            {
                if (*p != '[') return false;
                ++p;
                while (*p)
                {
                    p = skipWs(p);
                    if (*p == ']') { ++p; break; }
                    if (*p == ',') { ++p; continue; }
                    if (!parseJsonString(p, sCodeText, sizeof(sCodeText))) return false;
                    if (cheat.lineCount < MaxCodeLines)
                    {
                        CodeLine& line = cheat.lines[cheat.lineCount];
                        memset(&line, 0, sizeof(line));
                        if (ParseCodeLine(sCodeText, line))
                            ++cheat.lineCount;
                    }
                }
            }
            else
            {
                skipJsonValue(p);
            }
        }

        if (cheat.lineCount)
            ++_cheatCount;
    }

    if (_cheatCount)
        gLogger->Log(LogLevel::Debug, "Loaded %u cheats from %s\n", _cheatCount, path);
    return _cheatCount != 0;
}

bool CheatService::LoadForRom(const GbaHeader& header)
{
    // gCheatService is intentionally a trivial .ewram.bss object. Reset every
    // runtime field here rather than relying on a global C++ constructor.
    gCheatVBlankEnabled = 0;
    gCheatOverlayEnabled = 0;
    gCheatOverlayActive = 0;
    gCheatPendingDispCnt = 0;
    _cheatCount = 0;
    _selected = 0;
    _scroll = 0;
    _menuOpen = false;
    _uiInitialized = false;
    _touchWasDown = false;
    _hotkeyWasDown = false;
    memset(_cheats, 0, sizeof(_cheats));

    buildVersionPath(sCheatPath, header);
    if (TryLoadFile(sCheatPath))
    {
        return true;
    }
    buildMakerPath(sCheatPath, header);
    const bool loaded = TryLoadFile(sCheatPath);
    return loaded;
}

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
}

static const u8 sFont8x8[95][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
    {0x02, 0x02, 0x02, 0x02, 0x02, 0x00, 0x00, 0x02}, // '!'
    {0x06, 0x06, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00}, // '"'
    {0x14, 0x0C, 0x0C, 0x1E, 0x0A, 0x1E, 0x0A, 0x06}, // '#'
    {0x1C, 0x2A, 0x2A, 0x0E, 0x18, 0x28, 0x2A, 0x1C}, // '$'
    {0x4E, 0x2A, 0x2A, 0x1E, 0xF0, 0xA8, 0xA4, 0xE4}, // '%'
    {0x1C, 0x02, 0x22, 0x7C, 0x22, 0x22, 0x22, 0x3C}, // '&'
    {0x02, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00}, // "'"
    {0x04, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04}, // '('
    {0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00}, // ')'
    {0x00, 0x00, 0x00, 0x08, 0x2A, 0x1C, 0x14, 0x00}, // '*'
    {0x00, 0x00, 0x08, 0x08, 0x3E, 0x08, 0x08, 0x00}, // '+'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}, // ','
    {0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00}, // '-'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02}, // '.'
    {0x02, 0x02, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00}, // '/'
    {0x1C, 0x36, 0x22, 0x22, 0x22, 0x22, 0x36, 0x1C}, // '0'
    {0x0C, 0x0A, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08}, // '1'
    {0x0C, 0x12, 0x10, 0x10, 0x08, 0x04, 0x02, 0x1F}, // '2'
    {0x0E, 0x11, 0x10, 0x0C, 0x10, 0x11, 0x11, 0x0E}, // '3'
    {0x10, 0x18, 0x14, 0x14, 0x12, 0x3F, 0x10, 0x10}, // '4'
    {0x1E, 0x01, 0x01, 0x0D, 0x13, 0x10, 0x11, 0x0E}, // '5'
    {0x1C, 0x24, 0x22, 0x1E, 0x22, 0x22, 0x22, 0x1C}, // '6'
    {0x1F, 0x10, 0x08, 0x08, 0x04, 0x04, 0x02, 0x02}, // '7'
    {0x1C, 0x22, 0x22, 0x1C, 0x22, 0x22, 0x22, 0x1C}, // '8'
    {0x1C, 0x22, 0x22, 0x22, 0x3C, 0x22, 0x12, 0x1C}, // '9'
    {0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x02}, // ':'
    {0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x02}, // ';'
    {0x00, 0x00, 0x00, 0x18, 0x06, 0x02, 0x0C, 0x10}, // '<'
    {0x00, 0x00, 0x00, 0x1E, 0x00, 0x1E, 0x00, 0x00}, // '='
    {0x00, 0x00, 0x00, 0x06, 0x18, 0x10, 0x0C, 0x02}, // '>'
    {0x0C, 0x12, 0x12, 0x10, 0x08, 0x08, 0x00, 0x08}, // '?'
    {0x70, 0x8C, 0x74, 0x4A, 0x4A, 0x2A, 0xDA, 0x04}, // '@'
    {0x08, 0x0C, 0x14, 0x12, 0x1E, 0x22, 0x22, 0x21}, // 'A'
    {0x1E, 0x22, 0x22, 0x12, 0x3E, 0x22, 0x22, 0x1E}, // 'B'
    {0x38, 0x44, 0x42, 0x02, 0x02, 0x42, 0x44, 0x3C}, // 'C'
    {0x1E, 0x22, 0x42, 0x42, 0x42, 0x42, 0x22, 0x1E}, // 'D'
    {0x3E, 0x02, 0x02, 0x02, 0x1E, 0x02, 0x02, 0x3E}, // 'E'
    {0x3E, 0x02, 0x02, 0x02, 0x1E, 0x02, 0x02, 0x02}, // 'F'
    {0x38, 0x64, 0x42, 0x02, 0x72, 0x42, 0x64, 0x5C}, // 'G'
    {0x42, 0x42, 0x42, 0x42, 0x7E, 0x42, 0x42, 0x42}, // 'H'
    {0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02}, // 'I'
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x12, 0x12, 0x0C}, // 'J'
    {0x22, 0x12, 0x0A, 0x0A, 0x0E, 0x0A, 0x12, 0x22}, // 'K'
    {0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x3E}, // 'L'
    {0xC6, 0xC6, 0xC6, 0xAA, 0xAA, 0xAA, 0x9A, 0x92}, // 'M'
    {0x26, 0x26, 0x26, 0x2A, 0x2A, 0x2A, 0x32, 0x32}, // 'N'
    {0x38, 0x44, 0x82, 0x82, 0x82, 0x82, 0x44, 0x38}, // 'O'
    {0x1E, 0x22, 0x22, 0x22, 0x1E, 0x02, 0x02, 0x02}, // 'P'
    {0x38, 0x44, 0x82, 0x82, 0x82, 0x82, 0x44, 0xF8}, // 'Q'
    {0x1E, 0x22, 0x22, 0x22, 0x1E, 0x32, 0x22, 0x22}, // 'R'
    {0x1C, 0x22, 0x02, 0x04, 0x38, 0x20, 0x22, 0x1C}, // 'S'
    {0x3E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08}, // 'T'
    {0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x1C}, // 'U'
    {0x21, 0x22, 0x22, 0x12, 0x14, 0x14, 0x0C, 0x08}, // 'V'
    {0x31, 0x32, 0x32, 0x2A, 0xAA, 0xCA, 0xCC, 0xC4}, // 'W'
    {0x22, 0x12, 0x14, 0x0C, 0x0C, 0x14, 0x12, 0x22}, // 'X'
    {0x22, 0x22, 0x14, 0x14, 0x08, 0x08, 0x08, 0x08}, // 'Y'
    {0x3E, 0x20, 0x10, 0x08, 0x08, 0x04, 0x02, 0x3E}, // 'Z'
    {0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02}, // '['
    {0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x02}, // '\\'
    {0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01}, // ']'
    {0x00, 0x00, 0x0C, 0x0A, 0x0A, 0x12, 0x00, 0x00}, // '^'
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // '_'
    {0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // '`'
    {0x00, 0x00, 0x1C, 0x12, 0x18, 0x16, 0x12, 0x1E}, // 'a'
    {0x02, 0x02, 0x1E, 0x22, 0x22, 0x22, 0x22, 0x1E}, // 'b'
    {0x00, 0x00, 0x1C, 0x22, 0x02, 0x02, 0x22, 0x1C}, // 'c'
    {0x20, 0x20, 0x3C, 0x22, 0x22, 0x22, 0x22, 0x3C}, // 'd'
    {0x00, 0x00, 0x1C, 0x22, 0x3E, 0x02, 0x22, 0x1C}, // 'e'
    {0x02, 0x02, 0x07, 0x02, 0x02, 0x02, 0x02, 0x02}, // 'f'
    {0x00, 0x00, 0x3C, 0x22, 0x22, 0x22, 0x22, 0x3C}, // 'g'
    {0x02, 0x02, 0x1E, 0x26, 0x22, 0x22, 0x22, 0x22}, // 'h'
    {0x02, 0x00, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02}, // 'i'
    {0x02, 0x00, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02}, // 'j'
    {0x02, 0x02, 0x12, 0x0A, 0x06, 0x0A, 0x0A, 0x12}, // 'k'
    {0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x06}, // 'l'
    {0x00, 0x00, 0xEE, 0x92, 0x92, 0x92, 0x92, 0x92}, // 'm'
    {0x00, 0x00, 0x1E, 0x26, 0x22, 0x22, 0x22, 0x22}, // 'n'
    {0x00, 0x00, 0x1C, 0x22, 0x22, 0x22, 0x22, 0x1C}, // 'o'
    {0x00, 0x00, 0x1E, 0x22, 0x22, 0x22, 0x22, 0x1E}, // 'p'
    {0x00, 0x00, 0x3C, 0x22, 0x22, 0x22, 0x22, 0x3C}, // 'q'
    {0x00, 0x00, 0x0E, 0x02, 0x02, 0x02, 0x02, 0x02}, // 'r'
    {0x00, 0x00, 0x0E, 0x12, 0x06, 0x18, 0x12, 0x1E}, // 's'
    {0x02, 0x02, 0x07, 0x02, 0x02, 0x02, 0x02, 0x06}, // 't'
    {0x00, 0x00, 0x22, 0x22, 0x22, 0x22, 0x32, 0x3C}, // 'u'
    {0x00, 0x00, 0x11, 0x11, 0x0A, 0x0A, 0x0A, 0x04}, // 'v'
    {0x00, 0x00, 0x99, 0x59, 0x5A, 0x56, 0x66, 0x24}, // 'w'
    {0x00, 0x00, 0x04, 0x05, 0x02, 0x03, 0x05, 0x04}, // 'x'
    {0x00, 0x00, 0x11, 0x11, 0x0A, 0x0A, 0x0A, 0x04}, // 'y'
    {0x00, 0x00, 0x1E, 0x10, 0x08, 0x04, 0x02, 0x1E}, // 'z'
    {0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02}, // '{'
    {0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02}, // '|'
    {0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02}, // '}'
    {0x00, 0x00, 0x00, 0x00, 0x16, 0x1A, 0x00, 0x00}, // '~'
};

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
    _cheatCount = 0;
    buildVersionPath(sCheatPath, header);
    if (TryLoadFile(sCheatPath)) return true;
    buildMakerPath(sCheatPath, header);
    return TryLoadFile(sCheatPath);
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

static void uiInitFont()
{
    SUB_BG_PALETTE[0] = 0x0000;
    SUB_BG_PALETTE[1] = 0x7FFF;

    for (u32 g = 0; g < 95; ++g)
    {
        volatile u8* tile = SUB_BG_TILE_BASE + g * 32;
        for (u32 y = 0; y < 8; ++y)
        {
            const u8 bits = sFont8x8[g][y];
            for (u32 pair = 0; pair < 4; ++pair)
            {
                const u32 x = pair * 2;
                const u8 p0 = (bits >> x) & 1;
                const u8 p1 = (bits >> (x + 1)) & 1;
                tile[y * 4 + pair] = p0 | (p1 << 4);
            }
        }
    }
}

void CheatService::InitializeUi()
{
    if (!HasCheats()) return;

    sys_setMainEngineToTopScreen();
    sysipc_setTopBacklight(true);
    sysipc_setBottomBacklight(true);

    mem_setVramHMapping(MEM_VRAM_H_SUB_BG_00000);
    // Sub engine, display mode 1, BG0 enabled. BG0 uses 4bpp tiles at 0x0000
    // and a 32x32 tile map at screen base block 8 (offset 0x4000).
    REG_DISPCNT_SUB = (1u << 16) | (1u << 8);
    REG_BG0CNT_SUB = (8u << 8);
    uiInitFont();
    _uiInitialized = true;

    // Do not interpret a pen that was already down during startup as a fresh
    // press of the cheat button on the first emulated VBlank.
    dc_invalidateRange((void*)&gGbaSoundShared.cheatInput, sizeof(gGbaSoundShared.cheatInput));
    _touchWasDown = gGbaSoundShared.cheatInput.touchDown != 0;
    RenderClosed();
}

void CheatService::RenderClosed()
{
    if (!_uiInitialized) return;
    uiClear();
    uiPrint(CHEAT_BUTTON_COL, CHEAT_BUTTON_ROW, "[CHEAT]", 7);
}

void CheatService::RenderMenu()
{
    if (!_uiInitialized) return;
    uiClear();
    uiPrint(0, 0, "CHEATS   A: TOGGLE   D-PAD: MOVE");
    uiPrint(0, 1, "TAP [CHEAT] AGAIN TO RESUME");

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
            const u32 next = _selected + CHEAT_VISIBLE_ROWS;
            _selected = next < _cheatCount ? next : _cheatCount - 1;
            redraw = true;
        }
        if ((down & KEY_A) && _cheatCount)
        {
            _cheats[_selected].enabled = !_cheats[_selected].enabled;
            redraw = true;
        }
        if (ReadCheatButtonPressed())
        {
            _menuOpen = false;
            break;
        }
        if (redraw) RenderMenu();
    }

    while (readKeysHeld() != 0)
        waitNextFramePolling();

    SetPaused(false);
    RenderClosed();
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
    if (ReadCheatButtonPressed())
        RunMenuLoop();
}

extern "C" void cheat_onVBlank(void)
{
    gCheatService.OnVBlank();
}

#include <windows.h>
#include <cstring>
#include <vector>
#include "scan.h"

static bool TextSection(void* module, uint8_t*& start, size_t& size)
{
    auto base = static_cast<uint8_t*>(module ? module : GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 5) == 0) {
            start = base + sec->VirtualAddress;
            size = sec->Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

static std::vector<int> ParsePattern(const char* pattern)
{
    std::vector<int> bytes; // -1 = wildcard
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') { bytes.push_back(-1); p += (p[1] == '?') ? 2 : 1; continue; }
        bytes.push_back(static_cast<int>(strtoul(p, nullptr, 16)));
        p += 2;
    }
    return bytes;
}

// Calls onMatch for every match; stops early when it returns false.
template <class F> static bool ForEachMatch(const char* pattern, void* module, F onMatch)
{
    std::vector<int> bytes = ParsePattern(pattern);
    uint8_t* text;
    size_t size;
    if (bytes.empty() || bytes[0] < 0 || !TextSection(module, text, size))
        return false;
    const uint8_t first = static_cast<uint8_t>(bytes[0]);
    const size_t n = bytes.size();
    for (uint8_t* p = text; p + n <= text + size; ++p) {
        p = static_cast<uint8_t*>(memchr(p, first, (text + size) - p - n + 1));
        if (!p)
            break;
        size_t i = 1;
        while (i < n && (bytes[i] < 0 || p[i] == bytes[i]))
            ++i;
        if (i == n && !onMatch(p))
            return true;
    }
    return true;
}

uint8_t* FindUnique(const char* pattern, void* module)
{
    uint8_t* found = nullptr;
    bool unique = true;
    ForEachMatch(pattern, module, [&](uint8_t* p) {
        if (found) {
            unique = false;
            return false;
        }
        found = p;
        return true;
    });
    return unique ? found : nullptr;
}

uint8_t* ResolveRipConsensus(const char* pattern, int dispOffset, int insEnd)
{
    uint8_t* target = nullptr;
    bool agree = true;
    ForEachMatch(pattern, nullptr, [&](uint8_t* p) {
        int32_t disp;
        memcpy(&disp, p + dispOffset, 4);
        uint8_t* t = p + insEnd + disp;
        if (target && t != target) {
            agree = false;
            return false;
        }
        target = t;
        return true;
    });
    return agree ? target : nullptr;
}

uint8_t* ResolveLea(uint8_t* addr)
{
    if ((addr[0] != 0x48 && addr[0] != 0x4C) || addr[1] != 0x8D || (addr[2] & 0xC7) != 0x05)
        return nullptr;
    int32_t disp;
    memcpy(&disp, addr + 3, 4);
    return addr + 7 + disp;
}

static void WriteAbsJump(uint8_t* at, const void* to)
{
    at[0] = 0x48; at[1] = 0xB8; // mov rax, imm64
    memcpy(at + 2, &to, 8);
    at[10] = 0xFF; at[11] = 0xE0; // jmp rax
}

void* InstallHook(uint8_t* target, void* detour, size_t length)
{
    if (length < 12 || length > 32)
        return nullptr;
    auto tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp)
        return nullptr;
    memcpy(tramp, target, length);
    WriteAbsJump(tramp + length, target + length);

    DWORD old;
    if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &old))
        return nullptr;
    WriteAbsJump(target, detour);
    memset(target + 12, 0x90, length - 12); // nop out the rest of the overwritten instructions
    VirtualProtect(target, length, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, length);
    return tramp;
}

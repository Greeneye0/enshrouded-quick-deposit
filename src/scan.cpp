#include <windows.h>
#include <cstring>
#include <vector>
#include "scan.h"
#include "../../common/jumppatch.h"
#include "../../common/greeneye.h"

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

// Hot reload build: patterns are matched in a copy of the game's code taken before any hook,
// so functions the loader has patched are still found (at their real addresses).
static const uint8_t* g_copy = nullptr;
static uint8_t* g_real = nullptr;
static size_t g_copySize = 0;

bool GameText(uint8_t*& start, size_t& size)
{
    return TextSection(nullptr, start, size);
}

void UseCodeSnapshot(const uint8_t* copy, uint8_t* real, size_t size)
{
    g_copy = copy;
    g_real = real;
    g_copySize = size;
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
    uintptr_t shift = 0; // from the scanned bytes to the game's
    if (!module && g_copy) {
        text = const_cast<uint8_t*>(g_copy);
        size = g_copySize;
        shift = reinterpret_cast<uintptr_t>(g_real) - reinterpret_cast<uintptr_t>(g_copy);
    } else if (!TextSection(module, text, size)) {
        return false;
    }
    if (bytes.empty() || bytes[0] < 0)
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
        if (i == n && !onMatch(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(p) + shift)))
            return true;
    }
    return true;
}

// A function whose first instructions another mod's hook replaced (see jumppatch.h).
static uint8_t* FindBehindHook(const char* pattern, void* module)
{
    std::vector<int> bytes = ParsePattern(pattern);
    uint8_t* text;
    size_t size;
    uintptr_t shift = 0;
    if (!module && g_copy) {
        text = const_cast<uint8_t*>(g_copy);
        size = g_copySize;
        shift = reinterpret_cast<uintptr_t>(g_real) - reinterpret_cast<uintptr_t>(g_copy);
    } else if (!TextSection(module, text, size)) {
        return nullptr;
    }
    if (bytes.empty())
        return nullptr;
    uint8_t* p = jumppatch::FindBehindHook(text, size, bytes.data(), bytes.size());
    if (!p)
        return nullptr;
    return reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(p) + shift);
}

uint8_t* FindUnique(const char* pattern, void* module)
{
    // Under the Greeneye Loader: the code as it was before the loader's hooks, so a reloaded
    // Quick Deposit finds the functions it hooked the first time.
    if (const GreeneyeHost* host = greeneye::Host(); host && !module)
        return host->find(pattern, nullptr);
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
    if (!found)
        found = FindBehindHook(pattern, module);
    return unique ? found : nullptr;
}

bool IsHookPatch(const uint8_t* target)
{
    return jumppatch::KindOf(target) != jumppatch::None;
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
    // Under the Greeneye Loader (API 2) the loader owns the hook (hot reload).
    if (const GreeneyeHost* host = greeneye::Host())
        return host->hook(target, detour, length, nullptr, 0);
    // Another mod's jump is already here: point it at detour and call on through that mod.
    if (IsHookPatch(target))
        return jumppatch::Chain(target, detour);
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

// ItemInfoLookup starts with "mov [rsp+8], ecx; sub rsp, 28h; test ecx, ecx; jz short X": the
// short jump is relocated by hand into the trampoline.
void* InstallItemLookupHook(uint8_t* fn, void* detour)
{
    static const uint8_t prologue[] = {0x89, 0x4C, 0x24, 0x08, 0x48, 0x83, 0xEC, 0x28, 0x85, 0xC9, 0x74};
    if (const GreeneyeHost* host = greeneye::Host()) {
        // The loader appends "jump back to fn + 12" after this code: the first 10 bytes, then
        // "jnz" over a jump to the jz target (taken when ecx was 0) to that jump back.
        uint8_t original[12];
        host->read(fn, original, sizeof(original));
        if (memcmp(original, prologue, sizeof(prologue)) != 0)
            return nullptr;
        uint8_t* jzTarget = fn + 12 + static_cast<int8_t>(original[11]);
        uint8_t code[26];
        memcpy(code, original, 10);
        code[10] = 0x75; // jnz +14 -> the loader's jump back
        code[11] = 0x0E;
        code[12] = 0xFF; // jmp [rip+0]; jzTarget
        code[13] = 0x25;
        memset(code + 14, 0, 4);
        memcpy(code + 18, &jzTarget, 8);
        return host->hook(fn, detour, 12, code, sizeof(code));
    }
    if (IsHookPatch(fn))
        return jumppatch::Chain(fn, detour);
    if (memcmp(fn, prologue, sizeof(prologue)) != 0)
        return nullptr;
    uint8_t* jzTarget = fn + 12 + static_cast<int8_t>(fn[11]);
    auto* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t)
        return nullptr;
    memcpy(t, fn, 10);     // up to and including test ecx, ecx
    t[10] = 0x74;          // jz +12 -> second stub
    t[11] = 0x0C;
    auto absJump = [](uint8_t* at, const void* to) {
        at[0] = 0x48; at[1] = 0xB8;
        memcpy(at + 2, &to, 8);
        at[10] = 0xFF; at[11] = 0xE0;
    };
    absJump(t + 12, fn + 12);  // fall through: rest of the original
    absJump(t + 24, jzTarget); // taken branch
    FlushInstructionCache(GetCurrentProcess(), t, 64);

    DWORD old;
    if (!VirtualProtect(fn, 12, PAGE_EXECUTE_READWRITE, &old))
        return nullptr;
    absJump(fn, detour);
    VirtualProtect(fn, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 12);
    return t;
}


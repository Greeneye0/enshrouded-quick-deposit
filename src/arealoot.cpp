// The host's "collect nearby loot" system (build 1269579):
//
// 1402ec3d0 AreaLootSystem(system, bool collecting), run per player each tick:
//   candidates = entities with PickupItem in a box of +-7 m around the player
//   per candidate: dist^2 <= 12.25 (3.5 m) and line of sight, then if collecting and
//   dist <= 3.5 m (1.75 m in one state): dist^2 >= 0.5625 -> pull the item toward the player,
//   else -> LootInteractionEvent with bypass (the pickup).
// 1402d8e5e: collecting = FUN_1402ba4e0(...) (interact held long enough), passed in edx.
//
// Each float comes from a shared constant, so instead of changing those, the five loads in
// this function are redirected to our own values.
//
// Corpses: the system is hooked; after the game's pass, the same context finds corpses with the
// spatial query 1409d11f0 (filtered by the name hash of DisableColliderModeChanges, the marker
// the game adds when an enemy turns into loot) and sends each the event the system itself
// sends for a pickup: LootInteractionEvent with lootAll and bypassInteraction.
// System context: [0] world access, [5] -> player position (int64 x, y, z, 32 fraction bits),
// [0x7f] spatial index, [0x93] knowledge, [0xa9] event writer.
#include <windows.h>
#include <cstring>
#include <initializer_list>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include "arealoot.h"
#include "log.h"
#include "scan.h"

namespace arealoot {
namespace {

struct Site {
    size_t offset;       // instruction offset in the system function
    size_t length;       // instruction length
    size_t disp;         // offset of the rip-relative disp32 in the instruction
    uint8_t prefix[5];   // opcode bytes before disp32
    float vanilla;       // value the instruction loads in the game
    int32_t original;    // saved disp32
    uint8_t* at;
};

Site g_sites[] = {
    {0x054, 8, 4, {0xF3, 0x0F, 0x10, 0x2D}, 7.0f},           // movss xmm5, box half size
    {0x2d3, 9, 5, {0xF3, 0x44, 0x0F, 0x10, 0x25}, 1.75f},    // movss xmm12, radius (special state)
    {0x2e5, 9, 5, {0xF3, 0x44, 0x0F, 0x10, 0x2D}, 3.5f},     // movss xmm13, radius
    {0x2f7, 9, 5, {0xF3, 0x44, 0x0F, 0x10, 0x35}, 0.5625f},  // movss xmm14, pickup distance^2
    {0x500, 7, 3, {0x0F, 0x2F, 0x3D}, 12.25f},               // comiss xmm7, candidate distance^2
};
enum { kBox, kRadiusSpecial, kRadius, kPickup, kCandidate, kSiteCount };

// --- corpses ---------------------------------------------------------------------------------

using SystemFn = void (*)(uint64_t* ctx, char collecting);
using EntityIdOfFn = uint32_t* (*)(void* world, uint32_t* out);
using SpatialQueryFn = void (*)(void* world, void* list, void* index, const float* box, uint32_t typeHash, uint8_t flag);
using AllocEventFn = uint8_t* (*)(void* queue, uint32_t typeHash, uint32_t size, uint16_t flags);
using KnowledgeOpsFn = void (*)(void* ops, void* world, uint64_t knowledge, uint32_t host);

SystemFn g_originalSystem = nullptr;
EntityIdOfFn g_entityIdOf = nullptr;
SpatialQueryFn g_spatialQuery = nullptr;
AllocEventFn g_allocEvent = nullptr;
KnowledgeOpsFn g_knowledgeOps = nullptr;

constexpr uint32_t kCorpseMarkerHash = 0x761cca43;   // keen::ecs::DisableColliderModeChanges
constexpr uint32_t kLootInteractionEvent = 0x730ab56d; // keen::ecs::LootInteractionEvent
constexpr uint32_t kLootInteractionEventSize = 0x80;
constexpr ULONGLONG kRetryMs = 500;     // per corpse: it only becomes lootable after its death animation
constexpr uint16_t kMaxTries = 20;      // then give up (empty husk, or backpack full for 10 s)

bool g_corpses = false;
float g_corpseRadius = 10.0f;
// Loot-all targets: entities found by query A (and, if B is set, also by query B).
uint32_t g_queryA = 0xb19528a9; // keen::ecs::Inventory
uint32_t g_queryB = 0x6158772c; // keen::ecs::DestroyOnLoot
std::atomic<uint32_t> g_lastFoundA{0}, g_lastFoundB{0}, g_sent{0};
// Before "loot all", accept the target's interaction offer like pressing E (rolls a corpse's
// random loot, which only exists after the first search).
bool g_acceptFirst = true;
uint32_t g_acceptOffer = 1;
constexpr uint32_t kAcceptOfferedInteractionEvent = 0x82e404b2; // keen::ecs::AcceptOfferedInteractionEvent

struct Attempt {
    uint32_t entity;
    ULONGLONG at;
    uint16_t tries;
};
Attempt g_attempts[256];
std::mutex g_attemptLock;

// True if the corpse was not tried in the last kRetryMs (and records this try).
bool Due(uint32_t entity)
{
    std::lock_guard<std::mutex> lock(g_attemptLock);
    ULONGLONG now = GetTickCount64();
    Attempt* oldest = &g_attempts[0];
    for (Attempt& a : g_attempts) {
        if (a.entity == entity) {
            if (now - a.at < kRetryMs || a.tries >= kMaxTries)
                return false;
            a.at = now;
            ++a.tries;
            return true;
        }
        if (a.at < oldest->at)
            oldest = &a;
    }
    *oldest = {entity, now, 1};
    return true;
}

struct QueryList {
    uint8_t* data;
    uint64_t count;
    uint64_t capacity;
    void* grow; // null: capacity is never exceeded in a 40 m box
    uint64_t reserved;
};

void LootCorpses(uint64_t* ctx)
{
    auto* world = reinterpret_cast<void*>(ctx[0]);
    auto* pos = reinterpret_cast<int64_t*>(ctx[5]);
    void* writer = reinterpret_cast<void*>(ctx[0xa9]);
    if (!world || !pos || !writer)
        return;
    float x = pos[0] * 2.3283064e-10f, y = pos[1] * 2.3283064e-10f, z = pos[2] * 2.3283064e-10f, r = g_corpseRadius;
    alignas(16) float box[8] = {x - r, y - r, z - r, 0, x + r, y + r, z + r, 0};
    static thread_local uint8_t entries[2048 * 16];
    static thread_local uint8_t entriesB[2048 * 16];
    QueryList list{entries, 0, 2048, nullptr, 0};
    g_spatialQuery(world, &list, reinterpret_cast<void*>(ctx[0x7f]), box, g_queryA, 0);
    g_lastFoundA = static_cast<uint32_t>(list.count);
    QueryList listB{entriesB, 0, 2048, nullptr, 0};
    if (g_queryB) {
        g_spatialQuery(world, &listB, reinterpret_cast<void*>(ctx[0x7f]), box, g_queryB, 0);
        g_lastFoundB = static_cast<uint32_t>(listB.count);
    }
    if (!list.count)
        return;
    uint32_t idBuf = 0;
    uint32_t player = *g_entityIdOf(world, &idBuf);
    uint16_t flags = *reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(writer) + 0xb8);
    void* queue = *reinterpret_cast<void**>(static_cast<uint8_t*>(writer) + 0x1b8);
    for (uint64_t i = 0; i < list.count && i < list.capacity; ++i) {
        uint32_t corpse;
        memcpy(&corpse, entries + i * 16, 4);
        if (!corpse)
            continue;
        if (g_queryB) {
            bool inB = false;
            for (uint64_t k = 0; k < listB.count && k < listB.capacity && !inB; ++k)
                inB = memcmp(entriesB + k * 16, &corpse, 4) == 0;
            if (!inB)
                continue;
        }
        if (!Due(corpse))
            continue;
        ++g_sent;
        if (g_acceptFirst) {
            uint8_t* a = g_allocEvent(queue, kAcceptOfferedInteractionEvent, 0x18, flags);
            if (!a)
                return;
            memset(a + 8, 0, 0x10);
            *reinterpret_cast<uint32_t*>(a + 0x08) = player; // guestEntityId
            *reinterpret_cast<uint32_t*>(a + 0x0c) = corpse; // hostEntityId
            *reinterpret_cast<uint32_t*>(a + 0x10) = g_acceptOffer;
        }
        uint8_t* e = g_allocEvent(queue, kLootInteractionEvent, kLootInteractionEventSize, flags);
        if (!e)
            return;
        memset(e + 8, 0, kLootInteractionEventSize - 8);
        *reinterpret_cast<uint32_t*>(e + 0x08) = player;  // guestId
        *reinterpret_cast<uint32_t*>(e + 0x0c) = corpse;  // hostId
        e[0x14] = 1;                                     // lootAll
        e[0x78] = 1;                                     // bypassInteraction
        struct { uint8_t* ops; uint64_t count; } ops{e + 0x18, 8};
        g_knowledgeOps(&ops, world, ctx[0x93], corpse);
    }
}

std::atomic<uint64_t> g_permTable{0}; // ctx[0x79]: per player id, 0xa0-byte records, permissions at +0x9d
std::atomic<uint64_t> g_tables[2];     // both worlds' tables (hosted: server and client prediction)

void SystemHook(uint64_t* ctx, char collecting)
{
    g_permTable = ctx[0x79];
    if (g_tables[0].load() != ctx[0x79] && g_tables[1].load() != ctx[0x79])
        (g_tables[0].load() ? g_tables[1] : g_tables[0]) = ctx[0x79];
    g_originalSystem(ctx, collecting);
    if (!g_corpses)
        return;
    __try {
        LootCorpses(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_corpses = false;
        Log("arealoot: corpse looting faulted (0x%08X), switched off", GetExceptionCode());
    }
}

float* g_values = nullptr;   // our constants, within +-2 GiB of the game code
uint8_t* g_collectCall = nullptr;
uint8_t g_collectOriginal[5];
bool g_ready = false;

float* AllocNear(uint8_t* target)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t step = si.dwAllocationGranularity;
    uintptr_t center = reinterpret_cast<uintptr_t>(target) & ~(step - 1);
    for (uintptr_t d = step; d < 0x70000000; d += step) {
        for (uintptr_t a : {center - d, center + d}) {
            if (void* p = VirtualAlloc(reinterpret_cast<void*>(a), step, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
                return static_cast<float*>(p);
        }
    }
    return nullptr;
}

bool Write(uint8_t* at, const void* bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return true;
}

// The one call of the system: "call collecting; movzx edx, al; lea rcx, [rbp+0]; call system".
uint8_t* FindCollectCall(uint8_t* system)
{
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    static const uint8_t between[] = {0x0F, 0xB6, 0xD0, 0x48, 0x8D, 0x4D, 0x00};
    uint8_t* found = nullptr;
    int count = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        uint8_t* p = base + sec->VirtualAddress;
        uint8_t* end = p + sec->Misc.VirtualSize - 5;
        for (; p < end; ++p) {
            if (*p != 0xE8 || p < base + sec->VirtualAddress + 12)
                continue;
            int32_t rel;
            memcpy(&rel, p + 1, 4);
            if (p + 5 + rel != system)
                continue;
            if (p[-12] == 0xE8 && memcmp(p - 7, between, sizeof(between)) == 0) {
                found = p - 12;
                ++count;
            }
        }
    }
    return count == 1 ? found : nullptr;
}

} // namespace

bool Init()
{
    uint8_t* system = FindUnique("88 54 24 10 48 89 4C 24 08 55 53 57");
    if (!system) {
        Log("arealoot: system signature not found");
        return false;
    }
    for (Site& s : g_sites) {
        s.at = system + s.offset;
        size_t prefixLen = s.disp;
        if (memcmp(s.at, s.prefix, prefixLen) != 0) {
            Log("arealoot: instruction at +0x%zx does not match", s.offset);
            return false;
        }
        memcpy(&s.original, s.at + s.disp, 4);
        float now;
        memcpy(&now, s.at + s.length + s.original, 4);
        if (now != s.vanilla) {
            Log("arealoot: +0x%zx loads %g, expected %g", s.offset, now, s.vanilla);
            return false;
        }
    }
    g_collectCall = FindCollectCall(system);
    if (!g_collectCall) {
        Log("arealoot: call site not found");
        return false;
    }
    memcpy(g_collectOriginal, g_collectCall, 5);
    g_entityIdOf = reinterpret_cast<EntityIdOfFn>(FindUnique("8B 41 08 4C 8B C2 85 C0 75 ?? 89 02 48 8B C2 C3 8D 50 FF 48 8B 01 48 03 D2 48 8B 88 28 08 00 00 8B 04 D1"));
    g_spatialQuery = reinterpret_cast<SpatialQueryFn>(FindUnique("40 53 0F B6 44 24 38 4D 8B D1 44 8B 4C 24 30"));
    g_allocEvent = reinterpret_cast<AllocEventFn>(FindUnique("48 83 EC 28 4C 8B D9 41 81 F8 00 01 00 00"));
    g_knowledgeOps = reinterpret_cast<KnowledgeOpsFn>(FindUnique("48 89 5C 24 18 48 89 7C 24 20 41 56 48 83 EC 30 4C 8B F1 41 8B D9"));
    if (g_entityIdOf && g_spatialQuery && g_allocEvent && g_knowledgeOps)
        g_originalSystem = static_cast<SystemFn>(InstallHook(system, reinterpret_cast<void*>(&SystemHook), 12));
    if (!g_originalSystem)
        Log("arealoot: corpse looting unavailable (helpers not found)");
    g_values = AllocNear(system);
    if (!g_values) {
        Log("arealoot: no memory near the game code");
        return false;
    }
    for (int i = 0; i < kSiteCount; ++i)
        g_values[i] = g_sites[i].vanilla;
    g_ready = true;
    Log("arealoot: ready (system %p, call %p)", system, g_collectCall);
    return true;
}

void SetAcceptFirst(bool on, uint32_t offer)
{
    g_acceptFirst = on;
    g_acceptOffer = offer;
    Log("arealoot: accept first %s (offer %u)", on ? "on" : "off", offer);
}

void SetLootAllQuery(uint32_t a, uint32_t b)
{
    g_queryA = a;
    g_queryB = b;
    Log("arealoot: loot-all query %08x%s%08x", a, b ? " and " : "", b);
}

uint64_t PermissionTable()
{
    return g_permTable;
}

void Stats(uint32_t& foundA, uint32_t& foundB, uint32_t& sent)
{
    foundA = g_lastFoundA;
    foundB = g_lastFoundB;
    sent = g_sent;
}

bool SetCorpses(bool on, float radius)
{
    if (!g_originalSystem)
        return false;
    g_corpseRadius = radius < 1.0f ? 1.0f : radius > 20.0f ? 20.0f : radius;
    g_corpses = on;
    Log("arealoot: corpses %s (%.1f m)", on ? "on" : "off", g_corpseRadius);
    return true;
}

bool Apply(float radius, bool instant, bool always)
{
    if (!g_ready)
        return false;
    if (radius < 1.0f)
        radius = 1.0f;
    if (radius > 20.0f)
        radius = 20.0f;
    g_values[kBox] = radius > 7.0f ? radius : 7.0f;
    g_values[kRadiusSpecial] = radius;
    g_values[kRadius] = radius;
    g_values[kCandidate] = radius * radius;
    g_values[kPickup] = instant ? radius * radius : 0.5625f;
    for (int i = 0; i < kSiteCount; ++i) {
        Site& s = g_sites[i];
        int32_t disp = static_cast<int32_t>(reinterpret_cast<uint8_t*>(&g_values[i]) - (s.at + s.length));
        if (!Write(s.at + s.disp, &disp, 4))
            return false;
    }
    static const uint8_t movAl1[5] = {0xB0, 0x01, 0x90, 0x90, 0x90}; // mov al, 1
    Write(g_collectCall, always ? movAl1 : g_collectOriginal, 5);
    Log("arealoot: radius %.1f m, %s, %s", radius, instant ? "instant" : "pull in", always ? "always on" : "while holding E");
    return true;
}

// --- settings and chat commands -------------------------------------------------------------

namespace {

struct Settings {
    bool enabled = true;
    float radius = 5.0f;
    bool items = true;
    bool corpses = true;
    bool instant = true;
    bool adminOnly = true; // who may change settings in chat: admins (CanKickBan) or everyone
};
Settings g_settings;
char g_iniPath[MAX_PATH];

void IniPath()
{
    GetModuleFileNameA(nullptr, g_iniPath, MAX_PATH);
    char* slash = strrchr(g_iniPath, '\\');
    strcpy_s(slash ? slash + 1 : g_iniPath, MAX_PATH - (slash ? slash + 1 - g_iniPath : 0), "autoloot.ini");
}

bool OnOff(const char* v, bool fallback)
{
    if (!_stricmp(v, "on") || !_stricmp(v, "true") || !_stricmp(v, "1") || !_stricmp(v, "yes"))
        return true;
    if (!_stricmp(v, "off") || !_stricmp(v, "false") || !_stricmp(v, "0") || !_stricmp(v, "no"))
        return false;
    return fallback;
}

void LoadSettings()
{
    IniPath();
    char v[64];
    auto get = [&](const char* key, const char* def) {
        GetPrivateProfileStringA("autoloot", key, def, v, sizeof(v), g_iniPath);
        return v;
    };
    g_settings.enabled = OnOff(get("enabled", "on"), true);
    g_settings.radius = static_cast<float>(atof(get("radius", "5")));
    g_settings.items = OnOff(get("items", "on"), true);
    g_settings.corpses = OnOff(get("corpses", "on"), true);
    g_settings.instant = OnOff(get("instant", "on"), true);
    g_settings.adminOnly = _stricmp(get("chatAccess", "admin"), "everyone") != 0;
}

void SaveSettings()
{
    char r[16];
    sprintf_s(r, "%g", g_settings.radius);
    WritePrivateProfileStringA("autoloot", "enabled", g_settings.enabled ? "on" : "off", g_iniPath);
    WritePrivateProfileStringA("autoloot", "radius", r, g_iniPath);
    WritePrivateProfileStringA("autoloot", "items", g_settings.items ? "on" : "off", g_iniPath);
    WritePrivateProfileStringA("autoloot", "corpses", g_settings.corpses ? "on" : "off", g_iniPath);
    WritePrivateProfileStringA("autoloot", "instant", g_settings.instant ? "on" : "off", g_iniPath);
    WritePrivateProfileStringA("autoloot", "chatAccess", g_settings.adminOnly ? "admin" : "everyone", g_iniPath);
}

void ApplySettings()
{
    Settings& st = g_settings;
    if (st.radius < 1.0f)
        st.radius = 1.0f;
    if (st.radius > 20.0f)
        st.radius = 20.0f;
    if (st.enabled && st.items)
        Apply(st.radius, st.instant, true);
    else
        Restore();
    SetCorpses(st.enabled && st.corpses, st.radius);
}

std::string Describe()
{
    const Settings& st = g_settings;
    char buf[200];
    if (!st.enabled)
        return "[Auto Loot] off. /loot on to turn it on.";
    sprintf_s(buf, "[Auto Loot] radius %g m, loose loot %s, corpses %s%s.", st.radius, st.items ? "on" : "off",
              st.corpses ? "on" : "off", st.items && !st.instant ? " (pulled in)" : "");
    return buf;
}

// Permissions byte of the player who sent a chat line: the record whose +8 holds the sender's
// 64-bit id, in either permission table seen (a hosted game runs the system twice).

bool IsAdmin(uint64_t sender)
{
    for (auto& t : g_tables) {
        auto* table = reinterpret_cast<uint8_t*>(t.load());
        if (!table)
            continue;
        __try {
            for (int i = 0; i < 64; ++i) {
                uint8_t* rec = table + i * 0xa0;
                if (*reinterpret_cast<uint32_t*>(rec) == static_cast<uint32_t>(i + 1) &&
                    *reinterpret_cast<uint64_t*>(rec + 8) == sender)
                    return (rec[0x9d] & 1) != 0; // CanKickBan
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return false;
}

// Handles "/loot ..." and returns the answer.
std::string Command(const char* args, bool admin)
{
    Settings& st = g_settings;
    char a[32] = {}, b[32] = {};
    sscanf_s(args, "%31s %31s", a, static_cast<unsigned>(sizeof(a)), b, static_cast<unsigned>(sizeof(b)));
    if (!a[0])
        return Describe();
    if (!_stricmp(a, "help"))
        return "[Auto Loot] /loot, /loot <1-20>, /loot on|off, /loot items on|off, /loot corpses on|off, /loot instant on|off";
    if (!admin && st.adminOnly)
        return "[Auto Loot] only admins can change the settings.";
    auto is = [&](std::initializer_list<const char*> words) {
        for (const char* w : words)
            if (!_stricmp(a, w))
                return true;
        return false;
    };
    char* end = nullptr;
    const char* number = is({"radius", "range", "r"}) ? b : a;
    double r = strtod(number, &end);
    if (end && end != number && !*end) {
        st.radius = static_cast<float>(r);
    } else if (is({"on", "off"})) {
        st.enabled = OnOff(a, st.enabled);
    } else if (is({"items", "item", "loot", "drops", "drop", "loose"})) {
        st.items = OnOff(b, !st.items);
    } else if (is({"corpses", "corpse", "bodies", "body", "enemies"})) {
        st.corpses = OnOff(b, !st.corpses);
    } else if (is({"instant"})) {
        st.instant = OnOff(b, !st.instant);
    } else {
        return "[Auto Loot] unknown command. /loot help";
    }
    ApplySettings();
    SaveSettings();
    Log("arealoot: settings changed: %s", Describe().c_str());
    return Describe();
}

} // namespace

bool Start()
{
    if (!Init())
        return false;
    LoadSettings();
    ApplySettings();
    SaveSettings(); // writes the file with all keys on first start
    Log("arealoot: %s (ini %s)", Describe().c_str(), g_iniPath);
    return true;
}

bool HandleChat(const char* text, size_t length, uint64_t sender, std::string& answer)
{
    if (!g_ready || length < 5 || _strnicmp(text, "/loot", 5) != 0 || (length > 5 && text[5] != ' '))
        return false;
    std::string line(text, length);
    answer = Command(line.c_str() + 5, IsAdmin(sender));
    return true;
}

void Restore()
{
    if (!g_ready)
        return;
    for (Site& s : g_sites)
        Write(s.at + s.disp, &s.original, 4);
    Write(g_collectCall, g_collectOriginal, 5);
    Log("arealoot: game values restored");
}

} // namespace arealoot

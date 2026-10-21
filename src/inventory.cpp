// Client inventory reader.
//
// 1409bab60 ViewInit(iter, view, size): every ECS system calls it at the start of its
//           frame; *( *iter + 0x30) is that system's entity registry. Hooked to collect the
//           registries without scanning memory; the client's is the one that knows both the
//           local player and the base's chests.
// 1409a58c0 GetEntity(iter, id) -> entity record; record + 0x18 -> archetype, whose first
//           640 bits say which component types the entity has.
// 1409a5e20 GetEntityComponent(out, registry, &typeIndex, entity) -> component data.
// 140e7a870 ItemInfoLookup(itemId, itemDb) -> ItemInfo. Hooked to learn itemDb from the
//           game's own calls (the HUD looks items up all the time).
//
// Component type indices, measured in game (build 1269579): an inventory page entity has
// exactly one type below 640, Inventory = 283 (8 x {itemId, count, pide}). InventorySetup (286
// in that build: 16 link ids, then 16 InventoryCategory bytes) is detected at runtime as the
// chest type whose link ids are non-player entities that have an Inventory.
#include <windows.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "inventory.h"
#include "log.h"
#include "scan.h"

namespace inv {
namespace {

using GetEntityFn = uint8_t* (*)(void* iter, uint32_t entityId);
using ViewInitFn = void (*)(void* iter, void* view, uint32_t size);
using GetEntityComponentFn = void** (*)(void* out16, void* registry, void* accessor, uint32_t entityId);
using ItemInfoLookupFn = uint8_t* (*)(uint32_t itemId, void* itemDb);

ViewInitFn g_originalViewInit = nullptr;
ItemInfoLookupFn g_originalItemLookup = nullptr;
GetEntityFn getEntity = nullptr;
GetEntityComponentFn getEntityComponent = nullptr;

constexpr int kComponentBits = 640;
constexpr uint32_t kFirstNonPlayerId = 0x400; // player entities use ids 1..0x3ff
uint16_t g_typeInventory = 283;
uint16_t g_typeSetup = 0;  // detected
uint16_t g_typeHotbar = 0; // detected: player component with the hotbar page ids at +0xa0/+0xa4
constexpr uint16_t kTypePosition = 133; // int64 x, y, z with 32 fraction bits (build 1269579)
constexpr size_t kHotbarPageA = 0xa0;
constexpr size_t kHotbarPageB = 0xa4;

constexpr size_t kItemCategory = 0xcc;
constexpr size_t kItemWeaponType = 0xd0;
constexpr size_t kItemAmmoType = 0xcd;
constexpr size_t kItemMaxStack = 0x14;
constexpr size_t kItemDebugName = 0x820;
constexpr uint8_t kCategoryWeapons = 2;

constexpr int kMaxRegistries = 16;
std::atomic<void*> g_seen[kMaxRegistries];
constexpr int kMaxItemDbs = 8;
std::atomic<void*> g_itemDbs[kMaxItemDbs]; // every database the game looked items up in
void* g_registry = nullptr;

void ViewInitHook(void* iter, void* view, uint32_t size)
{
    if (!g_registry) {
        void* reg = nullptr;
        __try {
            reg = *reinterpret_cast<void**>(*static_cast<uint8_t**>(iter) + 0x30);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            reg = nullptr;
        }
        for (auto& slot : g_seen) {
            if (!reg)
                break;
            void* cur = slot.load(std::memory_order_relaxed);
            if (cur == reg)
                break;
            if (!cur) {
                void* expected = nullptr;
                if (slot.compare_exchange_strong(expected, reg) || expected == reg)
                    break;
            }
        }
    }
    g_originalViewInit(iter, view, size);
}

uint8_t* ItemLookupHook(uint32_t itemId, void* itemDb)
{
    uint8_t* info = g_originalItemLookup(itemId, itemDb);
    if (info && itemDb) {
        for (auto& slot : g_itemDbs) {
            void* cur = slot.load(std::memory_order_relaxed);
            if (cur == itemDb)
                break;
            if (!cur) {
                void* expected = nullptr;
                if (slot.compare_exchange_strong(expected, itemDb) || expected == itemDb)
                    break;
            }
        }
    }
    return info;
}

// ItemInfoLookup starts with "mov [rsp+8], ecx; sub rsp, 28h; test ecx, ecx; jz short X": the
// short jump is relocated by hand into the trampoline.
void* HookItemLookup(uint8_t* fn)
{
    static const uint8_t prologue[] = {0x89, 0x4C, 0x24, 0x08, 0x48, 0x83, 0xEC, 0x28, 0x85, 0xC9, 0x74};
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
    absJump(fn, reinterpret_cast<void*>(&ItemLookupHook));
    VirtualProtect(fn, 12, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 12);
    return t;
}

void* Component(void* registry, uint16_t type, uint32_t entity)
{
    void* out[2] = {};
    __try {
        return *getEntityComponent(out, registry, &type, entity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Component type bits (below 640) of an entity; returns how many were written.
int EntityTypes(void* registry, uint32_t entity, uint16_t* out, int max)
{
    uint8_t desc[0x40] = {};
    *reinterpret_cast<void**>(desc + 0x30) = registry;
    void* iter[2] = {desc, nullptr};
    int n = 0;
    __try {
        uint8_t* rec = getEntity(iter, entity);
        if (!rec)
            return -1;
        auto* bits = *reinterpret_cast<uint64_t**>(rec + 0x18);
        for (int i = 0; i < kComponentBits && n < max; ++i)
            if (bits[i >> 6] >> (i & 63) & 1)
                out[n++] = static_cast<uint16_t>(i);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}

// Of the chest's types, the one whose first 16 ids are entities with an Inventory.
uint16_t DetectSetupType(void* registry, uint32_t chest)
{
    uint16_t types[kComponentBits];
    int n = EntityTypes(registry, chest, types, kComponentBits);
    uint16_t best = 0;
    int bestScore = 0;
    for (int i = 0; i < n; ++i) {
        auto* data = static_cast<uint32_t*>(Component(registry, types[i], chest));
        if (!data)
            continue;
        int score = 0;
        __try {
            for (int k = 0; k < kMaxPages; ++k)
                if (data[k] >= kFirstNonPlayerId && data[k] != chest && Component(registry, g_typeInventory, data[k]))
                    ++score;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            score = 0;
        }
        if (score > bestScore) {
            bestScore = score;
            best = types[i];
        }
    }
    return best;
}

uint8_t* LookupItemIn(uint32_t itemId, void* db)
{
    __try {
        return g_originalItemLookup(itemId, db);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

uint8_t* LookupItem(uint32_t itemId)
{
    if (!itemId)
        return nullptr;
    for (auto& slot : g_itemDbs) {
        void* db = slot.load();
        if (!db)
            break;
        if (uint8_t* info = LookupItemIn(itemId, db))
            return info;
    }
    return nullptr;
}

int ItemDbCount()
{
    int n = 0;
    for (auto& slot : g_itemDbs)
        n += slot.load() ? 1 : 0;
    return n;
}

bool ReadPage(uint32_t entity, PageKind kind, Page& out)
{
    auto* data = static_cast<uint8_t*>(Component(g_registry, g_typeInventory, entity));
    if (!data)
        return false;
    out.entity = entity;
    out.kind = kind;
    __try {
        memcpy(out.slots, data, sizeof(out.slots));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    // An emptied slot keeps its old item id with count 0: it is empty.
    for (auto& st : out.slots)
        if (!st.count)
            st = {};
    return true;
}


template <class T> bool Find(T& out, const char* name, const char* pattern)
{
    out = reinterpret_cast<T>(FindUnique(pattern));
    if (!out)
        Log("signature not found: %s", name);
    return out != nullptr;
}

void HexLine(const uint8_t* p, int len, char* out)
{
    for (int i = 0; i < len; ++i)
        wsprintfA(out + i * 3, "%02X ", p[i]);
}

// Raw component data of one entity: every type below 640 with its first bytes.
void DumpEntity(void* registry, uint32_t entity, const char* label, int bytes)
{
    uint16_t types[kComponentBits];
    int n = EntityTypes(registry, entity, types, kComponentBits);
    if (n < 0) {
        Log("  %s %u: unknown", label, entity);
        return;
    }
    Log("  %s %u: %d types", label, entity, n);
    for (int i = 0; i < n; ++i) {
        auto* data = static_cast<uint8_t*>(Component(registry, types[i], entity));
        char hex[128 * 3 + 1] = "";
        __try {
            if (data)
                HexLine(data, bytes, hex);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            lstrcpyA(hex, "(unreadable)");
        }
        Log("    type %u: %s", types[i], data ? hex : "(no data)");
    }
}

} // namespace


bool Init()
{
    uint8_t* viewInit;
    uint8_t* itemLookup;
    bool ok = true;
    ok &= Find(viewInit, "ViewInit", "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60");
    ok &= Find(getEntityComponent, "GetEntityComponent", "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B C3 48 83 C4 20 5B C3 CC CC CC CC CC CC CC CC CC 4C 8B D1");
    ok &= Find(getEntity, "GetEntity", "40 53 48 83 EC 20 48 8B 01 89 54 24 38 48 8D 54 24 38 48 8B 58 30 48 8D 8B 48 01 00 00 E8 ?? ?? ?? ?? 48 3B 83 58 01 00 00 73 ?? 48 8B 8B 88 01 00 00 48 8D 04 C1 48 85 C0 74 ?? 48 8B 00 48 83 C4 20 5B C3 33 C0 48 83 C4 20 5B C3 CC CC CC CC 40 53");
    ok &= Find(itemLookup, "ItemInfoLookup", "89 4C 24 08 48 83 EC 28 85 C9 74 ?? 48 8D 4A 38 48 8D 54 24 30 E8 ?? ?? ?? ?? 48 85 C0 74 ??");
    if (!ok)
        return false;
    g_originalItemLookup = static_cast<ItemInfoLookupFn>(HookItemLookup(itemLookup));
    g_originalViewInit = static_cast<ViewInitFn>(InstallHook(viewInit, reinterpret_cast<void*>(&ViewInitHook), 12));
    if (!g_originalItemLookup || !g_originalViewInit) {
        Log("inventory hooks failed");
        return false;
    }
    return true;
}

bool Ready(uint32_t playerId, uint32_t probeChest)
{
    if (g_registry && g_typeSetup)
        return true;
    int seen = 0;
    for (auto& slot : g_seen) {
        void* reg = slot.load();
        if (!reg)
            continue;
        ++seen;
        uint16_t tmp[1];
        if (EntityTypes(reg, playerId, tmp, 1) < 0 || EntityTypes(reg, probeChest, tmp, 1) < 0)
            continue;
        uint16_t setup = DetectSetupType(reg, probeChest);
        if (!setup)
            continue;
        g_registry = reg;
        g_typeSetup = setup;
        Log("client registry %p (of %d seen), InventorySetup type %u, %d item databases", reg, seen, setup, ItemDbCount());
        return true;
    }
    Log("client registry not identified (%d registries seen, %d item databases)", seen, ItemDbCount());
    return false;
}

bool ReadOwner(uint32_t entity, Owner& out)
{
    out.entity = entity;
    out.pageCount = 0;
    if (!g_registry || !g_typeSetup)
        return false;
    uint32_t links[kMaxPages] = {};
    uint8_t kinds[kMaxPages] = {};
    auto* setup = static_cast<uint8_t*>(Component(g_registry, g_typeSetup, entity));
    if (setup) {
        __try {
            memcpy(links, setup, sizeof(links));
            memcpy(kinds, setup + 64, sizeof(kinds));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(links, 0, sizeof(links));
        }
    }
    // The owner's own 8 slots: their kind is the one of the setup link pointing at the owner
    // itself (a player's own page holds equipped items). Without such a link they are
    // generic storage (a chest without a setup).
    PageKind selfKind = setup ? PageKind::Invalid : PageKind::Generic;
    for (int i = 0; i < kMaxPages; ++i)
        if (links[i] == entity)
            selfKind = static_cast<PageKind>(kinds[i]);
    if (ReadPage(entity, selfKind, out.pages[out.pageCount]))
        ++out.pageCount;
    for (int i = 0; i < kMaxPages; ++i)
        if (links[i] && links[i] != entity && ReadPage(links[i], static_cast<PageKind>(kinds[i]), out.pages[out.pageCount]))
            ++out.pageCount;
    return out.pageCount > 0;
}

bool IsHotbarPage(const Owner& player, uint32_t pageEntity)
{
    if (!g_registry)
        return true;
    if (!g_typeHotbar) {
        // The player component whose +0xa0 or +0xa4 holds one of the player's own page ids.
        uint16_t types[kComponentBits];
        int n = EntityTypes(g_registry, player.entity, types, kComponentBits);
        for (int i = 0; i < n && !g_typeHotbar; ++i) {
            if (types[i] == g_typeSetup || types[i] == g_typeInventory)
                continue;
            auto* data = static_cast<uint8_t*>(Component(g_registry, types[i], player.entity));
            if (!data)
                continue;
            uint32_t a = 0, b = 0;
            __try {
                a = *reinterpret_cast<uint32_t*>(data + kHotbarPageA);
                b = *reinterpret_cast<uint32_t*>(data + kHotbarPageB);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
            // Both ids must be two distinct pages of the player: a single match can be a
            // coincidence (an offline world matched an unrelated component this way and the
            // deposit emptied the hotbar).
            bool hasA = false, hasB = false;
            for (int p = 0; p < player.pageCount; ++p) {
                uint32_t id = player.pages[p].entity;
                if (id == player.entity)
                    continue;
                hasA |= id == a;
                hasB |= id == b;
            }
            if (a && b && a != b && hasA && hasB) {
                g_typeHotbar = types[i];
                Log("hotbar: player type %u lists pages %u and %u", types[i], a, b);
            }
        }
        if (!g_typeHotbar) {
            Log("hotbar: not found - tier 2 treats all pages as hotbar and moves nothing");
            return true;
        }
    }
    auto* data = static_cast<uint8_t*>(Component(g_registry, g_typeHotbar, player.entity));
    if (!data)
        return true;
    __try {
        return pageEntity == *reinterpret_cast<uint32_t*>(data + kHotbarPageA) ||
               pageEntity == *reinterpret_cast<uint32_t*>(data + kHotbarPageB);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}


int FindContainers(const float lo[3], const float hi[3], uint32_t* out, int max)
{
    if (!g_registry || !g_typeSetup)
        return 0;
    // Entity table (see 1409a8070): map at registry + 0x148 with capacity at +0x10, occupancy
    // bits at +0x08, entity ids at +0x28; records at registry + 0x188, same slot index.
    auto* reg = static_cast<uint8_t*>(g_registry);
    uint8_t* map = reg + 0x148;
    int n = 0, scanned = 0;
    __try {
        uint64_t capacity = *reinterpret_cast<uint64_t*>(map + 0x10);
        auto* used = *reinterpret_cast<uint64_t**>(map + 0x08);
        auto* ids = *reinterpret_cast<uint32_t**>(map + 0x28);
        auto* records = *reinterpret_cast<uint8_t***>(reg + 0x188);
        if (capacity > (1u << 22))
            return 0;
        for (uint64_t i = 0; i < capacity && n < max; ++i) {
            if (!(used[i >> 6] >> (i & 63) & 1))
                continue;
            uint8_t* rec = records[i];
            uint32_t id = ids[i];
            if (!rec || id < kFirstNonPlayerId)
                continue;
            ++scanned;
            auto* bits = *reinterpret_cast<uint64_t**>(rec + 0x18);
            if (!(bits[g_typeSetup >> 6] >> (g_typeSetup & 63) & 1) || !(bits[kTypePosition >> 6] >> (kTypePosition & 63) & 1))
                continue;
            auto* pos = static_cast<int64_t*>(Component(g_registry, kTypePosition, id));
            if (!pos)
                continue;
            float p[3] = {pos[0] / 4294967296.0f, pos[1] / 4294967296.0f, pos[2] / 4294967296.0f};
            if (p[0] < lo[0] || p[0] >= hi[0] || p[1] < lo[1] || p[1] >= hi[1] || p[2] < lo[2] || p[2] >= hi[2])
                continue;
            out[n++] = id;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("containers: entity table walk failed");
    }
    static int lastN = -1; // FindContainers runs on every bridge status call; log changes only
    if (n != lastN)
        Log("containers: %d in the base (%d entities checked)", n, scanned);
    lastN = n;
    return n;
}

int KeepStorageChests(const uint32_t* magic, int magicCount, uint32_t* candidates, int n)
{
    // Types every magic chest has (bitset over the 640 component types)...
    uint64_t required[kComponentBits / 64];
    memset(required, 0xFF, sizeof(required));
    int counted = 0;
    for (int m = 0; m < magicCount; ++m) {
        uint16_t types[kComponentBits];
        int k = EntityTypes(g_registry, magic[m], types, kComponentBits);
        if (k < 0)
            continue;
        uint64_t have[kComponentBits / 64] = {};
        for (int i = 0; i < k; ++i)
            have[types[i] >> 6] |= 1ull << (types[i] & 63);
        for (int w = 0; w < kComponentBits / 64; ++w)
            required[w] &= have[w];
        ++counted;
    }
    if (!counted)
        return 0;
    // ...minus the ones only magic chests have: InventoryCraftingStock (284) and type 134,
    // which plain chests lack (measured in build 1269579).
    static const uint16_t kMagicOnly[] = {284, 134};
    for (uint16_t t : kMagicOnly)
        required[t >> 6] &= ~(1ull << (t & 63));

    int kept = 0, skipped = 0;
    for (int c = 0; c < n; ++c) {
        uint16_t types[kComponentBits];
        int k = EntityTypes(g_registry, candidates[c], types, kComponentBits);
        uint64_t have[kComponentBits / 64] = {};
        for (int i = 0; i < k; ++i)
            have[types[i] >> 6] |= 1ull << (types[i] & 63);
        bool chest = k > 0;
        for (int w = 0; w < kComponentBits / 64 && chest; ++w)
            chest = (have[w] & required[w]) == required[w];
        if (chest)
            candidates[kept++] = candidates[c];
        else
            ++skipped;
    }
    static int lastSkipped = -1;
    if (skipped != lastSkipped)
        Log("containers: %d are not storage chests (stations?), skipped", skipped);
    lastSkipped = skipped;
    return kept;
}

// Component types of an entity below 640, as text, so stations can be told from chests.
void TypeList(uint32_t entity, char* out, int size)
{
    uint16_t types[kComponentBits];
    int n = EntityTypes(g_registry, entity, types, kComponentBits);
    int len = 0;
    out[0] = 0;
    for (int i = 0; i < n && len < size - 8; ++i)
        len += wsprintfA(out + len, "%u ", types[i]);
}

// "Weapon_T1_1H_WandEpic_01_Fire" -> "Wand Epic Fire", "Ring_T1_Ring_of_Mana" -> "Ring of Mana":
// drops the asset group (first token), tier, hand, level and numbering tokens of the internal
// name and splits run-together words.
void PrettyName(const char* raw, char* out, size_t size)
{
    static const char* const groups[] = {"Material", "Weapon", "Food", "Build", "Block", "Ammo", "Consumable",
                                         "Armor", "Tool", "Ring", "Vanity", "Shield", "Blueprint"};
    static const char* const anywhere[] = {"TerrainMaterial", "Voxel", "CRAFTING", "1H", "2H", "raw", "processed", "Loot"};
    char buf[96];
    strncpy_s(buf, raw, _TRUNCATE);
    size_t len = 0;
    out[0] = 0;
    char* ctx = nullptr;
    bool first = true;
    for (char* tok = strtok_s(buf, "_", &ctx); tok; tok = strtok_s(nullptr, "_", &ctx), first = false) {
        bool skip = (tok[0] == 'T' && (isdigit(static_cast<unsigned char>(tok[1])) || tok[1] == 'X')) ||
                    strncmp(tok, "lvl", 3) == 0 || isdigit(static_cast<unsigned char>(tok[0]));
        for (const char* g : groups)
            skip |= first && strcmp(tok, g) == 0;
        for (const char* d : anywhere)
            skip |= strcmp(tok, d) == 0;
        if (skip)
            continue;
        for (const char* c = tok; *c && len + 2 < size; ++c) {
            bool wordStart = c == tok || (isupper(static_cast<unsigned char>(*c)) && islower(static_cast<unsigned char>(c[-1])));
            if (wordStart && len)
                out[len++] = ' ';
            out[len++] = *c;
        }
        out[len] = 0;
    }
    if (!out[0])
        strncpy_s(out, size, raw, _TRUNCATE);
}

bool Position(uint32_t entity, float out[3])
{
    if (!g_registry)
        return false;
    auto* p = static_cast<int64_t*>(Component(g_registry, kTypePosition, entity));
    if (!p)
        return false;
    __try {
        for (int i = 0; i < 3; ++i)
            out[i] = static_cast<float>(p[i]) / 4294967296.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// Custom string store (see 140e54bf0): map at store + 0x190 with occupancy bits at +0x198,
// capacity at +0x1a0, 64-bit ids at +0x1b8 and entry pointers at +0x1d0; an entry holds the
// text pointer at +0x18 and its length at +0x20.
const uint8_t* FindCustomString(uint8_t* store, uint64_t id, uint32_t* len)
{
    uint64_t capacity = *reinterpret_cast<uint64_t*>(store + 0x1a0);
    auto* used = *reinterpret_cast<uint64_t**>(store + 0x198);
    auto* keys = *reinterpret_cast<uint64_t**>(store + 0x1b8);
    auto* entries = *reinterpret_cast<uint8_t***>(store + 0x1d0);
    if (!id || capacity > 0x10000 || !used || !keys || !entries)
        return nullptr;
    for (uint64_t i = 0; i < capacity; ++i) {
        if (!(used[i >> 6] >> (i & 63) & 1) || keys[i] != id || !entries[i])
            continue;
        *len = static_cast<uint32_t>(*reinterpret_cast<uint64_t*>(entries[i] + 0x20));
        return *reinterpret_cast<const uint8_t**>(entries[i] + 0x18);
    }
    return nullptr;
}

uint16_t g_typeCustomName = 0; // detected: the component whose 8 bytes are a custom string id

bool CustomName(void* client, uint32_t entity, char* out, size_t size)
{
    if (!g_registry || !client)
        return false;
    __try {
        auto* store = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(client) + 0xf0);
        if (!store)
            return false;
        uint16_t types[kComponentBits];
        int n = 0;
        if (g_typeCustomName) {
            types[0] = g_typeCustomName;
            n = 1;
        } else {
            n = EntityTypes(g_registry, entity, types, kComponentBits);
        }
        for (int i = 0; i < n; ++i) {
            auto* data = static_cast<uint64_t*>(Component(g_registry, types[i], entity));
            if (!data)
                continue;
            uint32_t len = 0;
            const uint8_t* text = FindCustomString(store, *data, &len);
            if (!text || !len || len >= size)
                continue;
            memcpy(out, text, len);
            out[len] = 0;
            if (!g_typeCustomName) {
                g_typeCustomName = types[i];
                Log("chest names: CustomPlayerString is type %u", types[i]);
            }
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

ItemDesc Describe(uint32_t itemId)
{
    ItemDesc d = {};
    wsprintfA(d.name, "item %u", itemId);
    uint8_t* info = LookupItem(itemId);
    if (!info)
        return d;
    __try {
        d.known = true;
        d.category = info[kItemCategory];
        d.ammoType = info[kItemAmmoType];
        d.maxStack = *reinterpret_cast<uint16_t*>(info + kItemMaxStack);
        if (d.category == kCategoryWeapons)
            memcpy(d.weaponType, info + kItemWeaponType, sizeof(d.weaponType));
        // BlobString: int32 offset (relative to the field) + uint32 length
        int32_t rel = *reinterpret_cast<int32_t*>(info + kItemDebugName);
        uint32_t len = *reinterpret_cast<uint32_t*>(info + kItemDebugName + 4);
        const char* s = reinterpret_cast<const char*>(info + kItemDebugName + rel);
        if (rel && len && len < sizeof(d.name)) {
            bool printable = true;
            for (uint32_t i = 0; i < len; ++i)
                printable &= s[i] >= 32 && s[i] < 127;
            if (printable && len < sizeof(d.raw)) {
                memcpy(d.raw, s, len);
                d.raw[len] = 0;
                PrettyName(d.raw, d.name, sizeof(d.name));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return d;
}

const char* CategoryName(uint8_t category)
{
    static const char* const names[] = {"Customization", "Equipment", "Weapons", "Tools", "Instrument", "FishingRod",
                                        "BuildTools", "Consumables", "Ammunition", "Materials", "Blueprints", "Currency",
                                        "Collectibles", "AnimalFood", "PetFood", "WeaponGem", "ColorPalette"};
    return category < sizeof(names) / sizeof(names[0]) ? names[category] : "?";
}

} // namespace inv

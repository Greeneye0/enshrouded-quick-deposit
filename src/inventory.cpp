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
#include <span>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <string>
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
constexpr size_t kItemName = 0x48; // 16-byte text key of the name the game shows
constexpr uint8_t kCategoryWeapons = 2;

constexpr int kMaxRegistries = 16;
std::atomic<void*> g_seenLocal[kMaxRegistries];
// Under the Greeneye Loader both live in memory the loader keeps over a hot reload (UseKept).
std::span<std::atomic<void*>> g_seen{g_seenLocal};
constexpr int kMaxItemDbs = 8;
std::atomic<void*> g_itemDbsLocal[kMaxItemDbs];
std::span<std::atomic<void*>> g_itemDbs{g_itemDbsLocal}; // every database the game looked items up in
void* g_registry = nullptr;

// Every detour holds a lease while it runs Quick Deposit's code (the Greeneye Loader's hot
// reload waits for it); an older generation's detour just calls on.
void LearnRegistry(void* iter)
{
    {
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
}

void ViewInitHook(void* iter, void* view, uint32_t size)
{
    {
        greeneye::Lease lease;
        if (lease && !g_registry)
            LearnRegistry(iter);
    }
    g_originalViewInit(iter, view, size);
}

uint8_t* ItemLookupHook(uint32_t itemId, void* itemDb)
{
    uint8_t* info = g_originalItemLookup(itemId, itemDb);
    greeneye::Lease lease;
    if (lease && info && itemDb) {
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
        // Page links with a valid page kind (+64), the chest's own page included: a one-page
        // chest's setup links only to itself, and another component that merely holds a
        // neighbouring chest's id (no kinds) must not win.
        int score = 0;
        __try {
            const auto* kinds = reinterpret_cast<const uint8_t*>(data) + 64;
            for (int k = 0; k < kMaxPages; ++k)
                if (data[k] >= kFirstNonPlayerId && kinds[k] >= 1 && kinds[k] <= 5 && Component(registry, g_typeInventory, data[k]))
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

// The game's localized text lookup: key = fnv1a32 of a 16-byte text key. Its text type is
// {utf8, length, fnv1a64 of the text}. It reads the loca tables through a UI pointer the game
// sets only while it builds the UI (*root + 0xd0), so a lookup outside that (a deposit runs
// before) is queued and done in the next UI frame (ResolveNames); names are cached.
struct GameText {
    const char* utf8;
    size_t len;
    uint64_t hash;
};
using LocaTextFn = GameText* (*)(GameText* out, uint32_t key);
LocaTextFn g_locaText = nullptr;
void** g_locaRoot = nullptr; // the UI pointer the lookup reads; null: not found, never called
SRWLOCK g_namesLock = SRWLOCK_INIT;
std::unordered_map<uint32_t, std::string> g_shownNames; // item id -> name as shown ("": none)
std::vector<uint32_t> g_wantedNames;
std::atomic<bool> g_anyWanted{false};

bool LocaUsable()
{
    __try {
        return g_locaText && g_locaRoot && *g_locaRoot && *reinterpret_cast<void**>(static_cast<uint8_t*>(*g_locaRoot) + 0xd0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The item's name as the game shows it, looked up now (LocaUsable() first), or "".
bool LookupShown(const uint8_t* info, char* out, size_t size)
{
    uint32_t key = 0x811c9dc5u;
    GameText t{"", 0, 0xcbf29ce484222325ull};
    __try {
        for (size_t i = 0; i < 16; ++i)
            key = (key ^ info[kItemName + i]) * 0x1000193u;
        g_locaText(&t, key);
        if (!t.utf8 || !t.len || t.len >= size)
            return false;
        memcpy(out, t.utf8, t.len);
        out[t.len] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
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
    // Optional: item names as the game shows them. Creative Mode hooks the text lookup; Quick
    // Deposit only calls it, so it is also found by the code after its first 18 bytes.
    g_locaText = reinterpret_cast<LocaTextFn>(FindUnique("48 89 5C 24 10 48 89 74 24 18 55 57 41 55 41 56 41 57 48 8D AC 24 F0 F8 FF FF"));
    if (!g_locaText) {
        uint8_t* after = FindUnique("48 8D AC 24 F0 F8 FF FF 48 81 EC 10 08 00 00 4C 8B F1 48 C7 85 F0 00 00 00 40 00 00 00");
        if (after && IsHookPatch(after - 18))
            g_locaText = reinterpret_cast<LocaTextFn>(after - 18);
    }
    // In it: mov rax, [rip + root]; mov [rbp + 0xe8], rdi; mov rcx, [rax + 0xd0]
    if (g_locaText) {
        static const uint8_t tail[] = {0x48, 0x89, 0xBD, 0xE8, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x88, 0xD0, 0x00, 0x00, 0x00};
        auto* f = reinterpret_cast<uint8_t*>(g_locaText);
        for (int i = 0; i < 0x100 && !g_locaRoot; ++i)
            if (f[i] == 0x48 && f[i + 1] == 0x8B && f[i + 2] == 0x05 && memcmp(f + i + 7, tail, sizeof(tail)) == 0)
                g_locaRoot = reinterpret_cast<void**>(f + i + 7 + *reinterpret_cast<int32_t*>(f + i + 3));
    }
    if (!g_locaText || !g_locaRoot)
        Log("signature not found: LocaText - chat shows internal item names");
    g_originalItemLookup = static_cast<ItemInfoLookupFn>(InstallItemLookupHook(itemLookup, reinterpret_cast<void*>(&ItemLookupHook)));
    // ViewInit itself stays as it is (XHL's Area Harvest disassembles it and backs out when it
    // starts with a hook): only these client systems' calls to it go through ViewInitHook. Each
    // works on the local player's or the chests' inventories, so its registry is one Quick
    // Deposit needs.
    static const char* const systems[] = {"client_cursor", "setup_actionbar", "local_player_ui_render_setup",
                                          "render_collection_models", "inventory_setup", "inventory_setup_init"};
    int calls, found;
    g_originalViewInit = static_cast<ViewInitFn>(RedirectSystemCalls(viewInit, systems, 6, reinterpret_cast<void*>(&ViewInitHook), calls, found));
    Log("ViewInit: %d calls redirected from %d of 6 systems", g_originalViewInit ? calls : 0, found);
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
    // A linked page can have a setup of its own: the backpack you wear adds its extra rows that
    // way (a player with 6 backpack rows lists only 4 directly). Follow those links too, each
    // page once.
    for (int p = 1; p < out.pageCount && out.pageCount <= kMaxPages; ++p) {
        auto* sub = static_cast<uint8_t*>(Component(g_registry, g_typeSetup, out.pages[p].entity));
        if (!sub)
            continue;
        uint32_t subLinks[kMaxPages] = {};
        uint8_t subKinds[kMaxPages] = {};
        __try {
            memcpy(subLinks, sub, sizeof(subLinks));
            memcpy(subKinds, sub + 64, sizeof(subKinds));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        for (int i = 0; i < kMaxPages && out.pageCount <= kMaxPages; ++i) {
            uint32_t id = subLinks[i];
            bool known = !id;
            for (int q = 0; q < out.pageCount && !known; ++q)
                known = out.pages[q].entity == id;
            if (!known && ReadPage(id, static_cast<PageKind>(subKinds[i]), out.pages[out.pageCount]))
                ++out.pageCount;
        }
    }
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
            // coincidence (an offline world matched an unrelated component this way and tier 2
            // emptied the hotbar).
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


// Entities in the box with every one of the need types, from one registry's entity table.
int WalkEntities(void* registry, const uint16_t* need, int needCount, const float lo[3], const float hi[3],
                 uint32_t* out, int max, int& scanned)
{
    // Entity table (see 1409a8070): map at registry + 0x148 with capacity at +0x10, occupancy
    // bits at +0x08, entity ids at +0x28; records at registry + 0x188, same slot index.
    auto* reg = static_cast<uint8_t*>(registry);
    uint8_t* map = reg + 0x148;
    int n = 0;
    scanned = 0;
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
            bool all = true;
            for (int k = 0; k < needCount && all; ++k)
                all = bits[need[k] >> 6] >> (need[k] & 63) & 1;
            if (!all)
                continue;
            auto* pos = static_cast<int64_t*>(Component(registry, kTypePosition, id));
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
    return n;
}

int FindContainers(const float lo[3], const float hi[3], uint32_t* out, int max)
{
    if (!g_registry || !g_typeSetup)
        return 0;
    const uint16_t need[] = {g_typeSetup, kTypePosition};
    int scanned = 0;
    int n = WalkEntities(g_registry, need, 2, lo, hi, out, max, scanned);
    static int lastN = -1; // FindContainers runs on every bridge status call; log changes only
    if (n != lastN)
        Log("containers: %d in the base (%d entities checked)", n, scanned);
    lastN = n;
    return n;
}

int FindPlainChests(uint32_t playerId, const float lo[3], const float hi[3], uint32_t* out, int max)
{
    // Without a chest to tell the registries apart, your own pages do: only the client registry
    // holds their inventories.
    if (!g_registry || !g_typeSetup) {
        for (auto& slot : g_seen) {
            void* reg = slot.load();
            uint16_t tmp[1];
            if (!reg || EntityTypes(reg, playerId, tmp, 1) < 0)
                continue;
            if (uint16_t setup = DetectSetupType(reg, playerId)) {
                g_registry = reg;
                g_typeSetup = setup;
                Log("client registry %p (by your own pages), InventorySetup type %u, %d item databases", reg, setup,
                    ItemDbCount());
                break;
            }
        }
    }
    // Every inventory owner in the box whose pages are all storage: equipment, customization or
    // currency pages mean a player, an NPC or a mannequin. (Component type numbers change between
    // game sessions, so chests are not matched by their component types.)
    uint32_t found[512];
    int nf = FindContainers(lo, hi, found, max < 512 ? max : 512);
    int n = 0;
    for (int i = 0; i < nf; ++i) {
        Owner o;
        if (!ReadOwner(found[i], o))
            continue;
        bool storage = true;
        for (int p = 0; p < o.pageCount && storage; ++p)
            storage = o.pages[p].kind != PageKind::Equipment && o.pages[p].kind != PageKind::Customization &&
                      o.pages[p].kind != PageKind::Currency;
        if (storage)
            out[n++] = found[i];
    }
    static int lastN = -1;
    if (n != lastN)
        Log("containers: %d storage chests in a base that lists none (%d inventories)", n, nf);
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
    memcpy(d.shown, d.name, sizeof(d.name));
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
    memcpy(d.shown, d.name, sizeof(d.name));
    AcquireSRWLockExclusive(&g_namesLock);
    auto it = g_shownNames.find(itemId);
    if (it != g_shownNames.end()) {
        if (!it->second.empty())
            strncpy_s(d.shown, it->second.c_str(), _TRUNCATE);
    } else if (LocaUsable()) {
        char shown[sizeof(d.shown)];
        bool ok = LookupShown(info, shown, sizeof(shown));
        g_shownNames[itemId] = ok ? shown : "";
        if (ok)
            memcpy(d.shown, shown, sizeof(shown));
    } else if (g_locaRoot && g_wantedNames.size() < 4096 &&
               std::find(g_wantedNames.begin(), g_wantedNames.end(), itemId) == g_wantedNames.end()) {
        g_wantedNames.push_back(itemId);
        g_anyWanted = true;
    }
    ReleaseSRWLockExclusive(&g_namesLock);
    return d;
}

void ResolveNames()
{
    if (!g_anyWanted || !LocaUsable())
        return;
    AcquireSRWLockExclusive(&g_namesLock);
    for (int n = 0; n < 64 && !g_wantedNames.empty(); ++n) {
        uint32_t id = g_wantedNames.back();
        g_wantedNames.pop_back();
        char shown[96];
        uint8_t* info = LookupItem(id);
        g_shownNames[id] = info && LookupShown(info, shown, sizeof(shown)) ? shown : "";
    }
    g_anyWanted = !g_wantedNames.empty();
    ReleaseSRWLockExclusive(&g_namesLock);
}

const char* CategoryName(uint8_t category)
{
    static const char* const names[] = {"Customization", "Equipment", "Weapons", "Tools", "Instrument", "FishingRod",
                                        "BuildTools", "Consumables", "Ammunition", "Materials", "Blueprints", "Currency",
                                        "Collectibles", "AnimalFood", "PetFood", "WeaponGem", "ColorPalette"};
    return category < sizeof(names) / sizeof(names[0]) ? names[category] : "?";
}

void* InspectRegistry(uint32_t playerId)
{
    if (g_registry)
        return g_registry;
    void* best = nullptr;
    uint64_t bestCapacity = 0;
    for (auto& slot : g_seen) {
        void* reg = slot.load();
        uint16_t tmp[1];
        if (!reg || EntityTypes(reg, playerId, tmp, 1) < 0)
            continue;
        uint64_t capacity = 0;
        __try {
            capacity = *reinterpret_cast<uint64_t*>(static_cast<uint8_t*>(reg) + 0x148 + 0x10);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (capacity > bestCapacity) {
            best = reg;
            bestCapacity = capacity;
        }
    }
    return best;
}

int Registries(void** out, int max)
{
    int n = 0;
    for (auto& slot : g_seen)
        if (void* reg = slot.load(); reg && n < max)
            out[n++] = reg;
    return n;
}

int Types(void* registry, uint32_t entity, uint16_t* out, int max)
{
    return registry ? EntityTypes(registry, entity, out, max) : -1;
}

void* Data(void* registry, uint16_t type, uint32_t entity)
{
    return registry ? Component(registry, type, entity) : nullptr;
}

int EntitiesNear(void* registry, const float center[3], float radius, uint32_t* out, float* dist, int max)
{
    if (!registry || max <= 0)
        return 0;
    // Same entity table walk as FindContainers.
    auto* reg = static_cast<uint8_t*>(registry);
    uint8_t* map = reg + 0x148;
    int n = 0;
    __try {
        uint64_t capacity = *reinterpret_cast<uint64_t*>(map + 0x10);
        auto* used = *reinterpret_cast<uint64_t**>(map + 0x08);
        auto* ids = *reinterpret_cast<uint32_t**>(map + 0x28);
        auto* records = *reinterpret_cast<uint8_t***>(reg + 0x188);
        if (capacity > (1u << 22))
            return 0;
        for (uint64_t i = 0; i < capacity; ++i) {
            if (!(used[i >> 6] >> (i & 63) & 1))
                continue;
            uint8_t* rec = records[i];
            uint32_t id = ids[i];
            if (!rec || id < kFirstNonPlayerId)
                continue;
            auto* bits = *reinterpret_cast<uint64_t**>(rec + 0x18);
            if (!(bits[kTypePosition >> 6] >> (kTypePosition & 63) & 1))
                continue;
            auto* pos = static_cast<int64_t*>(Component(registry, kTypePosition, id));
            if (!pos)
                continue;
            float dx = pos[0] / 4294967296.0f - center[0], dy = pos[1] / 4294967296.0f - center[1],
                  dz = pos[2] / 4294967296.0f - center[2];
            float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d > radius)
                continue;
            // keep the nearest max, sorted: append (or replace the farthest), then bubble down
            int at;
            if (n < max)
                at = n++;
            else if (d < dist[max - 1])
                at = max - 1;
            else
                continue;
            out[at] = id;
            dist[at] = d;
            for (; at > 0 && dist[at - 1] > dist[at]; --at) {
                std::swap(out[at], out[at - 1]);
                std::swap(dist[at], dist[at - 1]);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("inspect: entity table walk failed");
    }
    return n;
}

void Forget()
{
    if (!g_registry && !g_seen[0].load())
        return;
    g_registry = nullptr;
    g_typeSetup = 0;
    g_typeHotbar = 0;
    for (auto& slot : g_seen)
        slot.store(nullptr);
    Log("left the world: client registry forgotten");
}

void UseKept(const GreeneyeHost* host)
{
    g_seen = {static_cast<std::atomic<void*>*>(host->keep("quick-deposit.registries", sizeof(g_seenLocal))), kMaxRegistries};
    g_itemDbs = {static_cast<std::atomic<void*>*>(host->keep("quick-deposit.itemdbs", sizeof(g_itemDbsLocal))), kMaxItemDbs};
}

} // namespace inv

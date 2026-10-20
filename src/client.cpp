// Deposits through the client, the way the chest window does it, so the host (this PC, a
// friend's game or a dedicated server) validates and performs every transfer.
//
// F7 runs as a small state machine on the client's per-frame UI loop, so nothing ever blocks
// a frame:
//   1. snapshot your backpack and the base's chests
//   2. tier 1: "Deposit Stacks" into every chest (same item -> chest that already holds it)
//   3. wait for the host's changes, then tier 2: each remaining backpack stack moves into a
//      free slot of a chest holding the same item, else of the chest holding the most items
//      of the same kind (category; weapons: same weapon type)
//   4. wait again, then write a report of what went into which chest
//
// 140845600 UiCommandLoop(app): runs every frame; client = *(*(app + 0x35280) + 0x1b8).
// 140851530 UiDispatch(client, cmd): command 0x2c becomes a TransferItemToInventoryEvent,
//           which the client sends to the host as an InventoryTransferAction.
// Layouts confirmed in game (build 1269579):
//   client + 0x47d4e0 -> client world; client + 0x466b44 local player handle,
//   player id = (handle & 0x3f) + 1; world + 0x2c08 local player position (int64 x, y, z);
//   world + 0x2c58 base permission object; world + 0x3eb8 -> base registry.
//   "Deposit Stacks": cmd 0x2c, flag index 1 (RefillStacks) on, target = chest, source = player.
//   Move one stack:  cmd 0x2c, no flag, target slot = (page entity, index) at +0x10,
//                    source slot = (page entity, index) at +0x1c.
//   Both use type 2 at +0x24.
#include <windows.h>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#ifdef QD_BRIDGE
#include "bridge.h"
#endif
#include "chat.h"
#include "client.h"
#include "inventory.h"
#include "log.h"
#include "scan.h"

namespace client {
namespace {

using UiCommandLoopFn = void (*)(void* app);
using UiDispatchFn = void (*)(void* client, uint8_t* cmd);

UiCommandLoopFn g_originalLoop = nullptr;
UiDispatchFn g_originalDispatch = nullptr;
std::atomic<bool> g_requested{false};
Config g_pending;
Config g_cfg;
bool g_broken = false;

constexpr size_t kAppUi = 0x35280;
constexpr size_t kUiClient = 0x1b8;
constexpr size_t kClientWorld = 0x47d4e0;
constexpr size_t kClientPlayer = 0x466b44;
constexpr size_t kWorldPlayerPos = 0x2c08;
constexpr size_t kWorldPermissions = 0x2c58;
constexpr size_t kWorldBases = 0x3eb8;

constexpr size_t kRegPermissions = 0x10;
constexpr size_t kRegBaseCount = 0x24;
constexpr size_t kRegBases = 0x30;
constexpr size_t kBaseStride = 0x130;
constexpr size_t kBaseChests = 0x00;
constexpr size_t kBaseChestCount = 0x08;
constexpr size_t kBaseMin = 0xF0;
constexpr size_t kBaseMax = 0x100;

constexpr size_t kCmdSize = 0x28;
constexpr uint8_t kCmdTransfer = 0x2c;
constexpr size_t kCmdFlagIndex = 0x04;
constexpr size_t kCmdFlagOn = 0x08;
constexpr size_t kCmdTarget = 0x0c;
constexpr size_t kCmdTargetSlot = 0x10;
constexpr size_t kCmdSource = 0x18;
constexpr size_t kCmdSourceSlot = 0x1c;
constexpr size_t kCmdType = 0x24;
constexpr uint32_t kFlagRefillStacks = 1;
constexpr uint8_t kTypeTransfer = 2;
constexpr uint8_t kCategoryWeapons = 2;
constexpr uint8_t kCategoryAmmunition = 8;
constexpr uint8_t kCategoryEquipment = 1;

constexpr float kFixedToMeters = 1.0f / 4294967296.0f;
constexpr int kMaxChests = 64;
constexpr int kMaxMoves = 128;

template <class T> T& At(void* base, size_t offset)
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

// --- state --------------------------------------------------------------------------------

enum class Phase { Idle, Tier1, Settle1, Tier2, Settle2 };

// Why a stack went to its chest, in order of preference.
enum class Reason { SameItem, SameKind, SameCategory, ChestName, Nearest };

const char* ReasonText(Reason r)
{
    switch (r) {
    case Reason::SameItem: return "same item";
    case Reason::SameKind: return "same kind";
    case Reason::SameCategory: return "weapons";
    case Reason::ChestName: return "chest name";
    default: return "nearest chest";
    }
}

struct Move {
    int srcPage, srcSlot;
    int chest, dstPage, dstSlot;
    uint32_t itemId, count;
    Reason reason;
};

struct Run {
    Phase phase = Phase::Idle;
    ULONGLONG nextTime = 0;
    uint32_t playerId = 0;
    int chestCount = 0;     // all containers of the base
    int magicCount = 0;     // the first magicCount are the base's crafting-storage chests
    uint32_t chests[kMaxChests];
    int next = 0;
    int moveCount = 0;
    Move moves[kMaxMoves];
    inv::Owner backpackBefore;
    inv::Owner backpackMid;
    inv::Owner chestsBefore[kMaxChests];
    inv::Owner chestsMid[kMaxChests];
    bool chestsVisible = false;
    char notes[4096];
    int notesLen = 0;
} g_run;

void Note(const char* fmt, ...)
{
    if (g_run.notesLen >= static_cast<int>(sizeof(g_run.notes)) - 200)
        return;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(g_run.notes + g_run.notesLen, sizeof(g_run.notes) - g_run.notesLen - 2, fmt, args);
    va_end(args);
    if (n < 0)
        return;
    g_run.notesLen += n;
    g_run.notes[g_run.notesLen++] = '\n';
    g_run.notes[g_run.notesLen] = 0;
}

void* ClientFromApp(void* app)
{
    void* ui = At<void*>(app, kAppUi);
    return ui ? At<void*>(ui, kUiClient) : nullptr;
}

void Send(void* cl, const uint8_t* cmd)
{
    alignas(8) uint8_t copy[kCmdSize];
    memcpy(copy, cmd, kCmdSize);
    g_originalDispatch(cl, copy);
}

// General storage: backpack rows (Generic) and the extra slots an equipped backpack adds
// (Virtual). Equipment, customization and currency pages are never storage.
bool IsStoragePage(const inv::Page& p)
{
    return p.kind == inv::PageKind::Generic || p.kind == inv::PageKind::Virtual;
}

// Total count per item id, written as parallel arrays.
int Totals(const inv::Owner& o, uint32_t* ids, uint64_t* counts, int max)
{
    int n = 0;
    for (int p = 0; p < o.pageCount; ++p) {
        if (!IsStoragePage(o.pages[p]))
            continue;
        for (const auto& s : o.pages[p].slots) {
            if (!s.itemId)
                continue;
            int k = 0;
            while (k < n && ids[k] != s.itemId)
                ++k;
            if (k == n) {
                if (n == max)
                    continue;
                ids[n] = s.itemId;
                counts[n++] = 0;
            }
            counts[k] += s.count;
        }
    }
    return n;
}

uint64_t CountOf(const inv::Owner& o, uint32_t itemId)
{
    uint64_t c = 0;
    for (int p = 0; p < o.pageCount; ++p)
        if (IsStoragePage(o.pages[p]))
            for (const auto& s : o.pages[p].slots)
                if (s.itemId == itemId)
                    c += s.count;
    return c;
}

void* g_client = nullptr; // client object of the current frame (UI thread only)

// What you call the chest in game, else what it mostly holds: "Wood Logs chest".
void ChestLabel(int index, const inv::Owner& o, char* out, size_t size)
{
    if (inv::CustomName(g_client, o.entity, out, size))
        return;
    uint32_t ids[64];
    uint64_t counts[64];
    int n = Totals(o, ids, counts, 64);
    int best = -1;
    for (int i = 0; i < n; ++i)
        if (best < 0 || counts[i] > counts[best])
            best = i;
    if (best < 0)
        _snprintf_s(out, size, _TRUNCATE, "empty chest %d", index + 1);
    else
        _snprintf_s(out, size, _TRUNCATE, "%s chest", inv::Describe(ids[best]).name);
}

// True if name is one of the comma-separated names in list (case and outer spaces ignored).
bool NameListed(const char* list, const char* name)
{
    const char* p = list;
    while (*p) {
        while (*p == ' ' || *p == ',')
            ++p;
        const char* start = p;
        while (*p && *p != ',')
            ++p;
        const char* end = p;
        while (end > start && end[-1] == ' ')
            --end;
        size_t len = static_cast<size_t>(end - start);
        if (len && len == strlen(name) && _strnicmp(start, name, len) == 0)
            return true;
    }
    return false;
}

// --- steps --------------------------------------------------------------------------------

// The base you stand in and its storage chests: the base's own (magic) chests first, then
// every other storage chest inside it; excluded chests ("--" names, excludeChests) left out.
struct BaseScan {
    uint32_t playerId = 0;
    float pos[3] = {};
    uint8_t* base = nullptr;
    int magicCount = 0;
    std::vector<uint32_t> chests;
    std::vector<std::string> excluded;
    bool readable = false; // inventories readable (registry known)
};

bool ScanBase(void* cl, const Config& cfg, BaseScan& out, std::string& why)
{
    auto* world = cl ? At<uint8_t*>(cl, kClientWorld) : nullptr;
    if (!world) {
        why = "not in a world";
        return false;
    }
    uint32_t handle = At<uint32_t>(cl, kClientPlayer);
    out.playerId = handle ? (handle & 0x3f) + 1 : 0;
    auto* reg = At<uint8_t*>(world, kWorldBases);
    if (!out.playerId || !reg || At<uint8_t*>(reg, kRegPermissions) != world + kWorldPermissions) {
        why = "player or base data not where expected - game version changed?";
        return false;
    }
    for (int i = 0; i < 3; ++i)
        out.pos[i] = static_cast<float>(At<int64_t>(world, kWorldPlayerPos + 8 * i)) * kFixedToMeters;

    uint32_t baseCount = At<uint32_t>(reg, kRegBaseCount);
    for (uint32_t b = 0; b < baseCount && b < 1024 && !out.base; ++b) {
        uint8_t* cand = reg + kRegBases + b * kBaseStride;
        const float* lo = &At<float>(cand, kBaseMin);
        const float* hi = &At<float>(cand, kBaseMax);
        if (out.pos[0] >= lo[0] && out.pos[0] < hi[0] && out.pos[1] >= lo[1] && out.pos[1] < hi[1] &&
            out.pos[2] >= lo[2] && out.pos[2] < hi[2])
            out.base = cand;
    }
    if (!out.base) {
        char buf[96];
        _snprintf_s(buf, _TRUNCATE, "not inside a base (%.0f, %.0f, %.0f)", out.pos[0], out.pos[1], out.pos[2]);
        why = buf;
        return false;
    }
    auto* ids = At<uint32_t*>(out.base, kBaseChests);
    uint64_t n = At<uint64_t>(out.base, kBaseChestCount);
    for (uint64_t i = 0; ids && i < n && out.chests.size() < kMaxChests; ++i)
        if (ids[i])
            out.chests.push_back(ids[i]);
    out.magicCount = static_cast<int>(out.chests.size());

    out.readable = !out.chests.empty() && inv::Ready(out.playerId, out.chests[0]);
    if (out.readable) {
        uint32_t found[kMaxChests];
        int nf = inv::FindContainers(&At<float>(out.base, kBaseMin), &At<float>(out.base, kBaseMax), found, kMaxChests);
        nf = inv::KeepStorageChests(out.chests.data(), out.magicCount, found, nf);
        for (int i = 0; i < nf && out.chests.size() < kMaxChests; ++i) {
            bool known = false;
            for (uint32_t c : out.chests)
                known |= c == found[i];
            if (!known)
                out.chests.push_back(found[i]);
        }
    }
    std::vector<uint32_t> kept;
    for (uint32_t c : out.chests) {
        char name[96];
        if (inv::CustomName(g_client, c, name, sizeof(name))) {
            const char* nm = name;
            while (*nm == ' ')
                ++nm;
            if ((nm[0] == '-' && nm[1] == '-') || NameListed(cfg.excludeChests, name)) {
                out.excluded.push_back(name);
                continue;
            }
        }
        kept.push_back(c);
    }
    out.chests = std::move(kept);
    return true;
}

bool Start(void* cl, std::string& why)
{
    BaseScan scan;
    if (!ScanBase(cl, g_cfg, scan, why)) {
        Log("deposit: %s", why.c_str());
        return false;
    }
    for (const std::string& name : scan.excluded)
        Log("deposit: skipping excluded chest '%s'", name.c_str());
    uint32_t playerId = scan.playerId;
    Run& r = g_run;
    r.playerId = playerId;
    r.chestCount = 0;
    r.next = 0;
    r.moveCount = 0;
    r.notesLen = 0;
    r.notes[0] = 0;
    r.magicCount = scan.magicCount;
    for (uint32_t c : scan.chests)
        r.chests[r.chestCount++] = c;
    bool readable = scan.readable && inv::ReadOwner(playerId, r.backpackBefore);
    int visible = 0;
    for (int i = 0; readable && i < r.chestCount; ++i)
        visible += inv::ReadOwner(r.chests[i], r.chestsBefore[i]) ? 1 : 0;
    r.chestsVisible = readable && visible == r.chestCount;
    if (!readable)
        r.backpackBefore.pageCount = 0;
    Log("deposit: %d chests in this base; inventories %s (%d of %d chests visible)", r.chestCount,
        readable ? "readable" : "not readable", visible, r.chestCount);
    r.phase = Phase::Tier1;
    r.nextTime = 0;
    return true;
}

void StepTier1(void* cl, ULONGLONG now)
{
    Run& r = g_run;
    alignas(8) uint8_t cmd[kCmdSize] = {};
    cmd[0] = kCmdTransfer;
    At<uint32_t>(cmd, kCmdFlagIndex) = kFlagRefillStacks;
    cmd[kCmdFlagOn] = 1;
    At<uint32_t>(cmd, kCmdTarget) = r.chests[r.next];
    At<uint32_t>(cmd, kCmdSource) = r.playerId;
    cmd[kCmdType] = kTypeTransfer;
    Send(cl, cmd);
    r.nextTime = now + g_cfg.delayMs;
    if (++r.next == r.chestCount) {
        Log("deposit: tier 1 sent to %d chests", r.chestCount);
        r.phase = Phase::Settle1;
        r.nextTime = now + g_cfg.settleMs;
    }
}

// The asset group an item's internal name starts with: "Armor" in "Armor_T0_Fur_Legs", "Ring"
// in "Ring_T1_Ring_of_Mana". Splits equipment into armor, rings, shields, backpacks...
bool SameGroup(const inv::ItemDesc& a, const inv::ItemDesc& b)
{
    const char* ea = strchr(a.raw, '_');
    const char* eb = strchr(b.raw, '_');
    size_t la = ea ? static_cast<size_t>(ea - a.raw) : strlen(a.raw);
    size_t lb = eb ? static_cast<size_t>(eb - b.raw) : strlen(b.raw);
    return la && la == lb && strncmp(a.raw, b.raw, la) == 0;
}

// Same category; weapons also need the same weapon type, ammunition the same kind of ammo
// (arrows, spells, fishing bait), equipment the same group (armor, rings, shields...).
bool SameKind(const inv::ItemDesc& a, const inv::ItemDesc& b)
{
    if (!a.known || !b.known || a.category != b.category)
        return false;
    if (a.category == kCategoryWeapons)
        return memcmp(a.weaponType, b.weaponType, sizeof(a.weaponType)) == 0;
    if (a.category == kCategoryAmmunition)
        return a.ammoType == b.ammoType;
    if (a.category == kCategoryEquipment)
        return SameGroup(a, b);
    return true;
}

// Ammunition belongs with the weapons that fire it, the way the game labels arrows "Bow Ammo":
// arrows with bows, spell charges with staves and wands.
bool AmmoFits(const inv::ItemDesc& ammo, const inv::ItemDesc& weapon)
{
    if (!ammo.known || !weapon.known || ammo.category != kCategoryAmmunition || weapon.category != kCategoryWeapons)
        return false;
    switch (ammo.ammoType) {
    case 1: return strstr(weapon.raw, "Bow") != nullptr;
    case 2: return strstr(weapon.raw, "Staff") != nullptr || strstr(weapon.raw, "Wand") != nullptr;
    default: return false;
    }
}

bool FreeSlot(const inv::Owner& o, int& page, int& slot)
{
    for (int p = 0; p < o.pageCount; ++p) {
        if (!IsStoragePage(o.pages[p]))
            continue;
        for (int s = 0; s < inv::kSlotsPerPage; ++s) {
            if (!o.pages[p].slots[s].itemId) {
                page = p;
                slot = s;
                return true;
            }
        }
    }
    return false;
}

// Lower-case words of at least 3 letters, plural "s" dropped, split at "_", spaces,
// punctuation and inner capitals ("WandEpic" -> wand, epic). Returns how many were written.
int Words(const char* text, char (*out)[24], int max)
{
    static const char* const ignore[] = {"and", "the", "misc", "chest", "stuff", "other", "epic", "craft"};
    int n = 0;
    char word[24];
    int len = 0;
    auto flush = [&]() {
        if (len >= 3 && n < max) {
            word[len] = 0;
            if (len > 3 && word[len - 1] == 's')
                word[len - 1] = 0;
            bool skip = false;
            for (const char* i : ignore)
                skip |= strcmp(word, i) == 0;
            if (!skip)
                strcpy_s(out[n++], word);
        }
        len = 0;
    };
    for (const char* c = text;; ++c) {
        unsigned char ch = static_cast<unsigned char>(*c);
        bool letter = isalpha(ch) != 0;
        if (letter && len && isupper(ch) && islower(static_cast<unsigned char>(c[-1])))
            flush();
        if (letter) {
            if (len < 23)
                word[len++] = static_cast<char>(tolower(ch));
        } else {
            flush();
            if (!ch)
                break;
        }
    }
    return n;
}

// How well a chest's name fits the item: a word of the item's own name ("Wand", "Fire") counts
// 3, a word only found in its asset group or category ("Weapon", "Materials") counts 1, so
// "Wands" beats "Melee Weapons" for a wand.
int NameScore(const char* chestName, const inv::ItemDesc& d)
{
    char chestWords[8][24], ownWords[12][24], groupWords[24][24];
    int nc = Words(chestName, chestWords, 8);
    int no = Words(d.name, ownWords, 12);
    int ng = Words(d.raw, groupWords, 20);
    ng += Words(inv::CategoryName(d.category), groupWords + ng, 24 - ng);
    int score = 0;
    for (int i = 0; i < nc; ++i) {
        int best = 0;
        for (int k = 0; k < no && best < 3; ++k)
            if (strcmp(chestWords[i], ownWords[k]) == 0)
                best = 3;
        for (int k = 0; k < ng && !best; ++k)
            if (strcmp(chestWords[i], groupWords[k]) == 0)
                best = 1;
        score += best;
    }
    return score;
}

void PlanTier2()
{
    Run& r = g_run;
    r.moveCount = 0;
    r.next = 0;
    if (!inv::ReadOwner(r.playerId, r.backpackMid))
        return;
    for (int i = 0; i < r.chestCount; ++i)
        if (!inv::ReadOwner(r.chests[i], r.chestsMid[i]))
            r.chestsMid[i].pageCount = 0;

    // Chest names (only the ones you named) and distances to you, for steps 3 and 4.
    static char names[kMaxChests][96];
    float dist[kMaxChests];
    float me[3] = {};
    bool haveMe = inv::Position(r.playerId, me);
    for (int c = 0; c < r.chestCount; ++c) {
        if (!inv::CustomName(g_client, r.chests[c], names[c], sizeof(names[c])))
            names[c][0] = 0;
        float pos[3];
        dist[c] = 1e30f;
        if (haveMe && inv::Position(r.chests[c], pos))
            dist[c] = (pos[0] - me[0]) * (pos[0] - me[0]) + (pos[1] - me[1]) * (pos[1] - me[1]) +
                      (pos[2] - me[2]) * (pos[2] - me[2]);
    }

    for (int p = 0; p < r.backpackMid.pageCount; ++p) {
        const inv::Page& page = r.backpackMid.pages[p];
        if (!IsStoragePage(page) || inv::IsHotbarPage(r.backpackMid, page.entity))
            continue; // equipment, currency and hotbar stay where they are
        for (int s = 0; s < inv::kSlotsPerPage; ++s) {
            const inv::Stack st = page.slots[s];
            if (!st.itemId)
                continue;
            inv::ItemDesc d = inv::Describe(st.itemId);
            if (!d.known || !(g_cfg.tier2Categories & (1u << d.category)))
                continue; // stays with you by design (category not enabled)

            // Among chests with a free slot: 1. one holding this exact item (the best of them),
            // 2. the one holding the most items of the same kind (weapon or ammo type), 3. the one
            // whose name matches the item best, 4. for weapons the one holding the most weapons,
            // 5. the nearest.
            int sameItem = -1, kindChest = -1, catChest = -1, nameChest = -1, nearChest = -1;
            int bestKind = 0, bestCat = 0, bestName = 0, sameItemKind = -1;
            for (int c = 0; c < r.chestCount; ++c) {
                const inv::Owner& ch = r.chestsMid[c];
                int pg, sl;
                if (!FreeSlot(ch, pg, sl))
                    continue;
                bool holdsItem = CountOf(ch, st.itemId) > 0;
                int kind = 0, cat = 0;
                for (int cp = 0; cp < ch.pageCount; ++cp) {
                    if (!IsStoragePage(ch.pages[cp]))
                        continue;
                    for (const auto& cs : ch.pages[cp].slots) {
                        if (!cs.itemId)
                            continue;
                        inv::ItemDesc cd = inv::Describe(cs.itemId);
                        kind += SameKind(cd, d) || AmmoFits(d, cd) ? 1 : 0;
                        cat += cd.known && cd.category == d.category ? 1 : 0;
                    }
                }
                if (kind > bestKind) {
                    bestKind = kind;
                    kindChest = c;
                }
                // Several chests may hold the item (someone's own chest, a stray stack): the one
                // holding the most of its kind is its real home, the nearer one on a tie.
                if (holdsItem && (kind > sameItemKind || (kind == sameItemKind && dist[c] < dist[sameItem]))) {
                    sameItemKind = kind;
                    sameItem = c;
                }
                if (cat > bestCat) {
                    bestCat = cat;
                    catChest = c;
                }
                int score = names[c][0] ? NameScore(names[c], d) : 0;
                if (score > bestName) {
                    bestName = score;
                    nameChest = c;
                }
                if (nearChest < 0 || dist[c] < dist[nearChest])
                    nearChest = c;
            }
            int chest = nearChest;
            Reason reason = Reason::Nearest;
            if (sameItem >= 0) {
                chest = sameItem;
                reason = Reason::SameItem;
            } else if (kindChest >= 0) {
                chest = kindChest;
                reason = Reason::SameKind;
            } else if (nameChest >= 0) {
                chest = nameChest;
                reason = Reason::ChestName;
            } else if (catChest >= 0 && d.category == kCategoryWeapons) {
                chest = catChest;
                reason = Reason::SameCategory;
            }
            if (chest < 0) {
                Note("  %s x%u: every chest is full", d.name, st.count);
                continue;
            }
            if (r.moveCount == kMaxMoves)
                continue;
            inv::Owner& ch = r.chestsMid[chest];
            Move& m = r.moves[r.moveCount++];
            FreeSlot(ch, m.dstPage, m.dstSlot);
            ch.pages[m.dstPage].slots[m.dstSlot] = st; // reserve the slot for the next stacks
            m.srcPage = p;
            m.srcSlot = s;
            m.chest = chest;
            m.itemId = st.itemId;
            m.count = st.count;
            m.reason = reason;
        }
    }
}

void StepTier2(void* cl, ULONGLONG now)
{
    Run& r = g_run;
    const Move& m = r.moves[r.next];
    alignas(8) uint8_t cmd[kCmdSize] = {};
    cmd[0] = kCmdTransfer;
    At<uint32_t>(cmd, kCmdTargetSlot) = r.chestsMid[m.chest].pages[m.dstPage].entity;
    cmd[kCmdTargetSlot + 4] = static_cast<uint8_t>(m.dstSlot);
    At<uint32_t>(cmd, kCmdSourceSlot) = r.backpackMid.pages[m.srcPage].entity;
    cmd[kCmdSourceSlot + 4] = static_cast<uint8_t>(m.srcSlot);
    cmd[kCmdType] = kTypeTransfer;
    Send(cl, cmd);
    r.nextTime = now + g_cfg.delayMs;
    if (++r.next == r.moveCount) {
        Log("deposit: tier 2 sent %d moves", r.moveCount);
        r.phase = Phase::Settle2;
        r.nextTime = now + g_cfg.settleMs;
    }
}

std::vector<std::string> g_report; // lines of the last report, for the bridge
uint64_t g_runsFinished = 0;

struct ReportFile {
    FILE* f = nullptr;
    void Line(const char* fmt, ...)
    {
        char buf[600];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        Log("%s", buf);
        g_report.push_back(buf);
        if (f)
            fprintf(f, "%s\n", buf);
    }
};

void WriteReport()
{
    Run& r = g_run;
    g_report.clear();
    ReportFile rep;
    rep.f = fopen("quick-deposit-report.txt", "w");
    SYSTEMTIME t;
    GetLocalTime(&t);
    rep.Line("Quick Deposit report %04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

    inv::Owner after;
    if (!r.backpackBefore.pageCount || !inv::ReadOwner(r.playerId, after)) {
        rep.Line("Inventories were not readable, so only Deposit Stacks ran and there is nothing to report.");
        if (rep.f)
            fclose(rep.f);
        return;
    }

    int lines = 0;
    // What left your backpack, per item. A chest's gain only counts up to that, so deposits
    // other players make at the same time don't show up as yours.
    uint32_t lostIds[128];
    uint64_t lost[128];
    int lostCount = Totals(r.backpackBefore, lostIds, lost, 128);
    for (int i = 0; i < lostCount; ++i) {
        uint64_t now = CountOf(after, lostIds[i]);
        lost[i] = lost[i] > now ? lost[i] - now : 0;
    }
    auto takeLost = [&](uint32_t id, uint64_t gained) -> uint64_t {
        for (int i = 0; i < lostCount; ++i)
            if (lostIds[i] == id) {
                uint64_t mine = gained < lost[i] ? gained : lost[i];
                lost[i] -= mine;
                return mine;
            }
        return 0;
    };
    if (r.chestsVisible) {
        // Per chest: what it gained from you, from its contents before and after.
        for (int c = 0; c < r.chestCount; ++c) {
            inv::Owner now;
            if (!inv::ReadOwner(r.chests[c], now))
                continue;
            uint32_t ids[64];
            uint64_t counts[64];
            int n = Totals(now, ids, counts, 64);
            char label[160];
            ChestLabel(c, r.chestsBefore[c], label, sizeof(label));
            bool header = false;
            char chatLine[220];
            int chatLen = _snprintf_s(chatLine, _TRUNCATE, "%s:", label);
            for (int i = 0; i < n; ++i) {
                uint64_t before = CountOf(r.chestsBefore[c], ids[i]);
                if (counts[i] <= before)
                    continue;
                uint64_t mine = takeLost(ids[i], counts[i] - before);
                if (!mine)
                    continue;
                if (!header) {
                    rep.Line("%s:", label);
                    header = true;
                }
                inv::ItemDesc item = inv::Describe(ids[i]);
                const char* name = item.name;
                unsigned long long added = mine;
                rep.Line("  + %s x%llu", name, added);
                if (chatLen > 0 && chatLen < 200) {
                    int w = _snprintf_s(chatLine + chatLen, sizeof(chatLine) - chatLen, _TRUNCATE, "%s %s x%llu",
                                        chatLen > static_cast<int>(strlen(label)) + 1 ? "," : "", name, added);
                    chatLen = w < 0 ? 200 : chatLen + w;
                }
                ++lines;
            }
            if (header)
                chat::Post(chatLine);
        }
    } else {
        // Chest contents unknown to the client: report what left the backpack.
        uint32_t ids[128];
        uint64_t counts[128];
        int n = Totals(r.backpackBefore, ids, counts, 128);
        for (int i = 0; i < n; ++i) {
            uint64_t now = CountOf(after, ids[i]);
            if (now < counts[i]) {
                rep.Line("  deposited %s x%llu", inv::Describe(ids[i]).name, static_cast<unsigned long long>(counts[i] - now));
                ++lines;
            }
        }
    }
    if (!lines) {
        rep.Line("Nothing was deposited.");
        chat::Post("Quick Deposit: nothing to deposit");
    }

    if (g_cfg.tier2 == Tier2Mode::DryRun && r.moveCount) {
        rep.Line("Tier 2 plan (dry run - nothing moved; set tier2=on in quick-deposit.ini to do it):");
        for (int i = 0; i < r.moveCount; ++i) {
            const Move& m = r.moves[i];
            char label[160];
            ChestLabel(m.chest, r.chestsBefore[m.chest], label, sizeof(label));
            inv::ItemDesc d = inv::Describe(m.itemId);
            rep.Line("  %s x%u -> %s, page %u slot %d (%s)", d.name, m.count, label,
                     r.chestsMid[m.chest].pages[m.dstPage].entity, m.dstSlot, ReasonText(m.reason));
        }
    }
    if (g_cfg.tier2 == Tier2Mode::On && r.moveCount) {
        // Each move checked at its target slot.
        rep.Line("Sorted by kind:");
        for (int i = 0; i < r.moveCount; ++i) {
            const Move& m = r.moves[i];
            char label[160];
            ChestLabel(m.chest, r.chestsBefore[m.chest], label, sizeof(label));
            // The host may pick another free slot, so check the chest as a whole.
            inv::Owner now;
            bool arrived = inv::ReadOwner(r.chests[m.chest], now) &&
                           CountOf(now, m.itemId) > CountOf(r.chestsBefore[m.chest], m.itemId);
            inv::ItemDesc item = inv::Describe(m.itemId);
            const char* name = item.name;
            rep.Line("  %s -> %s (%s): %s", name, label,
                     ReasonText(m.reason), arrived ? "done" : "did NOT move");
            if (!arrived) {
                char chatLine[220];
                _snprintf_s(chatLine, _TRUNCATE, "Quick Deposit: %s could not be moved to %s", name, label);
                chat::Post(chatLine);
            }
        }
    }
    if (r.notesLen) {
        rep.Line("Left in the backpack:");
        rep.Line("%s", r.notes);
    }
    if (rep.f)
        fclose(rep.f);
}

void Tick(void* app)
{
    g_client = ClientFromApp(app);
    chat::SetClient(g_client);
    if (g_requested.exchange(false)) {
        if (g_run.phase != Phase::Idle) {
            Log("deposit: still busy with the last one");
        } else {
            g_cfg = g_pending;
            std::string why;
            Start(ClientFromApp(app), why);
        }
    }
    Run& r = g_run;
    if (r.phase == Phase::Idle)
        return;
    ULONGLONG now = GetTickCount64();
    if (now < r.nextTime)
        return;
    void* cl = ClientFromApp(app);
    if (!cl || !At<void*>(cl, kClientWorld)) {
        Log("deposit: left the world, stopped");
        r.phase = Phase::Idle;
        g_report = {"Stopped: left the world."};
        ++g_runsFinished;
        return;
    }
    switch (r.phase) {
    case Phase::Tier1:
        if (r.chestCount) {
            StepTier1(cl, now);
        } else {
            r.phase = Phase::Settle1;
        }
        break;
    case Phase::Settle1:
        r.moveCount = 0;
        if (g_cfg.tier2 != Tier2Mode::Off && r.backpackBefore.pageCount && r.chestsVisible)
            PlanTier2();
        if (g_cfg.tier2 == Tier2Mode::On && r.moveCount) {
            r.phase = Phase::Tier2;
            r.next = 0;
        } else {
            r.phase = Phase::Settle2; // report right away
        }
        break;
    case Phase::Tier2:
        StepTier2(cl, now);
        break;
    case Phase::Settle2:
        WriteReport();
        r.phase = Phase::Idle;
        ++g_runsFinished;
        break;
    default:
        r.phase = Phase::Idle;
    }
}

void SafeTick(void* app)
{
    __try {
        Tick(app);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_broken = true;
        g_run.phase = Phase::Idle;
        Log("crash avoided (exception 0x%08X) - deposit disabled until restart", GetExceptionCode());
    }
}

// G in the inventory is "Sort Inventory" (UI command 0x2d, which becomes a
// SortInventoryEvent). Two of them within doubleSortMs start a deposit; both still sort, so G
// keeps working as usual. Sorting only exists while an inventory is open.
constexpr uint8_t kCmdSortInventory = 0x2d;
Config g_baseCfg;
ULONGLONG g_lastSort = 0;

void UiDispatchHook(void* cl, uint8_t* cmd)
{
    if (cmd && cmd[0] == kCmdSortInventory && g_baseCfg.doubleSortMs > 0) {
        ULONGLONG now = GetTickCount64();
        if (g_lastSort && now - g_lastSort <= static_cast<ULONGLONG>(g_baseCfg.doubleSortMs)) {
            g_lastSort = 0;
            g_pending = g_baseCfg;
            g_requested = true;
            Log("deposit requested (G twice)");
        } else {
            g_lastSort = now;
        }
    }
    g_originalDispatch(cl, cmd);
}

#ifdef QD_BRIDGE
// Bridge ops read game memory too; a fault fails that op, not the mod.
void SafePump()
{
    __try {
        bridge::Pump();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        bridge::AbortCurrent("the op faulted while reading game memory");
        Log("bridge: op faulted (exception 0x%08X)", GetExceptionCode());
    }
}
#endif

void UiCommandLoopHook(void* app)
{
    g_client = ClientFromApp(app);
#ifdef QD_BRIDGE
    SafePump();
#endif
    if (!g_broken)
        SafeTick(app);
    g_originalLoop(app);
}

template <class T> bool Find(T& out, const char* name, const char* pattern)
{
    out = reinterpret_cast<T>(FindUnique(pattern));
    if (!out)
        Log("signature not found: %s", name);
    return out != nullptr;
}

} // namespace

bool Init()
{
    uint8_t* loop;
    uint8_t* dispatch;
    bool ok = true;
    ok &= Find(loop, "UiCommandLoop", "40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 98 00 00 00 48 8D 81 58 4F 03 00");
    ok &= Find(dispatch, "UiDispatch", "4C 8B DC 55 56 57 49 8D AB 18 FF FF FF");
    if (!ok)
        return false;
    if (!inv::Init())
        Log("inventory reading unavailable - only Deposit Stacks will run");
    if (!chat::Init())
        Log("chat hook unavailable");
    g_originalDispatch = static_cast<UiDispatchFn>(InstallHook(dispatch, reinterpret_cast<void*>(&UiDispatchHook), 13));
    g_originalLoop = static_cast<UiCommandLoopFn>(InstallHook(loop, reinterpret_cast<void*>(&UiCommandLoopHook), 13));
    if (!g_originalLoop || !g_originalDispatch) {
        Log("hook install failed");
        return false;
    }
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    Log("hooked client UI loop at +0x%llX", static_cast<unsigned long long>(loop - base));
    return true;
}

void SetConfig(const Config& cfg)
{
    g_baseCfg = cfg;
}

const Config& CurrentConfig()
{
    return g_baseCfg;
}

void* CurrentClient()
{
    return g_client;
}

bool DescribeBase(BaseInfo& out, std::string& why)
{
    BaseScan scan;
    bool ok = ScanBase(g_client, g_baseCfg, scan, why);
    out.inWorld = g_client && At<void*>(g_client, kClientWorld);
    out.playerId = scan.playerId;
    memcpy(out.pos, scan.pos, sizeof(out.pos));
    out.inBase = scan.base != nullptr;
    if (scan.base) {
        memcpy(out.lo, &At<float>(scan.base, kBaseMin), sizeof(out.lo));
        memcpy(out.hi, &At<float>(scan.base, kBaseMax), sizeof(out.hi));
    }
    out.readable = scan.readable;
    out.magicCount = scan.magicCount;
    out.chests = std::move(scan.chests);
    out.excluded = std::move(scan.excluded);
    return ok;
}

bool StartDeposit(const Config& cfg, std::string& why)
{
    if (g_broken) {
        why = "deposit disabled after an earlier fault (restart the game)";
        return false;
    }
    if (g_run.phase != Phase::Idle) {
        why = "a deposit is already running";
        return false;
    }
    g_cfg = cfg;
    return Start(g_client, why);
}

bool DepositBusy()
{
    return g_run.phase != Phase::Idle;
}

uint64_t DepositRunsFinished()
{
    return g_runsFinished;
}

const std::vector<std::string>& LastReport()
{
    return g_report;
}

std::string ChestLabelFor(uint32_t chest)
{
    inv::Owner o;
    char label[160];
    if (!inv::ReadOwner(chest, o))
        o.pageCount = 0;
    o.entity = chest;
    ChestLabel(0, o, label, sizeof(label));
    return label;
}

void RequestDeposit(const Config& cfg)
{
    g_pending = cfg;
    g_requested = true;
    Log("deposit requested");
}

} // namespace client

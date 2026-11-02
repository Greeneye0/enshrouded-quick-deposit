// Deposits through the client, the way the chest window does it, so the host (this PC, a
// friend's game or a dedicated server) validates and performs every transfer.
//
// F7 runs as a small state machine on the client's per-frame UI loop, so nothing ever blocks
// a frame:
//   1. snapshot your backpack and the base's chests
//   2. tier 2 moves for the stacks no chest holds (each into a free slot of the
//      chest holding the most different items of the same kind...), then tier 1, "Deposit Stacks", into
//      the chests that hold something you carry
//   3. wait until the backpack stops changing, then plan again for what is left (stacks that
//      did not fit in tier 1, anything that arrived meanwhile) and send that
//   4. write a report of what went into which chest
//
// 140845600 UiCommandLoop(app): runs every frame; client = *(*(app + 0x352a0) + 0x1b8).
// 140851530 UiDispatch(client, cmd): command 0x2c becomes a TransferItemToInventoryEvent,
//           which the client sends to the host as an InventoryTransferAction.
// Layouts confirmed in game (build 1269579):
//   client + 0x47d4e0 -> client world; client + 0x466b44 local player handle,
//   player id = (handle & 0x3f) + 1; world + 0x2c18 local player position (int64 x, y, z);
//   world + 0x2c68 base permission object; world + 0x3ec8 -> base registry.
//   "Deposit Stacks": cmd 0x2c, flag index 1 (RefillStacks) on, target = chest, source = player.
//   Move one stack:  cmd 0x2c, no flag, target slot = (page entity, index) at +0x10,
//                    source slot = (page entity, index) at +0x1c.
//   Both use type 2 at +0x24.
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>
#ifdef QD_BRIDGE
#include "bridge.h"
#include "../dev/altar_upgrade.h"
#endif
#include "character.h"
#include "chat.h"
#include "../../common/greeneye.h"
#include "client.h"
#include "inventory.h"
#include "glow.h"
#include "host.h"
#include "keep.h"
#include "lang.h"
#include "match.h"
#include "log.h"
#include "scan.h"

namespace client {
namespace {

using namespace match;

using UiCommandLoopFn = void (*)(void* app);
using UiDispatchFn = void (*)(void* client, uint8_t* cmd);

UiCommandLoopFn g_originalLoop = nullptr;
UiDispatchFn g_originalDispatch = nullptr;
std::atomic<bool> g_requested{false};
std::atomic<bool> g_requestSorted{false}; // the request came with G twice, which sorts the backpack
Config g_pending;
Config g_cfg;
bool g_broken = false;

constexpr size_t kAppUi = 0x352a0;
constexpr size_t kUiClient = 0x1b8;
constexpr size_t kClientWorld = 0x47d4e0;
constexpr size_t kClientPlayer = 0x466b44;
size_t g_textChatAllowed = 0; // client flag: the server allows text chat (0: not found)
constexpr size_t kWorldPlayerPos = 0x2c18;
constexpr size_t kWorldPermissions = 0x2c68;
constexpr size_t kWorldBases = 0x3ec8;

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
constexpr uint8_t kTypeSplit = 3;       // move part of a stack: u16 count at +0x26 (the game's split)
constexpr size_t kCmdSplitCount = 0x26;

constexpr float kFixedToMeters = 1.0f / 4294967296.0f;
constexpr int kMaxChests = 256;
constexpr int kMaxContainers = 2048; // a base's containers: animals, NPCs and crafting stations too
constexpr int kMaxMoves = 128;
// Tier 2 runs again after its moves settle while it still finds stacks to move: a snapshot
// can miss a stack that arrived or changed during the deposit (it took a second G G before).
constexpr int kMaxPasses = 6; // the first pass and its retries

template <class T> T& At(void* base, size_t offset)
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

// --- state --------------------------------------------------------------------------------

enum class Phase { Idle, Whole, Tier1, Tier2, Settle2 };

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
    uint32_t srcCount, srcFull; // the source stack as this move expects it, and before any move
    bool part; // only count of the stack (the rest is kept)
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
    int pass = 0;
    int passFirst = 0; // first move of the current pass
    int tier1[kMaxChests]; // chests that get "Deposit Stacks" (indices into chests)
    int tier1Count = 0;
    bool tier1Pending = false;
    bool sentThisPass = false;
    bool retryWaited = false; // the pause before the next retry pass is over
    ULONGLONG wholeAt = 0;    // when the whole-base deposit was asked for (0: answered)
    // Settling: done once the backpack changed and then stayed the same for a moment.
    ULONGLONG settleStart = 0, lastChange = 0;
    uint64_t lastSig = 0;
    bool changed = false;
    Move moves[kMaxMoves];
    inv::Owner backpackBefore;
    inv::Owner backpackMid;
    inv::Owner chestsBefore[kMaxChests];
    inv::Owner chestsMid[kMaxChests];
    bool chestsVisible = false;
    char notes[4096];
    int notesLen = 0;
    // Stacks that stay because every chest for them is full (the last pass's say, as notes).
    uint32_t noRoomIds[64];
    uint64_t noRoomCounts[64];
    int noRoomCount = 0;
} g_run;

// A stack stays for want of room: added up per item, for the report's chat line.
void NoRoom(uint32_t itemId, uint64_t count)
{
    Run& r = g_run;
    for (int i = 0; i < r.noRoomCount; ++i)
        if (r.noRoomIds[i] == itemId) {
            r.noRoomCounts[i] += count;
            return;
        }
    if (r.noRoomCount < 64) {
        r.noRoomIds[r.noRoomCount] = itemId;
        r.noRoomCounts[r.noRoomCount++] = count;
    }
}

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
// chat: in the mod's language (lang.h); else English, for the report and the log.
void ChestLabel(int index, const inv::Owner& o, char* out, size_t size, bool chat = false)
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
        _snprintf_s(out, size, _TRUNCATE, chat ? lang::T(lang::Str::EmptyChest) : "empty chest %d", index + 1);
    else
        _snprintf_s(out, size, _TRUNCATE, chat ? lang::T(lang::Str::ChestOf) : "%s chest", inv::Describe(ids[best]).shown);
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
    int notChests = 0;
    for (uint64_t i = 0; ids && i < n && out.chests.size() < kMaxChests; ++i) {
        if (!ids[i])
            continue;
        // The registry is known once a chest of the list was read; before, take the list as it is.
        if (inv::Ready(out.playerId, ids[i]) && !inv::IsStorageProp(ids[i])) {
            ++notChests;
            continue;
        }
        out.chests.push_back(ids[i]);
    }
    out.magicCount = static_cast<int>(out.chests.size());
    static int lastNotChests = -1;
    if (notChests != lastNotChests)
        Log("containers: %d entries of the base's chest list are no storage chests, skipped", notChests);
    lastNotChests = notChests;

    if (!out.magicCount) {
        // A small flame altar lists no crafting-storage chests: find the plain chests themselves.
        uint32_t found[kMaxChests];
        int nf = inv::FindPlainChests(out.playerId, &At<float>(out.base, kBaseMin), &At<float>(out.base, kBaseMax), found, kMaxChests);
        out.chests.assign(found, found + nf);
        out.readable = nf > 0 && inv::Ready(out.playerId, out.chests[0]);
    } else {
        out.readable = inv::Ready(out.playerId, out.chests[0]);
    }
    if (out.readable && out.magicCount) {
        static thread_local uint32_t found[kMaxContainers];
        int nf = inv::FindContainers(&At<float>(out.base, kBaseMin), &At<float>(out.base, kBaseMax), found, kMaxContainers);
        nf = inv::KeepStorageChests(out.chests.data(), out.magicCount, found, nf);
        int props = 0;
        for (int i = 0; i < nf; ++i)
            if (inv::IsStorageProp(found[i]))
                found[props++] = found[i];
        nf = props;
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

void PlanTier2(bool onlyNew);

// Whether chest holds any item in the backpack's storage rows (so "Deposit Stacks" can act).
bool SharesItem(const inv::Owner& backpack, const inv::Owner& chest)
{
    for (int p = 0; p < backpack.pageCount; ++p) {
        const inv::Page& page = backpack.pages[p];
        if (!IsStoragePage(page) || inv::IsHotbarPage(backpack, page.entity))
            continue;
        for (const auto& st : page.slots)
            if (st.itemId && CountOf(chest, st.itemId))
                return true;
    }
    return false;
}

bool AnyChestHolds(uint32_t itemId)
{
    for (int c = 0; c < g_run.chestCount; ++c)
        if (CountOf(g_run.chestsMid[c], itemId))
            return true;
    return false;
}

// Whether a chest that gets "Deposit Stacks" holds the item (so tier 1 takes it).
bool AnyTier1ChestHolds(uint32_t itemId)
{
    for (int i = 0; i < g_run.tier1Count; ++i)
        if (CountOf(g_run.chestsMid[g_run.tier1[i]], itemId))
            return true;
    return false;
}

// Whether the chest holds an item type you keep that is in your backpack's storage rows:
// "Deposit Stacks" would take it, so that chest gets single moves instead.
bool HoldsKeptItem(const inv::Owner& backpack, const inv::Owner& chest)
{
    for (int p = 0; p < backpack.pageCount; ++p) {
        const inv::Page& page = backpack.pages[p];
        if (!IsStoragePage(page) || inv::IsHotbarPage(backpack, page.entity))
            continue;
        for (const auto& st : page.slots)
            if (keep::IsKept(st.itemId) && CountOf(chest, st.itemId))
                return true;
    }
    return false;
}

// What the keep marks hold in the backpack's storage rows, as counts per item.
int KeptTotals(const inv::Owner& backpack, uint32_t* ids, uint64_t* counts, int max)
{
    static keep::Plan plan;
    keep::PlanFor(backpack, plan);
    int n = 0;
    for (int p = 0; p < backpack.pageCount; ++p) {
        for (int s = 0; s < inv::kSlotsPerPage; ++s) {
            const inv::Stack& st = backpack.pages[p].slots[s];
            if (!plan.keep[p][s])
                continue;
            int k = 0;
            while (k < n && ids[k] != st.itemId)
                ++k;
            if (k == n) {
                if (n == max)
                    continue;
                ids[n] = st.itemId;
                counts[n++] = 0;
            }
            counts[k] += plan.keep[p][s];
        }
    }
    return n;
}

// A fingerprint of the backpack, to see when the host's changes have arrived.
uint64_t BackpackSig(uint32_t playerId)
{
    static inv::Owner o;
    if (!inv::ReadOwner(playerId, o))
        return 0;
    uint64_t h = 1469598103934665603ull;
    for (int p = 0; p < o.pageCount; ++p)
        for (const auto& st : o.pages[p].slots)
            h = (h ^ (static_cast<uint64_t>(st.itemId) << 20 ^ st.count)) * 1099511628211ull;
    return h;
}

void BeginSettle(ULONGLONG now)
{
    Run& r = g_run;
    r.phase = Phase::Settle2;
    r.settleStart = now;
    r.nextTime = now + 50;
}

// Whether every move of this pass has left its source slot: the host's answers arrive over
// several frames, and a pass planned before they all did would send the same moves again.
bool PassArrived()
{
    Run& r = g_run;
    static inv::Owner now;
    if (r.moveCount == r.passFirst || !inv::ReadOwner(r.playerId, now))
        return true;
    for (int i = r.passFirst; i < r.moveCount; ++i) {
        const Move& m = r.moves[i];
        const inv::Page& was = r.backpackMid.pages[m.srcPage];
        const inv::Stack& before = was.slots[m.srcSlot];
        for (int p = 0; p < now.pageCount; ++p) {
            const inv::Stack& st = now.pages[p].slots[m.srcSlot];
            if (now.pages[p].entity == was.entity && st.itemId == before.itemId && st.count == before.count)
                return false;
        }
    }
    return true;
}

// Settled once the backpack changed, every move of the pass arrived and then nothing changed
// for kQuietMs (settleMs at most: a move the host refused never arrives).
constexpr ULONGLONG kQuietMs = 300;
bool Settled(ULONGLONG now)
{
    Run& r = g_run;
    if (!r.sentThisPass)
        return true;
    uint64_t sig = BackpackSig(r.playerId);
    if (sig != r.lastSig) {
        r.lastSig = sig;
        r.lastChange = now;
        r.changed = true;
    }
    if (now - r.settleStart >= static_cast<ULONGLONG>(g_cfg.settleMs))
        return true;
    return r.changed && now - r.lastChange >= kQuietMs && PassArrived();
}

void BeginPasses(const inv::Owner& backpack, const inv::Owner* chests);

// Plans the next pass for what is still in your backpack; false when nothing is left to try.
bool StartRetry()
{
    Run& r = g_run;
    const int before = r.moveCount;
    const bool progressed = r.changed;
    r.lastSig = BackpackSig(r.playerId);
    r.changed = false;
    if (g_cfg.tier2 == Tier2Mode::On) {
        if (r.moveCount >= kMaxMoves)
            return false;
        PlanTier2(false);
    } else {
        // Without tier 2: "Deposit Stacks" again to the chests that still share an item with
        // you, as long as the last pass moved something (a full chest would take nothing).
        ++r.pass;
        if (!progressed || !inv::ReadOwner(r.playerId, r.backpackMid))
            return false;
        r.tier1Count = 0;
        for (int c = 0; c < r.chestCount; ++c) {
            if (!inv::ReadOwner(r.chests[c], r.chestsMid[c]))
                continue;
            if (SharesItem(r.backpackMid, r.chestsMid[c]) && !HoldsKeptItem(r.backpackMid, r.chestsMid[c]))
                r.tier1[r.tier1Count++] = c;
        }
        r.tier1Pending = r.tier1Count > 0;
    }
    r.sentThisPass = r.moveCount > before || r.tier1Pending;
    if (!r.sentThisPass)
        return false;
    Log("deposit: retry pass %d", r.pass);
    r.next = r.moveCount > before ? before : 0;
    r.phase = r.moveCount > before ? Phase::Tier2 : Phase::Tier1;
    return true;
}

bool Start(void* cl, std::string& why)
{
    // Whose marks count is not known yet (keep.h Waiting): a deposit now could take them.
    if (keep::Waiting()) {
        why = "the character you play is not known yet";
        Log("deposit: %s - nothing sent", why.c_str());
        char line[512];
        _snprintf_s(line, _TRUNCATE, "Quick Deposit: %s", lang::T(lang::Str::WaitingForCharacter));
        chat::Post(line);
        return false;
    }
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
    r.noRoomCount = 0;
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
    // Kept items need the contents: "Deposit Stacks" into every chest would take them along.
    if (keep::Any() && !r.chestsVisible) {
        why = "inventories not readable yet, and you keep marked items";
        Log("deposit: %s - nothing sent", why.c_str());
        char line[512];
        _snprintf_s(line, _TRUNCATE, "Quick Deposit: %s", lang::T(lang::Str::ChestsUnreadable));
        chat::Post(line);
        return false;
    }
    // Every page of the player: its InventoryCategory, filled slots, and whether it counts as
    // storage. Items on pages that are not storage are never deposited.
    {
        char pages[512];
        int len = 0;
        for (int p = 0; p < r.backpackBefore.pageCount && len < static_cast<int>(sizeof(pages)) - 32; ++p) {
            const inv::Page& page = r.backpackBefore.pages[p];
            int filled = 0;
            for (const inv::Stack& s : page.slots)
                filled += s.itemId ? 1 : 0;
            const char* role = inv::IsHotbarPage(r.backpackBefore, page.entity) ? "hotbar"
                               : IsStoragePage(page)                              ? "storage"
                                                                                  : "kept";
            len += snprintf(pages + len, sizeof(pages) - len, " %u:kind%u:%d/%d:%s", page.entity,
                            static_cast<unsigned>(page.kind), filled, inv::kSlotsPerPage, role);
        }
        Log("deposit: your %d pages (entity:kind:filled:role):%s", r.backpackBefore.pageCount, pages);
    }

    // With this game hosting the world: first everything that goes onto stacks the chests already
    // hold, at once, the way a Quick Stack Station deposits (host.h); then the rest as usual.
    if (r.chestsVisible && host::RequestWholeBase(playerId, r.chests, r.chestCount)) {
        Log("deposit: whole-base deposit asked of this game's host part");
        r.wholeAt = GetTickCount64();
        r.phase = Phase::Whole;
        r.nextTime = 0;
        return true;
    }
    BeginPasses(r.backpackBefore, r.chestsBefore);
    return true;
}

// Tier 1 and the first tier 2 pass, planned from these inventories (as they were at the start,
// or after the whole-base deposit).
void BeginPasses(const inv::Owner& backpack, const inv::Owner* chests)
{
    Run& r = g_run;
    // Tier 1 only where it can do something: chests holding an item you carry (all of them
    // when the contents can't be read).
    // Chests holding a kept item you carry are left out: their stacks go there one by one.
    r.tier1Count = 0;
    for (int c = 0; c < r.chestCount; ++c) {
        if (r.chestsVisible && !SharesItem(backpack, chests[c]))
            continue;
        if (r.chestsVisible && HoldsKeptItem(backpack, chests[c])) {
            Log("deposit: chest %u holds a kept item - single moves instead of Deposit Stacks", r.chests[c]);
            continue;
        }
        r.tier1[r.tier1Count++] = c;
    }
    r.pass = 0;
    r.retryWaited = false;
    if (g_cfg.tier2 == Tier2Mode::On && backpack.pageCount && r.chestsVisible) {
        // The moves go out first: a chest they put a kept item into holds it by the time
        // "Deposit Stacks" gets there, which would take the kept stacks along. Such a chest is
        // left out of tier 1 too, and the moves are planned again (its other stacks one by one).
        for (int round = 0; round < 4; ++round) {
            r.moveCount = 0;
            r.pass = 0;
            PlanTier2(true);
            int left = 0;
            for (int i = 0; i < r.tier1Count; ++i) {
                bool getsKept = false;
                for (int m = 0; m < r.moveCount && !getsKept; ++m)
                    getsKept = r.moves[m].chest == r.tier1[i] && keep::IsKept(r.moves[m].itemId);
                if (getsKept)
                    Log("deposit: chest %u gets a kept item - single moves instead of Deposit Stacks",
                        r.chests[r.tier1[i]]);
                else
                    r.tier1[left++] = r.tier1[i];
            }
            if (left == r.tier1Count)
                break;
            r.tier1Count = left;
        }
    }
    r.tier1Pending = r.tier1Count > 0;
    r.sentThisPass = r.tier1Pending || r.moveCount > 0;
    r.lastSig = BackpackSig(r.playerId);
    r.changed = false;
    r.settleStart = r.lastChange = GetTickCount64();
    r.next = 0;
    r.phase = r.moveCount ? Phase::Tier2 : r.tier1Pending ? Phase::Tier1 : Phase::Settle2;
    r.nextTime = 0;
}

// "Deposit Stacks" to one tier 1 chest per delayMs: the host takes one transfer per network
// tick and drops the rest of a burst.
void StepTier1(void* cl, ULONGLONG now)
{
    Run& r = g_run;
    // Checked again right before: "Deposit Stacks" into a chest that holds a kept item you carry
    // would take the kept stacks along.
    inv::Owner chest, backpack;
    const uint32_t target = r.chests[r.tier1[r.next]];
    if (r.chestsVisible && inv::ReadOwner(target, chest) && inv::ReadOwner(r.playerId, backpack) &&
        HoldsKeptItem(backpack, chest)) {
        Log("deposit: chest %u holds a kept item now - no Deposit Stacks there", target);
        if (++r.next == r.tier1Count) {
            Log("deposit: tier 1 sent to %d of %d chests", r.tier1Count, r.chestCount);
            r.tier1Pending = false;
            BeginSettle(now);
        }
        return;
    }
    alignas(8) uint8_t cmd[kCmdSize] = {};
    cmd[0] = kCmdTransfer;
    At<uint32_t>(cmd, kCmdFlagIndex) = kFlagRefillStacks;
    cmd[kCmdFlagOn] = 1;
    At<uint32_t>(cmd, kCmdTarget) = r.chests[r.tier1[r.next]];
    At<uint32_t>(cmd, kCmdSource) = r.playerId;
    cmd[kCmdType] = kTypeTransfer;
    Send(cl, cmd);
    r.nextTime = now + g_cfg.delayMs;
    if (++r.next == r.tier1Count) {
        Log("deposit: tier 1 sent to %d of %d chests", r.tier1Count, r.chestCount);
        r.tier1Pending = false;
        BeginSettle(now);
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

// Whether text holds part, in any language: ignoring case and accents, and without needing
// spaces (Chinese, Japanese, Korean, Thai). Both UTF-8.
bool HoldsText(const char* text, const char* part)
{
    if (!part || !*part || !text || !*text)
        return false;
    wchar_t t[160], p[160];
    int tl = MultiByteToWideChar(CP_UTF8, 0, text, -1, t, 160);
    int pl = MultiByteToWideChar(CP_UTF8, 0, part, -1, p, 160);
    if (tl <= 1 || pl <= 1)
        return false;
    return FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC,
                           t, tl - 1, p, pl - 1, nullptr, nullptr, nullptr, 0) >= 0;
}

// How a chest's name fits the item, from the game's own text in your language: the item's whole
// name (1000: "Adventurer Helmet"), then for equipment its type ("Head Armor", 500; a chest named
// for another type, "Foot Armor", never fits), then "Equippable" (100). Below that the words (see
// WordScore).
int NameScore(const char* chestName, const inv::ItemDesc& d)
{
    if (d.shown[0] && strcmp(d.shown, d.name) != 0 && HoldsText(chestName, d.shown))
        return 1000;
    if (HoldsText(chestName, d.name))
        return 1000;
    if (d.category == kCategoryEquipment && d.equipSlot) {
        for (uint8_t other : inv::TypeSlots())
            if (other != d.equipSlot)
                for (const std::string& name : inv::TypeNames(other))
                    if (HoldsText(chestName, name.c_str()))
                        return 0;
        for (const std::string& name : inv::TypeNames(d.equipSlot))
            if (HoldsText(chestName, name.c_str()))
                return 500;
        for (const std::string& name : inv::TypeNames(0))
            if (HoldsText(chestName, name.c_str()))
                return 100;
    }
    return WordScore(chestName, d);
}

// onlyNew: plan only the stacks no chest holds (tier 1 takes care of the others).
void PlanTier2(bool onlyNew)
{
    Run& r = g_run;
    // Each pass appends to the moves of the earlier ones, so the report lists them all.
    const int first = r.moveCount;
    r.next = first;
    r.passFirst = first;
    ++r.pass;
    // What stays in the backpack is this pass's to say (an earlier pass listed the same stacks).
    r.notesLen = 0;
    r.notes[0] = 0;
    r.noRoomCount = 0;
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

    static keep::Plan kept;
    keep::PlanFor(r.backpackMid, kept);
    int seen = 0;
    for (int p = 0; p < r.backpackMid.pageCount; ++p) {
        const inv::Page& page = r.backpackMid.pages[p];
        if (!IsStoragePage(page) || inv::IsHotbarPage(r.backpackMid, page.entity))
            continue; // equipment, currency and hotbar stay where they are
        for (int s = 0; s < inv::kSlotsPerPage; ++s) {
            const inv::Stack st = page.slots[s];
            if (!st.itemId)
                continue;
            ++seen;
            const uint32_t keepCount = kept.keep[p][s];
            if (keepCount >= st.count)
                continue; // you keep it
            inv::ItemDesc d = inv::Describe(st.itemId);
            if (!d.known)
                Log("deposit: pass %d: item %u has no item info, left in the backpack", r.pass, st.itemId);
            // Stacks whose chest was left out of tier 1 (it holds a kept item) move here
            // whatever tier2Categories says: Deposit Stacks would have taken them.
            bool tier1Home = AnyTier1ChestHolds(st.itemId);
            bool homeSkipped = !tier1Home && AnyChestHolds(st.itemId);
            if (!d.known || (!homeSkipped && !(g_cfg.tier2Categories & (1u << d.category))))
                continue; // stays with you by design (category not enabled)
            if (onlyNew && tier1Home)
                continue;
            if (g_cfg.newItemsStay && !AnyChestHolds(st.itemId)) {
                Note("  %s: no chest holds it yet (newItems=stay)", d.shown);
                continue;
            }

            // Among chests with a free slot, by default (tier2Order): 1. one holding this exact
            // item (the best of them), 2. the one holding the most items of the same kind (weapon
            // or ammo type), 3. the one whose name matches the item best, 4. for weapons the one
            // holding the most weapons, 5. the nearest.
            int sameItem = -1, kindChest = -1, catChest = -1, nameChest = -1, nearChest = -1;
            int bestCat = 0, bestName = 0, sameItemKind = -1;
            uint64_t sameItemCount = 0;
            KindFit bestFit;
            bool fullFits = false; // a chest for it (same item, kind or name) has no free slot
            for (int c = 0; c < r.chestCount; ++c) {
                const inv::Owner& ch = r.chestsMid[c];
                int pg, sl;
                const bool room = FreeSlot(ch, pg, sl);
                bool holdsItem = CountOf(ch, st.itemId) > 0;
                // The different items the chest holds, for the kind; and how many of its stacks
                // are of the item's category, for weapons.
                static inv::ItemDesc held[64];
                uint32_t heldIds[64];
                int heldCount = 0, cat = 0;
                for (int cp = 0; cp < ch.pageCount; ++cp) {
                    if (!IsStoragePage(ch.pages[cp]))
                        continue;
                    for (const auto& cs : ch.pages[cp].slots) {
                        if (!cs.itemId)
                            continue;
                        inv::ItemDesc cd = inv::Describe(cs.itemId);
                        cat += cd.known && cd.category == d.category ? 1 : 0;
                        if (heldCount < 64 && std::find(heldIds, heldIds + heldCount, cs.itemId) == heldIds + heldCount) {
                            heldIds[heldCount] = cs.itemId;
                            held[heldCount++] = cd;
                        }
                    }
                }
                int score = names[c][0] ? NameScore(names[c], d) : 0;
                const KindFit fit = FitByKind(d, held, heldCount, score);
                const int kind = fit.kind;
                if (!room) {
                    fullFits |= holdsItem || fit.Any() || score > 0;
                    continue;
                }
                if (fit.Any() && fit > bestFit) {
                    bestFit = fit;
                    kindChest = c;
                }
                // Several chests may hold the item (someone's own chest, a stray stack): the one
                // holding the most of the item itself is its real home (1600 Flax in the seed
                // chest beat 28 that strayed into the materials chest), then the one holding the
                // most of its kind, then the nearer one.
                uint64_t have = holdsItem ? CountOf(ch, st.itemId) : 0;
                if (holdsItem && (have > sameItemCount ||
                                  (have == sameItemCount && (kind > sameItemKind ||
                                                             (kind == sameItemKind && dist[c] < dist[sameItem]))))) {
                    sameItemCount = have;
                    sameItemKind = kind;
                    sameItem = c;
                }
                if (cat > bestCat) {
                    bestCat = cat;
                    catChest = c;
                }
                if (score > bestName) {
                    bestName = score;
                    nameChest = c;
                }
                if (nearChest < 0 || dist[c] < dist[nearChest])
                    nearChest = c;
            }
            int chest = -1;
            Reason reason = Reason::Nearest;
            bool gearStays = false;
            for (int i = 0; i < g_cfg.tier2RuleCount && chest < 0; ++i) {
                switch (g_cfg.tier2Order[i]) {
                case Tier2Rule::SameItem: chest = sameItem; reason = Reason::SameItem; break;
                case Tier2Rule::SameKind: chest = kindChest; reason = Reason::SameKind; break;
                case Tier2Rule::ChestName: chest = nameChest; reason = Reason::ChestName; break;
                case Tier2Rule::Weapons:
                    chest = d.category == kCategoryWeapons ? catChest : -1;
                    reason = Reason::SameCategory;
                    break;
                case Tier2Rule::Nearest:
                    // Gear no chest holds or is named for stays: the nearest chest is no place
                    // for a helmet.
                    gearStays = d.category == kCategoryEquipment && d.equipSlot;
                    chest = gearStays ? -1 : nearChest;
                    reason = Reason::Nearest;
                    break;
                }
            }
            if (chest < 0) {
                if (nearChest < 0 || fullFits) {
                    Note("  %s x%u: %s", d.shown, st.count - keepCount,
                         nearChest < 0 ? "every chest is full" : "its chests are full");
                    NoRoom(st.itemId, st.count - keepCount);
                } else if (gearStays)
                    Note("  %s: no chest for this kind of gear yet", d.shown);
                else
                    Note("  %s: no chest by tier2Order", d.shown);
                continue;
            }
            inv::Owner& ch = r.chestsMid[chest];
            uint32_t srcLeft = st.count;
            auto add = [&](int dstPage, int dstSlot, uint32_t count, bool part) {
                Move& m = r.moves[r.moveCount++];
                m.srcCount = srcLeft;
                m.srcFull = st.count;
                srcLeft -= count;
                m.srcPage = p;
                m.srcSlot = s;
                m.chest = chest;
                m.dstPage = dstPage;
                m.dstSlot = dstSlot;
                m.itemId = st.itemId;
                m.count = count;
                m.part = part;
                m.reason = reason;
            };
            // The chest's unfilled stacks of the item first, as Deposit Stacks would (a chest
            // holding a kept item gets no Deposit Stacks), so no new half stack starts beside them.
            uint32_t left = st.count - keepCount;
            for (int cp = 0; cp < ch.pageCount && left && d.maxStack > 1; ++cp) {
                if (!IsStoragePage(ch.pages[cp]))
                    continue;
                for (int cs = 0; cs < inv::kSlotsPerPage && left && r.moveCount < kMaxMoves; ++cs) {
                    inv::Stack& there = ch.pages[cp].slots[cs];
                    if (there.itemId != st.itemId || there.count >= d.maxStack)
                        continue;
                    uint32_t put = std::min<uint32_t>(left, d.maxStack - there.count);
                    add(cp, cs, put, true);
                    there.count += put; // for the next stacks of the item
                    left -= put;
                }
            }
            if (left && r.moveCount < kMaxMoves) {
                int pg, sl;
                FreeSlot(ch, pg, sl);
                ch.pages[pg].slots[sl] = st; // reserve the slot for the next stacks
                ch.pages[pg].slots[sl].count = left;
                add(pg, sl, left, keepCount > 0);
            }
        }
    }
    Log("deposit: tier 2 pass %d: %d stacks in the backpack, %d to move", r.pass, seen, r.moveCount - first);
}

// One move per delayMs, then tier 1 (if due).
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
    if (m.part) {
        cmd[kCmdType] = kTypeSplit;
        At<uint16_t>(cmd, kCmdSplitCount) = static_cast<uint16_t>(m.count > 0xffff ? 0xffff : m.count);
    }
    // The source slot must still hold the stack the move was planned for: in a backpack sorted
    // since, it holds another stack, maybe a kept one. (Up to the whole stack: an earlier part
    // of it may not have left yet.) The next pass plans again from what is there.
    static inv::Owner me;
    const inv::Stack* src = nullptr;
    if (inv::ReadOwner(r.playerId, me))
        for (int p = 0; p < me.pageCount && !src; ++p)
            if (me.pages[p].entity == r.backpackMid.pages[m.srcPage].entity)
                src = &me.pages[p].slots[m.srcSlot];
    if (src && src->itemId == m.itemId && src->count >= m.srcCount && src->count <= m.srcFull)
        Send(cl, cmd);
    else
        Log("deposit: move skipped - page %u slot %d holds item %u x%u now, not %u x%u",
            r.backpackMid.pages[m.srcPage].entity, m.srcSlot, src ? src->itemId : 0, src ? src->count : 0,
            m.itemId, m.srcCount);
    r.nextTime = now + g_cfg.delayMs;
    if (++r.next == r.moveCount) {
        Log("deposit: tier 2 sent %d moves", r.moveCount - r.passFirst);
        if (r.tier1Pending) {
            r.phase = Phase::Tier1;
            r.next = 0;
        } else {
            BeginSettle(now);
        }
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
    rep.f = fopen(greeneye::DataPath("quick-deposit-report.txt"), "w");
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
    uint64_t storedItems = 0; // for chat=short
    int storedChests = -1;    // -1: unknown (chest contents not readable)
    if (r.chestsVisible) {
        // Per chest: what it gained from you, from its contents before and after.
        uint32_t filled[kMaxChests];
        int filledCount = 0;
        for (int c = 0; c < r.chestCount; ++c) {
            inv::Owner now;
            if (!inv::ReadOwner(r.chests[c], now))
                continue;
            uint32_t ids[64];
            uint64_t counts[64];
            int n = Totals(now, ids, counts, 64);
            char label[160], chatLabel[160];
            ChestLabel(c, r.chestsBefore[c], label, sizeof(label));
            ChestLabel(c, r.chestsBefore[c], chatLabel, sizeof(chatLabel), true);
            bool header = false;
            char chatLine[320];
            int chatLen = _snprintf_s(chatLine, _TRUNCATE, "%s:", chatLabel);
            const int chatStart = chatLen;
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
                const char* name = item.shown;
                unsigned long long added = mine;
                storedItems += added;
                rep.Line("  + %s x%llu", name, added);
                if (chatLen > 0 && chatLen < 240) {
                    int w = _snprintf_s(chatLine + chatLen, sizeof(chatLine) - chatLen, _TRUNCATE, "%s %s x%llu",
                                        chatLen > chatStart ? "," : "", name, added);
                    chatLen = w < 0 ? 240 : chatLen + w;
                }
                ++lines;
            }
            if (header) {
                chat::Post(chatLine, chat::Kind::Detail);
                filled[filledCount++] = r.chests[c];
            }
        }
        storedChests = filledCount;
        if (g_cfg.glowMs > 0)
            glow::Pulse(filled, filledCount, static_cast<uint32_t>(g_cfg.glowMs));
    } else {
        // Chest contents unknown to the client: report what left the backpack.
        uint32_t ids[128];
        uint64_t counts[128];
        int n = Totals(r.backpackBefore, ids, counts, 128);
        for (int i = 0; i < n; ++i) {
            uint64_t now = CountOf(after, ids[i]);
            if (now < counts[i]) {
                storedItems += counts[i] - now;
                rep.Line("  deposited %s x%llu", inv::Describe(ids[i]).shown, static_cast<unsigned long long>(counts[i] - now));
                ++lines;
            }
        }
    }
    // What stayed because its chests are full.
    uint64_t noRoomItems = 0;
    char noRoom[260] = "";
    for (int i = 0, len = 0; i < r.noRoomCount; ++i) {
        noRoomItems += r.noRoomCounts[i];
        if (len >= 0 && len < 240) {
            int w = _snprintf_s(noRoom + len, sizeof(noRoom) - len, _TRUNCATE, "%s %s x%llu", i ? "," : "",
                                inv::Describe(r.noRoomIds[i]).shown, static_cast<unsigned long long>(r.noRoomCounts[i]));
            len = w < 0 ? 240 : len + w;
        }
    }
    if (!lines) {
        rep.Line("Nothing was deposited.");
        if (!r.noRoomCount) { // else the no-room line says why
            char chatLine[160];
            _snprintf_s(chatLine, _TRUNCATE, "Quick Deposit: %s", lang::T(lang::Str::NothingToDeposit));
            chat::Post(chatLine);
        }
    } else {
        // chat=short: everything above in one line.
        char text[200], chatLine[240];
        unsigned items = static_cast<unsigned>(storedItems < 0xFFFFFFFFull ? storedItems : 0xFFFFFFFFull);
        if (storedChests >= 0)
            _snprintf_s(text, _TRUNCATE, lang::T(lang::Str::DepositSummary), items, static_cast<unsigned>(storedChests));
        else
            _snprintf_s(text, _TRUNCATE, lang::T(lang::Str::DepositSummaryItems), items);
        _snprintf_s(chatLine, _TRUNCATE, "Quick Deposit: %s", text);
        chat::Post(chatLine, chat::Kind::Summary);
    }
    {
        // What stayed because you keep it.
        uint32_t ids[64];
        uint64_t counts[64];
        int n = KeptTotals(after, ids, counts, 64);
        char items[260] = "";
        int len = 0;
        for (int i = 0; i < n && len >= 0 && len < 240; ++i) {
            int w = _snprintf_s(items + len, sizeof(items) - len, _TRUNCATE, "%s %s x%llu", i ? "," : "",
                                inv::Describe(ids[i]).shown, static_cast<unsigned long long>(counts[i]));
            len = w < 0 ? 240 : len + w;
        }
        if (n) {
            rep.Line("Kept (marked):%s", items);
            char chatLine[360];
            _snprintf_s(chatLine, _TRUNCATE, "Quick Deposit: %s%s", lang::T(lang::Str::KeptMarked), items);
            chat::Post(chatLine, chat::Kind::Detail);
        }
    }
    if (r.noRoomCount) {
        rep.Line("No room in chests for:%s", noRoom);
        char chatLine[360], text[200];
        _snprintf_s(chatLine, _TRUNCATE, "Quick Deposit: %s%s", lang::T(lang::Str::NoRoom), noRoom);
        chat::Post(chatLine, chat::Kind::Detail);
        // chat=short: the count only.
        _snprintf_s(text, _TRUNCATE, lang::T(lang::Str::NoRoomCount),
                    static_cast<unsigned>(noRoomItems < 0xFFFFFFFFull ? noRoomItems : 0xFFFFFFFFull));
        _snprintf_s(chatLine, _TRUNCATE, "Quick Deposit: %s", text);
        chat::Post(chatLine, chat::Kind::Summary);
    }

    if (g_cfg.tier2 == Tier2Mode::DryRun && r.moveCount) {
        rep.Line("Tier 2 plan (dry run - nothing moved; set tier2=on in quick-deposit.ini to do it):");
        for (int i = 0; i < r.moveCount; ++i) {
            const Move& m = r.moves[i];
            char label[160];
            ChestLabel(m.chest, r.chestsBefore[m.chest], label, sizeof(label));
            inv::ItemDesc d = inv::Describe(m.itemId);
            rep.Line("  %s x%u -> %s, page %u slot %d (%s)", d.shown, m.count, label,
                     r.chestsMid[m.chest].pages[m.dstPage].entity, m.dstSlot, ReasonText(m.reason));
        }
    }
    if (g_cfg.tier2 == Tier2Mode::On && r.moveCount) {
        // Each move checked at its target slot.
        rep.Line("Sorted by kind:");
        for (int i = 0; i < r.moveCount; ++i) {
            const Move& m = r.moves[i];
            if (i && r.moves[i - 1].srcPage == m.srcPage && r.moves[i - 1].srcSlot == m.srcSlot &&
                r.moves[i - 1].chest == m.chest)
                continue; // one stack in parts (topping up the chest's stacks): one line
            char label[160];
            ChestLabel(m.chest, r.chestsBefore[m.chest], label, sizeof(label));
            // The host may pick another free slot, so check the chest as a whole.
            inv::Owner now;
            bool arrived = inv::ReadOwner(r.chests[m.chest], now) &&
                           CountOf(now, m.itemId) > CountOf(r.chestsBefore[m.chest], m.itemId);
            inv::ItemDesc item = inv::Describe(m.itemId);
            const char* name = item.shown;
            rep.Line("  %s -> %s (%s): %s", name, label,
                     ReasonText(m.reason), arrived ? "done" : "did NOT move");
            if (!arrived) {
                char chatLabel[160], text[320], chatLine[360];
                ChestLabel(m.chest, r.chestsBefore[m.chest], chatLabel, sizeof(chatLabel), true);
                _snprintf_s(text, _TRUNCATE, lang::T(lang::Str::CouldNotMove), name, chatLabel);
                _snprintf_s(chatLine, _TRUNCATE, "Quick Deposit: %s", text);
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

extern Config g_baseCfg;

// Shows the keep marks your backpack, four times a second.
void UpdateBackpackPages()
{
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 250)
        return;
    last = now;
    static inv::Owner me;
    static ULONGLONG lastIdentify = 0;
    uint32_t handle = g_client && At<void*>(g_client, kClientWorld) ? At<uint32_t>(g_client, kClientPlayer) : 0;
    uint32_t playerId = handle ? (handle & 0x3f) + 1 : 0;
    if (playerId && !inv::ReadOwner(playerId, me) && now - lastIdentify >= 5000) {
        // After a game start nothing has identified the client registry yet (a deposit does):
        // the way a deposit does in a base (a chest tells the registries apart), else by the
        // registry where your own pages have inventories.
        lastIdentify = now;
        BaseScan scan;
        std::string why;
        if (!ScanBase(g_client, g_baseCfg, scan, why) || !scan.readable)
            inv::Ready(playerId, playerId);
        inv::ReadOwner(playerId, me);
    }
    if (!playerId)
        me.pageCount = 0;
    keep::SetBackpack(me);
}

std::atomic<void*> g_lastApp{nullptr}; // dev inspection

void Tick(void* app)
{
    g_lastApp = app;
    g_client = ClientFromApp(app);
    chat::SetClient(g_client);
    // Another world (a server, back to your own): its inventories live in other registries.
    static void* lastWorld = nullptr;
    void* world = g_client ? At<void*>(g_client, kClientWorld) : nullptr;
    if (world != lastWorld) {
        if (lastWorld)
            inv::Forget();
        lastWorld = world;
    }
    character::Tick(g_client, world != nullptr);
    keep::FollowCharacter();
    UpdateBackpackPages();
    keep::Tick();
    // Gear you put away in a chest loses its mark (keep.h UnmarkStored); every few seconds, and
    // never during a deposit (which leaves kept items where they are anyway).
    static ULONGLONG lastStoredCheck = 0;
    if (g_run.phase == Phase::Idle && keep::AnyAmountMark() && GetTickCount64() - lastStoredCheck > 5000) {
        lastStoredCheck = GetTickCount64();
        BaseScan scan;
        std::string why;
        if (ScanBase(g_client, g_baseCfg, scan, why) && scan.readable) {
            static inv::Owner player;
            static std::vector<inv::Owner> chests;
            if (inv::ReadOwner(scan.playerId, player)) {
                chests.resize(scan.chests.size());
                int n = 0;
                for (uint32_t c : scan.chests)
                    if (inv::ReadOwner(c, chests[n]))
                        ++n;
                keep::UnmarkStored(player, chests.data(), n);
            }
        }
    }
    if (host::TakeStationUse()) {
        if (g_run.phase == Phase::Idle) {
            g_pending = g_baseCfg;
            g_requested = true;
            Log("deposit requested (Quick Stack Station)");
        }
    }
    if (g_baseCfg.glowMs > 0) {
        std::vector<uint32_t> filled = host::TakeFilled();
        if (!filled.empty())
            glow::Pulse(filled.data(), static_cast<int>(filled.size()), static_cast<uint32_t>(g_baseCfg.glowMs));
    }
    glow::Tick(keep::BackpackShown());
    host::ClientTick(world, LocalPlayerId());
    // G twice sorted the backpack twice too, and its stacks move when the host's answer arrives:
    // the deposit plans once the backpack stayed the same for kSortQuietMs (settleMs at most).
    constexpr ULONGLONG kSortQuietMs = 400;
    static ULONGLONG sortSince = 0, sortQuietFrom = 0;
    static uint64_t sortSig = 0;
    bool start = false;
    if (g_requested.exchange(false)) {
        if (g_run.phase != Phase::Idle || sortSince) {
            Log("deposit: still busy with the last one");
        } else if (g_requestSorted.exchange(false)) {
            sortSince = sortQuietFrom = GetTickCount64();
            sortSig = BackpackSig(LocalPlayerId());
        } else {
            start = true;
        }
    }
    if (sortSince) {
        ULONGLONG t = GetTickCount64();
        uint64_t sig = BackpackSig(LocalPlayerId());
        if (sig != sortSig) {
            sortSig = sig;
            sortQuietFrom = t;
        }
        if (t - sortQuietFrom >= kSortQuietMs || t - sortSince >= static_cast<ULONGLONG>(g_pending.settleMs)) {
            Log("deposit: backpack sorted (%llu ms)", t - sortSince);
            sortSince = 0;
            start = true;
        }
    }
    if (start) {
        g_cfg = g_pending;
        std::string why;
        Start(ClientFromApp(app), why);
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
    case Phase::Whole: {
        int filled = host::WholeBaseResult();
        if (filled < 0 && now - r.wholeAt < 1500) {
            r.nextTime = now + 20;
            break;
        }
        if (filled < 0) {
            host::CancelWholeBase();
            Log("deposit: the host part did not run the whole-base deposit - chest by chest instead");
        } else if (r.wholeAt) {
            // Let the host's changes reach this game's view of the inventories.
            r.wholeAt = 0;
            r.nextTime = now + 250;
            break;
        }
        r.wholeAt = 0;
        if (!inv::ReadOwner(r.playerId, r.backpackMid))
            r.backpackMid = r.backpackBefore;
        for (int c = 0; c < r.chestCount; ++c)
            if (!inv::ReadOwner(r.chests[c], r.chestsMid[c]))
                r.chestsMid[c] = r.chestsBefore[c];
        BeginPasses(r.backpackMid, r.chestsMid);
        break;
    }
    case Phase::Tier1:
        StepTier1(cl, now);
        break;
    case Phase::Tier2:
        StepTier2(cl, now);
        break;
    case Phase::Settle2:
        if (!Settled(now)) {
            r.nextTime = now + 50;
            break;
        }
        if (g_cfg.tier2 == Tier2Mode::DryRun && r.backpackBefore.pageCount && r.chestsVisible) {
            r.moveCount = 0;
            PlanTier2(false);
        } else if (g_cfg.tier2 != Tier2Mode::DryRun && r.sentThisPass && r.pass < kMaxPasses &&
                   r.backpackBefore.pageCount && r.chestsVisible) {
            // Staggered retry passes, each delayMs later than the one before: commands the host
            // dropped, stacks that did not fit, items that arrived meanwhile.
            if (!r.retryWaited) {
                r.retryWaited = true;
                r.nextTime = now + static_cast<ULONGLONG>(g_cfg.delayMs) * (r.pass + 1);
                break;
            }
            r.retryWaited = false;
            if (StartRetry())
                break;
        }
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
constexpr size_t kCmdSortOwner = 0x04; // u32: the entity whose inventory it sorts (you: your player id)
Config g_baseCfg;
ULONGLONG g_lastSort = 0;

#ifdef QD_BRIDGE
bool g_traceCommands = false;

void TraceCommand(const uint8_t* cmd)
{
    char hex[48 * 2 + 16];
    int n = 0;
    __try {
        for (int i = 0; i < 48; ++i)
            n += sprintf_s(hex + n, sizeof(hex) - n, i % 4 == 0 && i ? " %02x" : "%02x", cmd[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n += sprintf_s(hex + n, sizeof(hex) - n, " <unreadable>");
    }
    Log("cmd %02x: %s", cmd[0], hex);
}
#endif

void UiDispatchHook(void* cl, uint8_t* cmd)
{
    greeneye::Lease lease; // while it runs Quick Deposit's code; an older generation calls on
    if (!lease) {
        g_originalDispatch(cl, cmd);
        return;
    }
#ifdef QD_BRIDGE
    if (g_traceCommands && cmd)
        TraceCommand(cmd);
#endif
    // Only sorts of your own inventory count: a chest's sort (T, or its sort button) is the same
    // command with the chest at +4, and two of those started deposits too.
    if (cmd && cmd[0] == kCmdSortInventory && g_baseCfg.doubleSortMs > 0) {
        const uint32_t sorted = At<uint32_t>(cmd, kCmdSortOwner);
        const uint32_t me = LocalPlayerId();
        ULONGLONG now = GetTickCount64();
        if (me && sorted != me) {
            g_lastSort = 0;
        } else if (g_lastSort && now - g_lastSort <= static_cast<ULONGLONG>(g_baseCfg.doubleSortMs)) {
            g_lastSort = 0;
            g_pending = g_baseCfg;
            g_requested = true;
            g_requestSorted = true;
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
    {
        // Released before the frame goes on: the Greeneye Loader swaps generations at the top of
        // this same frame, which needs every lease ended.
        greeneye::Lease lease;
        if (lease) {
            g_client = ClientFromApp(app);
#ifdef QD_BRIDGE
            SafePump();
#endif
            if (!g_broken)
                SafeTick(app);
        }
    }
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
    ok &= Find(loop, "UiCommandLoop", "40 53 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 98 00 00 00 48 8D 81 78 4F 03 00");
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
    // In the chat command (0x74): cmp byte [rdi + allowed], 0; jne; cmp byte [rdx + 1], 4
    if (uint8_t* check = FindUnique("80 BF ?? ?? ?? ?? 00 75 ?? 80 7A 01 04")) {
        int32_t offset;
        memcpy(&offset, check + 2, 4);
        g_textChatAllowed = offset > 0 && offset < 0x800000 ? static_cast<size_t>(offset) : 0;
    }
    if (!g_textChatAllowed)
        Log("chat permission check not found: on servers with text chat off, the host's Quick Deposit is not reached");
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

uint32_t LocalPlayerId()
{
    uint32_t handle = g_client && At<void*>(g_client, kClientWorld) ? At<uint32_t>(g_client, kClientPlayer) : 0;
    return handle ? (handle & 0x3f) + 1 : 0;
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

#ifdef QD_BRIDGE
void SetCommandTrace(bool on)
{
    g_traceCommands = on;
    Log("command trace %s", on ? "on" : "off");
}

// UiDispatch command 0x74 "send chat": +1 type (0 = typed text), +2 text (up to 0x400 bytes),
// +0x402 u16 length; the client queues it for the server (see Auto Loot's window.cpp).
bool TransferSlot(uint32_t fromPage, int fromSlot, uint32_t toPage, int toSlot)
{
    if (!g_client || !g_originalDispatch || fromSlot < 0 || fromSlot >= inv::kSlotsPerPage || toSlot < 0 ||
        toSlot >= inv::kSlotsPerPage)
        return false;
    alignas(8) uint8_t cmd[kCmdSize] = {};
    cmd[0] = kCmdTransfer;
    At<uint32_t>(cmd, kCmdTargetSlot) = toPage;
    cmd[kCmdTargetSlot + 4] = static_cast<uint8_t>(toSlot);
    At<uint32_t>(cmd, kCmdSourceSlot) = fromPage;
    cmd[kCmdSourceSlot + 4] = static_cast<uint8_t>(fromSlot);
    cmd[kCmdType] = kTypeTransfer;
    Send(g_client, cmd);
    Log("transfer: page %u slot %d -> page %u slot %d (bridge)", fromPage, fromSlot, toPage, toSlot);
    return true;
}

bool SendRawCommand(const uint8_t* cmd)
{
    if (!g_client || !g_originalDispatch)
        return false;
    Send(g_client, cmd);
    return true;
}

bool SendAltarUpgrade(uint32_t altarEntity, uint32_t recipe)
{
    alignas(8) altar_upgrade::Command cmd{};
    if (!g_client || !g_originalDispatch || !altar_upgrade::Encode(altarEntity, recipe, cmd))
        return false;
    // Send() is intentionally limited to the 0x28-byte inventory command. CraftRecipe
    // needs all 0x30 bytes, including its uint16 amount at +0x2c.
    g_originalDispatch(g_client, reinterpret_cast<uint8_t*>(&cmd));
    Log("altar upgrade recipe %u submitted for entity %u; native validation pending", recipe, altarEntity);
    return true;
}

bool SendFlameUpgrade(uint32_t progressEntity, uint32_t recipe)
{
    alignas(8) altar_upgrade::Command cmd{};
    if (!g_client || !g_originalDispatch || !altar_upgrade::EncodeFlame(progressEntity, recipe, cmd))
        return false;
    g_originalDispatch(g_client, reinterpret_cast<uint8_t*>(&cmd));
    Log("Flame upgrade recipe %u submitted for progress entity %u; native validation pending", recipe, progressEntity);
    return true;
}

bool SendChatLine(const char* text)
{
    if (!g_client || !g_originalDispatch)
        return false;
    alignas(8) static uint8_t cmd[0x410];
    memset(cmd, 0, sizeof(cmd));
    size_t n = strlen(text);
    if (n > 0x400)
        n = 0x400;
    cmd[0] = 0x74;
    memcpy(cmd + 2, text, n);
    *reinterpret_cast<uint16_t*>(cmd + 0x402) = static_cast<uint16_t>(n);
    g_originalDispatch(g_client, cmd);
    return true;
}

#endif

bool SendHiddenLine(const char* text, uint8_t type)
{
    if (!g_client || !g_originalDispatch)
        return false;
    alignas(8) static uint8_t cmd[0x410];
    memset(cmd, 0, sizeof(cmd));
    size_t n = strlen(text);
    if (n > 0x400)
        n = 0x400;
    cmd[0] = 0x74;
    cmd[1] = type;
    memcpy(cmd + 2, text, n);
    *reinterpret_cast<uint16_t*>(cmd + 0x402) = static_cast<uint16_t>(n);
    // The chat command sends only while the server allows text chat, or system lines: allow this
    // one line too, so servers with text chat off still get it (as Embervale Teleport does).
    if (!g_textChatAllowed) {
        g_originalDispatch(g_client, cmd);
        return true;
    }
    uint8_t& allowed = At<uint8_t>(g_client, g_textChatAllowed);
    uint8_t saved = allowed;
    allowed = 1;
    g_originalDispatch(g_client, cmd);
    allowed = saved;
    return true;
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

void* LastApp()
{
    return g_lastApp.load();
}

} // namespace client

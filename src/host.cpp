// The host's side of Quick Deposit (build 1273043; the dedicated server has the same code).
//
// 1402726e0, the "area_deposit_trigger" system, runs a station's deposit when the station's
// actor event (keen::actor::AreaDepositEvent) fires: 14040bf70 collects the chests in range,
// then calls 14040cd30 PerChest(out, ctx, chest, player, state) for each. PerChest builds the
// page lists of the chest and the player with 14040f140 PageList(list, ctx, inventory) (the
// player's Generic pages, the ones a deposit takes from; entries {slots, page}, 8 slots of
// {item, count, pide}) and moves every matching stack. A slot with count 0 is skipped.
//
// PerChest is hooked: for a player who keeps items, their page list is built the same way first
// and each kept amount is taken off its stack's count; after the original ran it is put back
// (also where the stack moved out entirely and the slot emptied).
//
// The server's receive loop (MapTeleport's ReceiveHook) gives the connection table, server +
// 0x1e0 + i * 0x148 (16 slots: +0 in use, +0x18 player entity, +0x48 account), which tells which
// player a chat line's account belongs to.
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <string>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "../../common/greeneye.h"
#include "client.h"
#include "host.h"
#include "inventory.h"
#include "keep.h"
#include "log.h"
#include "scan.h"

namespace host {
namespace {

using PerChestFn = void* (*)(void* out, void* ctx, uint32_t chest, uint32_t player, void* state);
using WorldFn = void* (*)(void* world);
using LookupFn = void** (*)(void** out, void* world, void* type, uint32_t entity);
using PageListFn = uint64_t (*)(void* list, void* ctx, void* inventory);
using ReceiveFn = void (*)(void* server);

PerChestFn g_originalPerChest = nullptr;
WorldFn g_world = nullptr;
LookupFn g_lookup = nullptr;
PageListFn g_pageList = nullptr;
ReceiveFn g_originalReceive = nullptr;

std::atomic<void*> g_server{nullptr};
std::atomic<ULONGLONG> g_serverSeen{0}; // the server loop ran: this process hosts
std::atomic<uint32_t> g_localPlayer{0};

// Client side: whether the host answered, per world.
std::atomic<bool> g_hostAnswered{false};
std::atomic<ULONGLONG> g_answeredAt{0};
struct Link {
    void* world = nullptr;
    int asked = 0;
    ULONGLONG helloAt = 0, sentAt = 0;
    uint64_t sentVersion = ~0ull;
} g_link;

std::mutex g_filledLock;
std::vector<uint32_t> g_filled; // chests your own deposits filled since the last TakeFilled

std::mutex g_listsLock;
std::unordered_map<uint64_t, std::vector<keep::KeptItem>> g_lists; // account -> kept items

struct PageEntry {
    uint8_t* slots;
    uint32_t page;
    uint32_t pad;
};
// The game's growable array, with room for 16 entries in place (no growing: a player has fewer
// Generic pages).
struct PageList {
    PageEntry* data;
    uint64_t count;
    uint64_t capacity;
    void* grow;
    uint64_t extra;
    PageEntry inline_[16];
};

template <class T> T& At(void* base, size_t offset)
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

uint64_t AccountOf(uint32_t player)
{
    auto* s = static_cast<uint8_t*>(g_server.load());
    for (int i = 0; s && i < 16; ++i) {
        uint8_t* slot = s + 0x1e0 + i * 0x148;
        if (At<uint32_t>(slot, 0) && At<uint32_t>(slot, 0x18) == player)
            return At<uint64_t>(slot, 0x48);
    }
    return 0;
}

uint64_t SafeAccountOf(uint32_t player)
{
    __try {
        return AccountOf(player);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The kept items of player: the local player's own marks, else what their client sent.
bool KeptOf(uint32_t player, std::vector<keep::KeptItem>& out)
{
    if (player && player == g_localPlayer.load()) {
        out = keep::Snapshot();
        return !out.empty();
    }
    uint64_t account = SafeAccountOf(player);
    std::lock_guard<std::mutex> lock(g_listsLock);
    auto it = account ? g_lists.find(account) : g_lists.end();
    if (it == g_lists.end() || it->second.empty())
        return false;
    out = it->second;
    return true;
}

struct SetAside {
    inv::Stack* slot;
    inv::Stack was;
    uint32_t kept;
};

// The player's Generic pages, the way PerChest reads them.
int ReadPages(void* ctx, uint32_t player, PageList& list)
{
    list = {};
    list.data = list.inline_;
    list.capacity = 16;
    void* world = g_world(**static_cast<void***>(ctx));
    void* found[2] = {}; // a 16-byte result: the component first
    g_lookup(found, world, At<void*>(ctx, 0x18), player);
    void* inventory = found[0];
    if (!inventory)
        return 0;
    g_pageList(&list, ctx, inventory);
    return static_cast<int>(list.count < 16 ? list.count : 16);
}

// n counts what is set aside so far, so a fault part way still gets it put back.
void Hide(void* ctx, uint32_t player, const std::vector<keep::KeptItem>& items, SetAside* out, int max, int& n)
{
    PageList list;
    int pages = ReadPages(ctx, player, list);
    static thread_local inv::Owner owner;
    static thread_local keep::Plan plan;
    owner = {};
    owner.entity = player;
    owner.pageCount = pages < inv::kMaxPages ? pages : inv::kMaxPages;
    for (int p = 0; p < owner.pageCount; ++p) {
        owner.pages[p].entity = list.data[p].page;
        owner.pages[p].kind = inv::PageKind::Generic;
        memcpy(owner.pages[p].slots, list.data[p].slots, sizeof(owner.pages[p].slots));
    }
    keep::PlanForItems(owner, items, plan);
    for (int p = 0; p < owner.pageCount; ++p)
        for (int s = 0; s < inv::kSlotsPerPage && n < max; ++s) {
            uint32_t kept = plan.keep[p][s];
            if (!kept)
                continue;
            auto* slot = reinterpret_cast<inv::Stack*>(list.data[p].slots) + s;
            out[n] = {slot, *slot, kept};
            slot->count -= kept < slot->count ? kept : slot->count;
            ++n;
        }
}

void PutBack(const SetAside* set, int n)
{
    for (int i = 0; i < n; ++i) {
        inv::Stack* slot = set[i].slot;
        if (slot->itemId == set[i].was.itemId) {
            slot->count += set[i].kept;
        } else if (!slot->itemId) {
            *slot = set[i].was; // the rest moved out and the slot emptied
            slot->count = set[i].kept;
        } else {
            Log("host: a kept stack's slot holds item %u now (was %u) - %u not put back", slot->itemId,
                set[i].was.itemId, set[i].kept);
        }
    }
}

int Fault(EXCEPTION_POINTERS* e, uintptr_t& at, DWORD& code)
{
    at = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    code = e->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}

int SafeHide(void* ctx, uint32_t player, const std::vector<keep::KeptItem>& items, SetAside* out, int max)
{
    int n = 0;
    uintptr_t at = 0;
    DWORD code = 0;
    __try {
        Hide(ctx, player, items, out, max, n);
    } __except (Fault(GetExceptionInformation(), at, code)) {
        auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        Log("host: reading player %u's pages failed (exception %08lX at %s+0x%llX) - %d kept stacks set aside", player,
            code, at - base < 0x10000000 ? "exe" : "?", static_cast<unsigned long long>(at - base), n);
    }
    return n;
}

// PerChest's result: +0 an error (0: none), +8 how many items went into the chest.
uint32_t MovedInto(const void* out)
{
    __try {
        const auto* r = static_cast<const uint8_t*>(out);
        return r[0] ? 0 : *reinterpret_cast<const uint32_t*>(r + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* PerChestHook(void* out, void* ctx, uint32_t chest, uint32_t player, void* state)
{
    greeneye::Lease lease;
    std::vector<keep::KeptItem> items;
    void* result;
    if (!lease || !KeptOf(player, items)) {
        result = g_originalPerChest(out, ctx, chest, player, state);
    } else {
        SetAside set[inv::kMaxPages * inv::kSlotsPerPage];
        int n = SafeHide(ctx, player, items, set, static_cast<int>(sizeof(set) / sizeof(set[0])));
        result = g_originalPerChest(out, ctx, chest, player, state);
        PutBack(set, n);
    }
    // Your own deposits (a station, "Deposit Stacks"): the chests that got something light up.
    if (lease && player && player == g_localPlayer.load() && MovedInto(out)) {
        std::lock_guard<std::mutex> lock(g_filledLock);
        if (g_filled.size() < 64 && std::find(g_filled.begin(), g_filled.end(), chest) == g_filled.end())
            g_filled.push_back(chest);
    }
    return result;
}

void ReceiveHook(void* server)
{
    greeneye::Lease lease;
    if (lease) {
        g_server = server;
        g_serverSeen = GetTickCount64();
    }
    g_originalReceive(server);
}

// "id:n,id:n" appended to list; false if malformed.
bool ParseItems(std::string_view text, std::vector<keep::KeptItem>& list)
{
    while (!text.empty()) {
        size_t comma = text.find(',');
        std::string_view item = text.substr(0, comma);
        size_t colon = item.find(':');
        if (colon == std::string_view::npos)
            return false;
        uint32_t id = 0, n = 0;
        auto a = std::from_chars(item.data(), item.data() + colon, id);
        auto b = std::from_chars(item.data() + colon + 1, item.data() + item.size(), n);
        if (a.ec != std::errc{} || b.ec != std::errc{} || b.ptr != item.data() + item.size() || !id)
            return false;
        if (list.size() < 256)
            list.push_back({id, n ? n : keep::kAll});
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return true;
}

// A call's target, when the bytes at site are "call rel32".
uint8_t* CallTarget(uint8_t* site)
{
    uint8_t bytes[5];
    if (const GreeneyeHost* h = greeneye::Host())
        h->read(site, bytes, 5);
    else
        memcpy(bytes, site, 5);
    if (bytes[0] != 0xe8)
        return nullptr;
    int32_t rel;
    memcpy(&rel, bytes + 1, 4);
    return site + 5 + rel;
}

// --- whole-base deposits --------------------------------------------------------------------
//
// 1402726e0 ("area_deposit_trigger") runs every frame over the entities of its view: for each,
// 140213b50 collects its actor events into a list ({data, count, capacity, ...}, 16 in place),
// and for every AreaDepositEvent (hash 0xbe053f84; payload at record + 0x64 + offset) it builds
// the deposit context (140412170) and calls 14040bf70 AreaDeposit(result, ctx, player, station,
// payload). Afterwards the context's changes are committed when result byte 0 is 0
// (140408ed0), else rolled back.
//
// The events of an entity are only looked at when the station it is using exists (the first
// lookup, 1409a6ca0 on view + 0x78): with none, and a whole-base deposit pending for that player,
// the redirected lookup answers with the player itself (it has a position too).
//
// A whole-base deposit adds a marked event to those lists while one is pending; AreaDeposit,
// given the mark, runs PerChest for every chest of the base instead of the station's (keeping
// each chest's "Receive Quick Stacking" setting) and reports success, so the game commits it all
// at once.
using CollectFn = uint64_t (*)(void* list, void* view, void* a, uint64_t b, void* c);
using AreaDepositFn = void* (*)(void* result, void* ctx, uint32_t player, uint32_t station, void* payload);
using EntityFn = void* (*)(void* registry, uint32_t entity);
using FlagFn = uint8_t* (*)(void* record, void* type);
using ComponentFn = void** (*)(void** out, void* record, void* type);

using CurrentFn = void* (*)(void* iter, uint32_t* entity);

CollectFn g_collect = nullptr;
EntityFn g_propLookup = nullptr;
CurrentFn g_current = nullptr;
AreaDepositFn g_areaDeposit = nullptr;
EntityFn g_entity = nullptr;
FlagFn g_flag = nullptr;
ComponentFn g_component = nullptr;

constexpr uint32_t kAreaDepositEvent = 0xbe053f84;
const uint8_t kMark[16] = {'Q', 'D', ' ', 'w', 'h', 'o', 'l', 'e', ' ', 'b', 'a', 's', 'e', '!', '!', 1};
alignas(16) uint8_t g_event[0x80]; // the marked event: hash, offset 4, payload at +0x68

std::mutex g_wholeLock;
uint32_t g_wholePlayer = 0; // 0: none pending
std::vector<uint32_t> g_wholeChests;
std::atomic<bool> g_wholeAdded{false};
std::atomic<bool> g_stationUsed{false}; // you used a station here; the client starts a deposit // logged once per request
std::atomic<int> g_wholeResult{-1}; // chests the last run filled, -1 while one is pending

struct Growable {
    void** data;
    uint64_t count;
    uint64_t capacity;
};

uint64_t CollectHook(void* list, void* view, void* a, uint64_t b, void* c)
{
    greeneye::Lease lease;
    uint64_t result = g_collect(list, view, a, b, c);
    if (!lease)
        return result;
    bool pending;
    {
        std::lock_guard<std::mutex> lock(g_wholeLock);
        pending = g_wholePlayer != 0;
    }
    auto* l = static_cast<Growable*>(list);
    if (pending && l->data && l->count < l->capacity) {
        l->data[l->count++] = g_event;
        if (!g_wholeAdded.exchange(true))
            Log("host: whole-base event added (the list held %llu)", static_cast<unsigned long long>(l->count - 1));
    }
    return result;
}

void* PropLookupHook(void* iter, uint32_t station)
{
    greeneye::Lease lease;
    void* record = g_propLookup(iter, station);
    if (record || !lease)
        return record;
    uint32_t pending;
    {
        std::lock_guard<std::mutex> lock(g_wholeLock);
        pending = g_wholePlayer;
    }
    if (!pending)
        return record;
    uint32_t self = 0;
    g_current(iter, &self);
    return self == pending ? g_propLookup(iter, self) : record;
}

// AreaDeposit's own test: the chest takes a station's deposits.
bool ReceivesQuickStacking(void* ctx, uint32_t chest)
{
    void* record = g_entity(*static_cast<void**>(ctx), chest);
    if (!record)
        return false;
    uint8_t* storage = g_flag(record, At<void*>(ctx, 0x48));
    if (!storage || !*storage)
        return false;
    void* setting = nullptr;
    g_component(&setting, record, At<void*>(ctx, 0x50));
    auto* p = static_cast<uint8_t*>(setting);
    return !(p && p[0] && !p[1]);
}

bool SafeReceives(void* ctx, uint32_t chest)
{
    __try {
        return ReceivesQuickStacking(ctx, chest);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int WholeBase(void* ctx, uint32_t player, const std::vector<uint32_t>& chests)
{
    int filled = 0, skipped = 0;
    for (uint32_t chest : chests) {
        if (!SafeReceives(ctx, chest)) {
            ++skipped;
            continue;
        }
        alignas(8) uint8_t out[16] = {};
        PerChestHook(out, ctx, chest, player, nullptr);
        filled += MovedInto(out) ? 1 : 0;
    }
    Log("host: whole-base deposit for player %u: %d of %d chests got something (%d don't take quick stacking)",
        player, filled, static_cast<int>(chests.size()), skipped);
    return filled;
}

void* AreaDepositHook(void* result, void* ctx, uint32_t player, uint32_t station, void* payload)
{
    greeneye::Lease lease;
    bool marked = lease && payload && memcmp(payload, kMark, sizeof(kMark)) == 0;
    // You used a Quick Stack Station in the world this game hosts: a whole Quick Deposit (as G
    // twice) instead of the station's own.
    if (lease && !marked && player && player == g_localPlayer.load() &&
        GetTickCount64() - g_serverSeen.load() < 2000) {
        memset(result, 0, 12);
        if (!g_stationUsed.exchange(true))
            Log("host: you used Quick Stack Station %u - a whole Quick Deposit instead", station);
        return result;
    }
    if (!marked)
        return g_areaDeposit(result, ctx, player, station, payload);
    // The marked event. Its result {u8 error, u32, u32 count} says success, so the game commits
    // what PerChest did.
    memset(result, 0, 12);
    std::vector<uint32_t> chests;
    {
        std::lock_guard<std::mutex> lock(g_wholeLock);
        if (!g_wholePlayer || player != g_wholePlayer)
            return result;
        chests.swap(g_wholeChests);
        g_wholePlayer = 0;
    }
    g_wholeResult = WholeBase(ctx, player, chests);
    return result;
}

// Optional: without it deposits go chest by chest.
void InitWholeBase()
{
    // mov byte [rbp+7404h], r13b; call Collect; mov rdi, [rbp+100h]
    uint8_t* collect = FindUnique("44 88 AD 04 74 00 00 E8 ?? ?? ?? ?? 48 8B BD 00 01 00 00");
    // lea rcx, [rsp+30h]; call AreaDeposit; lea rcx, [rbp+1B0h]; cmp [rsp+30h], r13b
    uint8_t* call = FindUnique("48 8D 4C 24 30 E8 ?? ?? ?? ?? 48 8D 8D B0 01 00 00 44 38 6C 24 30");
    uint8_t* area = call ? CallTarget(call + 5) : nullptr;
    // mov edx, ebx; mov rcx, r15; call Entity; test rax, rax; jz ...; mov r8, [rbp+78h]
    uint8_t* prop = FindUnique("8B D3 49 8B CF E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 4C 8B 45 78");
    if (!collect || !area || !prop) {
        Log("host: whole-base deposits unavailable (%p %p %p)", collect, area, prop);
        return;
    }
    g_current = reinterpret_cast<CurrentFn>(CallTarget(prop + 0x66));
    g_entity = reinterpret_cast<EntityFn>(CallTarget(area + 0x3f));
    g_flag = reinterpret_cast<FlagFn>(CallTarget(area + 0x48f));
    g_component = reinterpret_cast<ComponentFn>(CallTarget(area + 0x4ab));
    if (!g_entity || CallTarget(area + 0x477) != reinterpret_cast<uint8_t*>(g_entity) || !g_flag || !g_component ||
        CallTarget(prop + 5) != reinterpret_cast<uint8_t*>(g_entity) || !g_current) {
        Log("host: AreaDeposit's calls not where expected - whole-base deposits unavailable");
        return;
    }
    memcpy(g_event, &kAreaDepositEvent, 4);
    const int32_t offset = 4;
    memcpy(g_event + 4, &offset, 4);
    memcpy(g_event + 0x68, kMark, sizeof(kMark));
    g_collect = static_cast<CollectFn>(RedirectCall(collect + 7, reinterpret_cast<void*>(&CollectHook)));
    g_areaDeposit = static_cast<AreaDepositFn>(RedirectCall(call + 5, reinterpret_cast<void*>(&AreaDepositHook)));
    g_propLookup = static_cast<EntityFn>(RedirectCall(prop + 5, reinterpret_cast<void*>(&PropLookupHook)));
    if (!g_collect || !g_areaDeposit || !g_propLookup) {
        g_collect = nullptr;
        g_areaDeposit = nullptr;
        g_propLookup = nullptr;
        Log("host: whole-base redirects failed");
    }
}

// Quick Stack Stations where another game hosts (a dedicated server, a friend's game). Pressing E
// on an entity runs 1402c35de on the client: it allocates the local AcceptOfferedInteractionEvent
// for the entity in edi (twice per press) and fills it in unless the allocation is null. Its
// allocation call is redirected through a thunk that passes edi in place of the (constant) type
// hash; for a station the redirect answers null and the client runs a Quick Deposit instead, the
// same as G twice.
using AllocEventFn = void* (*)(void* queue, uint32_t typeHash, uint32_t size, uint16_t flags);

constexpr uint32_t kAcceptOfferedInteraction = 0x82e404b2; // keen::ecs::AcceptOfferedInteractionEvent
constexpr uint32_t kStationItem = 3718491895u;             // "Prop Storage Drop Station", its build item

AllocEventFn g_acceptAlloc = nullptr;

uint32_t FirstWord(const void* data)
{
    uint32_t v = 0;
    __try {
        v = *static_cast<const uint32_t*>(data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return v;
}

// A placed Quick Stack Station: one of its components starts with the station's build item.
bool IsStation(uint32_t entity)
{
    void* reg = entity ? inv::InspectRegistry(g_localPlayer.load()) : nullptr;
    if (!reg)
        return false;
    uint16_t types[640];
    int n = inv::Types(reg, entity, types, 640);
    for (int i = 0; i < n; ++i)
        if (const void* data = inv::Data(reg, types[i], entity); data && FirstWord(data) == kStationItem)
            return true;
    return false;
}

void* AcceptAllocHook(void* queue, uint32_t target, uint32_t size, uint16_t flags)
{
    greeneye::Lease lease;
    // Where this game hosts, the station runs and AreaDepositHook takes it over.
    if (lease && GetTickCount64() - g_serverSeen.load() >= 2000 && IsStation(target)) {
        // One press allocates twice: one deposit per press.
        static std::atomic<ULONGLONG> lastUse{0};
        ULONGLONG now = GetTickCount64();
        if (now - lastUse.exchange(now) > 1000 && !g_stationUsed.exchange(true))
            Log("you used Quick Stack Station %u - a Quick Deposit instead", target);
        return nullptr;
    }
    return g_acceptAlloc(queue, kAcceptOfferedInteraction, size, flags);
}

// Optional: without it, stations where another game hosts deposit the game's way.
void InitRemoteStations()
{
    // movzx r9d, bx; mov edx, 82E404B2h; mov r8d, 18h; mov rcx, rax; call AllocEvent;
    // mov rbx, rax; test rax, rax; jz short ...; lea rdx, [rbp+77h]
    uint8_t* site = FindUnique("44 0F B7 CB BA B2 04 E4 82 41 B8 18 00 00 00 48 8B C8 E8 ?? ?? ?? ?? "
                               "48 8B D8 48 85 C0 74 ?? 48 8D 55 77");
    if (!site) {
        Log("stations: interaction code not found - stations where another game hosts deposit the game's way");
        return;
    }
    // mov edx, edi; mov rax, AcceptAllocHook; jmp rax
    auto* thunk = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!thunk)
        return;
    void* hook = reinterpret_cast<void*>(&AcceptAllocHook);
    const uint8_t head[] = {0x89, 0xFA, 0x48, 0xB8};
    memcpy(thunk, head, sizeof(head));
    memcpy(thunk + 4, &hook, 8);
    thunk[12] = 0xFF;
    thunk[13] = 0xE0;
    DWORD old;
    VirtualProtect(thunk, 16, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), thunk, 16);
    g_acceptAlloc = static_cast<AllocEventFn>(RedirectCall(site + 18, thunk));
    if (!g_acceptAlloc)
        Log("stations: redirect failed - stations where another game hosts deposit the game's way");
}

} // namespace

bool WholeBaseReady()
{
    return g_collect && g_areaDeposit && g_propLookup && g_localPlayer.load() && GetTickCount64() - g_serverSeen.load() < 2000;
}

bool RequestWholeBase(uint32_t player, const uint32_t* chests, int count)
{
    if (!WholeBaseReady() || !player || count <= 0)
        return false;
    std::lock_guard<std::mutex> lock(g_wholeLock);
    g_wholePlayer = player;
    g_wholeChests.assign(chests, chests + count);
    g_wholeResult = -1;
    g_wholeAdded = false;
    return true;
}

bool TakeStationUse()
{
    return g_stationUsed.exchange(false);
}

int WholeBaseResult()
{
    return g_wholeResult.load();
}

void CancelWholeBase()
{
    std::lock_guard<std::mutex> lock(g_wholeLock);
    g_wholePlayer = 0;
    g_wholeChests.clear();
}

bool Init()
{
    // PerChest: mov [rsp+18h], rbx; mov [rsp+8], rcx; push rbp; push rsi (12 bytes)
    uint8_t* perChest = FindUnique("48 89 5C 24 18 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 20 FE FF FF "
                                   "48 81 EC E0 02 00 00 48 8B 72 18");
    uint8_t* receive = FindUnique("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 D8 F6 FF FF 48 81 EC 28 0A 00 00 4C 8B E9");
    if (!receive)
        receive = FindUnique("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 98 F9 FF FF 48 81 EC 68 07 00 00 4C 8B F9");
    if (!perChest || !receive) {
        Log("host: signatures not found (%p %p) - Quick Stack Stations take kept items", perChest, receive);
        return false;
    }
    g_world = reinterpret_cast<WorldFn>(CallTarget(perChest + 0x3a));
    g_lookup = reinterpret_cast<LookupFn>(CallTarget(perChest + 0x4d));
    g_pageList = reinterpret_cast<PageListFn>(CallTarget(perChest + 0xcf));
    if (!g_world || !g_lookup || !g_pageList) {
        Log("host: PerChest's calls not where expected - Quick Stack Stations take kept items");
        return false;
    }
    g_originalPerChest = static_cast<PerChestFn>(InstallHook(perChest, reinterpret_cast<void*>(&PerChestHook), 12));
    g_originalReceive = static_cast<ReceiveFn>(InstallHook(receive, reinterpret_cast<void*>(&ReceiveHook), 13));
    if (!g_originalPerChest || !g_originalReceive) {
        Log("host: hook install failed - Quick Stack Stations take kept items");
        return false;
    }
    InitWholeBase();
    InitRemoteStations();
    return true;
}

std::vector<uint32_t> TakeFilled()
{
    std::lock_guard<std::mutex> lock(g_filledLock);
    std::vector<uint32_t> out;
    out.swap(g_filled);
    return out;
}

void SetLocalPlayer(uint32_t entity)
{
    g_localPlayer = entity;
}


int HostLine(const char* text, size_t length, uint64_t account, char* answer, size_t capacity)
{
    std::string_view line(text, length);
    if (!line.starts_with("/qd"))
        return 0;
    if (line == "/qdh1") {
        strncpy_s(answer, capacity, "/qds1 1", _TRUNCATE);
        return 1;
    }
    bool first = line == "/qdk1" || line.starts_with("/qdk1 ");
    bool more = line.starts_with("/qdk1+ ");
    if ((first || more) && !account)
        Log("host: a kept-items line without a sender account - ignored");
    if ((first || more) && account) {
        std::vector<keep::KeptItem> part;
        if (line.size() > 6 && !ParseItems(line.substr(first ? 6 : 7), part)) {
            Log("host: kept-items line not understood: %.*s", static_cast<int>(line.size() < 120 ? line.size() : 120), line.data());
            return 2;
        }
        std::lock_guard<std::mutex> lock(g_listsLock);
        auto& list = g_lists[account];
        if (first)
            list.clear();
        list.insert(list.end(), part.begin(), part.end());
        if (first)
            Log("host: account %llX keeps %d item types", static_cast<unsigned long long>(account), static_cast<int>(list.size()));
    }
    return 2; // never shown or relayed, the host's own replies (/qds1) included
}

void ClientLine(const char* text, size_t length, uint32_t senderHandle, uint32_t ownHandle)
{
    std::string_view line(text, length);
    // The host stamps the asker's handle on its answer: every client gets it, the asker reads it.
    if (line.starts_with("/qds1 ") && ownHandle && senderHandle == ownHandle && !g_hostAnswered.exchange(true)) {
        g_answeredAt = GetTickCount64();
        Log("host runs Quick Deposit: Quick Stack Stations leave your kept items");
    }
}

void ClientTick(void* world, uint32_t localPlayer)
{
    SetLocalPlayer(localPlayer);
    ULONGLONG now = GetTickCount64();
    if (world != g_link.world) {
        g_link = {};
        g_link.world = world;
        g_link.helloAt = now + 3000;
        g_hostAnswered = false;
    }
    // Also when this game hosts: its host part answers like any other, and reads your own marks
    // directly anyway.
    if (!world)
        return;
    if (!g_hostAnswered) {
        // Asked twice (3 s and 9 s after joining), in case the first is lost while the world loads.
        if (g_link.asked < 2 && now >= g_link.helloAt && client::SendHiddenLine("/qdh1", kChatType)) {
            ++g_link.asked;
            g_link.helloAt = now + 6000;
            Log(g_link.asked == 1 ? "asking the host whether it runs Quick Deposit" : "asking the host again");
        }
        return;
    }
    uint64_t version = 0;
    std::vector<keep::KeptItem> items = keep::Snapshot(&version);
    // Not right behind the hello, nor more than once a second: servers drop chat lines that
    // come too fast.
    if (version == g_link.sentVersion || now - g_link.sentAt < 1000 || now - g_answeredAt.load() < 2000)
        return;
    g_link.sentVersion = version;
    g_link.sentAt = now;
    // Lines of at most ~150 characters: "/qdk1 id:n,..." then "/qdk1+ id:n,...".
    std::string line = "/qdk1";
    bool any = false;
    for (const keep::KeptItem& k : items) {
        char item[32];
        sprintf_s(item, "%u:%u", k.itemId, k.amount == keep::kAll ? 0u : k.amount);
        if (line.size() + strlen(item) > 150) {
            client::SendHiddenLine(line.c_str(), kChatType);
            line = "/qdk1+";
            any = false;
        }
        line += any ? "," : " ";
        line += item;
        any = true;
    }
    client::SendHiddenLine(line.c_str(), kChatType);
    Log("sent the host your kept items (%d types)", static_cast<int>(items.size()));
}

} // namespace host

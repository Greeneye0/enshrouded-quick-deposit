// Deposited chests pulse green (build 1273043), trimmed from Find's native highlight
// (FindMod/src/nativehighlight.cpp: the same renderer calls, without its silhouettes).
//
// The render systems "dynamic_render_model" and "static_render_model" walk every model with
// the ECS iterator 'next'. Their calls to it are redirected: when an iteration ends, the chests
// to light get highlight group 6 on their render instance through the renderer's own setter
// (renderer, span of model handles, span of colour bytes; it marks the instance dirty), and
// chests whose time is up get their old group back, unless something else (the game's
// interaction highlight, Find's group 7) has set another since.
//
// Each group's colour comes from a palette entry: two functions convert it with 1401238c0, group
// 6's from [rbx + 0x60] at one call each. Those two calls are redirected, so while a chest is lit
// group 6 pulses green; Find redirects nothing here (it hooks the converter for group 7).
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "../../common/greeneye.h"
#include "glow.h"
#include "inventory.h"
#include "log.h"
#include "scan.h"
#include "silhouette.h"

namespace glow {
namespace {

constexpr unsigned kMax = 64;
constexpr uint8_t kGroup = 6; // HighlightColorIndex Group3 (Find uses Group4, 7)

struct Span {
    const void* data;
    uint64_t count;
};
struct Field {
    const char* text;
    uint64_t length;
};
struct System {
    uint8_t* fn;
    unsigned reads, writes, modelSlot;
};
struct Target {
    uint32_t entity;
    float position[3];
    ULONGLONG until;  // kWaiting while your inventory is still open
    ULONGLONG lit;    // when Pulse lit it
    uint32_t ms;
};
constexpr ULONGLONG kWaiting = ~0ull;
constexpr ULONGLONG kLongestWait = 60000; // an inventory left open: the time runs anyway
struct Applied {
    void* renderer;
    void* registry;
    uint8_t* instance;
    uint32_t handle;
    uint8_t before;
    bool haveSilhouette;
    silhouette::Snapshot silhouette; // its mesh and pose, for the HUD (silhouette.h)
};

using NextFn = bool (*)(void* iter, void* view, uint32_t size);
using SetFn = void (*)(void* renderer, const Span* handles, const Span* colours);
using InstanceFn = uint8_t* (*)(void* renderer, uint32_t handle, bool valid);
using ColourFn = float* (*)(float* out, uint32_t colour);

NextFn g_next = nullptr;
SetFn g_set = nullptr;
InstanceFn g_instance = nullptr;
ColourFn g_colour[2] = {};
System g_systems[2] = {};

SRWLOCK g_lock = SRWLOCK_INIT;
Target g_targets[kMax];
unsigned g_count = 0;
Applied g_applied[kMax];
unsigned g_appliedCount = 0;
volatile LONG g_pulsing = 0;

template <class T> T Read(const void* p, size_t off = 0)
{
    T value;
    memcpy(&value, static_cast<const uint8_t*>(p) + off, sizeof(value));
    return value;
}

bool InImage(const void* p, size_t length)
{
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto at = reinterpret_cast<uintptr_t>(p);
    return at >= base && length <= nt->OptionalHeader.SizeOfImage && at - base <= nt->OptionalHeader.SizeOfImage - length;
}

bool Named(const Field& f, const char* text)
{
    size_t n = strlen(text);
    return f.length == n && InImage(f.text, n) && memcmp(f.text, text, n) == 0;
}

// The system's definition in .rdata: {name, length}, its function at +0x10, then four spans of
// fields at +0x38 (two read, two written); ModelComponent is among the written ones.
bool DiscoverSystem(const char* name, System& out)
{
    out = {};
    out.fn = SystemFunction(name);
    if (!out.fn)
        return false;
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (memcmp(sec->Name, ".rdata", 6) != 0)
            continue;
        for (size_t off = 0; off + 0x80 <= sec->Misc.VirtualSize; off += 8) {
            uint8_t* p = base + sec->VirtualAddress + off;
            if (!Named(Read<Field>(p), name) || Read<uint8_t*>(p, 0x10) != out.fn)
                continue;
            unsigned slot = 0;
            bool model = false;
            for (unsigned list = 0; list < 4; ++list) {
                auto span = Read<Span>(p, 0x38 + 0x10 * list);
                if (span.count > 32 || (span.count && !InImage(span.data, span.count * sizeof(Field))))
                    return false;
                for (unsigned i = 0; i < span.count; ++i, ++slot)
                    if (Named(Read<Field>(span.data, i * sizeof(Field)), "ModelComponent")) {
                        if (model || list != 2)
                            return false;
                        model = true;
                        out.modelSlot = slot;
                    }
                (list < 2 ? out.reads : out.writes) += static_cast<unsigned>(span.count);
            }
            return model;
        }
    }
    return false;
}

// A "call rel32" target, from the code before the loader's hooks.
uint8_t* CallTarget(uint8_t* call)
{
    uint8_t bytes[5];
    if (const GreeneyeHost* host = greeneye::Host())
        host->read(call, bytes, 5);
    else
        memcpy(bytes, call, 5);
    int32_t rel;
    memcpy(&rel, bytes + 1, 4);
    return bytes[0] == 0xe8 ? call + 5 + rel : nullptr;
}

void Set(void* renderer, uint32_t handle, uint8_t group)
{
    Span handles{&handle, 1}, groups{&group, 1};
    g_set(renderer, &handles, &groups);
}

void Restore(unsigned i)
{
    const Applied& a = g_applied[i];
    uint8_t* current = g_instance(a.renderer, a.handle, true);
    // A group another mod set (Find's or the map mod's 7) is not put back: that mod restores its
    // own, and once both had lit the chest it stayed lit with neither.
    if (current == a.instance && current && current[0x78] == kGroup)
        Set(a.renderer, a.handle, a.before == 6 || a.before == 7 ? 0 : a.before);
    g_applied[i] = g_applied[--g_appliedCount];
}

bool SamePlace(void* registry, uint32_t entity, const float* position)
{
    auto pos = static_cast<const int64_t*>(inv::Data(registry, inv::TypePosition(), entity));
    if (!pos)
        return false;
    for (int j = 0; j < 3; ++j)
        if (std::fabs(static_cast<float>(pos[j] / 4294967296.0) - position[j]) >= 0.15f)
            return false;
    return true;
}

// At the end of a render system's iteration, on its own thread.
void Apply(void* iter, void* view, const System& system)
{
    auto state = Read<uint8_t*>(iter);
    if (!state || Read<uint16_t>(state, 0x4b8) != system.reads || Read<uint16_t>(state, 0x4ba) != system.writes)
        return;
    void* registry = Read<void*>(state, 0x30);
    uint8_t* record = state + 0x1c8 + system.modelSlot * 0x10;
    uint16_t type = Read<uint16_t>(record, 0xa);
    if (type >= 1024 || Read<uint16_t>(record, 8) != 4)
        return;
    // Global resources come last in the view; these two layouts are 0xb8 and 0x58 bytes.
    size_t viewSize = system.reads == 6 && system.writes == 2 ? 0xb8 : system.reads == 2 && system.writes == 1 ? 0x58 : 0;
    if (!viewSize)
        return;
    void* scene = Read<void*>(view, viewSize - 8);
    void* renderer = scene ? Read<void*>(scene, 0x98) : nullptr;
    if (!renderer)
        return;
    void* registries[16] = {};
    int registryCount = inv::Registries(registries, 16);

    AcquireSRWLockExclusive(&g_lock);
    __try {
        ULONGLONG now = GetTickCount64();
        uint32_t handles[kMax];
        unsigned count = 0;
        bool active = false;
        for (unsigned i = 0; i < g_count; ++i)
            active |= g_targets[i].until > now;
        // Render jobs may run on views without a registry: the one whose entities sit where the
        // chests are is the render registry.
        for (int r = 0; active && !registry && r < registryCount; ++r)
            for (unsigned i = 0; i < g_count && !registry; ++i) {
                auto model = static_cast<const uint32_t*>(inv::Data(registries[r], type, g_targets[i].entity));
                if (model && *model && g_instance(renderer, *model, true) &&
                    SamePlace(registries[r], g_targets[i].entity, g_targets[i].position))
                    registry = registries[r];
            }
        for (unsigned i = 0; active && registry && i < g_count; ++i) {
            const Target& t = g_targets[i];
            if (t.until <= now)
                continue;
            auto model = static_cast<const uint32_t*>(inv::Data(registry, type, t.entity));
            if (!model || !*model || !SamePlace(registry, t.entity, t.position) || !g_instance(renderer, *model, true))
                continue;
            if (std::find(handles, handles + count, *model) == handles + count)
                handles[count++] = *model;
        }
        for (unsigned i = 0; i < g_appliedCount;) {
            const Applied& a = g_applied[i];
            if (a.renderer == renderer && (!registry || a.registry == registry) &&
                std::find(handles, handles + count, a.handle) == handles + count)
                Restore(i);
            else
                ++i;
        }
        for (unsigned i = 0; i < count; ++i) {
            uint8_t* instance = g_instance(renderer, handles[i], true);
            if (!instance)
                continue;
            unsigned j = 0;
            while (j < g_appliedCount && !(g_applied[j].renderer == renderer && g_applied[j].handle == handles[i]))
                ++j;
            if (j == g_appliedCount) {
                if (j == kMax)
                    continue;
                g_applied[g_appliedCount++] = {renderer, registry, instance, handles[i], instance[0x78], false, {}};
            }
            if (g_applied[j].instance != instance)
                continue;
            // The game's interaction highlight and Find's go first.
            if (instance[0x78] != 0 && instance[0x78] != kGroup)
                continue;
            Set(renderer, handles[i], kGroup);
            // The mesh to draw through walls: resource +0x108 -> asset +0x38; its mesh records
            // at asset + 0x6c + rel32 (Find's nativehighlight).
            Applied& a = g_applied[j];
            a.haveSilhouette = false;
            auto resource = Read<uint8_t*>(instance, 0x108);
            auto asset = resource ? Read<uint8_t*>(resource, 0x38) : nullptr;
            if (asset && Read<uint32_t>(asset, 0x70) > 0 && Read<uint32_t>(asset, 0x70) < 256) {
                auto mesh = asset + 0x6c + Read<int32_t>(asset, 0x6c);
                memcpy(a.silhouette.asset, asset, sizeof(a.silhouette.asset));
                memcpy(a.silhouette.mesh, mesh, sizeof(a.silhouette.mesh));
                memcpy(&a.silhouette.transform, instance + 0x38, sizeof(a.silhouette.transform));
                a.silhouette.alpha = 1.0f;
                a.haveSilhouette = true;
            }
        }
        if (!active)
            g_count = 0;
        InterlockedExchange(&g_pulsing, g_appliedCount ? 1 : 0);
    } __finally {
        ReleaseSRWLockExclusive(&g_lock);
    }
}

void SafeApply(void* iter, void* view, const System& system)
{
    __try {
        Apply(iter, view, system);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_pulsing, 0);
        static volatile LONG logged;
        if (!InterlockedExchange(&logged, 1))
            Log("glow: render layout check failed - chests are not lit");
    }
}

// Which render system is iterating is told by the iterator (Apply checks its read / write counts, 6/2 and 2/1), not
// by the return address: The Worst Map Mod redirects the same calls (its GG chest glow), and when the loader chains
// the two detours the second one's return address is the first detour.
bool NextHook(void* iter, void* view, uint32_t size)
{
    greeneye::Lease lease;
    bool next = g_next(iter, view, size);
    if (!lease || next)
        return next;
    for (const System& system : g_systems)
        SafeApply(iter, view, system);
    return next;
}

// Group 6's colour while chests are lit: green, breathing (as Find's gold does).
uint32_t Green(uint32_t colour)
{
    if (!InterlockedCompareExchange(&g_pulsing, 0, 0))
        return colour;
    float phase = static_cast<float>(GetTickCount64() % 1200) / 1200.0f;
    float gain = 0.4f + 0.6f * (0.5f - 0.5f * std::cos(phase * 6.283185307f));
    auto red = static_cast<uint32_t>(70 * gain), green = static_cast<uint32_t>(255 * gain), blue = static_cast<uint32_t>(110 * gain);
    return 0xff000000u | blue << 16 | green << 8 | red;
}

float* Colour0(float* out, uint32_t colour)
{
    greeneye::Lease lease;
    return g_colour[0](out, lease ? Green(colour) : colour);
}

float* Colour1(float* out, uint32_t colour)
{
    greeneye::Lease lease;
    return g_colour[1](out, lease ? Green(colour) : colour);
}

} // namespace

bool Init()
{
    auto next = FindUnique("40 57 48 83 EC 20 4C 8B 01 48 8B FA 8B 41 08 4C 8B C9 49 3B 80 ?? 08 00 00");
    auto setter = FindUnique("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 33 DB 4D 8B F0 48 8B FA "
                             "48 8B F1 48 39 5A 08 76 46 66 66 0F 1F 84 00 00 00 00 00 48 8B 07 41 B0 01 48 8B CE 8B "
                             "14 98 E8 ?? ?? ?? ?? 48 85 C0 74 1D 49 8B 0E 0F B6 14 0B 38 50 78");
    auto instance = FindUnique("85 D2 74 29 48 8B 81 C8 01 00 00 44 8B C2 41 81 E0 FF FF FF 00 C1 EA 18");
    // mov edx, [rbx + 60h]; call converter - group 6's palette entry, in both palette functions
    auto palette0 = FindUnique("8B 53 60 E8 ?? ?? ?? ?? 0F 10 18 48 8D 4C 24 70");
    auto palette1 = FindUnique("8B 53 60 E8 ?? ?? ?? ?? 0F 28 C6 48 8D 8D ?? ?? ?? ?? 0F 28 CE 0F 28 D6");
    if (!next || !setter || !instance || !palette0 || !palette1 ||
        !DiscoverSystem("dynamic_render_model", g_systems[0]) || !DiscoverSystem("static_render_model", g_systems[1])) {
        Log("glow: not found (next %p, set %p, instance %p, palettes %p/%p) - deposited chests are not lit", next, setter,
            instance, palette0, palette1);
        return false;
    }
    // Both call the same converter, as the code was before any hook (a reload's redirects differ).
    if (CallTarget(palette0 + 3) != CallTarget(palette1 + 3)) {
        Log("glow: the palette calls differ - deposited chests are not lit");
        return false;
    }
    g_set = reinterpret_cast<SetFn>(setter);
    g_instance = reinterpret_cast<InstanceFn>(instance);
    static const char* const systems[] = {"dynamic_render_model", "static_render_model"};
    int calls, found;
    g_next = static_cast<NextFn>(RedirectSystemCalls(next, systems, 2, reinterpret_cast<void*>(&NextHook), calls, found));
    g_colour[0] = static_cast<ColourFn>(RedirectCall(palette0 + 3, reinterpret_cast<void*>(&Colour0)));
    g_colour[1] = static_cast<ColourFn>(RedirectCall(palette1 + 3, reinterpret_cast<void*>(&Colour1)));
    bool ok = g_next && g_colour[0] && g_colour[1];
    Log(ok ? "glow: ready (%d iterator calls in %d render systems)" : "glow: redirect failed (%d calls, %d systems)", calls, found);
    return ok;
}

void Pulse(const uint32_t* chests, int count, uint32_t ms)
{
    if (!g_next || count <= 0)
        return;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < count; ++i) {
        float pos[3];
        if (!inv::Position(chests[i], pos))
            continue;
        unsigned j = 0;
        while (j < g_count && g_targets[j].entity != chests[i])
            ++j;
        if (j == g_count) {
            if (g_count == kMax)
                continue;
            ++g_count;
        }
        g_targets[j] = {chests[i], {pos[0], pos[1], pos[2]}, kWaiting, now, ms};
    }
    ReleaseSRWLockExclusive(&g_lock);
}

unsigned Snapshots(silhouette::Snapshot* out, unsigned max)
{
    unsigned n = 0;
    AcquireSRWLockShared(&g_lock);
    for (unsigned i = 0; i < g_appliedCount && n < max; ++i)
        if (g_applied[i].haveSilhouette)
            out[n++] = g_applied[i].silhouette;
    ReleaseSRWLockShared(&g_lock);
    return n;
}

#ifdef QD_BRIDGE
void TestStop() {
    AcquireSRWLockExclusive(&g_lock);
    g_count=0; // Existing render-thread Apply restores native highlights.
    ReleaseSRWLockExclusive(&g_lock);
}
#endif
void Tick(bool screenOpen)
{
    if (!g_next)
        return;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_lock);
    for (unsigned i = 0; i < g_count; ++i) {
        Target& t = g_targets[i];
        if (t.until == kWaiting && (!screenOpen || now - t.lit > kLongestWait))
            t.until = now + t.ms;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace glow

// Dev only: event counts (build 1273043).
//
// 140956670 AllocEvent(queue, typeHash, size, flags) reserves an event in a world's event queue;
// its caller then fills it in. These are local ECS events: the ones the host needs reach it
// another way (player actions in the replicated input), so counting them shows what an action
// does on this client, not what it sends.
#ifdef QD_BRIDGE
#include <windows.h>
#include <atomic>
#include <map>
#include <mutex>
#include "../../common/greeneye.h"
#include "log.h"
#include "scan.h"
#include "station.h"

namespace station {
namespace {

using AllocEventFn = void* (*)(void* queue, uint32_t typeHash, uint32_t size, uint16_t flags);
AllocEventFn g_originalAlloc = nullptr;

std::atomic<bool> g_counting{false};
std::mutex g_countLock;
std::map<uint64_t, uint32_t> g_counts; // typeHash << 32 | size -> events allocated

void* AllocHook(void* queue, uint32_t typeHash, uint32_t size, uint16_t flags)
{
    greeneye::Lease lease;
    if (lease && g_counting) {
        std::lock_guard<std::mutex> lock(g_countLock);
        ++g_counts[static_cast<uint64_t>(typeHash) << 32 | size];
    }
    return g_originalAlloc(queue, typeHash, size, flags);
}

} // namespace

bool Init()
{
    // AllocEvent: sub rsp, 28h; mov r11, rcx; cmp r8d, 100h (14 bytes)
    auto* alloc = FindUnique("48 83 EC 28 4C 8B D9 41 81 F8 00 01 00 00");
    g_originalAlloc = alloc ? static_cast<AllocEventFn>(InstallHook(alloc, reinterpret_cast<void*>(&AllocHook), 14)) : nullptr;
    if (!g_originalAlloc)
        Log("events: AllocEvent hook unavailable");
    return g_originalAlloc != nullptr;
}

void CountEvents(bool on)
{
    std::lock_guard<std::mutex> lock(g_countLock);
    if (on)
        g_counts.clear();
    g_counting = on;
}

std::vector<EventCount> EventCounts()
{
    std::lock_guard<std::mutex> lock(g_countLock);
    std::vector<EventCount> out;
    for (const auto& [key, n] : g_counts)
        out.push_back({static_cast<uint32_t>(key >> 32), static_cast<uint32_t>(key), n});
    return out;
}

} // namespace station
#endif

// Client -> host network events (build 1269579).
//
// 140955400 AllocEvent(queue, typeHash, size, flags): reserves an event in the world's outgoing
//   queue; 140955480 is the same with queue = *(world + 0x1ccfcb0).
// Type hashes are FNV-1a 32 of the full type name.
#include <windows.h>
#include <cstring>
#include "events.h"
#include "log.h"
#include "scan.h"

namespace events {
namespace {

using AllocEventFn = void* (*)(void* queue, uint32_t typeHash, uint32_t size, uint16_t flags);
AllocEventFn g_allocEvent = nullptr;

constexpr size_t kClientWorld = 0x47d4e0;
constexpr size_t kClientEventHeader = 0x47d4c8;
constexpr size_t kClientPlayer = 0x466b44;
constexpr size_t kWorldEventQueue = 0x1ccfcb0;

constexpr uint32_t kAcceptOfferedInteraction = 0x82e404b2; // keen::ecs::AcceptOfferedInteractionEvent
constexpr uint32_t kAcceptOfferedInteractionSize = 0x18;

void* EventQueue(void* client)
{
    __try {
        auto* worldHolder = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(client) + kClientWorld);
        if (!worldHolder)
            return nullptr;
        auto* world = *reinterpret_cast<uint8_t**>(worldHolder + 8);
        return world ? *reinterpret_cast<void**>(world + kWorldEventQueue) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

uint32_t LocalPlayer(void* client)
{
    uint32_t h = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(client) + kClientPlayer);
    return h ? (h & 0x3f) + 1 : 0;
}

} // namespace

bool Init()
{
    g_allocEvent = reinterpret_cast<AllocEventFn>(FindUnique("48 83 EC 28 4C 8B D9 41 81 F8 00 01 00 00"));
    if (!g_allocEvent)
        Log("events: AllocEvent signature not found");
    return g_allocEvent != nullptr;
}

bool AcceptInteraction(void* client, uint32_t host, uint32_t offerId)
{
    if (!g_allocEvent || !client)
        return false;
    void* queue = EventQueue(client);
    if (!queue)
        return false;
    __try {
        auto* e = static_cast<uint8_t*>(g_allocEvent(queue, kAcceptOfferedInteraction, kAcceptOfferedInteractionSize, 0));
        if (!e)
            return false;
        memcpy(e, static_cast<uint8_t*>(client) + kClientEventHeader, 8);
        *reinterpret_cast<uint32_t*>(e + 0x08) = LocalPlayer(client); // guestEntityId
        *reinterpret_cast<uint32_t*>(e + 0x0c) = host;                // hostEntityId
        *reinterpret_cast<uint32_t*>(e + 0x10) = offerId;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace events

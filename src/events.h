#pragma once
#include <cstdint>

// Network events the client sends to the host, built the way the game's own UI code builds them
// (see UiDispatch 140851530: world = *(*(client + 0x47d4e0) + 8), event = AllocEvent(...),
// header = client + 0x47d4c8, actor = local player id).
namespace events {

// Finds the event allocator. Call once at startup.
bool Init();

// "Accept the interaction offer of host" as the local player, like pressing E on it
// (keen::ecs::AcceptOfferedInteractionEvent). The host checks range and permission.
bool AcceptInteraction(void* client, uint32_t host, uint32_t offerId);

} // namespace events

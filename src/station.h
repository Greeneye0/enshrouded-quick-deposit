#pragma once
#ifdef QD_BRIDGE
#include <cstdint>
#include <vector>

// Dev only: counts the events the client allocates (140956670 AllocEvent), by type hash and
// size, to see which ones an action in the game sends.
namespace station {

// Hooks AllocEvent. Call before the game's code runs.
bool Init();

struct EventCount {
    uint32_t typeHash, size, count;
};
// on: start counting (cleared); off: stop. The counts so far either way.
void CountEvents(bool on);
std::vector<EventCount> EventCounts();

} // namespace station
#endif

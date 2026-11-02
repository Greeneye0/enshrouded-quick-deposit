#pragma once
#include <cstdint>
#include "silhouette.h"

// Chests a deposit filled light up: their models get the game's own outline highlight, pulsing
// green, for a few seconds (the way Find lights the chests it finds, in another colour).
namespace glow {

// Finds the render systems and redirects their calls (see glow.cpp). Call before the game's code
// runs; false (logged) if this game build does not match.
bool Init();
#ifdef QD_BRIDGE
void TestStop();
#endif

// Lights these chests: while your inventory stays open, then for ms milliseconds (UI thread).
void Pulse(const uint32_t* chests, int count, uint32_t ms);

// Once per UI frame: screenOpen while an inventory or chest is on screen.
void Tick(bool screenOpen);

// The lit chests' meshes and poses, for the silhouettes drawn through walls; returns how many.
unsigned Snapshots(silhouette::Snapshot* out, unsigned max);

} // namespace glow

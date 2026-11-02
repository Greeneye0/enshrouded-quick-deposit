#pragma once
#include <cstdint>

// The lit chests drawn over the HUD as green silhouettes, so they show through walls (Find's
// silhouettes, FindMod/src/silhouette.cpp, in Quick Deposit's colour).
namespace silhouette {

// A model's pose, as its render instance holds it (+0x38).
struct Spatial {
    int64_t position[3]; // 32 fraction bits
    float rotation[4];
    float scale[3];
    uint32_t pad;
};
static_assert(sizeof(Spatial) == 56);

// One lit chest: its model asset and mesh records (copied from the renderer) and its pose.
struct Snapshot {
    unsigned char asset[0x160], mesh[56];
    Spatial transform;
    float alpha;
};

// Hooks the HUD's spatial elements pass, which then draws glow::Snapshots. Call before the
// game's code runs.
bool Init();
#ifdef QD_BRIDGE
struct TestStats { unsigned frames=0, targets=0, details=0, markers=0; double totalMs=0,maxMs=0; };
TestStats TestRead(bool reset);
#endif

} // namespace silhouette

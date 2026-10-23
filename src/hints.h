#pragma once
#include <cstdint>

// Quick Deposit's key hints in the inventory's hint bar, next to the game's "G Sort Inventory":
// "G Twice: Quick Deposit" and "K Keep in Backpack".
namespace hints {

// Hooks the hint bar. Call before the game's code runs. depositHint: G twice is on;
// keepKey: the keep marks' key (a virtual-key code, 0: no keep hint).
bool Init(bool depositHint, int keepKey);

#ifdef QD_BRIDGE
// Dev only: the entries the hint bar last drew, one line each (action, flags, text).
const char* LastEntries();
#endif

} // namespace hints

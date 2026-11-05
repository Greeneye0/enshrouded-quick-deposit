#pragma once
#include <cstdint>
#include "../../common/input_binds.h"

// Quick Deposit's key hints in the inventory's hint bar, next to the game's "G Sort Inventory":
// "G Twice: Quick Deposit" and "K Keep in Backpack".
namespace hints {

// Hooks the hint bar. Call before the game's code runs. depositHint: G twice is on.
bool Init(bool depositHint);
// The keep marks' key and controller button; unbound: no keep hint.
void SetKeep(const greeneye::input::Binding& binding);

#ifdef QD_BRIDGE
// Dev only: the entries the hint bar last drew, one line each (action, flags, text).
const char* LastEntries();
#endif

} // namespace hints

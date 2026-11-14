#pragma once
#include <cstdint>

// A voice line in the character's voice (voicepack.h), played on this PC only; noFun=true in the
// ini turns it off.
namespace voiceline {

void SetEnabled(bool on);

// Every keep key press on an item (keep.cpp), game thread.
void Pressed(uint32_t itemId);

} // namespace voiceline

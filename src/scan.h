#pragma once
#include <cstdint>

// Finds a unique byte pattern ("48 8B ?? 05", ?? = any byte) in the game's .text section.
// Returns nullptr if the pattern is missing or occurs more than once. A function another mod
// hooked first is still found, by the bytes behind its jump.
uint8_t* FindUnique(const char* pattern, void* module = nullptr);

// Finds every match of pattern (as FindUnique) and resolves the rip-relative operand whose
// 32-bit displacement starts dispOffset bytes into the match, the instruction ending at
// insEnd. Returns the target only if all matches agree on it.
uint8_t* ResolveRipConsensus(const char* pattern, int dispOffset, int insEnd);

// Resolves the target of a rip-relative "lea r64, [rip+disp32]" (7 bytes, 48/4C 8D) at addr.
uint8_t* ResolveLea(uint8_t* addr);

// Redirects the function at target to detour by overwriting its first length bytes (at least
// 12) with "mov rax, imm64; jmp rax" plus padding. Those bytes must be whole,
// position-independent instructions. Returns a callable trampoline for the original
// function, or nullptr on failure. If another mod already hooked target (IsHookPatch), its
// jump is pointed at detour instead and its previous destination returned
// (common/jumppatch.h).
void* InstallHook(uint8_t* target, void* detour, size_t length = 12);

// True if target starts with a hook jump another mod is known to write: "mov rax, imm64;
// jmp rax" (our mods), or safetyhook's "jmp rel32" / "jmp [rip+disp32]" (Creative Mode).
bool IsHookPatch(const uint8_t* target);

// The ItemInfoLookup hook ("mov [rsp+8], ecx; sub rsp, 28h; test ecx, ecx; jz short": the short
// jump is relocated into the trampoline). Returns the trampoline, or nullptr.
void* InstallItemLookupHook(uint8_t* fn, void* detour);

// The game's .text section.
bool GameText(uint8_t*& start, size_t& size);

// Hot reload: match patterns in this copy of the game's code (taken before any hook) and
// return the matching addresses in the real code.
void UseCodeSnapshot(const uint8_t* copy, uint8_t* real, size_t size);


// Hooks one call instead of the function it calls: the "call rel32" at call goes to detour,
// which calls on through the returned function (the game's, or another mod's stub). The called
// function's own code is left as it is, for other mods that look it up by its bytes (XHL).
void* RedirectCall(uint8_t* call, void* detour);

// The start of the function whose code includes code (any of its chunks), or null.
uint8_t* FunctionStart(const uint8_t* code);

// Every "call rel32" in the game's .text that calls target (as the code was before any of the
// loader's hooks). Returns how many were written.
int CallsTo(const uint8_t* target, uint8_t** out, int max);

// The function of the game's ECS system called name, from the system's definition in .rdata
// ({name text, length}, the system's function 0x10 bytes on). Null unless exactly one function
// is defined under that name.
uint8_t* SystemFunction(const char* name);

// Redirects target's calls from inside the named systems' functions to detour (RedirectCall);
// target itself and its other calls stay as they are. Returns the function to call on, or null
// unless every one of those calls was redirected. calls/systems receive the counts.
void* RedirectSystemCalls(const uint8_t* target, const char* const* names, int count, void* detour,
                          int& calls, int& systems);

#pragma once
#include <cstdint>

// Finds a unique byte pattern ("48 8B ?? 05", ?? = any byte) in the game's .text section.
// Returns nullptr if the pattern is missing or occurs more than once.
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
// function, or nullptr on failure.
void* InstallHook(uint8_t* target, void* detour, size_t length = 12);

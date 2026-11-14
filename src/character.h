#pragma once
#include <string>

// Which of your characters is being played, so each keeps its own list of kept items, the same
// in every world.
//
// The game keeps each character in its own save file, characterN.ksd (Steam: userdata\<you>\
// 1203620\remote; else Saved Games\Enshrouded), and characters are saved on your PC whatever
// world you play in. Each file starts with "SV2C", a version and, at +0x0c, a 16-byte id the
// character keeps for good (it is the same in every save and backup of that character). While you
// play, the game writes your character's file within about 30 s of a change (an item picked up,
// moved or deposited), and no other character's. So the first character file written after you
// enter a world is the one you play.
//
// Faster and first: the game's save manager holds the id of the character it loaded, read as soon
// as you are in a world (character.cpp). The save file is the fallback when a game update moves it.
namespace character {

// Reads lastCharacter and finds the save manager. Call once at start.
void Init();

// Every frame (UI thread): the client object, and inWorld while you are in a world.
void Tick(void* client, bool inWorld);

// The id (32 hex digits) of the character being played, or the last one played; "" before any.
std::string Current();

// True once this world visit has shown which character is played (the save manager, or its save
// file was written).
bool Confirmed();

// Counts world visits: changes each time you enter a world. InWorld: you are in one.
unsigned Visit();
bool InWorld();

// True once the save manager was found or the save folders were looked for; Tracking(): the
// character can be told (either was found).
bool Ready();
bool Tracking();

// The character save written last (characterN.ksd, the one being played), "" if none was found.
std::wstring NewestSave();

} // namespace character

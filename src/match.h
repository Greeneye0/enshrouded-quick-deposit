#pragma once
#include <cstdint>
#include <tuple>

#include "inventory.h"

// Which chest an item belongs with, judged by what the chests hold and are named: functions of
// item descriptions only (no game memory), so the deposit planner (client.cpp) and the offline
// test (test/chest_choice.cpp) run the same code.
namespace match {

// keen::ItemCategory
constexpr uint8_t kCategoryEquipment = 1;
constexpr uint8_t kCategoryWeapons = 2;
constexpr uint8_t kCategoryBuilding = 6; // BuildTools: blocks, but also farm animals and props
constexpr uint8_t kCategoryConsumables = 7;
constexpr uint8_t kCategoryAmmunition = 8;

bool SameGroup(const inv::ItemDesc& a, const inv::ItemDesc& b);
bool IsSeedling(const inv::ItemDesc& d);
bool IsTool(const inv::ItemDesc& d);
int Placeable(const inv::ItemDesc& d);
constexpr int kSameState = 1, kSameSort = 2; // ConsumableMatch
int ConsumableMatch(const inv::ItemDesc& a, const inv::ItemDesc& b);
bool SameKind(const inv::ItemDesc& a, const inv::ItemDesc& b);
bool AmmoFits(const inv::ItemDesc& ammo, const inv::ItemDesc& weapon);
int Words(const char* text, char (*out)[24], int max);
// The words of the name the game shows (Words), else of the readable internal name.
int NameWords(const inv::ItemDesc& d, char (*out)[24], int max);
bool SharesWord(const inv::ItemDesc& d, const char (*words)[24], int count);
uint8_t SlotOfWord(const char* word);
// WordScore: a word of the item's own name in the chest's name counts this much.
constexpr int kOwnWordScore = 3;
int WordScore(const char* chestName, const inv::ItemDesc& d);

// How well a chest fits an item by kind, strongest evidence first (compared field by field):
// sort: consumables of the same sort and state (raw meat, potions of health); kin: of the same
// sort in any state (stew with grilled meat, not with raw meat's "bad meat"); word: items of the kind whose
// names share a word with it (Fossilized Bone and Giant Bones); name: how well the chest's name
// fits it (NameScore); state: consumables of the same state (raw, processed, dish); kind: items of
// the kind (SameKind, or weapons its ammo fits). The most items of the category alone says least:
// a chest of odds and ends wins that.
struct KindFit {
    int sort = 0, kin = 0, word = 0, name = 0, state = 0, kind = 0;
    // A chest holds items of the kind, or its name has a word of the item's own.
    bool Any() const { return kind > 0 || name >= kOwnWordScore; }
    bool operator>(const KindFit& o) const
    {
        return std::tie(sort, kin, word, name, state, kind) >
               std::tie(o.sort, o.kin, o.word, o.name, o.state, o.kind);
    }
};

// held: the different items the chest holds, each once; nameScore: NameScore of its name.
KindFit FitByKind(const inv::ItemDesc& d, const inv::ItemDesc* held, int count, int nameScore);

} // namespace match

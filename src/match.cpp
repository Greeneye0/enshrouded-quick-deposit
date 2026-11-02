#include <cctype>
#include <cstring>

#include "match.h"

namespace inv {

// "Weapon_T1_1H_WandEpic_01_Fire" -> "Wand Epic Fire", "Ring_T1_Ring_of_Mana" -> "Ring of Mana":
// drops the asset group (first token), tier, hand, level and numbering tokens of the internal
// name and splits run-together words.
void PrettyName(const char* raw, char* out, size_t size)
{
    static const char* const groups[] = {"Material", "Weapon", "Food", "Build", "Block", "Ammo", "Consumable",
                                         "Armor", "Tool", "Ring", "Vanity", "Shield", "Blueprint"};
    static const char* const anywhere[] = {"TerrainMaterial", "Voxel", "CRAFTING", "1H", "2H", "raw", "processed", "Loot"};
    char buf[96];
    strncpy_s(buf, raw, _TRUNCATE);
    size_t len = 0;
    out[0] = 0;
    char* ctx = nullptr;
    bool first = true;
    for (char* tok = strtok_s(buf, "_", &ctx); tok; tok = strtok_s(nullptr, "_", &ctx), first = false) {
        bool skip = (tok[0] == 'T' && (isdigit(static_cast<unsigned char>(tok[1])) || tok[1] == 'X')) ||
                    strncmp(tok, "lvl", 3) == 0 || isdigit(static_cast<unsigned char>(tok[0]));
        for (const char* g : groups)
            skip |= first && strcmp(tok, g) == 0;
        for (const char* d : anywhere)
            skip |= strcmp(tok, d) == 0;
        if (skip)
            continue;
        for (const char* c = tok; *c && len + 2 < size; ++c) {
            bool wordStart = c == tok || (isupper(static_cast<unsigned char>(*c)) && islower(static_cast<unsigned char>(c[-1])));
            if (wordStart && len)
                out[len++] = ' ';
            out[len++] = *c;
        }
        out[len] = 0;
    }
    if (!out[0])
        strncpy_s(out, size, raw, _TRUNCATE);
}

const char* CategoryName(uint8_t category)
{
    static const char* const names[] = {"Customization", "Equipment", "Weapons", "Tools", "Instrument", "FishingRod",
                                        "BuildTools", "Consumables", "Ammunition", "Materials", "Blueprints", "Currency",
                                        "Collectibles", "AnimalFood", "PetFood", "WeaponGem", "ColorPalette"};
    return category < sizeof(names) / sizeof(names[0]) ? names[category] : "?";
}

} // namespace inv

namespace match {

// The asset group an item's internal name starts with: "Armor" in "Armor_T0_Fur_Legs", "Ring"
// in "Ring_T1_Ring_of_Mana". Splits equipment into armor, rings, shields, backpacks...
bool SameGroup(const inv::ItemDesc& a, const inv::ItemDesc& b)
{
    const char* ea = strchr(a.raw, '_');
    const char* eb = strchr(b.raw, '_');
    size_t la = ea ? static_cast<size_t>(ea - a.raw) : strlen(a.raw);
    size_t lb = eb ? static_cast<size_t>(eb - b.raw) : strlen(b.raw);
    return la && la == lb && strncmp(a.raw, b.raw, la) == 0;
}

// Seedlings share the building-materials category with blocks (both are placed in the world),
// but a seed chest is no place for blocks.
bool IsSeedling(const inv::ItemDesc& d)
{
    return _strnicmp(d.raw, "Seedling", 8) == 0 || _strnicmp(d.name, "Seedling", 8) == 0;
}

// The game's building category also holds what it shows as Tools: pickaxes, rakes, the watering
// can ("Tool_...") and NPCs' summoning staffs ("NPC_..."). They go with tools, not with blocks.
bool IsTool(const inv::ItemDesc& d)
{
    return _strnicmp(d.raw, "Tool_", 5) == 0 || _strnicmp(d.raw, "NPC_", 4) == 0 ||
           _strnicmp(d.raw, "NpcSummoner", 11) == 0;
}

// What else the building category holds besides blocks: animals for farms ("Animal Farming Yak")
// and props ("Prop Decoration Door ..."), each its own kind. 0: a block, roof or other material.
int Placeable(const inv::ItemDesc& d)
{
    if (_strnicmp(d.name, "Animal", 6) == 0 || _strnicmp(d.raw, "Animal", 6) == 0)
        return 1;
    if (_strnicmp(d.name, "Prop", 4) == 0 || _strnicmp(d.raw, "Prop", 4) == 0)
        return 2;
    return 0;
}

// Consumables are one category, but their internal names say more: "Food_T3_raw_bad_meat_Bird"
// is raw food of the raw-meat sort, "Consumable_Potion_Health_T1" a potion of the health sort.
// Flags: kSameState (same group and state: raw, processed, dish), kSameSort (same group and sort,
// in any state: stew, grilled and raw meat). Both: the same sort of food as it.
int ConsumableMatch(const inv::ItemDesc& a, const inv::ItemDesc& b)
{
    if (a.category != kCategoryConsumables || b.category != kCategoryConsumables)
        return 0;
    // Group, state (food only) and sort, tier tokens ("T3") left out.
    auto split = [](const char* raw, char (*tok)[24]) {
        char buf[96];
        strncpy_s(buf, raw, _TRUNCATE);
        int n = 0;
        char* ctx = nullptr;
        for (char* t = strtok_s(buf, "_", &ctx); t && n < 4; t = strtok_s(nullptr, "_", &ctx)) {
            if (t[0] == 'T' && isdigit(static_cast<unsigned char>(t[1])))
                continue;
            if (n == 3) {
                // "bad" (raw food that poisons) is no sort by itself: bad meat, bad fish.
                if (_stricmp(tok[2], "bad") != 0)
                    break;
                strcat_s(tok[2], 24, "_");
                strcat_s(tok[2], 24, t);
                break;
            }
            strncpy_s(tok[n++], 24, _stricmp(t, "proccessed") == 0 ? "processed" : t, _TRUNCATE); // the game's typo
            if (n == 1 && _stricmp(t, "Food") != 0)
                tok[n++][0] = 0; // only food has a state
        }
        for (n = n > 3 ? 3 : n; n < 3; ++n)
            tok[n][0] = 0;
    };
    char ta[3][24], tb[3][24];
    split(a.raw, ta);
    split(b.raw, tb);
    if (!ta[0][0] || _stricmp(ta[0], tb[0]) != 0)
        return 0;
    // The game names one sort "vegetable" and "vegetables": a plural "s" doesn't count.
    auto sameSort = [](const char* x, const char* y) {
        size_t lx = strlen(x), ly = strlen(y);
        lx -= lx > 3 && (x[lx - 1] == 's' || x[lx - 1] == 'S');
        ly -= ly > 3 && (y[ly - 1] == 's' || y[ly - 1] == 'S');
        return lx && lx == ly && _strnicmp(x, y, lx) == 0;
    };
    return (_stricmp(ta[1], tb[1]) == 0 ? kSameState : 0) | (sameSort(ta[2], tb[2]) ? kSameSort : 0);
}

// Same category; weapons also need the same weapon type, ammunition the same kind of ammo
// (arrows, spells, fishing bait), equipment the same group (armor, rings, shields...),
// seedlings go only with seedlings and tools only with tools, blocks apart from farm animals and
// props.
bool SameKind(const inv::ItemDesc& a, const inv::ItemDesc& b)
{
    if (!a.known || !b.known || a.category != b.category)
        return false;
    if (IsSeedling(a) != IsSeedling(b) || IsTool(a) != IsTool(b))
        return false;
    if (a.category == kCategoryWeapons)
        return memcmp(a.weaponType, b.weaponType, sizeof(a.weaponType)) == 0;
    if (a.category == kCategoryAmmunition)
        return a.ammoType == b.ammoType;
    if (a.category == kCategoryEquipment) {
        // Where it is worn: helmets with helmets, boots with boots, every shield (wards too) with
        // shields. The type reference is finer than the slot (a ward and a shield differ), so the
        // slot decides.
        if (a.equipSlot && b.equipSlot)
            return a.equipSlot == b.equipSlot;
        return SameGroup(a, b);
    }
    if (a.category == kCategoryBuilding)
        return Placeable(a) == Placeable(b);
    return true;
}

// Ammunition belongs with the weapons that fire it, the way the game labels arrows "Bow Ammo":
// arrows with bows, spell charges with staves and wands.
bool AmmoFits(const inv::ItemDesc& ammo, const inv::ItemDesc& weapon)
{
    if (!ammo.known || !weapon.known || ammo.category != kCategoryAmmunition || weapon.category != kCategoryWeapons)
        return false;
    switch (ammo.ammoType) {
    case 1: return strstr(weapon.raw, "Bow") != nullptr;
    case 2: return strstr(weapon.raw, "Staff") != nullptr || strstr(weapon.raw, "Wand") != nullptr;
    default: return false;
    }
}

// Lower-case words of at least 3 letters, plural "s" dropped, split at "_", spaces,
// punctuation and inner capitals ("WandEpic" -> wand, epic). Returns how many were written.
int Words(const char* text, char (*out)[24], int max)
{
    static const char* const ignore[] = {"and", "the", "misc", "chest", "stuff", "other", "epic", "craft"};
    int n = 0;
    char word[24];
    int len = 0;
    auto flush = [&]() {
        if (len >= 3 && n < max) {
            word[len] = 0;
            if (len > 3 && word[len - 1] == 's')
                word[len - 1] = 0;
            bool skip = false;
            for (const char* i : ignore)
                skip |= strcmp(word, i) == 0;
            if (!skip)
                strcpy_s(out[n++], word);
        }
        len = 0;
    };
    for (const char* c = text;; ++c) {
        unsigned char ch = static_cast<unsigned char>(*c);
        bool letter = isalpha(ch) != 0;
        if (letter && len && isupper(ch) && islower(static_cast<unsigned char>(c[-1])))
            flush();
        if (letter) {
            if (len < 23)
                word[len++] = static_cast<char>(tolower(ch));
        } else {
            flush();
            if (!ch)
                break;
        }
    }
    return n;
}

// Whether an item's name shares a word with the words of another ("Fossilized Bone" and "Giant
// Bones" share "bone").
int NameWords(const inv::ItemDesc& d, char (*out)[24], int max)
{
    // The internal name says less than it seems ("Material_T2_SlurpBroth" is Ectoplasm), so the
    // name the game shows, once known.
    int n = d.shown[0] && strcmp(d.shown, d.name) != 0 ? Words(d.shown, out, max) : 0;
    return n ? n : Words(d.name, out, max);
}

bool SharesWord(const inv::ItemDesc& d, const char (*words)[24], int count)
{
    char own[12][24];
    int n = NameWords(d, own, 12);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < count; ++j)
            if (strcmp(own[i], words[j]) == 0)
                return true;
    return false;
}

// Words a chest named for an equipment slot uses (as Words() writes them), by slot number.
// "Chest" is no word to Words(), so chest pieces go by "body" or "torso".
struct SlotWords {
    uint8_t slot;
    const char* words[7];
};
const SlotWords kSlotWords[] = {
    {0x0d, {"head", "helmet", "helm", "hat", "hood", "mask"}},
    {0x0e, {"body", "torso", "chestpiece", "breastplate"}},
    {0x0f, {"hand", "glove", "gauntlet", "arm", "bracer"}},
    {0x10, {"leg", "pant", "trouser", "legging"}},
    {0x11, {"foot", "feet", "boot", "shoe"}},
    {0x12, {"shield", "ward", "buckler"}},
    {0x25, {"ring"}},
    {0x26, {"backpack", "bag"}},
};

// The slot a word names, 0 for none.
uint8_t SlotOfWord(const char* word)
{
    for (const SlotWords& s : kSlotWords)
        for (const char* w : s.words)
            if (w && strcmp(word, w) == 0)
                return s.slot;
    return 0;
}

// How well a chest's name fits the item word by word: a word of the item's own name ("Wand",
// "Fire") counts 3, a word only found in its asset group or category ("Weapon", "Materials")
// counts 1, so "Wands" beats "Melee Weapons" for a wand. For equipment the slot's words count as
// its own name ("Helmets" for a helmet), and a chest named for another slot ("Boots") never fits.
int WordScore(const char* chestName, const inv::ItemDesc& d)
{
    char chestWords[8][24], ownWords[24][24], groupWords[24][24];
    int nc = Words(chestName, chestWords, 8);
    int no = Words(d.name, ownWords, 12);
    if (d.shown[0] && strcmp(d.shown, d.name) != 0)
        no += Words(d.shown, ownWords + no, 24 - no); // the name as shown ("Ectoplasm", not "Slurp Broth")
    int ng = Words(d.raw, groupWords, 20);
    ng += Words(inv::CategoryName(d.category), groupWords + ng, 24 - ng);
    int score = 0;
    const uint8_t slot = d.category == kCategoryEquipment ? d.equipSlot : 0;
    for (int i = 0; i < nc; ++i) {
        if (uint8_t named = slot ? SlotOfWord(chestWords[i]) : 0) {
            if (named != slot)
                return 0;
            score += 3;
            continue;
        }
        int best = 0;
        for (int k = 0; k < no && best < 3; ++k)
            if (strcmp(chestWords[i], ownWords[k]) == 0)
                best = 3;
        for (int k = 0; k < ng && !best; ++k)
            if (strcmp(chestWords[i], groupWords[k]) == 0)
                best = 1;
        score += best;
    }
    return score;
}


KindFit FitByKind(const inv::ItemDesc& d, const inv::ItemDesc* held, int count, int nameScore)
{
    char words[12][24];
    const int wordCount = NameWords(d, words, 12);
    KindFit f;
    f.name = nameScore;
    for (int i = 0; i < count; ++i) {
        const inv::ItemDesc& cd = held[i];
        if (!SameKind(cd, d) && !AmmoFits(d, cd))
            continue;
        ++f.kind;
        int m = ConsumableMatch(cd, d);
        f.sort += m == (kSameState | kSameSort);
        f.kin += (m & kSameSort) != 0;
        f.state += (m & kSameState) != 0;
        f.word += SharesWord(cd, words, wordCount);
    }
#ifdef QD_BASELINE // test only: the rule of 1.3.15 (the most items of the kind), to compare against
    f = KindFit{0, 0, 0, 0, 0, f.kind};
#endif
    return f;
}

} // namespace match

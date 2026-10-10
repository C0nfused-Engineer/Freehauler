// goods.h - everything you can carry and trade: forty goods in three tiers.
//
//   Raw        dug, pumped, skimmed or grown: ores and ices (which also come
//              out of asteroids), helium-3 (skimmed from gas giants), crops
//   Refined    made from raw materials: metals, chemicals, fuel, food
//   Finished   made from refined goods (and the odd raw one): machines,
//              electronics, ship parts, weapons, what people buy
//
// Every refined and finished good has a recipe: the tonnes of each input that
// go into a tonne of it. A place that makes something needs its inputs, so a
// shortage upstream is felt downstream (economy.h).
//
// Saves and the network store cargo by number: new goods go on the END of
// their tier's block only with a new save version.
#pragma once
#include "raylib.h"

enum Good {
    // --- Raw ---
    ORE_IRON, ORE_COPPER, ORE_TITANIUM, ORE_PRECIOUS, ORE_RARE_EARTHS, ORE_URANIUM, ORE_SILICATES, ORE_CARBON,
    ORE_WATER_ICE, ORE_VOLATILES,                  // (the ten above are what asteroids and ring chunks hold)
    RAW_HELIUM3,                                   // skimmed from gas giants
    RAW_GRAIN, RAW_LIVESTOCK, RAW_FLORA,           // grown
    // --- Refined ---
    REF_STEEL, REF_ALLOYS, REF_CONDUCTORS, REF_SILICON, REF_CERAMICS, REF_POLYMERS, REF_CHEMICALS, REF_FERTILIZER,
    REF_WATER, REF_FUEL, REF_REACTOR_FUEL, REF_FOOD,
    // --- Finished ---
    GOOD_MACHINERY, GOOD_ELECTRONICS, GOOD_ROBOTICS, GOOD_HULL_PLATING, GOOD_SHIP_COMPONENTS, GOOD_REACTOR_CORES,
    GOOD_WEAPONS, GOOD_MUNITIONS, GOOD_MEDICINES, GOOD_CONSUMER, GOOD_LUXURY, GOOD_LIQUOR, GOOD_HABITAT_MODULES,
    GOOD_INSTRUMENTS,
    GOOD_COUNT,
    FIRST_REFINED = REF_STEEL, FIRST_FINISHED = GOOD_MACHINERY,
    MINED_COUNT = RAW_HELIUM3,                     // ores and ices are [0, MINED_COUNT)
};

struct Ingredient { int good; float tonnes; };     // per tonne made

struct GoodInfo {
    const char *name;
    Color       color;
    double      basePrice;    // credits per tonne where supply meets demand
    bool        ice;          // mined as ice chunks rather than rock
    Ingredient  recipe[3];    // unused slots: tonnes 0
};

inline int GoodTier(int g) { return g < FIRST_REFINED ? 0 : g < FIRST_FINISHED ? 1 : 2; }
inline const char *TierName(int tier) { return tier == 0 ? "Raw" : tier == 1 ? "Refined" : "Finished"; }

inline const GoodInfo &GoodData(int g)
{
    static const Color ICE = { 170, 220, 255, 255 }, CROP = { 150, 230, 120, 255 }, METAL = { 200, 200, 210, 255 },
                       CHEM = { 210, 185, 255, 255 }, TECH = { 110, 210, 255, 255 }, ARMS = { 255, 120, 110, 255 },
                       LIFE = { 255, 150, 190, 255 }, SHOP = { 240, 200, 150, 255 }, RICH = { 220, 150, 255, 255 };
    const Ingredient _ = { 0, 0.0f };
    static const GoodInfo table[GOOD_COUNT] = {
        // Raw
        { "Iron Ore",            { 190, 150, 120, 255 },   40, false, { _, _, _ } },
        { "Copper Ore",          { 230, 140,  90, 255 },   70, false, { _, _, _ } },
        { "Titanium Ore",        { 200, 205, 215, 255 },  140, false, { _, _, _ } },
        { "Precious Metals",     { 255, 210,  70, 255 },  800, false, { _, _, _ } },
        { "Rare Earths",         { 255, 160, 200, 255 },  320, false, { _, _, _ } },
        { "Uranium Ore",         { 170, 255, 110, 255 },  400, false, { _, _, _ } },
        { "Silicates",           { 180, 170, 140, 255 },   25, false, { _, _, _ } },
        { "Carbonaceous Rock",   { 130, 120, 110, 255 },   35, false, { _, _, _ } },
        { "Water Ice",           ICE,                      30, true,  { _, _, _ } },
        { "Volatile Ices",       { 190, 240, 220, 255 },   50, true,  { _, _, _ } },
        { "Helium-3",            { 255, 240, 170, 255 },  600, false, { _, _, _ } },
        { "Grain",               CROP,                     30, false, { _, _, _ } },
        { "Livestock",           CROP,                     90, false, { _, _, _ } },
        { "Exotic Flora",        LIFE,                    500, false, { _, _, _ } },
        // Refined
        { "Steel",               METAL,                   110, false, { { ORE_IRON, 1.0f }, { ORE_CARBON, 0.5f }, _ } },
        { "Light Alloys",        METAL,                   280, false, { { ORE_TITANIUM, 1.0f }, { ORE_IRON, 0.5f }, _ } },
        { "Conductors",          { 240, 170, 110, 255 },  200, false, { { ORE_COPPER, 1.0f }, { ORE_PRECIOUS, 0.05f }, _ } },
        { "Silicon",             TECH,                    180, false, { { ORE_SILICATES, 1.0f }, { ORE_RARE_EARTHS, 0.2f }, _ } },
        { "Ceramics",            { 220, 210, 190, 255 },   80, false, { { ORE_SILICATES, 1.5f }, _, _ } },
        { "Polymers",            CHEM,                    120, false, { { ORE_CARBON, 1.0f }, { ORE_VOLATILES, 0.5f }, _ } },
        { "Chemicals",           CHEM,                    130, false, { { ORE_VOLATILES, 1.0f }, { ORE_WATER_ICE, 0.5f }, _ } },
        { "Fertilizer",          CROP,                     70, false, { { ORE_VOLATILES, 0.5f }, { ORE_CARBON, 0.5f }, _ } },
        { "Purified Water",      ICE,                      45, false, { { ORE_WATER_ICE, 1.0f }, _, _ } },
        { "Fuel",                { 255, 230, 140, 255 },   60, false, { { RAW_HELIUM3, 0.05f }, { ORE_WATER_ICE, 0.5f }, _ } },
        { "Reactor Fuel",        { 170, 255, 110, 255 },  600, false, { { ORE_URANIUM, 1.0f }, _, _ } },
        { "Foodstuffs",          CROP,                     70, false, { { RAW_GRAIN, 0.8f }, { RAW_LIVESTOCK, 0.2f }, { REF_WATER, 0.2f } } },
        // Finished
        { "Machinery",           METAL,                   420, false, { { REF_STEEL, 1.0f }, { REF_CONDUCTORS, 0.5f }, _ } },
        { "Electronics",         TECH,                    700, false, { { REF_SILICON, 0.5f }, { REF_CONDUCTORS, 0.5f }, _ } },
        { "Robotics",            TECH,                   1300, false, { { REF_ALLOYS, 0.5f }, { REF_SILICON, 0.5f }, { REF_CONDUCTORS, 0.5f } } },
        { "Hull Plating",        METAL,                   450, false, { { REF_STEEL, 1.0f }, { REF_ALLOYS, 0.5f }, { REF_CERAMICS, 0.3f } } },
        { "Ship Components",     TECH,                   1100, false, { { REF_ALLOYS, 0.7f }, { REF_CONDUCTORS, 0.5f }, { REF_SILICON, 0.5f } } },
        { "Reactor Cores",       { 170, 255, 110, 255 }, 1800, false, { { REF_STEEL, 1.0f }, { REF_REACTOR_FUEL, 0.5f }, { REF_CERAMICS, 0.5f } } },
        { "Weapons",             ARMS,                   1500, false, { { REF_ALLOYS, 0.6f }, { REF_CONDUCTORS, 0.5f }, { REF_CHEMICALS, 0.3f } } },
        { "Munitions",           ARMS,                    300, false, { { REF_STEEL, 0.5f }, { REF_CHEMICALS, 0.6f }, _ } },
        { "Medicines",           LIFE,                    650, false, { { REF_CHEMICALS, 0.6f }, { RAW_FLORA, 0.2f }, _ } },
        { "Consumer Goods",      SHOP,                    300, false, { { REF_POLYMERS, 0.6f }, { REF_SILICON, 0.2f }, _ } },
        { "Luxury Goods",        RICH,                   1600, false, { { ORE_PRECIOUS, 0.3f }, { REF_POLYMERS, 0.4f }, _ } },
        { "Liquor",              SHOP,                    260, false, { { RAW_GRAIN, 1.0f }, { REF_WATER, 0.5f }, _ } },
        { "Habitat Modules",     METAL,                   500, false, { { REF_STEEL, 1.0f }, { REF_POLYMERS, 0.5f }, { REF_CERAMICS, 0.5f } } },
        { "Instruments",         { 140, 255, 220, 255 }, 1000, false, { { REF_SILICON, 0.5f }, { REF_CERAMICS, 0.3f }, { REF_CONDUCTORS, 0.4f } } },
    };
    return table[g];
}

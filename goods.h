// goods.h - everything you can carry and trade.
//
// Ores come out of asteroids (and helium-3 out of gas giants' upper clouds);
// goods are bought and sold at markets. Every settlement and station has an
// economy (see economy.h) that decides what it exports cheaply and what it
// pays well for, so trade routes run from producers to consumers.
//
// The first seven entries keep their original numbers: save files store cargo
// by index, so new goods only ever go on the end.
#pragma once
#include "raylib.h"

enum Good {
    // --- Ores (mined) ---
    ORE_IRON, ORE_NICKEL, ORE_SILICATES, ORE_PLATINUM, ORE_WATER_ICE, ORE_AMMONIA_ICE, ORE_METHANE_ICE,
    ORE_COPPER, ORE_COBALT, ORE_TITANIUM, ORE_GOLD, ORE_IRIDIUM, ORE_URANIUM, ORE_RARE_EARTHS, ORE_CARBONACEOUS,
    ORE_HELIUM3,                                   // skimmed from gas giants
    // --- Goods (made, grown and traded) ---
    GOOD_FOOD, GOOD_FINE_FOOD, GOOD_WATER, GOOD_SPIRITS, GOOD_TEXTILES, GOOD_FERTILIZER,
    GOOD_ALLOYS, GOOD_POLYMERS, GOOD_MACHINERY, GOOD_ELECTRONICS, GOOD_ROBOTICS, GOOD_REACTOR_PARTS,
    GOOD_FUEL_CELLS, GOOD_MEDICAL, GOOD_CONSUMER, GOOD_LUXURY, GOOD_EXOTIC_BIO, GOOD_RESEARCH_DATA,
    GOOD_INSTRUMENTS, GOOD_HABITAT_MODULES,
    GOOD_COUNT,
    FIRST_GOOD = GOOD_FOOD,                        // ores are [0, FIRST_GOOD)
};

enum class GoodClass { Metal, PreciousMetal, Mineral, Ice, Gas, Food, Industrial, Tech, Medical, Consumer, Luxury, Science };

struct GoodInfo {
    const char *name;
    Color       color;
    double      basePrice;    // credits per tonne, before each market's adjustment
    GoodClass   cls;
    bool        ice;          // ores: ice chunks rather than rock (ring particles)
};

inline bool IsOre(int g) { return g >= 0 && g < FIRST_GOOD; }

inline const GoodInfo &GoodData(int g)
{
    static const Color ICE = { 170, 220, 255, 255 }, FOOD = { 150, 230, 120, 255 }, INDUSTRIAL = { 200, 200, 210, 255 },
                       TECH = { 110, 210, 255, 255 }, MEDICAL = { 255, 140, 150, 255 }, CONSUMER = { 240, 200, 150, 255 },
                       LUXURY = { 220, 150, 255, 255 }, SCIENCE = { 140, 255, 220, 255 };
    static const GoodInfo table[GOOD_COUNT] = {
        { "Iron",               { 190, 150, 120, 255 },   40, GoodClass::Metal,         false },
        { "Nickel",             { 160, 170, 185, 255 },   55, GoodClass::Metal,         false },
        { "Silicates",          { 180, 170, 140, 255 },   25, GoodClass::Mineral,       false },
        { "Platinum",           { 245, 245, 255, 255 },  600, GoodClass::PreciousMetal, false },
        { "Water Ice",          { 170, 220, 255, 255 },   30, GoodClass::Ice,           true  },
        { "Ammonia Ice",        { 170, 255, 220, 255 },   45, GoodClass::Ice,           true  },
        { "Methane Ice",        { 210, 185, 255, 255 },   50, GoodClass::Ice,           true  },
        { "Copper",             { 230, 140,  90, 255 },   70, GoodClass::Metal,         false },
        { "Cobalt",             { 120, 140, 230, 255 },  110, GoodClass::Metal,         false },
        { "Titanium",           { 200, 205, 215, 255 },  140, GoodClass::Metal,         false },
        { "Gold",               { 255, 210,  70, 255 },  900, GoodClass::PreciousMetal, false },
        { "Iridium",            { 230, 230, 200, 255 }, 1100, GoodClass::PreciousMetal, false },
        { "Uranium Ore",        { 170, 255, 110, 255 },  400, GoodClass::Mineral,       false },
        { "Rare Earths",        { 255, 160, 200, 255 },  320, GoodClass::Mineral,       false },
        { "Carbonaceous",       { 130, 120, 110, 255 },   35, GoodClass::Mineral,       false },
        { "Helium-3",           { 255, 240, 170, 255 }, 1500, GoodClass::Gas,           false },
        { "Food",               FOOD,                     25, GoodClass::Food,          false },
        { "Fine Food",          FOOD,                    180, GoodClass::Food,          false },
        { "Purified Water",     ICE,                      12, GoodClass::Food,          false },
        { "Spirits",            CONSUMER,                260, GoodClass::Consumer,      false },
        { "Textiles",           CONSUMER,                 90, GoodClass::Consumer,      false },
        { "Fertilizer",         FOOD,                     40, GoodClass::Industrial,    false },
        { "Alloys",             INDUSTRIAL,              160, GoodClass::Industrial,    false },
        { "Polymers",           INDUSTRIAL,              110, GoodClass::Industrial,    false },
        { "Machinery",          INDUSTRIAL,              420, GoodClass::Industrial,    false },
        { "Electronics",        TECH,                    750, GoodClass::Tech,          false },
        { "Robotics",           TECH,                   1300, GoodClass::Tech,          false },
        { "Reactor Parts",      TECH,                   1800, GoodClass::Tech,          false },
        { "Fuel Cells",         TECH,                    220, GoodClass::Industrial,    false },
        { "Medical Supplies",   MEDICAL,                 650, GoodClass::Medical,       false },
        { "Consumer Goods",     CONSUMER,                300, GoodClass::Consumer,      false },
        { "Luxury Goods",       LUXURY,                 1600, GoodClass::Luxury,        false },
        { "Exotic Biologicals", LUXURY,                 1200, GoodClass::Luxury,        false },
        { "Research Data",      SCIENCE,                 900, GoodClass::Science,       false },
        { "Sci Instruments",    SCIENCE,                1000, GoodClass::Science,       false },
        { "Habitat Modules",    INDUSTRIAL,              500, GoodClass::Industrial,    false },
    };
    return table[g];
}

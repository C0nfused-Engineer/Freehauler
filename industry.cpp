// industry.cpp - the industries: what each makes and needs, and where it can go
#include "industry.h"
#include <algorithm>

const IndustryInfo &IndustryData(int type)
{
    const Ingredient _ = { 0, 0.0f };
    static const IndustryInfo table[IND_COUNT] = {
        //  name                        labour cells  besides its recipes                    a good site
        { "Open farming",                1.0f, 0.02f, { _, _ },                              "a sky to breathe under, and life in the soil" },
        { "Agridomes",                   2.0f, 0.12f, { _, _ },                              "any ground; better with ice to hand" },
        { "Hydroponics",                 1.6f, 0.15f, { _, _ },                              "a station" },
        { "Subsurface mines",            0.5f, 0.08f, { _, _ },                              "metals and minerals in the rock" },
        { "Ice works",                   0.6f, 0.06f, { _, _ },                              "a cold world rich in ices" },
        { "Gas skimming",                0.6f, 0.05f, { _, _ },                              "a gas giant below" },
        { "Xenobiology outpost",         1.5f, 0.30f, { _, _ },                              "a world with complex life of its own" },
        { "Smelters",                    0.8f, 0.15f, { _, _ },                              "anywhere; best in a belt" },
        { "Chemical works",              0.8f, 0.10f, { _, _ },                              "ices, or an ocean" },
        { "Food processing",             1.0f, 0.04f, { _, _ },                              "anywhere" },
        { "Light industry",              1.3f, 0.06f, { _, _ },                              "an open sky suits it" },
        { "Pharmaceuticals",             1.2f, 0.06f, { { GOOD_DATA, 0.02f }, _ },           "an open sky suits it" },
        { "Heavy industry",              1.0f, 0.10f, { _, _ },                              "anywhere; low gravity helps" },
        { "Munitions plant",             0.9f, 0.08f, { _, _ },                              "anywhere" },
        { "Arms plant",                  1.2f, 0.10f, { { GOOD_DATA, 0.01f }, _ },           "anywhere" },
        { "Semiconductor foundry",       1.1f, 0.15f, { { GOOD_DATA, 0.01f }, _ },           "anywhere; vacuum and low gravity help" },
        { "Robotics factory",            1.2f, 0.12f, { { GOOD_DATA, 0.02f }, _ },           "anywhere" },
        { "Reactor works",               1.3f, 0.15f, { { GOOD_DATA, 0.02f }, _ },           "anywhere" },
        { "Precision works",             1.4f, 0.12f, { { GOOD_DATA, 0.02f }, _ },           "anywhere" },
        { "Research labs",               1.5f, 0.50f, { _, _ },                              "strange stars and inhospitable worlds" },
        { "Shipyard",                    1.2f, 0.12f, { _, _ },                              "an orbital yard" },
        { "Solar arrays",                0.2f, 0.00f, { _, _ },                              "strong starlight and no air in the way" },
        { "Geothermal plant",            0.3f, 0.00f, { _, _ },                              "a hot or heavy rocky world" },
        { "Fission plant",               0.4f, 0.00f, { { REF_REACTOR_FUEL, 0.04f }, _ },    "anywhere" },
        { "Fusion plant",                0.5f, 0.00f, { { REF_FUEL, 0.012f }, _ },           "anywhere" },
        { "Cities",                      1.0f, 0.00f, { _, _ },                              "cheapest under an open sky" },
        { "Services",                    1.0f, 0.00f, { _, _ },                              "wherever there are people" },
    };
    return table[type >= 0 && type < IND_COUNT ? type : 0];
}

static double Clamp(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }

double SiteQuality(int type, const SiteInfo &s)
{
    const double metals = s.res[RES_METALS], minerals = s.res[RES_MINERALS], ices = s.res[RES_ICES], bio = s.res[RES_BIOMASS];
    switch (type) {
    case IND_FARM:        return s.openAir ? 0.7 + 0.5 * bio : 0.0;
    case IND_AGRIDOME:    return s.surface && !s.openAir ? 0.7 + 0.3 * ices : 0.0;
    case IND_HYDROPONICS: return s.surface ? 0.0 : 1.0;
    case IND_MINE:        return s.belt ? 1.2 : s.surface ? (fmax(metals, minerals) > 0.05 ? 0.3 + 0.9 * fmax(metals, minerals) : 0.0) : 0.0;
    case IND_ICEWORKS:    return s.belt ? 0.5 : s.surface && ices > 0.15 ? 0.3 + 0.9 * ices : 0.0;
    case IND_SKIMMER:     return s.giant ? 1.0 : 0.0;
    case IND_XENO:        return s.surface && s.life >= 2 ? 0.5 * (s.life - 1) + 0.6 * bio : 0.0;
    case IND_SMELTER:     return s.belt ? 1.3 : 1.0;
    case IND_CHEMICAL:    return 0.6 + 0.4 * fmax(ices, s.openAir ? 1.0 : 0.0);
    case IND_FOOD:        return 1.0;
    case IND_LIGHT: case IND_PHARMA: return s.openAir ? 1.0 : 0.7;
    case IND_HEAVY:       return s.gravity > 0.0f && s.gravity < 0.5f ? 1.2 : 1.0;
    case IND_FOUNDRY:     return s.surface && s.openAir ? 0.9 : 1.1;
    case IND_MUNITIONS: case IND_ARMS: case IND_ROBOTICS: case IND_REACTOR: case IND_PRECISION: return 1.0;
    case IND_LAB:
        // The stranger the place, the more there is to learn there
        return 0.3 + (s.strangeStar ? 1.0 : 0.0) + (s.surface && (s.tempK > 600.0f || s.tempK < 70.0f) ? 0.5 : 0.0) +
               (s.surface && !s.openAir ? 0.3 : 0.0) + (s.life >= 1 ? 0.5 : 0.0) + (s.giant ? 0.2 : 0.0);
    case IND_SHIPYARD:    return s.yard ? 1.0 : 0.0;
    case IND_SOLAR:       { double q = s.flux * (s.openAir ? 0.5 : 1.0); return q < 0.15 ? 0.0 : Clamp(q, 0.0, 3.0); }
    case IND_GEOTHERMAL:  return !s.surface ? 0.0 : s.tempK > 450.0f ? 1.2 : s.gravity > 1.2f ? 0.7 : 0.0;
    case IND_FISSION: case IND_FUSION: return 1.0;
    case IND_CITY:        return s.openAir ? 1.0 : s.surface ? 0.5 : 0.4;
    case IND_SERVICES:    return s.openAir ? 1.0 : 0.6;
    default:              return 0.0;
    }
}

int IndustryProducts(int type, const SiteInfo &s, Product out[MAX_PRODUCTS])
{
    int n = 0;
    auto add = [&](int good, double share) { if (share > 1e-4 && n < MAX_PRODUCTS) { out[n].good = (uint8_t)good; out[n].share = (float)share; n++; } };
    const double metals = s.belt ? 0.5 : s.res[RES_METALS], precious = s.belt ? 0.15 : s.res[RES_PRECIOUS],
                 minerals = s.belt ? 0.5 : s.res[RES_MINERALS], bio = s.res[RES_BIOMASS];
    switch (type) {
    case IND_FARM:        add(RAW_GRAIN, 0.75); add(RAW_LIVESTOCK, 0.22); add(RAW_FLORA, 0.03 * (0.3 + bio)); break;
    case IND_AGRIDOME:    add(RAW_GRAIN, 0.96); add(RAW_FLORA, 0.04 * bio); break;
    case IND_HYDROPONICS: add(RAW_GRAIN, 1.0); break;
    case IND_MINE:
        add(ORE_IRON, 0.50 * metals); add(ORE_COPPER, 0.25 * metals); add(ORE_TITANIUM, 0.15 * metals); add(ORE_PRECIOUS, 0.05 * precious);
        add(ORE_RARE_EARTHS, 0.08 * minerals); add(ORE_URANIUM, 0.05 * minerals); add(ORE_SILICATES, 0.40 * minerals);
        add(ORE_CARBON, 0.25 * (1.0 - metals) + 0.05);
        break;
    case IND_ICEWORKS:    add(ORE_WATER_ICE, 0.6); add(ORE_VOLATILES, 0.4); break;
    case IND_SKIMMER:     add(RAW_HELIUM3, 1.0); break;
    case IND_XENO:        add(RAW_FAUNA, 0.6); add(RAW_FLORA, 0.4); break;
    case IND_SMELTER:     add(REF_STEEL, 0.38); add(REF_ALLOYS, 0.15); add(REF_CONDUCTORS, 0.15); add(REF_SILICON, 0.13); add(REF_CERAMICS, 0.19); break;
    case IND_CHEMICAL:    add(REF_POLYMERS, 0.20); add(REF_CHEMICALS, 0.20); add(REF_FERTILIZER, 0.15); add(REF_WATER, 0.30); add(REF_FUEL, 0.07);
                          add(REF_REACTOR_FUEL, 0.08); break;
    case IND_FOOD:        add(REF_FOOD, 0.88); add(GOOD_LIQUOR, 0.12); break;
    case IND_LIGHT:       add(GOOD_CONSUMER, 0.9); add(GOOD_LUXURY, 0.1); break;
    case IND_PHARMA:      add(GOOD_MEDICINES, 1.0); break;
    case IND_HEAVY:       add(GOOD_MACHINERY, 0.45); add(GOOD_HULL_PLATING, 0.30); add(GOOD_HABITAT_MODULES, 0.25); break;
    case IND_MUNITIONS:   add(GOOD_MUNITIONS, 1.0); break;
    case IND_ARMS:        add(GOOD_WEAPONS, 1.0); break;
    case IND_FOUNDRY:     add(GOOD_ELECTRONICS, 1.0); break;
    case IND_ROBOTICS:    add(GOOD_ROBOTICS, 1.0); break;
    case IND_REACTOR:     add(GOOD_REACTOR_CORES, 1.0); break;
    case IND_PRECISION:   add(GOOD_INSTRUMENTS, 0.5); add(GOOD_SHIP_COMPONENTS, 0.5); break;
    case IND_LAB:         add(GOOD_DATA, 1.0); break;
    case IND_SOLAR: case IND_GEOTHERMAL: case IND_FISSION: case IND_FUSION: add(REF_CELLS, 1.0); break;
    default: break;                                                // (cities make housing, which isn't a good)
    }
    double sum = 0.0;
    for (int i = 0; i < n; i++) sum += out[i].share;
    for (int i = 0; i < n && sum > 0.0; i++) out[i].share = (float)(out[i].share / sum);
    return n;
}

double IndustryLabour(int type, double cap, double site)
{
    if (cap <= 0.0) return 0.0;
    if (type == IND_CITY) return CITY_LABOUR * pow(cap, LABOUR_POWER);       // (a city's size is in people housed: its site only changes its power bill)
    if (type == IND_SERVICES) return SERVICES_LABOUR * pow(cap, LABOUR_POWER);   // (likewise: in people served)
    return IndustryData(type).labour * pow(cap / fmax(site, 0.05), LABOUR_POWER);
}

double CivicCells(int type, double cap, double site) { return (type == IND_CITY ? CITY_CELLS : SERVICES_CELLS) * cap / fmax(site, 0.1); }

double CityHousing(double cap)   { return cap > 0.0 ? pow(cap, LABOUR_POWER) : 0.0; }
double CityFor(double people)    { return people > 0.0 ? pow(people, 1.0 / LABOUR_POWER) : 0.0; }

const IndustrySeed *SettlementIndustries(Settlement s)
{
    #define END { IND_COUNT, 0.0f }
    static const IndustrySeed core[]     = { { IND_FARM, 0.30f }, { IND_FOOD, 0.22f }, { IND_LIGHT, 0.16f }, { IND_PHARMA, 0.06f }, { IND_FOUNDRY, 0.10f },
                                             { IND_ROBOTICS, 0.04f }, { IND_PRECISION, 0.06f }, { IND_HEAVY, 0.06f }, { IND_ARMS, 0.015f }, { IND_REACTOR, 0.015f },
                                             { IND_CHEMICAL, 0.14f }, { IND_LAB, 0.03f }, { IND_XENO, 0.01f }, END };
    static const IndustrySeed settled[]  = { { IND_FARM, 0.75f }, { IND_FOOD, 0.50f }, { IND_CHEMICAL, 0.12f }, { IND_XENO, 0.02f }, { IND_LAB, 0.005f }, END };
    static const IndustrySeed domes[]    = { { IND_AGRIDOME, 0.18f }, { IND_FOOD, 0.10f }, { IND_CHEMICAL, 0.12f }, { IND_MINE, 0.12f }, { IND_ICEWORKS, 0.10f },
                                             { IND_LAB, 0.01f }, { IND_XENO, 0.01f }, END };
    static const IndustrySeed habitats[] = { { IND_SKIMMER, 0.30f }, { IND_CHEMICAL, 0.10f }, { IND_HYDROPONICS, 0.06f }, END };
    static const IndustrySeed mining[]   = { { IND_MINE, 1.00f }, { IND_ICEWORKS, 0.50f }, { IND_AGRIDOME, 0.03f }, END };
    static const IndustrySeed refinery[] = { { IND_MINE, 0.90f }, { IND_SMELTER, 0.80f }, { IND_CHEMICAL, 0.22f }, { IND_ICEWORKS, 0.10f }, { IND_HYDROPONICS, 0.04f }, END };
    static const IndustrySeed underground[] = { { IND_MINE, 0.30f }, { IND_SMELTER, 0.20f }, { IND_HEAVY, 0.28f }, { IND_CHEMICAL, 0.16f }, { IND_MUNITIONS, 0.04f },
                                             { IND_ARMS, 0.02f }, { IND_REACTOR, 0.02f }, { IND_AGRIDOME, 0.06f }, { IND_FOOD, 0.04f }, END };
    static const IndustrySeed research[] = { { IND_LAB, 0.50f }, { IND_XENO, 0.15f }, { IND_PHARMA, 0.02f }, { IND_AGRIDOME, 0.03f }, { IND_HYDROPONICS, 0.03f }, END };
    static const IndustrySeed solar[]    = { { IND_SOLAR, 1.00f }, { IND_FOUNDRY, 0.03f }, END };
    static const IndustrySeed garden[]   = { { IND_HYDROPONICS, 0.80f }, { IND_FOOD, 0.35f }, END };
    static const IndustrySeed port[]     = { { IND_HYDROPONICS, 0.04f }, END };
    static const IndustrySeed yard[]     = { { IND_SHIPYARD, 0.50f }, { IND_PRECISION, 0.05f }, END };
    static const IndustrySeed starbase[] = { { IND_SHIPYARD, 0.08f }, { IND_HYDROPONICS, 0.04f }, END };
    static const IndustrySeed none[]     = { END };
    #undef END
    switch (s) {
    case Settlement::CoreWorld:         return core;
    case Settlement::SettledWorld:      return settled;
    case Settlement::DomedColonies:     return domes;
    case Settlement::OrbitalHabitats: case Settlement::SkimmerStation: return habitats;
    case Settlement::MiningOutpost: case Settlement::OreTerminal:      return mining;
    case Settlement::OreRefinery:       return refinery;
    case Settlement::UndergroundColony: return underground;
    case Settlement::ResearchStation: case Settlement::ResearchOutpost: return research;
    case Settlement::SolarCollector:    return solar;
    case Settlement::Greenhouse:        return garden;
    case Settlement::Shipyard:          return yard;
    case Settlement::Starbase:          return starbase;
    case Settlement::OrbitalPort: case Settlement::Waystation: case Settlement::Freeport: return port;
    default:                            return none;
    }
}

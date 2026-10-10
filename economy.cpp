// economy.cpp - markets and trading
#include "economy.h"
#include "format.h"

// What each kind of economy makes, needs, and passes through
struct EconTemplate {
    std::vector<int> exports, imports, trades;
    double fuel;                          // fuel price factor
};

static const EconTemplate &TemplateFor(Econ e)
{
    static const EconTemplate none = { {}, {}, {}, 1.0 };
    static const EconTemplate t[] = {
        none,
        // HighTech
        { { GOOD_ELECTRONICS, GOOD_ROBOTICS, GOOD_CONSUMER, GOOD_LUXURY, GOOD_MEDICAL, GOOD_INSTRUMENTS },
          { GOOD_FOOD, GOOD_FINE_FOOD, GOOD_ALLOYS, GOOD_POLYMERS, ORE_RARE_EARTHS, ORE_GOLD, ORE_PLATINUM, ORE_IRIDIUM,
            GOOD_EXOTIC_BIO, GOOD_SPIRITS, ORE_HELIUM3 },
          { GOOD_TEXTILES, GOOD_MACHINERY, GOOD_WATER, GOOD_FUEL_CELLS }, 1.0 },
        // Agricultural
        { { GOOD_FOOD, GOOD_FINE_FOOD, GOOD_SPIRITS, GOOD_TEXTILES, GOOD_EXOTIC_BIO },
          { GOOD_FERTILIZER, GOOD_MACHINERY, GOOD_ELECTRONICS, GOOD_MEDICAL, GOOD_CONSUMER, GOOD_ROBOTICS, GOOD_LUXURY },
          { GOOD_WATER, GOOD_POLYMERS }, 1.05 },
        // Colony
        { { GOOD_WATER, GOOD_POLYMERS },
          { GOOD_FOOD, GOOD_FINE_FOOD, GOOD_MEDICAL, GOOD_MACHINERY, GOOD_HABITAT_MODULES, GOOD_FERTILIZER, GOOD_CONSUMER,
            GOOD_ELECTRONICS, GOOD_SPIRITS },
          { GOOD_FUEL_CELLS, GOOD_TEXTILES }, 1.15 },
        // GasMining
        { { ORE_HELIUM3, GOOD_FUEL_CELLS, GOOD_POLYMERS, ORE_METHANE_ICE },
          { GOOD_FOOD, GOOD_WATER, GOOD_MEDICAL, GOOD_MACHINERY, GOOD_CONSUMER, GOOD_SPIRITS, GOOD_REACTOR_PARTS,
            GOOD_HABITAT_MODULES },
          { GOOD_FINE_FOOD }, 0.7 },
        // Research
        { { GOOD_RESEARCH_DATA, GOOD_EXOTIC_BIO },
          { GOOD_INSTRUMENTS, GOOD_FINE_FOOD, GOOD_FOOD, GOOD_MEDICAL, GOOD_ELECTRONICS, GOOD_SPIRITS, GOOD_LUXURY },
          { GOOD_WATER }, 1.2 },
        // Mining
        { { ORE_IRON, ORE_NICKEL, ORE_COPPER, ORE_TITANIUM, ORE_COBALT, ORE_URANIUM, ORE_RARE_EARTHS, ORE_SILICATES },
          { GOOD_FOOD, GOOD_WATER, GOOD_MACHINERY, GOOD_MEDICAL, GOOD_SPIRITS, GOOD_CONSUMER, GOOD_FUEL_CELLS, GOOD_ROBOTICS },
          { GOOD_FINE_FOOD }, 1.0 },
        // Industrial
        { { GOOD_ALLOYS, GOOD_MACHINERY, GOOD_REACTOR_PARTS, GOOD_HABITAT_MODULES, GOOD_ROBOTICS },
          { ORE_IRON, ORE_NICKEL, ORE_COPPER, ORE_TITANIUM, ORE_COBALT, ORE_URANIUM, ORE_RARE_EARTHS, ORE_CARBONACEOUS,
            GOOD_FOOD, GOOD_MEDICAL, GOOD_WATER },
          { GOOD_POLYMERS, GOOD_ELECTRONICS }, 0.95 },
        // Refinery
        { { GOOD_ALLOYS, GOOD_FUEL_CELLS, GOOD_WATER, GOOD_POLYMERS },
          { ORE_IRON, ORE_NICKEL, ORE_COBALT, ORE_COPPER, ORE_TITANIUM, ORE_PLATINUM, ORE_GOLD, ORE_IRIDIUM,
            ORE_CARBONACEOUS, ORE_WATER_ICE, ORE_AMMONIA_ICE, GOOD_FOOD, GOOD_MACHINERY, GOOD_SPIRITS },
          {}, 0.85 },
        // Skimmer
        { { ORE_HELIUM3, GOOD_FUEL_CELLS, ORE_METHANE_ICE },
          { GOOD_FOOD, GOOD_WATER, GOOD_MEDICAL, GOOD_MACHINERY, GOOD_SPIRITS, GOOD_CONSUMER },
          {}, 0.55 },
        // Freeport: a little of everything, at fair prices
        { {}, {},
          { GOOD_FOOD, GOOD_FINE_FOOD, GOOD_WATER, GOOD_SPIRITS, GOOD_TEXTILES, GOOD_ALLOYS, GOOD_POLYMERS, GOOD_MACHINERY,
            GOOD_ELECTRONICS, GOOD_FUEL_CELLS, GOOD_MEDICAL, GOOD_CONSUMER, GOOD_LUXURY, GOOD_HABITAT_MODULES }, 0.9 },
        // Shipyard
        { { GOOD_HABITAT_MODULES, GOOD_FUEL_CELLS },
          { GOOD_ALLOYS, ORE_TITANIUM, GOOD_ELECTRONICS, GOOD_MACHINERY, GOOD_REACTOR_PARTS, GOOD_ROBOTICS, GOOD_POLYMERS,
            GOOD_INSTRUMENTS },
          {}, 0.9 },
        // Power (a stand-in until the economy is rebuilt): stored energy out, upkeep in
        { { GOOD_FUEL_CELLS },
          { GOOD_FOOD, GOOD_WATER, GOOD_MACHINERY, GOOD_ELECTRONICS, GOOD_MEDICAL },
          {}, 0.6 },
    };
    int i = (int)e;
    return i >= 0 && i < (int)(sizeof t / sizeof t[0]) ? t[i] : none;
}

Market MakeMarket(const Body &b)
{
    uint64_t h = Mix64(b.id ^ 0xEC0ull);                   // the body's own number: stable, and never shared
    auto vary = [&](uint64_t k) { return (double)(Mix64(h + k) >> 11) * (1.0 / 9007199254740992.0); };   // [0, 1)
    Market m;
    m.econ = b.econ;
    const EconTemplate &t = TemplateFor(b.econ);
    for (int g : t.trades)  { m.role[g] = Role::Trade;  m.price[g] = GoodData(g).basePrice * (0.95 + 0.15 * vary(g)); }
    for (int g : t.imports) { m.role[g] = Role::Import; m.price[g] = GoodData(g).basePrice * (1.30 + 0.30 * vary(g)); }
    for (int g : t.exports) { m.role[g] = Role::Export; m.price[g] = GoodData(g).basePrice * (0.60 + 0.15 * vary(g)); }
    for (int g = 0; g < FIRST_GOOD; g++)            // anywhere with people buys raw ore, if only for scrap prices
        if (m.role[g] == Role::None) { m.role[g] = Role::Ore; m.price[g] = GoodData(g).basePrice * (0.75 + 0.15 * vary(g)); }
    m.fuelPrice = 50.0 * t.fuel * (0.9 + 0.2 * vary(99));
    return m;
}

std::vector<int> Market::Listed() const
{
    std::vector<int> out;
    for (Role r : { Role::Export, Role::Import, Role::Trade, Role::Ore })
        for (int g = 0; g < GOOD_COUNT; g++) if (role[g] == r) out.push_back(g);
    return out;
}

static std::string RoleLine(const Market &m, Role r, int max)
{
    std::string s;
    int n = 0;
    for (int g = 0; g < GOOD_COUNT; g++)
        if (m.role[g] == r) {
            if (n == max) { s += ", ..."; break; }
            s += (n++ ? ", " : "") + std::string(GoodData(g).name);
        }
    return s;
}
std::string ExportsLine(const Market &m, int max) { return RoleLine(m, Role::Export, max); }
std::string ImportsLine(const Market &m, int max) { return RoleLine(m, Role::Import, max); }

const Body *TradePartner(const Captain &c)
{
    if (c.flight.nearBody < 0 || c.flight.nearBody >= (int)c.loaded.bodies.size()) return nullptr;
    const Body &b = c.loaded.bodies[c.flight.nearBody];
    return b.population > 0 ? &b : nullptr;
}

static void Tell(World &w, Captain &c, const std::string &text) { w.Emit(EventType::Notice, text, {}, -1, 0.0, 0.0, c.id); }

bool SellGoods(World &w, Captain &c, int good, double qty)
{
    const Body *b = TradePartner(c);
    if (!b || good < 0 || good >= GOOD_COUNT) return false;
    Market m = MakeMarket(*b);
    Player &p = c.player;
    if (!m.Buys(good)) { Tell(w, c, b->name + " doesn't trade in " + GoodData(good).name); return false; }
    qty = fmin(qty, p.cargo[good]);
    if (qty <= 1e-6) { Tell(w, c, std::string("No ") + GoodData(good).name + " in the hold"); return false; }
    double paid = qty * m.SellPrice(good);
    p.cargo[good] -= qty;
    if (p.cargo[good] < 1e-6) p.cargo[good] = 0.0;
    p.credits += paid;
    w.Emit(EventType::SoldGoods, TextFormat("Sold %.1f t %s for %s cr", qty, GoodData(good).name, Thousands(paid).c_str()),
           TargetId::MakeBody(c.loaded.index, c.flight.nearBody), good, qty, paid, c.id);
    return true;
}

bool BuyGoods(World &w, Captain &c, int good, double qty)
{
    const Body *b = TradePartner(c);
    if (!b || good < 0 || good >= GOOD_COUNT) return false;
    Market m = MakeMarket(*b);
    Player &p = c.player;
    if (!m.Sells(good)) { Tell(w, c, b->name + " has no " + GoodData(good).name + " for sale"); return false; }
    double price = m.BuyPrice(good);
    double room = CARGO_CAPACITY - p.CargoTotal(), afford = p.credits / price;
    qty = fmin(qty, fmin(room, afford));
    if (room <= 1e-6) { Tell(w, c, "Cargo hold full"); return false; }
    if (qty <= 1e-6)  { Tell(w, c, "Not enough credits"); return false; }
    p.cargo[good] += qty;
    p.credits -= qty * price;
    w.Emit(EventType::BoughtGoods, TextFormat("Bought %.1f t %s for %s cr", qty, GoodData(good).name, Thousands(qty * price).c_str()),
           TargetId::MakeBody(c.loaded.index, c.flight.nearBody), good, qty, qty * price, c.id);
    return true;
}

bool BuyFuel(World &w, Captain &c, double qty)
{
    const Body *b = TradePartner(c);
    if (!b) return false;
    Player &p = c.player;
    double price = MakeMarket(*b).fuelPrice;
    double room = FUEL_CAPACITY - p.fuel, afford = p.credits / price;
    qty = fmin(qty, fmin(room, afford));
    if (room <= 1e-6)  { Tell(w, c, "Tank already full");  return false; }
    if (qty <= 1e-6)   { Tell(w, c, "Not enough credits"); return false; }
    p.fuel    += qty;
    p.credits -= qty * price;
    w.Emit(EventType::BoughtFuel, TextFormat("Bought %.1f t fuel for %s cr", qty, Thousands(qty * price).c_str()),
           TargetId::MakeBody(c.loaded.index, c.flight.nearBody), -1, qty, qty * price, c.id);
    return true;
}

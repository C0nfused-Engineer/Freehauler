// shipyard.cpp - buying, fitting and selling parts and hulls
#include "shipyard.h"
#include "economy.h"
#include "format.h"
#include <algorithm>
#include <cstring>

static void Tell(World &w, Captain &c, const std::string &text) { w.Emit(EventType::Notice, text, {}, -1, 0.0, 0.0, c.id); }

const Body *Shipyard(const Captain &c)
{
    const Body *b = TradePartner(c);
    return b && (b->settlement == Settlement::Shipyard || b->settlement == Settlement::Starbase) ? b : nullptr;
}

int YardMaker(const World &w, const Body &yard, PartType type)
{
    const Faction *owner = w.systems.factions.Get(yard.owner);
    return owner && owner->kind == FactionKind::Conglomerate && Specialises(owner->slant, type) ? owner->id : NO_FACTION;
}

int YardGrade(const World &w, const Captain &c, const Body &yard, PartType type)
{
    const Faction *owner = w.systems.factions.Get(yard.owner);
    int grade = 1;
    if (yard.settlement == Settlement::Starbase || (owner && owner->kind == FactionKind::Conglomerate)) grade++;
    if (YardMaker(w, yard, type) != NO_FACTION) grade++;
    if (grade >= PART_GRADES - 1 && c.Standing(yard.owner) < ELITE_STANDING) grade = PART_GRADES - 2;
    return std::min(grade, PART_GRADES - 1);
}

void Refit(Captain &c)
{
    Player &p = c.player;
    p.spec = SpecOf(p.build);
    p.fuel = fmin(p.fuel, p.spec.tank);
    p.power.bank = fmin(p.power.bank, p.spec.bankMJ);
    p.power.driveCap = fmin(p.power.driveCap, p.spec.driveCapMJ);
}

// What the yard's stock of a part's material does to its price, and whether there's enough of it
static double Supply(const World &w, const Body &yard, int good, double tonnes, bool &enough)
{
    enough = true;
    int m = w.econ.MarketOf(yard.id);
    if (m < 0 || good < 0 || w.econ.base->defs[m].target[good] <= 0.0f) return 1.0;       // (not something this yard keeps: it's brought in)
    enough = w.econ.state[m].stock[good] >= tonnes;
    double ratio = MarketPrice(w, m, good) / GoodData(good).basePrice;
    return 0.5 + 0.5 * fmax(0.6, fmin(3.0, ratio));
}

static void TakeStock(World &w, const Body &yard, int good, double tonnes)
{
    int m = w.econ.MarketOf(yard.id);
    if (m < 0 || good < 0 || w.econ.base->defs[m].target[good] <= 0.0f) return;
    w.econ.state[m].stock[good] = fmaxf(0.0f, w.econ.state[m].stock[good] - (float)tonnes);
}

double PartPrice(const World &w, const Captain &c, const Body &yard, const Part &p)
{
    PartInfo i = Describe(p);
    bool enough;
    double k = Supply(w, yard, i.material, i.tonnes, enough);
    double price = i.cost * k * (1.0 - StandingEdge(c, yard.owner));
    return enough ? price : -price;
}

double HullPrice(const World &w, const Captain &c, const Body &yard, int hull)
{
    const HullDef &h = Hull(hull);
    bool enough;
    double k = Supply(w, yard, GOOD_HULL_PLATING, 0.5 * h.mass, enough);
    ShipBuild bare = c.player.build;
    bare.parts.clear();
    double price = h.cost * k * (1.0 - StandingEdge(c, yard.owner)) - RESALE * BuildValue(bare);
    return enough ? price : (price > 0.0 ? -price : -1.0);
}

std::string FitProblem(const ShipBuild &b, const Part &p)
{
    PartInfo i = Describe(p);
    if (p.size < 1 || p.size > PART_SIZES || p.grade >= PART_GRADES || (int)p.type >= (int)PartType::Count) return "No such part";
    if (VolumeUsed(b) + i.volume > BuildVolume(b) + 1e-6)
        return TextFormat("No room: it needs %.0f m3 and there are %.0f free", i.volume, BuildVolume(b) - VolumeUsed(b));
    if (i.external && HardpointsUsed(b) >= BuildHardpoints(b)) return "No free hardpoint";
    if (p.type == PartType::Reactor)
        for (const Part &o : b.parts)
            if (o.type == PartType::Reactor && o.variant != p.variant) return "Every reactor aboard must burn the same fuel";
    return "";
}

bool FitPart(World &w, Captain &c, Part p)
{
    const Body *yard = Shipyard(c);
    if (!yard) { Tell(w, c, "Dock at a shipyard or a starbase to fit out"); return false; }
    EnsureEconomy(w);
    if (p.variant >= REACTOR_FUELS || p.type != PartType::Reactor) p.variant = p.type == PartType::Reactor ? p.variant % REACTOR_FUELS : 0;
    std::string no = FitProblem(c.player.build, p);
    if (!no.empty()) { Tell(w, c, no); return false; }
    if (p.grade > YardGrade(w, c, *yard, p.type)) { Tell(w, c, std::string(yard->name) + " doesn't sell that grade to you"); return false; }
    p.maker = (int16_t)YardMaker(w, *yard, p.type);
    PartInfo i = Describe(p, &w.systems.factions);
    double price = PartPrice(w, c, *yard, p);
    if (price < 0.0) { Tell(w, c, std::string(yard->name) + " is out of " + GoodData(i.material).name + " to build it with"); return false; }
    if (c.player.credits < price) { Tell(w, c, TextFormat("That costs %s cr", Thousands(price).c_str())); return false; }
    c.player.credits -= price;
    TakeStock(w, *yard, i.material, i.tonnes);
    c.player.build.parts.push_back(p);
    Refit(c);
    Tell(w, c, "Fitted: " + i.name + TextFormat("  (%s cr)", Thousands(price).c_str()));
    return true;
}

bool RemovePart(World &w, Captain &c, int index)
{
    const Body *yard = Shipyard(c);
    Player &pl = c.player;
    if (!yard) { Tell(w, c, "Dock at a shipyard or a starbase to fit out"); return false; }
    if (index < 0 || index >= (int)pl.build.parts.size()) return false;
    ShipBuild after = pl.build;
    Part p = after.parts[index];
    after.parts.erase(after.parts.begin() + index);
    ShipSpec spec = SpecOf(after);
    if (pl.CargoTotal() > spec.hold + 1e-6) { Tell(w, c, "The hold is too full to take that bay out"); return false; }
    if (pl.fuel > spec.tank + 1e-6 && p.type == PartType::Tank) { Tell(w, c, "There's too much fuel aboard to take that tank out"); return false; }
    PartInfo i = Describe(p, &w.systems.factions);
    double back = RESALE * i.cost;
    pl.build = after;
    pl.credits += back;
    int m = w.econ.Built() ? w.econ.MarketOf(yard->id) : -1;                       // (what it's made of goes back into the yard's stock)
    if (m >= 0 && w.econ.base->defs[m].target[i.material] > 0.0f) w.econ.state[m].stock[i.material] += (float)(0.5 * i.tonnes);
    Refit(c);
    Tell(w, c, "Removed: " + i.name + TextFormat("  (+%s cr)", Thousands(back).c_str()));
    return true;
}

bool BuyHull(World &w, Captain &c, int hull)
{
    const Body *yard = Shipyard(c);
    Player &pl = c.player;
    if (!yard) { Tell(w, c, "Dock at a shipyard or a starbase to buy a hull"); return false; }
    if (hull < 0 || hull >= HULL_COUNT || hull == pl.build.hull) return false;
    EnsureEconomy(w);
    // Everything fitted moves across, if the new hull has the room and the hardpoints
    ShipBuild next = pl.build;
    next.hull = hull; next.stretch = next.mounts = 0;
    if (VolumeUsed(next) > BuildVolume(next) + 1e-6) {
        Tell(w, c, TextFormat("What you have fitted needs %.0f m3: the %s has %.0f. Take some out first.", VolumeUsed(next), Hull(hull).name, BuildVolume(next)));
        return false;
    }
    if (HardpointsUsed(next) > BuildHardpoints(next)) { Tell(w, c, TextFormat("The %s has only %d hardpoints", Hull(hull).name, BuildHardpoints(next))); return false; }
    double price = HullPrice(w, c, *yard, hull);
    bool enough;
    Supply(w, *yard, GOOD_HULL_PLATING, 0.5 * Hull(hull).mass, enough);
    if (!enough) { Tell(w, c, std::string(yard->name) + " hasn't the hull plating to build one"); return false; }
    if (pl.credits < price) { Tell(w, c, TextFormat("That costs %s cr with your hull traded in", Thousands(price).c_str())); return false; }
    pl.credits -= price;
    TakeStock(w, *yard, GOOD_HULL_PLATING, 0.5 * Hull(hull).mass);
    pl.build = next;
    Refit(c);
    Tell(w, c, TextFormat("Your gear is moved across to a new %s  (%s%s cr)", Hull(hull).name, price < 0.0 ? "+" : "", Thousands(fabs(price)).c_str()));
    return true;
}

bool StretchHull(World &w, Captain &c, bool hardpoint)
{
    const Body *yard = Shipyard(c);
    Player &pl = c.player;
    if (!yard) { Tell(w, c, "Dock at a shipyard or a starbase for hull work"); return false; }
    if ((hardpoint ? pl.build.mounts : pl.build.stretch) >= MAX_STRETCH) { Tell(w, c, "This hull can't take any more of that: time for a bigger one"); return false; }
    EnsureEconomy(w);
    bool enough;
    double tonnes = 0.03 * Hull(pl.build.hull).mass;
    double price = StretchCost(pl.build, hardpoint) * Supply(w, *yard, GOOD_HULL_PLATING, tonnes, enough) * (1.0 - StandingEdge(c, yard->owner));
    if (!enough) { Tell(w, c, std::string(yard->name) + " hasn't the hull plating for the work"); return false; }
    if (pl.credits < price) { Tell(w, c, TextFormat("That work costs %s cr", Thousands(price).c_str())); return false; }
    pl.credits -= price;
    TakeStock(w, *yard, GOOD_HULL_PLATING, tonnes);
    if (hardpoint) pl.build.mounts++; else pl.build.stretch++;
    Refit(c);
    Tell(w, c, hardpoint ? TextFormat("Another hardpoint welded on: %d now  (%s cr)", BuildHardpoints(pl.build), Thousands(price).c_str())
                         : TextFormat("Hull stretched: %.0f m3 now  (%s cr)", BuildVolume(pl.build), Thousands(price).c_str()));
    return true;
}

// ---------------------------------------------------------------------------
// As numbers and as text
// ---------------------------------------------------------------------------
int32_t PackPart(const Part &p)
{
    return (int32_t)((uint32_t)p.type | ((uint32_t)p.size << 8) | ((uint32_t)p.grade << 16) | ((uint32_t)p.variant << 24));
}

Part UnpackPart(int32_t v)
{
    Part p;
    uint32_t u = (uint32_t)v;
    p.type = (PartType)(u & 255); p.size = (uint8_t)((u >> 8) & 255); p.grade = (uint8_t)((u >> 16) & 255); p.variant = (uint8_t)((u >> 24) & 255);
    return p;
}

void WriteBuild(const Captain &c, std::string &out)
{
    const ShipBuild &b = c.player.build;
    out += TextFormat("hull %d %d %d\n", b.hull, (int)b.stretch, (int)b.mounts);
    for (const Part &p : b.parts) out += TextFormat("part %d %d %d %d %d\n", (int)p.type, (int)p.size, (int)p.grade, (int)p.variant, (int)p.maker);
}

bool ReadBuildLine(Captain &c, const char *key, const char *rest)
{
    ShipBuild &b = c.player.build;
    if (!strcmp(key, "hull")) {
        int hull, stretch, mounts;
        if (sscanf(rest, "%d %d %d", &hull, &stretch, &mounts) == 3 && hull >= 0 && hull < HULL_COUNT) {
            b.hull = hull; b.stretch = (uint8_t)std::max(0, std::min(MAX_STRETCH, stretch)); b.mounts = (uint8_t)std::max(0, std::min(MAX_STRETCH, mounts));
            b.parts.clear();                                       // (its parts follow)
        }
        return true;
    }
    if (!strcmp(key, "part")) {
        int type, size, grade, variant, maker;
        if (sscanf(rest, "%d %d %d %d %d", &type, &size, &grade, &variant, &maker) == 5 && type >= 0 && type < (int)PartType::Count &&
            size >= 1 && size <= PART_SIZES && grade >= 0 && grade < PART_GRADES && variant >= 0 && variant < REACTOR_FUELS) {
            Part p;
            p.type = (PartType)type; p.size = (uint8_t)size; p.grade = (uint8_t)grade; p.variant = (uint8_t)variant; p.maker = (int16_t)maker;
            b.parts.push_back(p);
        }
        return true;
    }
    return false;
}

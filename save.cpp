// save.cpp - reading and writing save games
//
// Format (one record per line, "key values..."):
//   SPACEFLIGHT_SAVE <version>
//   name <the name you gave it>      (display only)
//   location <nearest star system>   (display only)
//   seed 1977
//   clock 123.4
//   ship <anchor system> <pos x y z> <vel x y z> <rot x y z w> <dampeners> <throttle>
//   fuel 41.2
//   credits 1068
//   cargo <ore index> <tonnes>       (one line per ore carried)
//   mined <rock id> <ore left>       (one line per rock you've mined)
//   charted <system>                 (one line per system you have charts of)
//   scanned <body id>                (one line per world you've scanned yourself)
//   standing <faction> <-100..100>   (one line per faction you have any standing with)
//   prices ...                       (one line per market you have prices for)
//   econ / stock / owner / faction / embargo / shock / news ...   (the economy: see WriteEconomy)
//   hull / part                                                   (your ship: see WriteBuild)
//   papers / job / boardseen / offer                              (your contracts and licences: see WriteCaptainContracts)
//   contractseq / boardmade / contract                            (the contracts on offer: see WriteContracts)
//   end
// Unknown keys are ignored, so newer saves still partly load in older builds.
#include "save.h"
#include "contracts.h"
#include "economy.h"
#include "shipyard.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

std::string SaveDirectory()
{
    std::string dir = std::string(GetApplicationDirectory()) + "saves";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir + (char)std::filesystem::path::preferred_separator;
}

std::string SavePathFor(const std::string &name) { return SaveDirectory() + name + ".txt"; }
std::string QuicksavePath()                      { return SavePathFor("quicksave"); }

bool ValidSaveName(const std::string &name)
{
    if (name.empty() || name.size() > 40 || name[0] == ' ' || name.back() == ' ' || name[0] == '.') return false;
    for (char c : name)
        if (!(isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_' || c == '.')) return false;
    return true;
}

bool SaveGame(const GameState &g, const char *path, const std::string &name)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    const Player &p = g.player;
    const Ship &s = p.ship;
    fprintf(f, "SPACEFLIGHT_SAVE %d\n", SAVE_VERSION);
    if (!name.empty()) fprintf(f, "name %s\n", name.c_str());
    if (!g.shipName.empty()) fprintf(f, "shipname %s\n", g.shipName.c_str());
    fprintf(f, "location %s\n", g.systems[s.anchor].name.c_str());
    fprintf(f, "seed %u\n", g.seed);
    fprintf(f, "clock %.17g\n", g.clock);
    fprintf(f, "ship %lld %.17g %.17g %.17g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %d %.9g\n", (long long)s.anchor,
            s.pos.x, s.pos.y, s.pos.z, s.vel.x, s.vel.y, s.vel.z, s.rot.x, s.rot.y, s.rot.z, s.rot.w, s.dampeners ? 1 : 0, s.throttle);
    fprintf(f, "fuel %.17g\n", p.fuel);
    fprintf(f, "credits %.17g\n", p.credits);
    if (p.marked > 0.0) fprintf(f, "marked %.17g\n", p.marked);
    for (int o = 0; o < GOOD_COUNT; o++)
        if (p.cargo[o] > 0.0) fprintf(f, "cargo %d %.17g\n", o, p.cargo[o]);
    for (const auto &m : g.deltas.mined)
        fprintf(f, "mined %llu %.9g\n", (unsigned long long)m.first, m.second);
    for (int64_t sys : g.know.charted) fprintf(f, "charted %lld\n", (long long)sys);
    for (uint64_t id : g.know.scanned) fprintf(f, "scanned %llu\n", (unsigned long long)id);
    for (size_t i = 0; i < g.standing.size(); i++) if (g.standing[i] != 0.0f) fprintf(f, "standing %d %.9g\n", (int)i, g.standing[i]);
    std::string econ;
    WriteBuild(g, econ);
    WritePrices(g, econ);
    WriteEconomy(g, econ);
    WriteCaptainContracts(g, econ);
    WriteContracts(g, econ);
    fputs(econ.c_str(), f);
    fprintf(f, "end\n");
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool LoadGame(GameState &g, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    static char line[8192];
    int version = 0;
    if (!fgets(line, sizeof line, f) || sscanf(line, "SPACEFLIGHT_SAVE %d", &version) != 1 ||
        version > SAVE_VERSION || version < SAVE_OLDEST) {
        fclose(f);
        return false;
    }

    // Read everything first; only touch the game once the file has parsed
    unsigned seed = DEFAULT_GALAXY_SEED;
    double   clock = 0.0;
    Player   p;
    MinedMap mined;
    Knowledge know;
    Captain scratch;                               // (price records are read into this)
    std::vector<std::pair<std::string, std::string>> econ;   // the economy's lines, applied once the galaxy is rebuilt
    std::string shipName;
    std::vector<std::pair<int, float>> standing;
    bool     haveShip = false, ended = false;
    while (fgets(line, sizeof line, f)) {
        char key[32] = "";
        if (sscanf(line, "%31s", key) != 1) continue;
        const char *rest = line + strlen(key);
        if (!strcmp(key, "seed"))         sscanf(rest, "%u", &seed);
        else if (!strcmp(key, "clock"))   sscanf(rest, "%lf", &clock);
        else if (!strcmp(key, "fuel"))    sscanf(rest, "%lf", &p.fuel);
        else if (!strcmp(key, "credits")) sscanf(rest, "%lf", &p.credits);
        else if (!strcmp(key, "marked")) sscanf(rest, "%lf", &p.marked);
        else if (!strcmp(key, "ship")) {
            Ship &s = p.ship;
            int damp = 1;
            long long anchor = -1;
            haveShip = sscanf(rest, "%lld %lf %lf %lf %f %f %f %f %f %f %f %d %f", &anchor, &s.pos.x, &s.pos.y, &s.pos.z,
                              &s.vel.x, &s.vel.y, &s.vel.z, &s.rot.x, &s.rot.y, &s.rot.z, &s.rot.w, &damp, &s.throttle) >= 12;
            s.anchor = anchor;
            s.dampeners = damp != 0;
        } else if (!strcmp(key, "cargo")) {
            int ore; double t;
            if (version >= SAVE_GOODS && sscanf(rest, "%d %lf", &ore, &t) == 2 && ore >= 0 && ore < GOOD_COUNT) p.cargo[ore] = t;
        } else if (!strcmp(key, "mined")) {
            unsigned long long id; float left;
            if (sscanf(rest, "%llu %f", &id, &left) == 2) mined[(uint64_t)id] = left;
        } else if (!strcmp(key, "charted")) {
            long long sys;
            if (sscanf(rest, "%lld", &sys) == 1) know.charted.insert(sys);
        } else if (ReadPriceLine(scratch, key, rest) || ReadCaptainContractLine(scratch, key, rest) || ReadBuildLine(scratch, key, rest)) {
        } else if (!strcmp(key, "econ") || !strcmp(key, "stock") || !strcmp(key, "owner") || !strcmp(key, "people") || !strcmp(key, "industry") || !strcmp(key, "border") || !strcmp(key, "fleet") || !strcmp(key, "clanships") || !strcmp(key, "money") || !strcmp(key, "coffer") || !strcmp(key, "faction") ||
                   !strcmp(key, "embargo") || !strcmp(key, "shock") || !strcmp(key, "news") || !strcmp(key, "contractseq") ||
                   !strcmp(key, "boardmade") || !strcmp(key, "contract")) {
            econ.push_back({ key, rest });
        } else if (!strcmp(key, "shipname")) {
            shipName = rest + (*rest == ' ' ? 1 : 0);
            while (!shipName.empty() && (shipName.back() == '\n' || shipName.back() == '\r')) shipName.pop_back();
        } else if (!strcmp(key, "standing")) {
            int id; float v;
            if (sscanf(rest, "%d %f", &id, &v) == 2) standing.push_back({ id, v });
        } else if (!strcmp(key, "scanned")) {
            unsigned long long id;
            if (sscanf(rest, "%llu", &id) == 1) know.scanned.insert((uint64_t)id);
        } else if (!strcmp(key, "end")) { ended = true; break; }
    }
    fclose(f);
    if (!haveShip || !ended) return false;

    GameState fresh;
    NewGame(fresh, seed);
    if (!fresh.systems.Exists(p.ship.anchor)) return false;
    fresh.clock         = clock;
    if (!econ.empty()) {
        EnsureEconomy(fresh);
        for (const auto &l : econ)
            if (!ReadEconomyLine(fresh, l.first.c_str(), l.second.c_str())) ReadContractLine(fresh, l.first.c_str(), l.second.c_str());
    }
    fresh.know.prices   = std::move(scratch.know.prices);
    fresh.stakes        = std::move(scratch.stakes);
    fresh.jobs          = std::move(scratch.jobs);
    fresh.offers        = std::move(scratch.offers);
    fresh.boards        = std::move(scratch.boards);
    fresh.papers        = std::move(scratch.papers);
    p.build = scratch.player.build;                 // (a save from before ships were built from parts: the starter ship)
    if (version < SAVE_SENSORS)                     // (from before sensors: what was the scanner is the telescope; the rest come fitted)
        for (PartType t : { PartType::Radar, PartType::IrArray, PartType::TachyonDetector }) {
            bool has = false;
            for (const Part &q : p.build.parts) if (q.type == t) has = true;
            Part add; add.type = t;
            if (!has) p.build.parts.push_back(add);
        }
    fresh.player        = p;
    Refit(fresh);
    fresh.player.power.bank = fresh.player.spec.bankTJ; fresh.player.power.driveCap = fresh.player.spec.driveCapTJ;
    fresh.deltas.mined  = std::move(mined);
    fresh.shipName      = shipName;
    fresh.know.charted.insert(know.charted.begin(), know.charted.end());   // (on top of home, which a new game already has)
    fresh.know.scanned  = std::move(know.scanned);
    for (const auto &s : standing) if (s.first >= 0 && s.first < (int)fresh.standing.size()) fresh.standing[s.first] = s.second;
    fresh.loaded        = LoadedSystem();          // reload around the saved position
    fresh.field         = AsteroidField();
    UpdateLoadedSystem(fresh);
    fresh.events.clear();
    g = std::move(fresh);
    return true;
}

SaveSummary ReadSaveSummary(const char *path)
{
    SaveSummary sum;
    FILE *f = fopen(path, "r");
    if (!f) return sum;
    char line[512];
    if (!fgets(line, sizeof line, f) || sscanf(line, "SPACEFLIGHT_SAVE %d", &sum.version) != 1) { fclose(f); return sum; }
    auto rest = [](const char *l, const char *key) {          // the text after "key ", without the newline
        std::string r = l + strlen(key) + 1;
        while (!r.empty() && (r.back() == '\n' || r.back() == '\r')) r.pop_back();
        return r;
    };
    bool ended = false;
    while (fgets(line, sizeof line, f)) {
        char key[32] = "";
        if (sscanf(line, "%31s", key) != 1) continue;
        const char *r = line + strlen(key);
        if (!strcmp(key, "name"))          sum.name = rest(line, "name");
        else if (!strcmp(key, "location")) sum.location = rest(line, "location");
        else if (!strcmp(key, "seed"))     sscanf(r, "%u", &sum.seed);
        else if (!strcmp(key, "clock"))    sscanf(r, "%lf", &sum.clock);
        else if (!strcmp(key, "credits"))  sscanf(r, "%lf", &sum.credits);
        else if (!strcmp(key, "fuel"))     sscanf(r, "%lf", &sum.fuel);
        else if (!strcmp(key, "cargo"))    { int o; double t; if (sscanf(r, "%d %lf", &o, &t) == 2) sum.cargo += t; }
        else if (!strcmp(key, "end"))      { ended = true; break; }
    }
    fclose(f);
    sum.valid = ended && sum.version <= SAVE_VERSION && sum.version >= SAVE_OLDEST;
    return sum;
}

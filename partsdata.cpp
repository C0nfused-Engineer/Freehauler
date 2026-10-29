// partsdata.cpp - reading parts.txt
#include "partsdata.h"
#include "partsdefault.h"
#include "process.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

// ---------------------------------------------------------------------------
// What can be said in the file
// ---------------------------------------------------------------------------
double ReactorCurveAt(const PartModel &m, double load);

static const char *const TYPE_KEYS[(int)PartType::Count] = {
    "reactor", "engine", "thrusters", "hyperdrive", "warpdrive", "drivecap", "bank", "tank", "hold", "mininglaser", "scoop", "skimmer",
    "telescope", "radar", "irarray", "tachyondetector", "cabins",
};
static const char *const HULL_KEYS[HULL_COUNT] = { "courier", "hauler", "prospector", "cutter", "freighter", "bulk" };

struct ModelKey { const char *key; double PartModel::*value; };
static const ModelKey MODEL_KEYS[] = {
    { "mass", &PartModel::mass }, { "volume", &PartModel::volume }, { "cost", &PartModel::cost },
    { "burn", &PartModel::burn }, { "floor", &PartModel::floor }, { "restart", &PartModel::restart },
    { "efficiency", &PartModel::efficiency }, { "efficiency_grade", &PartModel::efficiencyGrade },
    { "thrust", &PartModel::thrust }, { "turn", &PartModel::turn }, { "draw", &PartModel::draw },
    { "speed", &PartModel::speed }, { "speed_grade", &PartModel::speedGrade },
    { "field_base", &PartModel::fieldBase }, { "field_mass", &PartModel::fieldMass }, { "field_power", &PartModel::fieldPower },
    { "accel", &PartModel::accel }, { "max_mass", &PartModel::maxMass }, { "jump", &PartModel::jump }, { "jump_grade", &PartModel::jumpGrade },
    { "capacity", &PartModel::capacity }, { "rate", &PartModel::rate }, { "range", &PartModel::range }, { "range_grade", &PartModel::rangeGrade },
    { "reach_size", &PartModel::reachSize }, { "reach_grade", &PartModel::reachGrade },
    { "area", &PartModel::area }, { "depth", &PartModel::depth }, { "catches", &PartModel::catches }, { "catches_grade", &PartModel::catchesGrade },
    { "resolution", &PartModel::resolution }, { "resolution_grade", &PartModel::resolutionGrade },
};

struct RuleKey { const char *key; double PartRules::*value; };
static const RuleKey RULE_KEYS[] = {
    { "grade_better", &PartRules::better }, { "grade_leaner", &PartRules::leaner },
    { "stretch_room", &PartRules::stretchRoom }, { "stretch_mass", &PartRules::stretchMass }, { "mount_mass", &PartRules::mountMass },
    { "stretch_cost", &PartRules::stretchCost }, { "stretch_cost_growth", &PartRules::stretchCostGrowth },
    { "mount_cost", &PartRules::mountCost }, { "mount_cost_growth", &PartRules::mountCostGrowth },
};

static std::string Trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::string Lower(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }

static int GoodNamed(const std::string &name)
{
    std::string want = Lower(name);
    for (int g = 0; g < GOOD_COUNT; g++) if (Lower(GoodData(g).name) == want) return g;
    return -1;
}

static bool Number(const std::string &s, double &out)
{
    if (s.empty()) return false;
    char *end = nullptr;
    out = strtod(s.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}

static std::vector<std::string> List(const std::string &s)
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= s.size()) {
        size_t comma = s.find(',', at);
        if (comma == std::string::npos) comma = s.size();
        out.push_back(Trim(s.substr(at, comma - at)));
        at = comma + 1;
    }
    return out;
}

// (the hulls' names are pointed at from HullDef: after any copy of a PartsData, point them again)
static void PointHulls(PartsData &d)
{
    for (int h = 0; h < HULL_COUNT; h++) { d.hulls[h].name = d.hullName[h].c_str(); d.hulls[h].blurb = d.hullBlurb[h].c_str(); }
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------
bool ParseParts(const std::string &text, PartsData &out, std::string &problem)
{
    PartsData d;
    for (int g = 0; g < PART_GRADES; g++) d.rules.gradeName[g] = "";
    bool hullSeen[HULL_COUNT] = {};
    enum { NONE, RULES, FUEL, HULL, MODEL } in = NONE;
    int fuel = -1, hull = -1;
    PartModel *model = nullptr;
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&](const std::string &s) { for (unsigned char c : s) { hash ^= c; hash *= 1099511628211ull; } hash ^= '\n'; hash *= 1099511628211ull; };
    int lineNo = 0;
    auto fail = [&](const std::string &why) { problem = "line " + std::to_string(lineNo) + ": " + why; return false; };

    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1; lineNo++;
        size_t hashMark = line.find('#');
        if (hashMark != std::string::npos) line.erase(hashMark);
        line = Trim(line);
        if (line.empty()) continue;

        if (line[0] == '[') {
            if (line.back() != ']') return fail("a section heading needs its closing ]");
            std::string head = Trim(line.substr(1, line.size() - 2));
            size_t space = head.find_first_of(" \t");
            std::string kind = Lower(space == std::string::npos ? head : head.substr(0, space)), id = space == std::string::npos ? "" : Trim(head.substr(space + 1));
            mix("[" + kind + " " + Lower(id) + "]");
            model = nullptr;
            if (kind == "rules") { in = RULES; continue; }
            if (kind == "fuel") {
                fuel = GoodNamed(id);
                if (fuel < 0) return fail("there is no good called \"" + id + "\"");
                in = FUEL; continue;
            }
            if (kind == "hull") {
                hull = -1;
                for (int h = 0; h < HULL_COUNT; h++) if (Lower(id) == HULL_KEYS[h]) hull = h;
                if (hull < 0) return fail("there is no hull \"" + id + "\" (the hulls are courier, hauler, prospector, cutter, freighter and bulk)");
                hullSeen[hull] = true;
                in = HULL; continue;
            }
            int type = -1;
            for (int t = 0; t < (int)PartType::Count; t++) if (kind == TYPE_KEYS[t]) type = t;
            if (type < 0) return fail("\"" + kind + "\" isn't a kind of part");
            if (id.empty()) return fail("a part's section needs a name for the model, as in [reactor fission]");
            for (const PartModel &m : d.models[type]) if (Lower(m.id) == Lower(id)) return fail("there are two [" + kind + " " + id + "] sections");
            if (d.models[type].size() >= 255) return fail("too many models of one part");
            d.models[type].push_back(PartModel());
            model = &d.models[type].back();
            model->id = id; model->name = id;
            in = MODEL;
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) return fail("expected key = value");
        std::string key = Lower(Trim(line.substr(0, eq))), value = Trim(line.substr(eq + 1));
        mix(key + "=" + value);
        double x = 0.0;
        bool known = false;
        switch (in) {
        case NONE: return fail("a value before any [section]");
        case RULES:
            if (key == "grade_names") {
                std::vector<std::string> names = List(value);
                if ((int)names.size() != PART_GRADES) return fail("grade_names wants " + std::to_string(PART_GRADES) + " names");
                for (int g = 0; g < PART_GRADES; g++) d.rules.gradeName[g] = names[g];
                known = true;
            } else if (key == "grade_cost") {
                std::vector<std::string> costs = List(value);
                if ((int)costs.size() != PART_GRADES) return fail("grade_cost wants " + std::to_string(PART_GRADES) + " numbers");
                for (int g = 0; g < PART_GRADES; g++) if (!Number(costs[g], d.rules.gradeCost[g]) || d.rules.gradeCost[g] <= 0.0) return fail("\"" + costs[g] + "\" isn't a price");
                known = true;
            } else for (const RuleKey &k : RULE_KEYS)
                if (key == k.key) { if (!Number(value, x)) return fail("\"" + value + "\" isn't a number"); d.rules.*k.value = x; known = true; }
            break;
        case FUEL:
            if (key == "energy") { if (!Number(value, x) || x <= 0.0) return fail("a fuel's energy must be a number above zero"); d.fuelTJ[fuel] = x; known = true; }
            break;
        case HULL:
            if (key == "name") { d.hullName[hull] = value; known = true; }
            else if (key == "blurb") { d.hullBlurb[hull] = value; known = true; }
            else {
                if (!Number(value, x)) return fail("\"" + value + "\" isn't a number");
                if (key == "volume") { d.hulls[hull].volume = x; known = true; }
                else if (key == "hardpoints") { d.hulls[hull].hardpoints = (int)x; known = true; }
                else if (key == "mass") { d.hulls[hull].mass = x; known = true; }
                else if (key == "cost") { d.hulls[hull].cost = x; known = true; }
            }
            break;
        case MODEL:
            if (key == "name") { model->name = value; known = true; }
            else if (key == "curve") {                   // load:efficiency, load:efficiency, ...
                model->curve.clear();
                for (const std::string &point : List(value)) {
                    size_t colon = point.find(':');
                    double load = 0.0, eff = 0.0;
                    if (colon == std::string::npos || !Number(Trim(point.substr(0, colon)), load) || !Number(Trim(point.substr(colon + 1)), eff))
                        return fail("a curve is points of load:efficiency, as in 0.2:0.30, 0.8:0.41, 1:0.40");
                    model->curve.push_back({ load, eff });
                }
                known = true;
            }
            else if (key == "material" || key == "fuel") {
                int g = GoodNamed(value);
                if (g < 0) return fail("there is no good called \"" + value + "\"");
                (key == "fuel" ? model->fuel : model->material) = g;
                known = true;
            } else for (const ModelKey &k : MODEL_KEYS)
                if (key == k.key) { if (!Number(value, x)) return fail("\"" + value + "\" isn't a number"); model->*k.value = x; known = true; }
            break;
        }
        if (!known) return fail("\"" + key + "\" isn't something that can be set here");
    }

    // Is it all there, and can a ship be built from it?
    lineNo = 0;
    auto lacks = [&](const std::string &why) { problem = why; return false; };
    for (int g = 0; g < PART_GRADES; g++) if (d.rules.gradeName[g].empty()) return lacks("[rules] needs grade_names");
    if (d.rules.leaner * (PART_GRADES - 1) >= 1.0 || d.rules.better < 0.0) return lacks("[rules]: with those grades the best parts would weigh nothing");
    for (int h = 0; h < HULL_COUNT; h++) {
        if (!hullSeen[h]) return lacks(std::string("there is no [hull ") + HULL_KEYS[h] + "]");
        if (d.hullName[h].empty()) d.hullName[h] = HULL_KEYS[h];
        if (d.hulls[h].volume <= 0.0 || d.hulls[h].mass <= 0.0 || d.hulls[h].cost <= 0.0 || d.hulls[h].hardpoints < 0)
            return lacks(std::string("[hull ") + HULL_KEYS[h] + "] needs a volume, a mass and a cost above zero");
    }
    for (int t = 0; t < (int)PartType::Count; t++) {
        if (d.models[t].empty()) return lacks(std::string("there is no [") + TYPE_KEYS[t] + " ...] section: every kind of part needs at least one model");
        for (const PartModel &m : d.models[t]) {
            std::string where = std::string("[") + TYPE_KEYS[t] + " " + m.id + "]";
            if (m.mass <= 0.0 || m.volume <= 0.0 || m.cost <= 0.0) return lacks(where + " needs a mass, a volume and a cost above zero");
            if (m.material < 0) return lacks(where + " needs a material: the good a yard builds it from");
            switch ((PartType)t) {
            case PartType::Reactor:
                if (m.fuel < 0 || d.fuelTJ[m.fuel] <= 0.0) return lacks(where + " needs a fuel, and that fuel a [fuel ...] section giving its energy");
                if (m.burn <= 0.0 || m.floor < 0.0 || m.floor > 0.9 || m.restart < 0.0) return lacks(where + " needs a burn rate above zero, a floor between 0 and 0.9, and a restart time");
                if (m.curve.size() < 2 || m.curve.back().first != 1.0) return lacks(where + " needs a curve of at least two points, the last of them at load 1");
                for (size_t k = 0; k < m.curve.size(); k++) {
                    if (m.curve[k].first <= 0.0 || m.curve[k].first > 1.0 || (k > 0 && m.curve[k].first <= m.curve[k - 1].first)) return lacks(where + ": the curve's loads must rise from above 0 to 1");
                    if (m.curve[k].second <= 0.0 || m.curve[k].second + m.efficiencyGrade * (PART_GRADES - 1) >= 1.0) return lacks(where + ": an efficiency must be between 0 and 1, the best grade's too");
                }
                for (int k = 1; k <= 100; k++)      // (more fuel must mean more power, or there'd be no telling what burn rate a load asks for)
                    if (k * ReactorCurveAt(m, k / 100.0) < (k - 1) * ReactorCurveAt(m, (k - 1) / 100.0))
                        return lacks(where + ": its efficiency falls so fast near load " + std::to_string(k) + "% that burning more would give less power");
                break;
            case PartType::Engine:     if (m.thrust <= 0.0) return lacks(where + " needs a thrust"); break;
            case PartType::Thrusters:  if (m.turn <= 0.0) return lacks(where + " needs a turn"); break;
            case PartType::HyperDrive:
            case PartType::WarpDrive:
                if (m.speed <= 0.0 || m.fieldMass <= 0.0 || m.fieldPower <= 0.0 || m.accel <= 0.0 || m.maxMass <= 0.0)
                    return lacks(where + " needs a speed, field_mass, field_power, accel and max_mass above zero");
                if (t == (int)PartType::WarpDrive && (m.jump <= 0.0 || m.jumpGrade * (PART_GRADES - 1) >= 1.0)) return lacks(where + " needs a jump energy above zero");
                break;
            case PartType::DriveCap:
                if (m.capacity <= 0.0 || m.efficiency <= 0.0 || m.efficiency + m.efficiencyGrade * (PART_GRADES - 1) > 1.0) return lacks(where + " needs a capacity, and an efficiency between 0 and 1");
                break;
            case PartType::Bank: case PartType::Tank: case PartType::Hold: case PartType::Cabins:
                if (m.capacity <= 0.0) return lacks(where + " needs a capacity");
                break;
            case PartType::MiningLaser: if (m.rate <= 0.0 || m.range <= 0.0) return lacks(where + " needs a rate and a range"); break;
            case PartType::Scoop: case PartType::Skimmer: if (m.rate <= 0.0) return lacks(where + " needs a rate"); break;
            case PartType::TachyonDetector:
                if (m.area <= 0.0 || m.depth <= 0.0 || m.catches <= 0.0 || m.resolution <= 0.0) return lacks(where + " needs an area, a depth, catches and a resolution");
                break;
            default: break;
            }
        }
    }
    d.hash = hash;
    out = d;
    PointHulls(out);
    problem.clear();
    return true;
}

// ---------------------------------------------------------------------------
// What the game flies by
// ---------------------------------------------------------------------------
static PartsData &Current()
{
    static PartsData data;
    static bool ready = false;
    if (!ready) {
        ready = true;
        std::string problem;
        if (!ParseParts(DEFAULT_PARTS_TEXT, data, problem)) { fprintf(stderr, "The built-in parts are broken: %s\n", problem.c_str()); abort(); }
    }
    return data;
}

const PartsData &Parts() { return Current(); }

double ReactorCurveAt(const PartModel &m, double load)
{
    if (m.curve.empty()) return 0.0;
    if (load <= m.curve.front().first) return m.curve.front().second;       // (below the first point: as there)
    for (size_t k = 1; k < m.curve.size(); k++)
        if (load <= m.curve[k].first) {
            double f = (load - m.curve[k - 1].first) / (m.curve[k].first - m.curve[k - 1].first);
            return m.curve[k - 1].second + (m.curve[k].second - m.curve[k - 1].second) * f;
        }
    return m.curve.back().second;
}
const char *DefaultPartsText() { return DEFAULT_PARTS_TEXT; }
uint64_t PartsHash() { return Current().hash; }
int ModelCount(PartType type) { return (int)type < (int)PartType::Count ? (int)Current().models[(int)type].size() : 0; }

const PartModel &PartModelOf(PartType type, int variant)
{
    const std::vector<PartModel> &all = Current().models[(int)type < (int)PartType::Count ? (int)type : 0];
    return all[variant >= 0 && variant < (int)all.size() ? variant : 0];
}

void UseDefaultParts()
{
    std::string problem;
    PartsData fresh;
    if (ParseParts(DEFAULT_PARTS_TEXT, fresh, problem)) { Current() = fresh; PointHulls(Current()); }
}

bool LoadPartsFile(const std::string &path, std::string &problem)
{
    problem.clear();
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    fclose(f);
    PartsData fresh;
    if (!ParseParts(text, fresh, problem)) { problem = path + ", " + problem; return false; }
    Current() = fresh;
    PointHulls(Current());
    return true;
}

std::string LoadPartsBesideExe()
{
    std::string problem;
    std::error_code ec;
    std::filesystem::path beside = std::filesystem::path(SelfPath()).parent_path() / "parts.txt";
    LoadPartsFile(beside.string(), problem);
    return problem;
}

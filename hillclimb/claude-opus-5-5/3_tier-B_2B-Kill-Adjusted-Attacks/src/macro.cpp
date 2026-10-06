#include "common.h"

#include <chrono>

namespace Strat {
bool expandNow = false;
bool wantCannons = false;
int lastRetreatFrame = -100000;
}  // namespace Strat

namespace Macro {
int reservedMin = 0, reservedGas = 0;

struct Task {
    UnitType type;
    TilePosition tile;
    Unit builder = nullptr;
    int created = 0;
    Position near = Positions::None;
    int failures = 0;
    int arrived = -1;  // frame the builder first got close to the site
    int bestDist = 1 << 30;
    int bestGround = 1 << 30;
    int progressFrame = 0;
};
static std::vector<Task> tasks;

int pendingBuild(UnitType t) {
    int n = 0;
    for (auto &k : tasks)
        if (k.type == t) ++n;
    return n;
}

static int queued(UnitType t) {
    int n = 0;
    for (auto u : Self()->getUnits())
        if (u->getType().canProduce() || u->getType() == UT::Protoss_Gateway || u->getType() == UT::Protoss_Nexus ||
            u->getType() == UT::Protoss_Robotics_Facility || u->getType() == UT::Protoss_Stargate)
            for (auto q : u->getTrainingQueue())
                if (q == t) ++n;
    return n;
}

int planned(UnitType t) {
    if (t.isBuilding()) return Self()->allUnitCount(t) + pendingBuild(t);
    return Self()->completedUnitCount(t) + queued(t);
}

static int done(UnitType t) { return Self()->completedUnitCount(t); }

void onUnitCreate(Unit u) {
    if (u->getPlayer() != Self() || !u->getType().isBuilding()) return;
    for (auto it = tasks.begin(); it != tasks.end(); ++it)
        if (it->type == u->getType() && it->tile == u->getTilePosition()) {
            LOG("placed %s at (%d,%d)", u->getType().c_str(), it->tile.x, it->tile.y);
            if (it->builder && it->builder->exists()) Workers::release(it->builder);
            tasks.erase(it);
            return;
        }
}

static void updateTasks() {
    reservedMin = reservedGas = 0;
    for (auto it = tasks.begin(); it != tasks.end();) {
        auto &k = it->type;
        // placed?
        bool placed = false;
        for (auto u : Broodwar->getUnitsOnTile(it->tile))
            if (u->getPlayer() == Self() && u->getType() == k) placed = true;
        if (placed) {
            if (it->builder && it->builder->exists()) Workers::release(it->builder);
            it = tasks.erase(it);
            continue;
        }
        if (it->builder && it->builder->exists() && Now() % 8 == 0) {
            // progress by air or along the ground path (paths can lead away from the site for a while)
            int d = it->builder->getDistance(Position(it->tile));
            int g = MapInfo::groundDist(it->builder->getPosition(), Position(it->tile) + Position(16, 16));
            if (d < it->bestDist - 16) it->bestDist = d, it->progressFrame = Now();
            if (g >= 0 && g < it->bestGround - 16) it->bestGround = g, it->progressFrame = Now();
        }
        if (it->progressFrame == 0) it->progressFrame = Now();
        bool stale = it->arrived >= 0 ? Now() - it->arrived > 24 * 25
                                      : Now() - it->progressFrame > 24 * (k == UT::Protoss_Nexus ? 30 : 12);
        if (stale || it->failures > 6) {
            LOG("giving up on %s at (%d,%d)", k.c_str(), it->tile.x, it->tile.y);
            if (k == UT::Protoss_Nexus) Placement::markFailed(it->tile);
            if (it->builder && it->builder->exists()) Workers::release(it->builder);
            it = tasks.erase(it);
            continue;
        }
        if (!it->builder || !it->builder->exists() || !Workers::hasRole(it->builder, Workers::Role::Build)) {
            it->builder = Workers::takeBuilder(Position(it->tile));
            if (!it->builder) {
                ++it;
                continue;
            }
        }
        Unit b = it->builder;
        Position site = Position(it->tile) + Position(k.tileWidth() * 16, k.tileHeight() * 16);
        // site still valid?
        if (Now() % 12 == 0 && Broodwar->isVisible(it->tile) && !Broodwar->canBuildHere(it->tile, k, b)) {
            ++it->failures;
            TilePosition t2 = Placement::find(k, it->near);
            if (t2.isValid() && t2 != TilePositions::None) it->tile = t2;
            else {
                if (b->exists()) Workers::release(b);
                it = tasks.erase(it);
                continue;
            }
        }
        if (Now() % 240 == 0 && Now() - it->created > 480)
            LOG("task %s at (%d,%d) waiting: builder %d at (%d,%d) dist %d order %s", k.c_str(), it->tile.x, it->tile.y,
                b->getID(), b->getTilePosition().x, b->getTilePosition().y, b->getDistance(site), b->getOrder().c_str());
        if (it->arrived < 0 && b->getDistance(site) < 32 * 8) it->arrived = Now();
        reservedMin += k.mineralPrice();
        reservedGas += k.gasPrice();
        bool afford = Self()->minerals() >= k.mineralPrice() && Self()->gas() >= k.gasPrice();
        if (afford && b->getDistance(site) < 32 * 7) {
            if (b->getOrder() != Orders::PlaceBuilding || b->getBuildType() != k) {
                if (!b->build(k, it->tile)) {
                    auto e = Broodwar->getLastError();
                    if (Now() % 24 == 0) ++it->failures;
                    if (e != Errors::Insufficient_Minerals && e != Errors::Insufficient_Gas)
                        if (Now() % 24 == 0) LOG("build %s failed: %s", k.c_str(), e.c_str());
                }
            }
        } else if (b->getDistance(site) > 48) {
            // follow the map analysis' chokepoints: the engine's own path finding can get stuck on narrow ledges
            if (b->getOrder() != Orders::PlaceBuilding)
                cmdMove(b, b->getDistance(site) > 32 * 10 ? MapInfo::nextWaypoint(b->getPosition(), site) : site);
        }
        ++it;
    }
}

static bool newTask(UnitType t, Position near = Positions::None) {
    auto t0 = std::chrono::steady_clock::now();
    TilePosition tile = Placement::find(t, near);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (ms > 10) LOG("placement of %s took %.1f ms", t.c_str(), ms);
    if (!tile.isValid() || tile == TilePositions::None) {
        if (Now() % 240 == 0) LOG("no site for %s", t.c_str());
        return false;
    }
    Position site = Position(tile) + Position(t.tileWidth() * 16, t.tileHeight() * 16);
    Unit b = Workers::takeBuilder(site);
    if (!b) return false;
    Task k;
    k.type = t;
    k.tile = tile;
    k.builder = b;
    k.created = Now();
    k.near = near;
    tasks.push_back(k);
    LOG("task %s at (%d,%d) builder %d", t.c_str(), tile.x, tile.y, b->getID());
    return true;
}

// ---------------------------------------------------------------- planning
static int budgetM, budgetG, supplyFree;

static double incomePerFrame() { return Workers::mineralWorkers() * 0.045; }

// Build: create a task when affordable soon; otherwise reserve if blocking.
static void wantBuild(UnitType t, bool block = true, Position near = Positions::None, int travel = 200) {
    int lead = int(incomePerFrame() * travel);
    if (budgetM + lead >= t.mineralPrice() && budgetG >= t.gasPrice()) {
        if (newTask(t, near)) {
            budgetM -= t.mineralPrice();
            budgetG -= t.gasPrice();
            return;
        }
        return;
    }
    if (block) {
        budgetM -= t.mineralPrice();
        budgetG -= t.gasPrice();
    }
}

static Unit idleProducer(UnitType producer) {
    for (auto u : Self()->getUnits())
        if (u->getType() == producer && u->isCompleted() && u->isPowered() && u->getTrainingQueue().empty() &&
            !u->isUpgrading() && !u->isResearching())
            return u;
    return nullptr;
}

static bool wantTrain(UnitType t, bool block = true) {
    Unit p = idleProducer(t.whatBuilds().first);
    if (!p) return false;
    if (budgetM >= t.mineralPrice() && budgetG >= t.gasPrice() && supplyFree >= t.supplyRequired()) {
        if (p->train(t)) {
            budgetM -= t.mineralPrice();
            budgetG -= t.gasPrice();
            supplyFree -= t.supplyRequired();
            return true;
        }
        return false;
    }
    if (block) {
        budgetM -= t.mineralPrice();
        budgetG -= t.gasPrice();
    }
    return false;
}

static void wantUpgrade(UpgradeType up, bool block = true) {
    int lvl = Self()->getUpgradeLevel(up);
    if (lvl >= Self()->getMaxUpgradeLevel(up) || Self()->isUpgrading(up)) return;
    UnitType where = up.whatUpgrades();
    Unit b = nullptr;
    for (auto u : Self()->getUnits())
        if (u->getType() == where && u->isCompleted() && !u->isUpgrading() && !u->isResearching() && u->isPowered())
            b = u;
    if (!b) return;
    int m = up.mineralPrice(lvl + 1), g = up.gasPrice(lvl + 1);
    if (budgetM >= m && budgetG >= g) {
        if (b->upgrade(up)) {
            LOG("upgrade %s %d", up.c_str(), lvl + 1);
            budgetM -= m, budgetG -= g;
        }
        return;
    }
    if (block) budgetM -= m, budgetG -= g;
}

static void wantResearch(TechType tech, bool block = true) {
    if (Self()->hasResearched(tech) || Self()->isResearching(tech)) return;
    Unit b = nullptr;
    for (auto u : Self()->getUnits())
        if (u->getType() == tech.whatResearches() && u->isCompleted() && !u->isUpgrading() && !u->isResearching() &&
            u->isPowered())
            b = u;
    if (!b) return;
    int m = tech.mineralPrice(), g = tech.gasPrice();
    if (budgetM >= m && budgetG >= g) {
        if (b->research(tech)) {
            LOG("research %s", tech.c_str());
            budgetM -= m, budgetG -= g;
        }
        return;
    }
    if (block) budgetM -= m, budgetG -= g;
}

static int nexusCount() { return Self()->allUnitCount(UT::Protoss_Nexus) + pendingBuild(UT::Protoss_Nexus); }

static int armyCount() {
    int n = 0;
    for (auto u : Self()->getUnits())
        if (u->isCompleted() && isArmyType(u->getType())) ++n;
    return n;
}

struct BOStep {
    int supply;
    UnitType type;
    int count;
};
static std::vector<BOStep> opening;
static bool openingDone = false;

static void initOpening() {
    opening = {
        {8, UT::Protoss_Pylon, 1},   {10, UT::Protoss_Gateway, 1}, {11, UT::Protoss_Assimilator, 1},
        {13, UT::Protoss_Zealot, 1}, {14, UT::Protoss_Cybernetics_Core, 1}, {15, UT::Protoss_Gateway, 2},
        {16, UT::Protoss_Zealot, 2},
    };
}

static void supplyRule() {
    int total = Self()->supplyTotal(), used = Self()->supplyUsed();
    if (total >= 400) return;
    int pending = 16 * (pendingBuild(UT::Protoss_Pylon) + Self()->incompleteUnitCount(UT::Protoss_Pylon));
    for (auto u : Self()->getUnits())
        if (u->getType() == UT::Protoss_Nexus && !u->isCompleted() && u->getRemainingBuildTime() < 500) pending += 18;
    int rate = 4 + 4 * done(UT::Protoss_Nexus) + 5 * done(UT::Protoss_Gateway) + 5 * done(UT::Protoss_Robotics_Facility);
    if (total < 18 * 2 + 1 && Self()->allUnitCount(UT::Protoss_Pylon) == 0 && pendingBuild(UT::Protoss_Pylon) == 0)
        return;  // the first pylon is in the opening
    if (used + rate >= total + pending) {
        // allow two pylons in flight when production is high
        int inFlight = pendingBuild(UT::Protoss_Pylon) + Self()->incompleteUnitCount(UT::Protoss_Pylon);
        if (inFlight == 0 || (rate >= 20 && inFlight < 2) || (rate >= 36 && inFlight < 3)) wantBuild(UT::Protoss_Pylon);
    }
}

// Nexuses that still have a useful amount of minerals around them (completed or not).
static int activeBases() {
    int n = 0;
    for (auto d : Self()->getUnits()) {
        if (d->getType() != UT::Protoss_Nexus) continue;
        if (!d->isCompleted()) {  // its minerals may not be visible yet
            ++n;
            continue;
        }
        int left = 0, patches = 0;
        for (auto m : Broodwar->getMinerals())
            if (m->exists() && m->getDistance(d) < 300 && m->getResources() > 0) left += m->getResources(), ++patches;
        if (patches >= 4 && left >= 1500) ++n;
    }
    return n + pendingBuild(UT::Protoss_Nexus);
}

// Enough probes for every mineral patch we can mine (2.2 each) and every refinery, plus the ones a new base will need.
static int workerTarget() {
    double t = 4;
    for (auto d : Self()->getUnits()) {
        if (d->getType() != UT::Protoss_Nexus) continue;
        if (!d->isCompleted()) {
            t += 8 * 2.2;
            continue;
        }
        int patches = 0;
        for (auto m : Broodwar->getMinerals())
            if (m->exists() && m->getDistance(d) < 300 && m->getResources() > 0) ++patches;
        t += patches * 2.2;
    }
    t += 3 * Self()->allUnitCount(UT::Protoss_Assimilator);
    t += 10 * pendingBuild(UT::Protoss_Nexus);
    if (Self()->allUnitCount(UT::Protoss_Nexus) <= 1) t = std::max(t, 30.0);  // the extras move to the natural
    return std::min(75, (int)t);
}

static void probeRule() {
    int workers = Self()->completedUnitCount(UT::Protoss_Probe) + queued(UT::Protoss_Probe);
    if (workers >= workerTarget()) return;
    for (auto u : Self()->getUnits())
        if (u->getType() == UT::Protoss_Nexus && u->isCompleted() && u->getTrainingQueue().empty()) {
            // probes come first: they may dip into money saved for a building
            if ((budgetM >= 50 || Self()->minerals() - reservedMin / 2 >= 50) && supplyFree >= 2) {
                if (u->train(UT::Protoss_Probe)) budgetM -= 50, supplyFree -= 2;
            } else
                budgetM -= 50;
        }
}

static void detectionRule() {
    // Emergency static detection against dark templar.
    bool threat = Info::dtThreat();
    if (!threat) return;
    if (planned(UT::Protoss_Forge) == 0) {
        wantBuild(UT::Protoss_Forge);
        return;
    }
    if (done(UT::Protoss_Forge) == 0) return;
    // one cannon per mining base, in the mineral line
    for (auto d : Self()->getUnits()) {
        if (!d->getType().isResourceDepot() || !d->isCompleted()) continue;
        // mineral line centre
        Position sum(0, 0);
        int n = 0;
        for (auto m : Broodwar->getMinerals())
            if (m->getDistance(d) < 300) sum += m->getPosition(), ++n;
        if (!n) continue;
        Position line = sum / n;
        Position spot = (line + d->getPosition()) / 2;
        int cannons = 0;
        for (auto c : Self()->getUnits())
            if (c->getType() == UT::Protoss_Photon_Cannon && c->getDistance(spot) < 200) ++cannons;
        for (auto &k : tasks)
            if (k.type == UT::Protoss_Photon_Cannon && Position(k.tile).getDistance(spot) < 250) ++cannons;
        if (cannons >= 1) continue;
        bool powered = false;
        for (auto p : Self()->getUnits())
            if (p->getType() == UT::Protoss_Pylon && p->getDistance(spot) < 280) powered = powered || p->isCompleted();
        bool pylonComing = false;
        for (auto p : Self()->getUnits())
            if (p->getType() == UT::Protoss_Pylon && !p->isCompleted() && p->getDistance(spot) < 280) pylonComing = true;
        for (auto &k : tasks)
            if (k.type == UT::Protoss_Pylon && (Position(k.tile) + Position(32, 32)).getDistance(spot) < 280) pylonComing = true;
        if (!powered) {
            if (!pylonComing) wantBuild(UT::Protoss_Pylon, true, spot);
            continue;
        }
        if (pendingBuild(UT::Protoss_Photon_Cannon) < 2) wantBuild(UT::Protoss_Photon_Cannon, true, spot);
    }
}

static void unitRule() {
    int zealots = planned(UT::Protoss_Zealot), goons = planned(UT::Protoss_Dragoon);
    bool coreDone = done(UT::Protoss_Cybernetics_Core) > 0;
    bool legs = Self()->getUpgradeLevel(UpgradeTypes::Leg_Enhancements) > 0;
    for (auto g : Self()->getUnits()) {
        if (g->getType() != UT::Protoss_Gateway || !g->isCompleted() || !g->getTrainingQueue().empty()) continue;
        UnitType t = UT::Protoss_Zealot;
        int hts = planned(UT::Protoss_High_Templar);
        int archons = done(UT::Protoss_Archon);
        bool zealotHeavy = Info::count(UT::Protoss_Zealot) >= 8 && Info::count(UT::Protoss_Zealot) > Info::count(UT::Protoss_Dragoon);
        bool storm = Self()->hasResearched(TechTypes::Psionic_Storm);
        int armyN = planned(UT::Protoss_Dragoon) + planned(UT::Protoss_Zealot);
        bool wantHT = done(UT::Protoss_Templar_Archives) && budgetM >= 50 &&
                      ((zealotHeavy && budgetG >= 150 && archons * 2 + hts < 12) || (budgetG >= 350 && hts < 4) ||
                       (storm && budgetG >= 150 && hts < std::min(6, 1 + armyN / 6)));
        if (wantHT) {
            if (!wantTrain(UT::Protoss_High_Templar)) return;
            continue;
        }
        if (coreDone && budgetG >= 50) {
            double zr = legs ? 0.6 : 0.25;
            if (zealotHeavy) zr = legs ? 0.9 : 0.5;
            if (zealots < goons * zr && Self()->minerals() > 400) t = UT::Protoss_Zealot;
            else t = UT::Protoss_Dragoon;
        } else if (coreDone && Self()->gas() < 50 && Self()->minerals() < 250) {
            // wait for gas rather than spend on zealots
            budgetM -= 125;
            continue;
        }
        if (!wantTrain(t)) return;
        if (t == UT::Protoss_Zealot) ++zealots;
        else ++goons;
    }
}

static void planPostOpening() {
    bool coreDone = done(UT::Protoss_Cybernetics_Core) > 0;
    int supply = Self()->supplyUsed() / 2;
    int army = armyCount();
    bool threatened = Combat::homeThreat();
    int nexus = nexusCount();
    int gates = planned(UT::Protoss_Gateway);

    detectionRule();
    // the opening's buildings can fail (builder killed, spot blocked): make sure the basics exist
    if (planned(UT::Protoss_Gateway) == 0) wantBuild(UT::Protoss_Gateway);
    if (planned(UT::Protoss_Cybernetics_Core) == 0 && done(UT::Protoss_Gateway) > 0) wantBuild(UT::Protoss_Cybernetics_Core);
    bool rush = Info::rushThreat() && !Info::dtThreat();
    // Behind in army: units come before tech and expansions.
    int armySupply = 0;
    for (auto u : Self()->getUnits())
        if (isArmyType(u->getType())) armySupply += u->getType().supplyRequired();
    armySupply /= 2;
    double minutes = Now() / (24.0 * 60);
    int minArmy = (int)std::clamp((minutes - 3.0) * 5.0, 0.0, 40.0);
    bool pressured = (rush && armyCount() < 10) || (Combat::theirStrength > Combat::myStrength * 1.2 && supply < 120) ||
                     armySupply < minArmy || (coreDone && planned(UT::Protoss_Dragoon) < 3);
    if (pressured) unitRule();
    if (coreDone && planned(UT::Protoss_Dragoon) >= (Info::rushThreat() ? 3 : 1)) wantUpgrade(UpgradeTypes::Singularity_Charge);

    if (rush && gates < 4 && done(UT::Protoss_Gateway) >= 3) wantBuild(UT::Protoss_Gateway);
    // Robotics for observers.
    if (coreDone && planned(UT::Protoss_Robotics_Facility) == 0 &&
        ((rush ? (army >= 12 || Now() > 24 * 60 * 7) : planned(UT::Protoss_Dragoon) >= 4) || Info::dtThreat()))
        wantBuild(UT::Protoss_Robotics_Facility);
    if (done(UT::Protoss_Robotics_Facility)) {
        int wantObs = supply > 120 || Info::dtThreat() ? 3 : 2;
        if (planned(UT::Protoss_Observatory) == 0) wantBuild(UT::Protoss_Observatory);
        else if (done(UT::Protoss_Observatory) && planned(UT::Protoss_Observer) < wantObs) wantTrain(UT::Protoss_Observer);
    }

    // Expansion.
    // Against a one-base opponent (a gateway timing is coming) expand a little later.
    int enemyBases = 0;
    for (auto &b : MapInfo::bases)
        if (b.enemyHere) ++enemyBases;
    // against zealot pressure, expand once we hold the stronger army (they usually expand behind it)
    int natArmy = rush ? (Combat::myStrength >= Combat::theirStrength ? 10 : 16)
                       : (Info::dtThreat() || enemyBases < 2) ? 8 : 7;
    (void)enemyBases;
    bool wantNat = nexus < 2 && coreDone && !threatened &&
                   ((supply >= 30 && army >= natArmy) ||
                    (Self()->minerals() > 500 && Combat::theirStrength <= Combat::myStrength * 1.2));
    if (wantNat) wantBuild(UT::Protoss_Nexus, true, Positions::None, 400);
    // later bases: keep enough bases with minerals left, replacing the ones that mine out
    if (nexus >= 2 && done(UT::Protoss_Nexus) >= 2 && pendingBuild(UT::Protoss_Nexus) == 0 && !threatened) {
        int active = activeBases();
        int probes = Self()->completedUnitCount(UT::Protoss_Probe);
        int want = 2;
        if (supply >= 130 || Self()->minerals() > 800 || (Now() > 24 * 60 * 10 && probes >= 40 && armySupply >= 25)) want = 3;
        if (supply >= 150 && probes >= 50) want = 4;
        // a new base needs an army that can protect it
        bool safe = Combat::myStrength >= Combat::theirStrength * 0.9 || Self()->minerals() > 800;
        if (active < want && safe) wantBuild(UT::Protoss_Nexus, active < 3, Positions::None, 500);
    }

    // Gas.
    int gasTarget = 1;
    if (coreDone && Self()->allUnitCount(UT::Protoss_Probe) >= 19) gasTarget = 2;
    if (done(UT::Protoss_Nexus) >= 2 && Self()->allUnitCount(UT::Protoss_Probe) >= 30) gasTarget = 4;
    if (done(UT::Protoss_Nexus) >= 3 && Self()->allUnitCount(UT::Protoss_Probe) >= 45) gasTarget = 6;
    if (planned(UT::Protoss_Assimilator) < gasTarget && Placement::gasSpot().isValid()) wantBuild(UT::Protoss_Assimilator, Self()->gas() < 150);

    // Gateways.
    int gateTarget = planned(UT::Protoss_Dragoon) >= 2 ? 3 : 2;
    if (done(UT::Protoss_Nexus) >= 2) gateTarget = Self()->allUnitCount(UT::Protoss_Probe) >= 34 ? 7 : 5;
    if (done(UT::Protoss_Nexus) >= 3) gateTarget = 11;
    if (gates < gateTarget && (planned(UT::Protoss_Robotics_Facility) > 0 || gates < 3)) wantBuild(UT::Protoss_Gateway);

    // Forge / upgrades.
    if (done(UT::Protoss_Nexus) >= 2 && supply >= 55 && planned(UT::Protoss_Forge) == 0) wantBuild(UT::Protoss_Forge);
    if (done(UT::Protoss_Forge) && supply >= 110 && planned(UT::Protoss_Forge) < 2 && Self()->gas() > 150)
        wantBuild(UT::Protoss_Forge, false);
    if (done(UT::Protoss_Forge)) {
        wantUpgrade(UpgradeTypes::Protoss_Ground_Weapons, false);
        if (supply > 100) wantUpgrade(UpgradeTypes::Protoss_Ground_Armor, false);
    }
    // templar tech for storms once two bases run
    bool zealotHeavyEnemy = Info::count(UT::Protoss_Zealot) >= 8 && Info::count(UT::Protoss_Zealot) > Info::count(UT::Protoss_Dragoon);
    bool templarTech = done(UT::Protoss_Nexus) >= 2 && supply >= 90 && Self()->allUnitCount(UT::Protoss_Probe) >= 38;
    bool wantCitadel = done(UT::Protoss_Nexus) >= 2 && (templarTech || supply >= 140 || zealotHeavyEnemy);
    if (wantCitadel && planned(UT::Protoss_Citadel_of_Adun) == 0) wantBuild(UT::Protoss_Citadel_of_Adun, false);
    bool wantArchives = templarTech || supply >= 160 || Self()->gas() > 1000 || zealotHeavyEnemy;
    if (wantArchives && done(UT::Protoss_Citadel_of_Adun) && planned(UT::Protoss_Templar_Archives) == 0)
        wantBuild(UT::Protoss_Templar_Archives, false);
    if (done(UT::Protoss_Templar_Archives)) wantResearch(TechTypes::Psionic_Storm, true);
    if (done(UT::Protoss_Citadel_of_Adun)) wantUpgrade(UpgradeTypes::Leg_Enhancements, false);

    unitRule();
}

static void planOpening() {
    if (Info::rushThreat() && planned(UT::Protoss_Gateway) >= 2 && planned(UT::Protoss_Gateway) < 3 &&
        done(UT::Protoss_Gateway) >= 1)
        wantBuild(UT::Protoss_Gateway);
    for (auto &s : opening) {
        if (planned(s.type) >= s.count) continue;
        if (Self()->supplyUsed() / 2 < s.supply) return;
        if (Info::rushThreat() && !Info::dtThreat() && (s.type == UT::Protoss_Assimilator || s.type == UT::Protoss_Cybernetics_Core) &&
            planned(UT::Protoss_Zealot) < 5) {
            unitRule();
            return;
        }
        if (s.type.isBuilding()) wantBuild(s.type);
        else wantTrain(s.type);
        return;
    }
}

void update() {
    static bool init = false;
    if (!init) initOpening(), init = true;
    updateTasks();
    if (Now() % 2) return;
    budgetM = Self()->minerals() - reservedMin;
    budgetG = Self()->gas() - reservedGas;
    supplyFree = Self()->supplyTotal() - Self()->supplyUsed();

    if (!openingDone) {
        bool all = true;
        for (auto &s : opening)
            if (planned(s.type) < s.count && s.type.isBuilding()) all = false;
        if (all) {
            openingDone = true;
            LOG("opening done");
        }
    }
    // Under early attack we keep making units.
    probeRule();
    supplyRule();
    if (!openingDone) {
        if (Info::dtThreat()) detectionRule();
        planOpening();
        // extra zealots from idle gateways if rich
        if (budgetM >= 100 || Info::rushThreat()) unitRule();
    } else
        planPostOpening();

    if (Now() % (24 * 20) == 0)
        LOG("status min=%d gas=%d supply=%d/%d workers=%d army=%d tasks=%d mode=%s", Self()->minerals(),
            Self()->gas(), Self()->supplyUsed() / 2, Self()->supplyTotal() / 2, Workers::count(), armyCount(),
            (int)tasks.size(), Combat::mode.c_str());
}
}  // namespace Macro

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
        if (it->builder && it->builder->exists()) {
            int d = it->builder->getDistance(Position(it->tile));
            if (d < it->bestDist - 16) it->bestDist = d, it->progressFrame = Now();
        }
        if (it->progressFrame == 0) it->progressFrame = Now();
        bool stale = it->arrived >= 0 ? Now() - it->arrived > 24 * 25 : Now() - it->progressFrame > 24 * 12;
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
        if (Now() % 12 == 0 && !Broodwar->canBuildHere(it->tile, k, b)) {
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
            if (b->getOrder() != Orders::PlaceBuilding) cmdMove(b, site);
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
    int rate = 3 * done(UT::Protoss_Nexus) + 4 * done(UT::Protoss_Gateway) + 4 * done(UT::Protoss_Robotics_Facility);
    if (total < 18 * 2 + 1 && Self()->allUnitCount(UT::Protoss_Pylon) == 0 && pendingBuild(UT::Protoss_Pylon) == 0)
        return;  // the first pylon is in the opening
    if (used + rate >= total + pending) {
        // allow two pylons in flight when production is high
        int inFlight = pendingBuild(UT::Protoss_Pylon) + Self()->incompleteUnitCount(UT::Protoss_Pylon);
        if (inFlight == 0 || (rate >= 20 && inFlight < 2) || (rate >= 36 && inFlight < 3)) wantBuild(UT::Protoss_Pylon);
    }
}

static int workerTarget() {
    int n = std::min(nexusCount(), 4);
    return std::min(56, 20 + (n - 1) * 20 + std::min(n, 3) * 3);
}

static void probeRule() {
    int workers = Self()->completedUnitCount(UT::Protoss_Probe) + queued(UT::Protoss_Probe);
    if (workers >= workerTarget()) return;
    for (auto u : Self()->getUnits())
        if (u->getType() == UT::Protoss_Nexus && u->isCompleted() && u->getTrainingQueue().empty()) {
            if (budgetM >= 50 && supplyFree >= 2) {
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
        bool wantHT = done(UT::Protoss_Templar_Archives) && budgetM >= 50 &&
                      ((zealotHeavy && budgetG >= 150 && archons * 2 + hts < 12) || (budgetG >= 350 && hts < 4));
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
        ((rush ? (army >= 12 || Now() > 24 * 60 * 7) : true) || Info::dtThreat()))
        wantBuild(UT::Protoss_Robotics_Facility);
    if (done(UT::Protoss_Robotics_Facility)) {
        int wantObs = supply > 120 || Info::dtThreat() ? 3 : 2;
        if (planned(UT::Protoss_Observatory) == 0) wantBuild(UT::Protoss_Observatory);
        else if (done(UT::Protoss_Observatory) && planned(UT::Protoss_Observer) < wantObs) wantTrain(UT::Protoss_Observer);
    }

    // Expansion.
    bool wantNat = nexus < 2 && coreDone && !threatened &&
                   ((supply >= 30 && army >= (rush ? 16 : Info::dtThreat() ? 10 : 7)) || Self()->minerals() > 500);
    if (wantNat) wantBuild(UT::Protoss_Nexus, true, Positions::None, 400);
    if (nexus >= 2 && done(UT::Protoss_Nexus) >= 2 && nexus < 3 && !threatened &&
        (supply >= 130 || Self()->minerals() > 800))
        wantBuild(UT::Protoss_Nexus, true, Positions::None, 500);
    if (nexus >= 3 && done(UT::Protoss_Nexus) >= 3 && nexus < 4 && supply >= 160 && Workers::baseNeed() < 6)
        wantBuild(UT::Protoss_Nexus, false, Positions::None, 500);

    // Gas.
    int gasTarget = 1;
    if (planned(UT::Protoss_Robotics_Facility) > 0 && Self()->allUnitCount(UT::Protoss_Probe) >= 18) gasTarget = 2;
    if (done(UT::Protoss_Nexus) >= 2 && Self()->allUnitCount(UT::Protoss_Probe) >= 30) gasTarget = 4;
    if (done(UT::Protoss_Nexus) >= 3 && Self()->allUnitCount(UT::Protoss_Probe) >= 45) gasTarget = 6;
    if (planned(UT::Protoss_Assimilator) < gasTarget && Placement::gasSpot().isValid()) wantBuild(UT::Protoss_Assimilator, false);

    // Gateways.
    int gateTarget = planned(UT::Protoss_Dragoon) >= 3 ? 3 : 2;
    if (done(UT::Protoss_Nexus) >= 2) gateTarget = Self()->allUnitCount(UT::Protoss_Probe) >= 34 ? 7 : 5;
    if (done(UT::Protoss_Nexus) >= 3) gateTarget = 11;
    if (gates < gateTarget && (planned(UT::Protoss_Robotics_Facility) > 0 || gates < 3)) wantBuild(UT::Protoss_Gateway);

    // Forge / upgrades.
    if (done(UT::Protoss_Nexus) >= 2 && supply >= 55 && planned(UT::Protoss_Forge) == 0) wantBuild(UT::Protoss_Forge);
    if (done(UT::Protoss_Forge)) {
        wantUpgrade(UpgradeTypes::Protoss_Ground_Weapons, false);
        if (supply > 100) wantUpgrade(UpgradeTypes::Protoss_Ground_Armor, false);
    }
    if (done(UT::Protoss_Nexus) >= 2 && supply >= 140 && planned(UT::Protoss_Citadel_of_Adun) == 0)
        wantBuild(UT::Protoss_Citadel_of_Adun, false);
    if (done(UT::Protoss_Citadel_of_Adun)) wantUpgrade(UpgradeTypes::Leg_Enhancements, false);
    bool zealotHeavyEnemy = Info::count(UT::Protoss_Zealot) >= 8 && Info::count(UT::Protoss_Zealot) > Info::count(UT::Protoss_Dragoon);
    if (zealotHeavyEnemy && done(UT::Protoss_Nexus) >= 2 && planned(UT::Protoss_Citadel_of_Adun) == 0)
        wantBuild(UT::Protoss_Citadel_of_Adun, false);
    if (done(UT::Protoss_Citadel_of_Adun) && (supply >= 160 || Self()->gas() > 1000 || zealotHeavyEnemy) &&
        planned(UT::Protoss_Templar_Archives) == 0)
        wantBuild(UT::Protoss_Templar_Archives, false);

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

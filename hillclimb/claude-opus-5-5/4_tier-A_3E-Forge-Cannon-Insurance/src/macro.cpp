#include "common.h"

#include <chrono>

namespace Strat {
bool expandNow = false;
bool wantCannons = false;
int lastRetreatFrame = -100000;
bool timingOver = false;
bool timingActive() {
    if (timingOver) return false;
    int enemyBases = 0;
    for (auto &b : MapInfo::bases)
        if (b.enemyHere) ++enemyBases;
    // their own one-base gateway army is coming: hold it at home instead
    bool gates = Info::count(UT::Protoss_Gateway) >= 3 && enemyBases < 2;
    // a nexus first behind cannons: match the economy instead
    bool nexusFirst = enemyBases >= 2 && Now() < 24 * 60 * 3.5;
    if (nexusFirst) {
        timingOver = true;
        LOG("timing over (nexus first)");
        return false;
    }
    if (Now() > 24 * 60 * 10 || (Info::rushThreat() && !Info::dtThreat()) || gates) {
        timingOver = true;
        LOG("timing over (%s)", Now() > 24 * 60 * 10 ? "time" : gates ? "gateways" : "rush");
        return false;
    }
    return true;
}
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
    int blockedSince = -1;  // a base site blocked by units since
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
    // placed somewhere else (the site moved after the builder had its order): the task is done all the same
    for (auto it = tasks.begin(); it != tasks.end(); ++it)
        if (it->type == u->getType()) {
            LOG("placed %s at (%d,%d) for task at (%d,%d)", u->getType().c_str(), u->getTilePosition().x,
                u->getTilePosition().y, it->tile.x, it->tile.y);
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
            if (it->builder)
                LOG("task %s lost builder %d (exists %d)", k.c_str(), it->builder->getID(), it->builder->exists());
            it->builder = Workers::takeBuilder(Position(it->tile));
            if (!it->builder) {
                ++it;
                continue;
            }
            // progress is measured per builder
            it->bestDist = it->bestGround = 1 << 30;
            it->progressFrame = Now();
            it->arrived = -1;
        }
        Unit b = it->builder;
        Position site = Position(it->tile) + Position(k.tileWidth() * 16, k.tileHeight() * 16);
        // a base site blocked only by units (a scouting probe, an army passing by) stays the site: we wait rather
        // than move to a farther, less defensible base
        bool unitsOnly = false;
        if (k == UT::Protoss_Nexus && Now() % 12 == 0 && Broodwar->isVisible(it->tile) && !Broodwar->canBuildHere(it->tile, k, b)) {
            unitsOnly = true;
            for (int x = 0; x < 4; ++x)
                for (int y = 0; y < 3; ++y)
                    if (!Broodwar->isBuildable(it->tile.x + x, it->tile.y + y, true)) unitsOnly = false;
            if (unitsOnly && it->blockedSince < 0) it->blockedSince = Now();
            if (unitsOnly && Now() - it->blockedSince > 24 * 45) unitsOnly = false;  // camped: look elsewhere after all
            if (unitsOnly) {
                it->arrived = -1;
                it->progressFrame = Now();
            }
        }
        // site still valid?
        if (!unitsOnly && Now() % 12 == 0 && Broodwar->isVisible(it->tile) && !Broodwar->canBuildHere(it->tile, k, b)) {
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
                    if (Now() % 24 == 0 && !unitsOnly) ++it->failures;
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
    // don't send a probe to build in the middle of the enemy army
    int hostile = 0;
    for (auto &[id, e] : Info::enemies)
        if (e.posValid && Now() - e.lastSeen < 24 * 15 && isArmyType(e.type) && e.pos.getDistance(site) < 32 * 10) ++hostile;
    if (hostile >= 2) return false;
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
        {8, UT::Protoss_Pylon, 1},   {10, UT::Protoss_Gateway, 1}, {12, UT::Protoss_Assimilator, 1},
        {13, UT::Protoss_Cybernetics_Core, 1}, {14, UT::Protoss_Zealot, 1}, {15, UT::Protoss_Gateway, 2},
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
    if (Self()->allUnitCount(UT::Protoss_Nexus) <= 1) t = std::max(t, 26.0);  // one base saturates early: the rest goes to units  // the extras move to the natural
    return std::min(75, (int)t);
}

static void probeRule() {
    int workers = Self()->completedUnitCount(UT::Protoss_Probe) + queued(UT::Protoss_Probe);
    if (workers >= workerTarget()) return;
    // a one-base opponent hits before extra probes pay off: units first until the hit is over
    int enemyBases = 0;
    for (auto &b : MapInfo::bases)
        if (b.enemyHere) ++enemyBases;
    if (enemyBases < 2 && Info::count(UT::Protoss_Gateway) >= 3 && Now() < 24 * 60 * 8.5 && Info::armySupplyKilled < 12 &&
        workers >= 28)
        return;
    for (auto u : Self()->getUnits())
        if (u->getType() == UT::Protoss_Nexus && u->isCompleted() && u->getTrainingQueue().empty()) {
            // probes come first: they may dip into money saved for a building
            if (Self()->minerals() >= 50 && supplyFree >= 2) {
                if (u->train(UT::Protoss_Probe)) budgetM -= 50, supplyFree -= 2;
            } else
                budgetM -= 50;
        }
}

static void detectionRule() {
    // Emergency static detection against dark templar.
    // a citadel alone gets observers (robotics); cannons only once templar tech is certain or we're blind
    bool threat = Info::seenTypes.count(UT::Protoss_Templar_Archives) || Info::seenTypes.count(UT::Protoss_Dark_Templar) ||
                  Info::seenTypes.count(UT::Protoss_Arbiter) || Info::techUnknown() ||
                  (Info::dtThreat() && Now() > 24 * 60 * 5 && done(UT::Protoss_Observer) == 0) ||
                  // one base with their tech unseen: dark templar can arrive before our first observer (~6:00);
                  // a cannon at the main mineral line costs no gas
                  (Strat::timingActive() && Now() > 24 * 250 && done(UT::Protoss_Observer) == 0);
    // early mass zealots: two cannons by the main mineral line hold them with the probes' help
    // (not against the first wave of an early rush: the forge would cost the zealots that hold it)
    bool zealotRush = false;  // measured: cannons against zealot rushes cost more units than they save
    if (!threat && !zealotRush) return;
    if (planned(UT::Protoss_Forge) == 0) {
        wantBuild(UT::Protoss_Forge);
        return;
    }
    if (done(UT::Protoss_Forge) == 0) return;
    // cannons in the mineral lines
    auto mainNexus = [&](Unit d) { return d->getTilePosition() == Self()->getStartLocation(); };
    for (auto d : Self()->getUnits()) {
        if (!d->getType().isResourceDepot() || !d->isCompleted()) continue;
        int want = threat ? 1 : 0;
        if (zealotRush && mainNexus(d)) want = 2;
        if (want == 0) continue;
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
            if (c->getType() == UT::Protoss_Photon_Cannon && c->getDistance(spot) < 250) ++cannons;
        for (auto &k : tasks)
            if (k.type == UT::Protoss_Photon_Cannon && Position(k.tile).getDistance(spot) < 250) ++cannons;
        if (cannons >= want) continue;
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

// zealots are coming early and we don't have the units to meet them: units before probes and buildings
static bool rushDefense() {
    return Info::rushThreat() && !Info::dtThreat() && Now() < 24 * 60 * 5.5 &&
           armyCount() < std::max(3, Info::count(UT::Protoss_Zealot) + 1);
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
        } else if (coreDone && Self()->gas() < 50 && Self()->minerals() < 250 && !rushDefense()) {
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
    // behind what a one-base gateway opponent probably has by now: units before infrastructure
    bool behind = Now() < 24 * 60 * 10 && Combat::myStrength < Combat::theirEstimate * 0.9 && !Info::dtThreat();
    bool pressured = (rush && armyCount() < 10) || (Combat::theirStrength > Combat::myStrength * 1.2 && supply < 120) ||
                     armySupply < minArmy || (coreDone && planned(UT::Protoss_Dragoon) < 3) || behind;
    // dark templar coming: the robotics facility (observers) comes before more units
    // (a few dragoons first: a citadel is as often a dragoon push with speedy zealots as dark templar)
    if (coreDone && Info::dtThreat() && planned(UT::Protoss_Robotics_Facility) == 0 && (army >= 3 || Now() > 24 * 60 * 4))
        wantBuild(UT::Protoss_Robotics_Facility);
    // dark templar can come from any build we did not see: detection by 5:30 comes before more units
    if (coreDone && !(rush && Now() < 24 * 60 * 6.5) && planned(UT::Protoss_Robotics_Facility) == 0 && Now() > 24 * 60 * 5.5)
        wantBuild(UT::Protoss_Robotics_Facility);
    // the first observers (detection, scouting) are cheap: they come before more units too
    if (done(UT::Protoss_Robotics_Facility) && planned(UT::Protoss_Observatory) == 0) wantBuild(UT::Protoss_Observatory);
    else if (done(UT::Protoss_Observatory) && planned(UT::Protoss_Observer) < 2) wantTrain(UT::Protoss_Observer);
    // dragoon range decides every dragoon fight: it comes before more units
    if (coreDone && planned(UT::Protoss_Dragoon) >= (Info::rushThreat() ? 3 : 1)) wantUpgrade(UpgradeTypes::Singularity_Charge);
    // they expanded early: no big attack can come soon, match their economy before more units
    int enemyBasesSeen = 0;
    for (auto &b : MapInfo::bases)
        if (b.enemyHere) ++enemyBasesSeen;
    // three or more gateways on one base: a dragoon timing is coming, units before anything else
    bool fourGate = !rush && Info::count(UT::Protoss_Gateway) >= 3 && enemyBasesSeen < 2 && Now() < 24 * 60 * 8;
    bool timing = Strat::timingActive();
    static bool fourGateLogged = false;
    if (fourGate && !fourGateLogged) {
        fourGateLogged = true;
        LOG("one-base gateway timing expected (%d gateways)", Info::count(UT::Protoss_Gateway));
    }
    bool matchNat = enemyBasesSeen >= 2 && nexus < 2 && coreDone && !threatened && army >= 3 && Now() < 24 * 60 * 8;
    if (timing) matchNat = false;
    if (matchNat) wantBuild(UT::Protoss_Nexus, true, Positions::None, 400);
    // Natural expansion: before more units once the army that guards it exists (the rest of the
    // build waits for it rather than the other way round).
    int enemyBases = enemyBasesSeen;
    // against zealot pressure, expand once we hold the stronger army (they usually expand behind it)
    // (a zealot rush usually keeps coming from a one-base economy: hold it off first)
    int natArmy = rush ? (Info::armySupplyKilled >= 12 || Now() > 24 * 60 * 7.5 || enemyBases >= 2 ? 10 : 18)
                   : timing ? 99
                   : fourGate ? (Info::armySupplyKilled >= 10 || Now() > 24 * 60 * 8.5 ? 10 : 99)  // hold their push on one base first
                   : (Info::count(UT::Protoss_Gateway) >= 3 && enemyBases < 2) ? 10 : Info::dtThreat() ? 8 : 5;
    bool wantNat = nexus < 2 && coreDone && !threatened &&
                   ((supply >= 24 && army >= natArmy) ||
                    (Self()->minerals() > 500 && Combat::theirStrength <= Combat::myStrength * 1.2 && !timing));
    if (wantNat && !matchNat) wantBuild(UT::Protoss_Nexus, true, Positions::None, 400);
    if (pressured) unitRule();

    if (rush && gates < 4 && done(UT::Protoss_Gateway) >= 3) wantBuild(UT::Protoss_Gateway);
    // Robotics for observers.
    if (coreDone && planned(UT::Protoss_Robotics_Facility) == 0 &&
        ((rush ? (!behind && (army >= 12 || Now() > 24 * 60 * 6.5))
               : fourGate ? Now() > 24 * 60 * 6.5 : timing ? Now() > 24 * 225 : ((planned(UT::Protoss_Dragoon) >= 4 && Now() > 24 * 270) || Now() > 24 * 60 * 6)) ||
         (Now() > 24 * 60 * 7 && !timing) || Info::dtThreat() || Info::techUnknown()))
        wantBuild(UT::Protoss_Robotics_Facility);
    if (done(UT::Protoss_Robotics_Facility)) {
        int wantObs = supply > 120 || Info::dtThreat() ? 3 : 2;
        if (planned(UT::Protoss_Observatory) == 0) wantBuild(UT::Protoss_Observatory);
        else if (done(UT::Protoss_Observatory) && planned(UT::Protoss_Observer) < wantObs) wantTrain(UT::Protoss_Observer);
    }

    // later bases: keep enough bases with minerals left, replacing the ones that mine out
    if (nexus >= 2 && done(UT::Protoss_Nexus) >= 2 && pendingBuild(UT::Protoss_Nexus) == 0 && !threatened) {
        int active = activeBases();
        int probes = Self()->completedUnitCount(UT::Protoss_Probe);
        int want = 2;
        // keep up with their expansions: a two-base economy loses to three
        if (supply >= 130 || Self()->minerals() > 800 || (Now() > 24 * 60 * 8 && probes >= 28 && armySupply >= 20) ||
            (enemyBasesSeen >= 3 && probes >= 32))
            want = 3;
        if ((supply >= 150 && probes >= 50) || (enemyBasesSeen >= 4 && probes >= 45)) want = 4;
        // a new base needs an army that can protect it
        // from 8:00 the third base is on schedule (with no army of theirs in our bases): two bases lose to three
        bool onSchedule = Now() > 24 * 60 * 8 && want == 3 && active == 2 && Combat::myStrength >= Combat::theirStrength * 0.7;
        bool safe = (Combat::myStrength >= Combat::theirStrength * 0.9 && !behind) || Self()->minerals() > 1000 || onSchedule;
        if (active < want && safe) wantBuild(UT::Protoss_Nexus, active < 3, Positions::None, 500);
    }

    // Gas.
    int gasTarget = 1;
    if (coreDone && Self()->allUnitCount(UT::Protoss_Probe) >= 19) gasTarget = 2;
    if (done(UT::Protoss_Nexus) >= 2 && Self()->allUnitCount(UT::Protoss_Probe) >= 30) gasTarget = 4;
    if (done(UT::Protoss_Nexus) >= 3 && Self()->allUnitCount(UT::Protoss_Probe) >= 45) gasTarget = 6;
    if (planned(UT::Protoss_Assimilator) < gasTarget && Placement::gasSpot().isValid()) wantBuild(UT::Protoss_Assimilator, Self()->gas() < 150);

    // Gateways.
    // one base: two gateways feed the natural; more only against a one-base gateway army or a rush
    int gateTarget = 2;
    if (coreDone) {
        if (rush || fourGate || timing) gateTarget = Now() > 24 * 210 && Self()->allUnitCount(UT::Protoss_Probe) >= 20 ? 4 : 3;
        else if (nexus < 2) gateTarget = Now() > 24 * 60 * 6 ? 3 : 2;
        else gateTarget = 3;
    }
    if (done(UT::Protoss_Nexus) >= 2) gateTarget = Self()->allUnitCount(UT::Protoss_Probe) >= 34 ? 7 : 5;
    if (done(UT::Protoss_Nexus) >= 3) gateTarget = 11;
    // minerals piling up with every gateway busy (one base runs out of gas first): more gateways for zealots
    if (coreDone && Self()->minerals() > 450 && pendingBuild(UT::Protoss_Gateway) == 0 && gates < 6 + 4 * (nexus - 1)) {
        bool allBusy = true;
        for (auto g : Self()->getUnits())
            if (g->getType() == UT::Protoss_Gateway && g->isCompleted() && g->getTrainingQueue().empty()) allBusy = false;
        if (allBusy) gateTarget = std::max(gateTarget, gates + 1);
    }
    if (gates < gateTarget && (planned(UT::Protoss_Robotics_Facility) > 0 || gates < 4 || Self()->minerals() > 450))
        wantBuild(UT::Protoss_Gateway);

    // Forge / upgrades.
    if (done(UT::Protoss_Nexus) >= 2 && supply >= 55 && planned(UT::Protoss_Forge) == 0 && !behind) wantBuild(UT::Protoss_Forge);
    if (done(UT::Protoss_Forge) && supply >= 110 && planned(UT::Protoss_Forge) < 2 && Self()->gas() > 150)
        wantBuild(UT::Protoss_Forge, false);
    if (done(UT::Protoss_Forge) && (done(UT::Protoss_Nexus) >= 2 || supply >= 60)) {  // a cannon forge on one base: units first
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
    if (Info::rushThreat() && !Info::dtThreat() && done(UT::Protoss_Gateway) >= 1) {
        if (planned(UT::Protoss_Gateway) < 2) wantBuild(UT::Protoss_Gateway);
        else if (planned(UT::Protoss_Gateway) < 3 && Self()->minerals() >= 250) wantBuild(UT::Protoss_Gateway);
    }
    for (auto &s : opening) {
        if (planned(s.type) >= s.count) continue;
        if (Self()->supplyUsed() / 2 < s.supply) return;
        if (Info::rushThreat() && !Info::dtThreat() && (s.type == UT::Protoss_Assimilator || s.type == UT::Protoss_Cybernetics_Core) &&
            planned(UT::Protoss_Zealot) < 5) {
            unitRule();
            return;
        }
        // the core waits for the scout's first look unless their gas/core already says "no rush"
        if (s.type == UT::Protoss_Cybernetics_Core && Now() < 24 * 150 && !Info::seenTypes.count(UT::Protoss_Assimilator) &&
            !Info::seenTypes.count(UT::Protoss_Cybernetics_Core))
            return;
        if (s.type.isBuilding()) {
            // the builder may set off shortly before the required building finishes
            for (auto &[rt, n] : s.type.requiredUnits()) {
                if (rt.isWorker() || done(rt) > 0) continue;
                bool soon = false;
                for (auto u : Self()->getUnits())
                    if (u->getType() == rt && u->isBeingConstructed() && u->getRemainingBuildTime() < 150) soon = true;
                if (!soon) return;
            }
            wantBuild(s.type);
        } else
            wantTrain(s.type);
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
    if (rushDefense()) unitRule();
    probeRule();
    supplyRule();
    if (!openingDone) {
        detectionRule();
        planOpening();
        // extra zealots from idle gateways if rich
        if (budgetM >= 100 || Info::rushThreat()) unitRule();
    } else
        planPostOpening();

    if (Now() % (24 * 20) == 0)
        LOG("status min=%d gas=%d supply=%d/%d workers=%d army=%d tasks=%d mode=%s eup=%d/%d", Self()->minerals(),
            Self()->gas(), Self()->supplyUsed() / 2, Self()->supplyTotal() / 2, Workers::count(), armyCount(),
            (int)tasks.size(), Combat::mode.c_str(), Opp()->getUpgradeLevel(UpgradeTypes::Singularity_Charge),
            Opp()->getUpgradeLevel(UpgradeTypes::Protoss_Ground_Weapons));
}
}  // namespace Macro

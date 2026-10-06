#include "common.h"

#include <FAP.hpp>

namespace Combat {
std::string mode = "hold";
double myStrength = 0, theirStrength = 0;
static Position attackTarget = Positions::None;
static std::map<Unit, int> engagedUntil;   // unit -> frame its cluster last decided to fight
static std::set<Unit> fightingWorkers;
static int lastAttackStart = -100000;

// ---------------------------------------------------------------- helpers
static int groundRange(Player p, UnitType t) {
    if (t == UT::Protoss_Reaver) return 8 * 32;
    if (t == UT::Protoss_Carrier) return 8 * 32;
    int r = p->weaponMaxRange(t.groundWeapon());
    // enemy upgrades are often invisible to us: assume the standard ones
    if (p != Self() && t == UT::Protoss_Dragoon && Now() > 24 * 60 * 5) r = std::max(r, 6 * 32);
    return r;
}

static bool canAttackGround(UnitType t) {
    return t.groundWeapon() != WeaponTypes::None || t == UT::Protoss_Reaver || t == UT::Protoss_Carrier;
}

static bool isMelee(UnitType t) {
    return t == UT::Protoss_Zealot || t == UT::Protoss_Dark_Templar || t.isWorker() || t == UT::Zerg_Zergling ||
           t == UT::Zerg_Ultralisk || t == UT::Terran_Firebat || t == UT::Protoss_Archon;
}

static auto makeFap(UnitType t, Position p, int hp, int sh, Player pl, int cd) {
    int wpn = pl->getUpgradeLevel(t.groundWeapon().upgradeType());
    int arm = pl->getUpgradeLevel(t.armorUpgrade());
    int shu = pl->getUpgradeLevel(UpgradeTypes::Protoss_Plasma_Shields);
    bool spd = t == UT::Protoss_Zealot && pl->getUpgradeLevel(UpgradeTypes::Leg_Enhancements) > 0;
    bool rng = t == UT::Protoss_Dragoon && pl->getUpgradeLevel(UpgradeTypes::Singularity_Charge) > 0;
    return FAP::makeUnit<>()
        .setUnitType(t)
        .setPosition(p)
        .setHealth(std::max(1, hp))
        .setShields(std::max(0, sh))
        .setFlying(t.isFlyer())
        .setElevation(-1)
        .setAttackerCount(t == UT::Protoss_Carrier ? 8 : 0)
        .setArmorUpgrades(arm)
        .setAttackUpgrades(wpn)
        .setShieldUpgrades(shu)
        .setSpeedUpgrade(spd)
        .setAttackCooldownRemaining(std::max(0, cd))
        .setAttackSpeedUpgrade(false)
        .setStimmed(false)
        .setRangeUpgrade(rng)
        .setData(std::tuple<>{});
}

static double fapValue(const std::vector<FAP::FAPUnit<std::tuple<>>> &v) {
    double s = 0;
    for (auto &u : v) {
        double maxv = double(u.maxHealth + u.maxShields);
        if (maxv <= 0) continue;
        s += unitValue(u.unitType) * (u.health + u.shields) / maxv;
    }
    return s;
}

struct SimResult {
    double ourBefore = 0, ourAfter = 0, theirBefore = 0, theirAfter = 0;
    int theirCombat = 0;
};

static bool fapCombat(UnitType t) {
    return canAttackGround(t) || t.airWeapon() != WeaponTypes::None;
}

static SimResult simulate(const std::vector<Unit> &ours, const std::vector<const EnemyRec *> &theirs, int frames) {
    FAP::FastAPproximation<> fap;
    SimResult r;
    for (auto u : ours) {
        auto t = u->getType();
        if (!fapCombat(t)) continue;
        fap.addIfCombatUnitPlayer1(
            makeFap(t, u->getPosition(), u->getHitPoints(), u->getShields(), Self(), u->getGroundWeaponCooldown()));
    }
    // friendly cannons near
    for (auto e : theirs) {
        auto t = e->type;
        if (!fapCombat(t)) continue;
        if (t.isBuilding() && !e->completed) continue;
        if (t.isWorker()) {
            // workers only count if they are fighting
            if (!e->unit || !e->unit->isVisible() || !(e->unit->isAttacking() || e->unit->getOrder() == Orders::AttackUnit))
                continue;
        }
        fap.addIfCombatUnitPlayer2(makeFap(t, e->pos, e->hp, e->shields, Opp(), 0));
        ++r.theirCombat;
    }
    auto st = fap.getState();
    r.ourBefore = fapValue(*st.first);
    r.theirBefore = fapValue(*st.second);
    fap.simulate(frames);
    st = fap.getState();
    r.ourAfter = fapValue(*st.first);
    r.theirAfter = fapValue(*st.second);
    return r;
}

static double armyStrength(const std::vector<Unit> &units) {
    double hp = 0, dps = 0;
    for (auto u : units) {
        hp += u->getHitPoints() + u->getShields();
        double d = dpsOf(u->getType());
        if (u->getType() == UT::Protoss_Dragoon && Self()->getUpgradeLevel(UpgradeTypes::Singularity_Charge)) d *= 1.3;
        dps += d;
    }
    return std::sqrt(hp * dps);
}

// ---------------------------------------------------------------- threats at home
static std::vector<const EnemyRec *> threats;

static void computeThreats() {
    threats.clear();
    for (auto &[id, e] : Info::enemies) {
        if (!e.posValid || Now() - e.lastSeen > 24 * 4) continue;
        if (e.type == UT::Protoss_Observer) continue;
        if (!MapInfo::inMyTerritory(e.pos)) continue;
        if (e.type.isWorker()) {
            bool hostile = e.unit && e.unit->isVisible() &&
                           (e.unit->isAttacking() || e.unit->getOrder() == Orders::AttackUnit ||
                            e.unit->isConstructing() || e.unit->getOrder() == Orders::PlaceBuilding);
            if (!hostile) continue;
        }
        threats.push_back(&e);
    }
}

bool homeThreat() {
    for (auto e : threats)
        if (!e->type.isWorker() && (!e->type.isBuilding() || e->type == UT::Protoss_Photon_Cannon)) return true;
    return false;
}

// ---------------------------------------------------------------- unit micro
static double targetScore(Unit u, Unit e) {
    auto et = e->getType();
    if (!e->exists() || !e->isVisible() || !e->isDetected()) return -1e9;
    if (e->isInvincible() || e->isStasised()) return -1e9;
    bool air = e->isFlying();
    if (air && u->getType().airWeapon() == WeaponTypes::None) return -1e9;
    if (!air && !canAttackGround(u->getType())) return -1e9;
    if (et == UT::Protoss_Interceptor || et == UT::Protoss_Scarab) return -1e9;
    double d = u->getDistance(e);
    int range = air ? Self()->weaponMaxRange(u->getType().airWeapon()) : groundRange(Self(), u->getType());
    double pri;
    if (et.isWorker()) pri = (e->isAttacking() || e->isConstructing()) ? 75 : 55;
    else if (et == UT::Protoss_Photon_Cannon || et == UT::Zerg_Sunken_Colony || et == UT::Terran_Bunker)
        pri = e->isCompleted() ? 85 : 70;
    else if (et.isBuilding()) {
        pri = 15;
        if (et == UT::Protoss_Pylon) pri = 22;
        if (et.isResourceDepot()) pri = 18;
    } else if (canAttackGround(et) || et.airWeapon() != WeaponTypes::None || et == UT::Protoss_High_Templar)
        pri = 100;
    else
        pri = 50;
    if (et == UT::Protoss_Dark_Templar) pri += 15;
    if (et == UT::Protoss_Reaver || et == UT::Protoss_High_Templar) pri += 45;
    if (et == UT::Protoss_Shuttle) pri += 30;
    if (et == UT::Protoss_Arbiter) pri += 40;
    if (et == UT::Protoss_Observer && Info::count(UT::Protoss_Dark_Templar) == 0) pri -= 20;
    double hpFrac = double(e->getHitPoints() + e->getShields()) / std::max(1, et.maxHitPoints() + et.maxShields());
    double s = pri + (1 - hpFrac) * 25;
    if (d <= range) s += 40;
    else s -= (d - range) / 6.0;
    return s;
}

static Unit pickTarget(Unit u, const std::vector<Unit> &enemies) {
    Unit best = nullptr;
    double bs = -1e8;
    for (auto e : enemies) {
        if (u->getDistance(e) > 32 * 14) continue;
        double s = targetScore(u, e);
        // stickiness
        if (u->getOrderTarget() == e) s += 10;
        if (s > bs) bs = s, best = e;
    }
    return best;
}

static Position homePos() {
    auto mb = MapInfo::mainBase();
    return mb ? mb->center : Position(Self()->getStartLocation());
}

static Position walkableAway(Unit u, Position from, int dist) {
    Position p = u->getPosition();
    double dx = p.x - from.x, dy = p.y - from.y;
    double len = std::max(1.0, std::sqrt(dx * dx + dy * dy));
    for (int a = 0; a < 8; ++a) {
        double ang = (a % 2 ? 1 : -1) * ((a + 1) / 2) * 0.45;
        double rx = dx / len * std::cos(ang) - dy / len * std::sin(ang);
        double ry = dx / len * std::sin(ang) + dy / len * std::cos(ang);
        Position q(int(p.x + rx * dist), int(p.y + ry * dist));
        q = q.makeValid();
        if (Broodwar->isWalkable(WalkPosition(q)) && Broodwar->getGroundHeight(TilePosition(q)) >= 0) {
            // stay connected
            if (MapInfo::areaOf(q) && MapInfo::areaOf(q)->AccessibleFrom(MapInfo::areaOf(p))) return q;
        }
    }
    return homePos();
}

static void microUnit(Unit u, const std::vector<Unit> &enemies, Position fallback) {
    if (u->isAttackFrame() || u->isStartingAttack()) return;
    Unit t = pickTarget(u, enemies);
    if (!t) {
        cmdAttackMove(u, fallback);
        return;
    }
    auto ut = u->getType();
    if (ut == UT::Protoss_Dragoon) {
        int range = groundRange(Self(), ut);
        int cd = t->isFlying() ? u->getAirWeaponCooldown() : u->getGroundWeaponCooldown();
        // nearest melee threat that is coming for this dragoon
        Unit melee = nullptr;
        double md = 1e9;
        int rangedNear = 0;
        for (auto e : enemies) {
            if (!e->isDetected()) continue;
            if (!isMelee(e->getType()) && canAttackGround(e->getType()) && !e->getType().isBuilding() &&
                u->getDistance(e) < 32 * 8)
                ++rangedNear;
            if (!isMelee(e->getType()) || e->getType().isWorker()) continue;
            double d = u->getDistance(e);
            bool chasing = e->getOrderTarget() == u || d < 24;
            double speed = std::sqrt(e->getVelocityX() * e->getVelocityX() + e->getVelocityY() * e->getVelocityY());
            if (speed > 5.3) continue;  // leg-upgraded zealots outrun a kiting dragoon
            if (chasing && d < md) md = d, melee = e;
        }
        if (cd > 10 && melee && md < 64 && range > 100 && rangedNear < 4) {
            cmdMove(u, walkableAway(u, melee->getPosition(), 80));
            return;
        }
        // ranged threat out-ranged by us: step back when on cooldown
        if (cd > 10) {
            Unit ranged = nullptr;
            double rd = 1e9;
            for (auto e : enemies) {
                if (!e->isDetected() || isMelee(e->getType()) || !canAttackGround(e->getType())) continue;
                if (e->getType().isBuilding()) {
                    // stay out of cannon range while cooling down
                    if (e->getType() == UT::Protoss_Photon_Cannon && e->isCompleted() && u->getDistance(e) < 7 * 32 + 8) {
                        cmdMove(u, walkableAway(u, e->getPosition(), 64));
                        return;
                    }
                    continue;
                }
                double d = u->getDistance(e);
                if (d < rd) rd = d, ranged = e;
            }
            if (ranged && groundRange(Opp(), ranged->getType()) + 16 < range && rd < groundRange(Opp(), ranged->getType()) + 48) {
                cmdMove(u, walkableAway(u, ranged->getPosition(), 64));
                return;
            }
        }
        cmdAttack(u, t);
        return;
    }
    cmdAttack(u, t);
}

// ---------------------------------------------------------------- clusters
struct Cluster {
    std::vector<Unit> units;
    Position center;
};

static std::vector<Cluster> makeClusters(const std::vector<Unit> &army) {
    std::vector<Cluster> cs;
    std::vector<int> comp(army.size(), -1);
    int n = 0;
    for (size_t i = 0; i < army.size(); ++i) {
        if (comp[i] >= 0) continue;
        comp[i] = n;
        std::vector<size_t> stack{i};
        while (!stack.empty()) {
            size_t a = stack.back();
            stack.pop_back();
            for (size_t j = 0; j < army.size(); ++j)
                if (comp[j] < 0 && army[a]->getDistance(army[j]) < 320) comp[j] = n, stack.push_back(j);
        }
        ++n;
    }
    cs.resize(n);
    for (size_t i = 0; i < army.size(); ++i) cs[comp[i]].units.push_back(army[i]);
    for (auto &c : cs) {
        Position s(0, 0);
        for (auto u : c.units) s += u->getPosition();
        c.center = s / int(c.units.size());
    }
    return cs;
}

static Position holdPos() {
    auto nb = MapInfo::natBase();
    auto mb = MapInfo::mainBase();
    bool natDone = false;
    if (nb)
        for (auto u : Self()->getUnits())
            if (u->getType().isResourceDepot() && u->isCompleted() && u->getPosition().getDistance(nb->center) < 128) natDone = true;
    if (natDone) {
        Position c = MapInfo::natChoke;
        Position b = nb->center;
        return Position((c.x * 2 + b.x) / 3, (c.y * 2 + b.y) / 3);
    }
    if (mb) {
        Position c = MapInfo::mainChoke;
        Position b = mb->center;
        if (Info::rushThreat()) return Position((c.x + b.x * 2) / 3, (c.y + b.y * 2) / 3);
        return Position((c.x * 3 + b.x) / 4, (c.y * 3 + b.y) / 4);
    }
    return homePos();
}

static Position chooseAttackTarget(Position from) {
    // nearest known enemy building
    Position best = Positions::None;
    double bd = 1e18;
    for (auto &[id, e] : Info::enemies) {
        if (!e.type.isBuilding()) continue;
        double d = from.getDistance(e.pos);
        if (e.type.isResourceDepot()) d -= 200;
        if (d < bd) bd = d, best = e.pos;
    }
    if (best.isValid()) return best;
    if (MapInfo::enemyStart != TilePositions::Unknown) {
        auto p = Position(MapInfo::enemyStart) + Position(64, 48);
        if (!Broodwar->isExplored(MapInfo::enemyStart) || Now() - 24 * 30 < 0) return p;
    }
    for (auto t : MapInfo::enemyStartCandidates)
        if (!Broodwar->isVisible(t)) return Position(t) + Position(64, 48);
    // explore the least recently seen base
    BaseLoc *oldest = nullptr;
    for (auto &b : MapInfo::bases)
        if (!oldest || b.lastSeen < oldest->lastSeen) oldest = &b;
    return oldest ? oldest->center : MapInfo::enemyMainPos();
}

// ---------------------------------------------------------------- workers defending
static void workerDefense(const std::vector<Unit> &army) {
    std::vector<const EnemyRec *> close;
    auto mb = MapInfo::mainBase();
    for (auto e : threats) {
        if (e->type.isFlyer()) continue;
        if (e->unit && e->unit->isVisible() && !e->unit->isDetected()) continue;
        bool nearNexus = false;
        for (auto u : Self()->getUnits())
            if (u->getType().isResourceDepot() && u->getDistance(e->pos) < 32 * 11) nearNexus = true;
        if (!nearNexus) continue;
        if (e->type.isBuilding() && e->completed && e->type != UT::Protoss_Pylon) continue;  // don't run into finished cannons
        close.push_back(e);
    }
    int armyNear = 0;
    double armyPow = 0;
    for (auto u : army)
        for (auto e : close)
            if (u->getDistance(e->pos) < 32 * 12) {
                ++armyNear;
                armyPow += dpsOf(u->getType());
                break;
            }
    double threatPow = 0;
    int threatUnits = 0, threatBuildings = 0;
    for (auto e : close) {
        if (e->type.isBuilding()) ++threatBuildings;
        else {
            ++threatUnits;
            threatPow += dpsOf(e->type);
        }
    }
    int want = 0;
    bool smallThreat = threatUnits <= 5 && army.size() < 8;
    if (threatUnits > 0 && smallThreat && armyPow < threatPow * 1.5) want = std::min(12, threatUnits * 3 + 1);
    if (threatUnits > 0 && threatPow <= dpsOf(UT::Protoss_Probe) * 1.5 && armyNear == 0) want = threatUnits * 2;  // worker harass
    if (threatBuildings > 0 && armyNear < 3) want = std::max(want, std::min(10, threatBuildings * 4));
    // don't pull more than we can spare
    want = std::min(want, std::max(0, Workers::count() - 3));
    // release extras / hurt ones
    for (auto it = fightingWorkers.begin(); it != fightingWorkers.end();) {
        Unit w = *it;
        bool drop = !w->exists() || (int)fightingWorkers.size() > want || w->getHitPoints() < 12 ||
                    (mb && w->getDistance(mb->center) > 32 * 20);
        if (drop) {
            if (w->exists()) Workers::release(w);
            it = fightingWorkers.erase(it);
        } else
            ++it;
    }
    if (close.empty()) return;
    while ((int)fightingWorkers.size() < want) {
        Unit w = Workers::takeAny(close.front()->pos);
        if (!w || w->getHitPoints() < 20) break;
        Workers::setRole(w, Workers::Role::Fight);
        fightingWorkers.insert(w);
    }
    for (auto w : fightingWorkers) {
        // target: closest threat, prefer units over buildings, prefer incomplete buildings
        Unit best = nullptr;
        double bd = 1e18;
        for (auto e : close) {
            if (!e->unit || !e->unit->exists() || !e->unit->isVisible() || !e->unit->isDetected()) continue;
            double d = w->getDistance(e->unit);
            if (e->type.isBuilding()) d += 64;
            if (e->type.isWorker()) d -= 32;
            if (d < bd) bd = d, best = e->unit;
        }
        if (best) cmdAttack(w, best);
        else Workers::release(w);
    }
}

// ---------------------------------------------------------------- observers
static Position mineralLine(const BaseLoc *b) {
    Position sum(0, 0);
    int n = 0;
    for (auto m : b->bw->Minerals()) sum += Position(m->Pos()), ++n;
    if (!n) return b->center;
    Position line = sum / n;
    return (line + b->center) / 2;
}

static Position homeGuardPos() {
    auto nb = MapInfo::natBase();
    auto mb = MapInfo::mainBase();
    if (MapInfo::myNaturalTaken() && nb) return mineralLine(nb);
    return mb ? mineralLine(mb) : homePos();
}

static std::vector<Unit> observerList;

static void observers(const std::vector<Cluster> &cs, int mainCluster) {
    observerList.clear();
    for (auto u : Self()->getUnits())
        if (u->getType() == UT::Protoss_Observer && u->isCompleted()) observerList.push_back(u);
    auto &obs = observerList;
    // the observer nearest the army goes with it
    if (mainCluster >= 0 && obs.size() > 1)
        std::sort(obs.begin(), obs.end(), [&](Unit a, Unit b) {
            return a->getDistance(cs[mainCluster].center) < b->getDistance(cs[mainCluster].center);
        });
    bool guardFirst = obs.size() == 1 && (Info::dtThreat() || mode != "attack");
    // cloaked enemies we know about: send the nearest observer to each
    std::vector<Position> cloaked;
    for (auto &[id, e] : Info::enemies) {
        if (!e.posValid || Now() - e.lastSeen > 24 * 6) continue;
        bool cl = e.type == UT::Protoss_Dark_Templar || e.type == UT::Zerg_Lurker || e.type == UT::Protoss_Arbiter ||
                  (e.unit && e.unit->isVisible() && !e.unit->isDetected());
        if (!cl) continue;
        bool relevant = MapInfo::inMyTerritory(e.pos);
        for (auto &c : cs)
            for (auto u : c.units)
                if (!relevant && u->getDistance(e.pos) < 32 * 14) relevant = true;
        if (relevant) cloaked.push_back(e.pos);
    }
    std::set<Unit> hunting;
    for (auto &p : cloaked) {
        Unit best = nullptr;
        for (auto o : obs)
            if (!hunting.count(o) && (!best || o->getDistance(p) < best->getDistance(p))) best = o;
        if (!best) break;
        // one observer covers cloaked units close together
        bool covered = false;
        for (auto h : hunting)
            if (h->getDistance(p) < 32 * 5) covered = true;
        if (covered) continue;
        hunting.insert(best);
        cmdMove(best, p);
    }
    for (size_t i = 0; i < obs.size(); ++i) {
        Unit o = obs[i];
        if (hunting.count(o)) continue;
        Position goal;
        if (i == 0 && mainCluster >= 0 && !guardFirst) {
            const auto &c = cs[mainCluster];
            Position tgt = mode == "attack" && attackTarget.isValid() ? attackTarget : c.center;
            double dx = tgt.x - c.center.x, dy = tgt.y - c.center.y;
            double len = std::max(1.0, std::sqrt(dx * dx + dy * dy));
            goal = Position(int(c.center.x + dx / len * std::min(len, 64.0)), int(c.center.y + dy / len * std::min(len, 64.0)));
        } else if (i <= 1) {
            goal = homeGuardPos();
        } else {
            goal = MapInfo::mainBase() ? mineralLine(MapInfo::mainBase()) : holdPos();
        }
        // avoid known detectors that can shoot it
        for (auto &[id, e] : Info::enemies)
            if ((e.type == UT::Protoss_Photon_Cannon) && e.posValid && o->getDistance(e.pos) < 8 * 32) {
                goal = walkableAway(o, e.pos, 128);
                break;
            }
        cmdMove(o, goal.makeValid());
    }
}

static bool detectionAt(Position p) {
    for (auto o : observerList)
        if (o->getDistance(p) < 32 * 9) return true;
    for (auto u : Self()->getUnits())
        if (u->getType() == UT::Protoss_Photon_Cannon && u->isCompleted() && u->getDistance(p) < 32 * 7) return true;
    return false;
}

static Unit nearestObserver(Position p) {
    Unit best = nullptr;
    for (auto o : observerList)
        if (!best || o->getDistance(p) < best->getDistance(p)) best = o;
    return best;
}

// ---------------------------------------------------------------- main loop
void update() {
    computeThreats();
    std::vector<Unit> army;
    for (auto u : Self()->getUnits())
        if (u->isCompleted() && isArmyType(u->getType()) && u->getType() != UT::Protoss_High_Templar) army.push_back(u);

    workerDefense(army);

    // merge high templar into archons
    {
        std::vector<Unit> hts;
        for (auto u : Self()->getUnits())
            if (u->getType() == UT::Protoss_High_Templar && u->isCompleted() && u->getOrder() != Orders::ArchonWarp &&
                u->getOrder() != Orders::ArchonWarp)
                hts.push_back(u);
        while (hts.size() >= 2) {
            Unit a = hts.back();
            hts.pop_back();
            Unit b = nullptr;
            for (auto h : hts)
                if (!b || a->getDistance(h) < a->getDistance(b)) b = h;
            hts.erase(std::find(hts.begin(), hts.end(), b));
            if (Now() - a->getLastCommandFrame() > 24 || a->getLastCommand().getType() != UnitCommandTypes::Use_Tech_Unit)
                a->useTech(TechTypes::Archon_Warp, b);
        }
        for (auto h : hts)
            if (h->getDistance(homePos()) > 200 && h->isIdle()) cmdMove(h, homePos());
    }

    // visible enemies
    std::vector<Unit> visibleEnemies;
    for (auto e : Opp()->getUnits())
        if (e->exists() && e->isVisible()) visibleEnemies.push_back(e);

    // global mode
    double mine = armyStrength(army);
    double theirs = Info::enemyArmyStrength();
    // We rarely see the whole enemy army: assume a typical army for the game time.
    double minutes = Now() / (24.0 * 60);
    double expectedSupply = std::clamp((minutes - 3.0) * 7.0, 0.0, 150.0);
    double theirsEst = std::max(theirs, 26.8 * expectedSupply);
    myStrength = mine;
    theirStrength = theirs;
    int armySupply = 0;
    for (auto u : army) armySupply += u->getType().supplyRequired();
    armySupply /= 2;
    bool rangeDone = Self()->getUpgradeLevel(UpgradeTypes::Singularity_Charge) > 0;
    std::string prev = mode;
    if (homeThreat()) {
        mode = "defend";
    } else if (mode == "attack") {
        if (mine < theirsEst * 0.8 && Self()->supplyUsed() < 340) mode = "hold";
    } else {
        bool ready = (armySupply >= 16 && rangeDone && mine > theirsEst * 1.35) ||
                     (armySupply >= 50 && mine > theirsEst * 1.1) || Self()->supplyUsed() >= 370;
        if (Info::dtThreat() && observerList.size() < 2) ready = false;
        if (ready && Now() - Strat::lastRetreatFrame > 24 * 25) mode = "attack";
        else mode = "hold";
    }
    if (mode != prev) {
        LOG("mode %s -> %s (mine %.0f theirs %.0f est %.0f, army supply %d)", prev.c_str(), mode.c_str(), mine, theirs,
            theirsEst, armySupply);
        if (mode == "attack") lastAttackStart = Now();
    }

    auto cs = makeClusters(army);
    int mainCluster = -1;
    for (int i = 0; i < (int)cs.size(); ++i)
        if (mainCluster < 0 || cs[i].units.size() > cs[mainCluster].units.size()) mainCluster = i;

    Position goal = holdPos();
    if (mode == "defend") {
        // closest threat to our main
        double bd = 1e18;
        for (auto e : threats) {
            double d = e->pos.getDistance(homePos());
            if (e->type.isWorker()) d += 2000;
            if (d < bd) bd = d, goal = e->pos;
        }
    } else if (mode == "attack") {
        Position from = mainCluster >= 0 ? cs[mainCluster].center : homePos();
        attackTarget = chooseAttackTarget(from);
        goal = attackTarget;
    }

    observers(cs, mainCluster);

    bool haveDetection = false;
    for (int ci = 0; ci < (int)cs.size(); ++ci) {
        auto &c = cs[ci];
        // nearby enemies
        std::vector<const EnemyRec *> near;
        std::vector<Unit> nearVisible;
        double radius = 32 * 13;
        for (auto &[id, e] : Info::enemies) {
            if (!e.posValid || Now() - e.lastSeen > 24 * 10) continue;
            double d = 1e9;
            for (auto u : c.units) d = std::min(d, (double)u->getDistance(e.pos));
            if (d > radius) continue;
            near.push_back(&e);
        }
        for (auto e : visibleEnemies) {
            double d = 1e9;
            for (auto u : c.units) d = std::min(d, (double)u->getDistance(e));
            if (d <= radius) nearVisible.push_back(e);
        }
        // undetected cloaked units we can't fight
        bool cloakedDanger = false;
        for (auto e : nearVisible)
            if (!e->isDetected() && (e->isCloaked() || e->isBurrowed() || e->getType() == UT::Protoss_Dark_Templar) &&
                !detectionAt(e->getPosition()) && canAttackGround(e->getType()) && !e->getType().isWorker())
                cloakedDanger = true;
        (void)haveDetection;

        bool inHome = MapInfo::inMyTerritory(c.center);
        bool fight = true;
        SimResult sr;
        if (!near.empty()) {
            sr = simulate(c.units, near, 24 * 12);
            double ourLoss = sr.ourBefore - sr.ourAfter, theirLoss = sr.theirBefore - sr.theirAfter;
            bool wasEngaged = false;
            for (auto u : c.units)
                if (engagedUntil.count(u) && engagedUntil[u] >= Now() - 24) wasEngaged = true;
            // in weapon contact? then turning around costs more than it saves
            bool contact = false;
            for (auto e : nearVisible) {
                if (!e->isDetected() || !canAttackGround(e->getType()) || e->getType().isWorker()) continue;
                for (auto u : c.units)
                    if (u->getDistance(e) <= std::max(groundRange(Self(), u->getType()), groundRange(Opp(), e->getType())) + 48) {
                        contact = true;
                        break;
                    }
                if (contact) break;
            }
            double k = contact ? 0.4 : (wasEngaged ? 0.9 : 1.25);
            if (inHome) k *= 0.6;
            if (mode == "defend" && MapInfo::areaOf(c.center) == (MapInfo::mainBase() ? MapInfo::mainBase()->area : nullptr)) k *= 0.5;
            if (sr.theirCombat == 0) fight = true;
            else fight = theirLoss + 1 >= ourLoss * k;
            // an army with clear numbers advantage keeps pushing
            if (sr.ourAfter > sr.theirAfter * 3 && sr.ourAfter > 0) fight = true;
        }
        if (cloakedDanger) fight = false;

        if (fight) {
            for (auto u : c.units) engagedUntil[u] = Now();
        }

        // regroup goal for small clusters joining the main one in attack mode
        Position cgoal = goal;
        if (mode == "attack" && ci != mainCluster && mainCluster >= 0 && cs[mainCluster].units.size() > c.units.size())
            cgoal = cs[mainCluster].center;
        if (mode == "attack" && ci == mainCluster) {
            cgoal = goal;
            // against cloak, don't outrun the observer
            if (Info::dtThreat()) {
                Unit o = nearestObserver(c.center);
                if (o && o->getDistance(c.center) > 32 * 4) cgoal = o->getPosition();
            }
        }

        if (!fight) {
            bool obsComing = cloakedDanger && nearestObserver(c.center) && nearestObserver(c.center)->getDistance(c.center) < 32 * 30;
            if (mode == "attack" && (sr.theirCombat > 0 || cloakedDanger) && !obsComing) {
                Strat::lastRetreatFrame = Now();
                if (ci == mainCluster) {
                    mode = "hold";
                    LOG("retreat: sim says lose (ours %.0f->%.0f theirs %.0f->%.0f)", sr.ourBefore, sr.ourAfter,
                        sr.theirBefore, sr.theirAfter);
                }
            }
            Position back = cloakedDanger ? homePos() : (inHome ? homePos() : holdPos());
            if (cloakedDanger) {
                Unit o = nearestObserver(c.center);
                if (o) back = o->getPosition();
            }
            for (auto u : c.units) {
                // shoot while retreating if something is in range and we're ready
                Unit t = pickTarget(u, nearVisible);
                if (t && u->getType() == UT::Protoss_Dragoon && u->getGroundWeaponCooldown() == 0 &&
                    u->getDistance(t) <= groundRange(Self(), u->getType()) && !cloakedDanger) {
                    cmdAttack(u, t);
                    continue;
                }
                if (u->isAttackFrame()) continue;
                cmdMove(u, back);
            }
            continue;
        }

        bool enemiesClose = false;
        for (auto e : nearVisible)
            if (e->isDetected() && !e->isFlying()) enemiesClose = true;
        if (enemiesClose) {
            // Before contact, let the rear catch up so we arrive together.
            std::vector<std::pair<double, Unit>> dist;
            for (auto u : c.units) {
                double d = 1e9;
                for (auto e : nearVisible)
                    if (e->isDetected() && !e->getType().isBuilding() && canAttackGround(e->getType()) && !e->getType().isWorker())
                        d = std::min(d, (double)u->getDistance(e));
                dist.push_back({d, u});
            }
            std::vector<double> ds;
            for (auto &p : dist) ds.push_back(p.first);
            std::sort(ds.begin(), ds.end());
            double median = ds.empty() ? 0 : ds[ds.size() / 2];
            bool anyInRange = false;
            for (auto &[d, u] : dist)
                if (d <= groundRange(Self(), u->getType()) + 32) anyInRange = true;
            bool gather = !anyInRange && c.units.size() >= 4 && median < 1e8 && mode != "defend";
            for (auto &[d, u] : dist) {
                if (gather && d < median - 96 && d > groundRange(Self(), u->getType()) + 64) {
                    if (!u->isHoldingPosition()) u->holdPosition();
                    continue;
                }
                microUnit(u, nearVisible, cgoal);
            }
            continue;
        }
        // no enemies near: move as a group
        for (auto u : c.units) {
            if (mode == "hold") {
                if (u->getDistance(cgoal) > 96) cmdAttackMove(u, cgoal);
                else if (!u->isHoldingPosition() && u->isIdle()) {
                }
                continue;
            }
            if (ci == mainCluster && c.units.size() > 3 &&
                u->getDistance(c.center) > 32 * (4 + 1.6 * std::sqrt((double)c.units.size())) &&
                u->getDistance(cgoal) < c.center.getDistance(cgoal)) {
                cmdMove(u, c.center);
                continue;
            }
            cmdAttackMove(u, cgoal);
        }
    }
}
}  // namespace Combat

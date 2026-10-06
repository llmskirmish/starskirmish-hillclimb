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
    if (et == UT::Protoss_Reaver || et == UT::Protoss_High_Templar) pri += 60;
    if (et == UT::Protoss_Dark_Archon) pri = 140;
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

static std::vector<Unit> scarabTargets;  // my units currently targeted by enemy scarabs

// Step out of psionic storms and out of the splash of scarabs aimed at nearby friends.
static bool dodge(Unit u) {
    if (u->isFlying()) return false;
    if (u->isUnderStorm()) {
        Unit ht = nullptr;
        for (auto e : Opp()->getUnits())
            if (e->isVisible() && e->getType() == UT::Protoss_High_Templar && (!ht || u->getDistance(e) < u->getDistance(ht))) ht = e;
        cmdMove(u, ht && u->getDistance(ht) < 32 * 12 ? walkableAway(u, ht->getPosition(), 96) : walkableAway(u, u->getPosition() + Position(0, 1), 96));
        return true;
    }
    for (auto t : scarabTargets) {
        if (t == u || !t->exists()) continue;
        if (u->getPosition().getDistance(t->getPosition()) < 80) {
            cmdMove(u, walkableAway(u, t->getPosition(), 80));
            return true;
        }
    }
    return false;
}

static void microUnit(Unit u, const std::vector<Unit> &enemies, Position fallback) {
    if (u->isAttackFrame() || u->isStartingAttack()) return;
    if (dodge(u)) return;
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
        // wounded: step behind the line while reloading so the enemy wastes shots re-targeting
        if (cd > 10 && u->getShields() < 5 && u->getHitPoints() < 50) {
            Unit shooter = nullptr;
            for (auto e : enemies)
                if (e->isDetected() && !e->getType().isBuilding() && canAttackGround(e->getType()) && !e->getType().isWorker() &&
                    u->getDistance(e) <= groundRange(Opp(), e->getType()) + 32 && (!shooter || u->getDistance(e) < u->getDistance(shooter)))
                    shooter = e;
            if (shooter) {
                cmdMove(u, walkableAway(u, shooter->getPosition(), 96));
                return;
            }
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

static Position holdFacing = Positions::None;  // the point the hold position looks at
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
        holdFacing = c + (c - b);
        return Position((c.x * 2 + b.x) / 3, (c.y * 2 + b.y) / 3);
    }
    if (mb) {
        Position c = MapInfo::mainChoke;
        Position b = mb->center;
        holdFacing = c + (c - b);
        if (Info::rushThreat()) return Position((c.x + b.x * 2) / 3, (c.y + b.y * 2) / 3);
        return Position((c.x * 3 + b.x) / 4, (c.y * 3 + b.y) / 4);
    }
    return homePos();
}

// attack targets the army failed to reach: ignored for a while
static std::vector<std::pair<Position, int>> unreachable;
static bool isUnreachable(Position p) {
    for (auto &[q, until] : unreachable)
        if (until > Now() && q.getDistance(p) < 96) return true;
    return false;
}

// ---------------------------------------------------------------- search grid
// When no enemy building is known, the army spreads out over the least recently seen ground.
static const int CELL = 6;  // tiles
struct SearchCell {
    Position center;
    int lastSeen = 0;
};
static std::vector<SearchCell> searchCells;
static bool searching = false;

static void updateSearchGrid() {
    if (searchCells.empty()) {
        auto mb = MapInfo::mainBase();
        auto home = mb ? MapInfo::areaOf(mb->center) : nullptr;
        for (int y = CELL / 2; y < Broodwar->mapHeight(); y += CELL)
            for (int x = CELL / 2; x < Broodwar->mapWidth(); x += CELL) {
                Position c(TilePosition(x, y));
                c += Position(16, 16);
                if (!Broodwar->isWalkable(WalkPosition(c))) continue;
                auto a = MapInfo::areaAtTile(TilePosition(x, y));
                if (!a || (home && !a->AccessibleFrom(home))) continue;
                searchCells.push_back({c, 0});
            }
    }
    if (Now() % 24) return;
    for (auto &c : searchCells)
        if (Broodwar->isVisible(TilePosition(c.center))) c.lastSeen = Now();
}

// Holding: stand in a concave facing the way the enemy comes, so that every ranged unit can shoot.
static std::map<Unit, Position> holdSlots(const std::vector<Unit> &units, Position center) {
    std::map<Unit, Position> out;
    Position face = holdFacing.isValid() && center.getDistance(holdPos()) < 64 ? holdFacing : MapInfo::enemyMainPos();
    double vx = face.x - center.x, vy = face.y - center.y, len = std::sqrt(vx * vx + vy * vy);
    if (len < 1) return out;
    vx /= len, vy /= len;
    double px = -vy, py = vx;
    std::vector<Unit> us;
    for (auto u : units)
        if (u->getType() != UT::Protoss_Observer && !u->isFlying()) us.push_back(u);
    int n = (int)us.size();
    if (n < 3) return out;
    // rows of up to 10 units on arcs of radius 5 tiles, the second row 1.5 tiles behind
    std::vector<Position> slotPos;
    for (int row = 0; (int)slotPos.size() < n; ++row) {
        int m = std::min(10, n - (int)slotPos.size());
        double R = 32 * 5 + row * 32;
        for (int i = 0; i < m; ++i) {
            double th = m == 1 ? 0 : (-1.0 + 2.0 * i / (m - 1)) * 1.0 * std::min(1.0, m / 8.0);
            double fx = center.x + vx * (R - row * 48), fy = center.y + vy * (R - row * 48);
            Position q(int(fx - R * std::cos(th) * vx + R * std::sin(th) * px), int(fy - R * std::cos(th) * vy + R * std::sin(th) * py));
            if (!q.isValid() || !Broodwar->isWalkable(WalkPosition(q)) || MapInfo::areaOf(q) != MapInfo::areaOf(center)) q = center;
            slotPos.push_back(q);
        }
    }
    // pair units and slots by their angle around the centre
    auto ang = [&](Position q) { return std::atan2((q.x - center.x) * px + (q.y - center.y) * py, (q.x - center.x) * vx + (q.y - center.y) * vy + 1e-3); };
    std::vector<int> si(n), ui(n);
    for (int i = 0; i < n; ++i) si[i] = ui[i] = i;
    std::sort(si.begin(), si.end(), [&](int a, int b) { return ang(slotPos[a]) < ang(slotPos[b]); });
    std::sort(ui.begin(), ui.end(), [&](int a, int b) { return ang(us[a]->getPosition()) < ang(us[b]->getPosition()); });
    for (int i = 0; i < n; ++i) out[us[ui[i]]] = slotPos[si[i]];
    if (Now() % 480 == 0) {
        std::string sl;
        for (auto &q : slotPos) sl += " (" + std::to_string(q.x / 32) + "," + std::to_string(q.y / 32) + ")";
        LOG("hold slots centre (%d,%d) face (%d,%d):%s", center.x / 32, center.y / 32, face.x / 32, face.y / 32, sl.c_str());
    }
    return out;
}

static Position chooseAttackTarget(Position from) {
    // nearest known enemy building
    Position best = Positions::None;
    double bd = 1e18;
    for (auto &[id, e] : Info::enemies) {
        if (!e.type.isBuilding()) continue;
        if (isUnreachable(e.pos)) continue;
        double d = from.getDistance(e.pos);
        if (e.type.isResourceDepot()) d -= 200;
        if (d < bd) bd = d, best = e.pos;
    }
    searching = !best.isValid();
    if (best.isValid()) return best;
    if (MapInfo::enemyStart != TilePositions::Unknown) {
        auto p = Position(MapInfo::enemyStart) + Position(64, 48);
        if (!Broodwar->isExplored(MapInfo::enemyStart) || Now() - 24 * 30 < 0) return p;
    }
    for (auto t : MapInfo::enemyStartCandidates)
        if (!Broodwar->isExplored(t) && !isUnreachable(Position(t) + Position(64, 48))) return Position(t) + Position(64, 48);
    // explore the least recently seen base
    BaseLoc *oldest = nullptr;
    for (auto &b : MapInfo::bases)
        if (!isUnreachable(b.center) && (!oldest || b.lastSeen < oldest->lastSeen)) oldest = &b;
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
// ---------------------------------------------------------------- psionic storm
static std::vector<std::pair<Position, int>> recentStorms;  // where and when we cast

static double stormScore(Position p, const std::vector<Unit> &enemies) {
    double s = 0;
    for (auto e : enemies) {
        auto t = e->getType();
        if (t.isBuilding() || t == UT::Protoss_Interceptor || t == UT::Protoss_Scarab || e->isUnderStorm() || !e->isDetected())
            continue;
        if (e->getPosition().getDistance(p) > 56) continue;
        if (t.isWorker()) s += 40;
        else s += std::min(250.0, unitValue(t)) * (e->isMoving() ? 0.7 : 1.0);
    }
    for (auto u : Self()->getUnits()) {
        if (!u->isCompleted() || u->getType().isBuilding()) continue;
        double d = u->getPosition().getDistance(p);
        if (d < 80) s -= 1.5 * std::min(250.0, unitValue(u->getType()));
    }
    for (auto &[q, f] : recentStorms)
        if (Now() - f < 72 && q.getDistance(p) < 96) return -1;
    return s;
}

static void controlCasters(std::vector<Unit> &casters, const std::vector<Unit> &visibleEnemies, Position follow) {
    bool storm = Self()->hasResearched(TechTypes::Psionic_Storm);
    for (auto ht : casters) {
        if (ht->getOrder() == Orders::CastPsionicStorm) continue;
        if (storm && ht->getEnergy() >= 75) {
            Position best = Positions::None;
            double bs = 250;  // at least ~two dragoons' worth
            for (auto e : visibleEnemies) {
                if (e->getDistance(ht) > 32 * 11) continue;
                double sc = stormScore(e->getPosition(), visibleEnemies);
                if (sc > bs) bs = sc, best = e->getPosition();
            }
            if (best.isValid()) {
                ht->useTech(TechTypes::Psionic_Storm, best);
                recentStorms.push_back({best, Now()});
                LOG("storm at (%d,%d) score %.0f", best.x / 32, best.y / 32, bs);
                continue;
            }
        }
        // stay behind the army, away from enemies
        Unit threat = nullptr;
        for (auto e : visibleEnemies)
            if (e->isDetected() && canAttackGround(e->getType()) && !e->getType().isWorker() &&
                e->getDistance(ht) < 32 * 5 && (!threat || e->getDistance(ht) < threat->getDistance(ht)))
                threat = e;
        if (threat && (ht->getEnergy() < 75 || !storm)) {
            cmdMove(ht, walkableAway(ht, threat->getPosition(), 128));
            continue;
        }
        Position p = follow.isValid() ? follow : homePos();
        if (ht->getDistance(p) > 64) cmdMove(ht, p);
    }
    while (!recentStorms.empty() && Now() - recentStorms.front().second > 72) recentStorms.erase(recentStorms.begin());
}

void update() {
    computeThreats();
    updateSearchGrid();
    std::vector<Unit> army;
    for (auto u : Self()->getUnits())
        if (u->isCompleted() && isArmyType(u->getType()) && u->getType() != UT::Protoss_High_Templar) army.push_back(u);

    workerDefense(army);

    // high templar: storm casters once storm is researched; spent (or storm-less) templar merge into archons
    std::vector<Unit> casters;
    {
        bool storm = Self()->hasResearched(TechTypes::Psionic_Storm);
        bool stormSoon = storm || Self()->isResearching(TechTypes::Psionic_Storm);
        std::vector<Unit> hts;
        for (auto u : Self()->getUnits())
            if (u->getType() == UT::Protoss_High_Templar && u->isCompleted() && u->getOrder() != Orders::ArchonWarp) {
                if (stormSoon && u->getEnergy() >= 40) casters.push_back(u);
                else hts.push_back(u);
            }
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
        (void)storm;
    }

    // visible enemies
    std::vector<Unit> visibleEnemies;
    for (auto e : Opp()->getUnits())
        if (e->exists() && e->isVisible()) visibleEnemies.push_back(e);
    scarabTargets.clear();
    for (auto e : visibleEnemies)
        if (e->getType() == UT::Protoss_Scarab && e->getOrderTarget() && e->getOrderTarget()->getPlayer() == Self())
            scarabTargets.push_back(e->getOrderTarget());

    // global mode
    double mine = armyStrength(army);
    double theirs = Info::enemyArmyStrength();
    // We rarely see the whole enemy army: assume a typical army for the game time.
    double minutes = Now() / (24.0 * 60);
    // ... less what we have killed
    double expectedSupply = std::clamp((minutes - 3.0) * 7.0 - Info::armySupplyKilled, 0.0, 150.0);
    double theirsEst = std::max(theirs, 26.8 * expectedSupply);
    myStrength = mine;
    theirStrength = theirs;
    int armySupply = 0;
    for (auto u : army) armySupply += u->getType().supplyRequired();
    armySupply /= 2;
    bool rangeDone = Self()->getUpgradeLevel(UpgradeTypes::Singularity_Charge) > 0;
    std::string prev = mode;
    static int lastThreatFrame = -100000;
    static Position lastThreatPos = Positions::None;
    if (homeThreat()) lastThreatFrame = Now();
    if (Now() - lastThreatFrame < 24 * 4) {  // stay defending a few seconds: threats at the edge of our ground flicker
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
        if (bd < 1e17) lastThreatPos = goal;
        else if (lastThreatPos.isValid()) goal = lastThreatPos;
    } else if (mode == "attack") {
        Position from = mainCluster >= 0 ? cs[mainCluster].center : homePos();
        attackTarget = chooseAttackTarget(from);
        // no progress towards the target for a long time: give up on it
        static Position progTarget = Positions::None;
        static double progBest = 1e18;
        static int progFrame = 0;
        if (!progTarget.isValid() || progTarget.getDistance(attackTarget) > 96 || progFrame < lastAttackStart) {
            progTarget = attackTarget, progBest = 1e18, progFrame = Now();
        }
        double dNow = from.getDistance(attackTarget);
        if (dNow < progBest - 64) progBest = dNow, progFrame = Now();
        if (Now() - progFrame > 24 * 60 && dNow > 32 * 6) {
            LOG("attack target (%d,%d) unreachable, skipping", attackTarget.x / 32, attackTarget.y / 32);
            unreachable.push_back({attackTarget, Now() + 24 * 60 * 3});
            progTarget = Positions::None;
            attackTarget = chooseAttackTarget(from);
        }
        goal = attackTarget;
    }

    observers(cs, mainCluster);
    {
        // templar follow a few tiles behind the main cluster
        Position follow = homePos();
        if (mainCluster >= 0) {
            Position c = cs[mainCluster].center;
            Position back = MapInfo::nextWaypoint(c, homePos());
            double dx = back.x - c.x, dy = back.y - c.y, l = std::max(1.0, std::sqrt(dx * dx + dy * dy));
            follow = Position(int(c.x + dx / l * 96), int(c.y + dy / l * 96)).makeValid();
        }
        controlCasters(casters, visibleEnemies, follow);
    }

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
            // hopeless outside the main: fall back towards reinforcements instead of feeding units
            bool inMain = MapInfo::mainBase() && MapInfo::areaOf(c.center) == MapInfo::mainBase()->area;
            if (!inMain && sr.ourAfter < sr.ourBefore * 0.15 && sr.theirAfter > sr.theirBefore * 0.5) fight = false;
        }
        if (cloakedDanger) fight = false;
        if (!near.empty() && Now() % 48 == ci % 48)
            LOG("cluster %d n=%d at (%d,%d) mode %s fight %d cloak %d sim ours %.0f->%.0f theirs %.0f->%.0f near %d", ci,
                (int)c.units.size(), c.center.x / 32, c.center.y / 32, mode.c_str(), (int)fight, (int)cloakedDanger, sr.ourBefore,
                sr.ourAfter, sr.theirBefore, sr.theirAfter, (int)near.size());

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
            std::vector<Unit> holdTargets;
            if (mode == "hold")
                for (auto e : nearVisible)
                    if (e->getDistance(cgoal) < 32 * 8 || MapInfo::inMyTerritory(e->getPosition())) holdTargets.push_back(e);
            for (auto &[d, u] : dist) {
                if (gather && d < median - 96 && d > groundRange(Self(), u->getType()) + 64) {
                    if (!u->isHoldingPosition()) u->holdPosition();
                    continue;
                }
                // holding: only fight what comes to the hold position or into our bases; don't chase out
                if (mode == "hold" && !holdTargets.empty()) {
                    microUnit(u, holdTargets, cgoal);
                    continue;
                }
                if (mode == "hold") {
                    Unit t = pickTarget(u, nearVisible);
                    if (t && u->getDistance(t) <= groundRange(Self(), u->getType()) && u->getGroundWeaponCooldown() == 0) cmdAttack(u, t);
                    else if (!u->isAttackFrame() && u->getDistance(cgoal) > 32 * 3) cmdMove(u, cgoal);
                    else if (!u->isAttackFrame() && t && u->getDistance(t) <= groundRange(Self(), u->getType())) cmdAttack(u, t);
                    continue;
                }
                microUnit(u, nearVisible, cgoal);
            }
            continue;
        }
        // no enemies near: move as a group
        if (mode == "attack" && searching && !searchCells.empty()) {
            // spread out to find the remaining buildings: each unit takes one of the stalest cells
            static std::vector<SearchCell *> stale;
            static std::vector<bool> taken;
            static int staleFrame = -1;
            int k = std::min<int>((int)searchCells.size(), std::max<int>(1, (int)army.size()));
            if (staleFrame != Now()) {
                staleFrame = Now();
                stale.clear();
                for (auto &sc : searchCells) stale.push_back(&sc);
                std::partial_sort(stale.begin(), stale.begin() + k, stale.end(),
                                  [](SearchCell *a, SearchCell *b) { return a->lastSeen < b->lastSeen; });
                taken.assign(k, false);
            }
            for (auto u : c.units) {
                if (u->getType() == UT::Protoss_Observer) continue;
                // keep the current cell while it is still stale
                auto lc = u->getLastCommand();
                int pick = -1;
                for (int i = 0; i < k; ++i)
                    if (!taken[i] && lc.getTargetPosition().getDistance(stale[i]->center) < 32) pick = i;
                if (pick < 0) {
                    double bd = 1e18;
                    for (int i = 0; i < k; ++i)
                        if (!taken[i] && u->getDistance(stale[i]->center) < bd) bd = u->getDistance(stale[i]->center), pick = i;
                }
                if (pick < 0) pick = 0;
                else taken[pick] = true;
                cmdAttackMove(u, stale[pick]->center);
            }
            continue;
        }
        std::map<Unit, Position> slots;
        if (mode == "hold") slots = holdSlots(c.units, cgoal);
        for (auto u : c.units) {
            if (mode == "hold") {
                auto it = slots.find(u);
                Position p = it != slots.end() ? it->second : cgoal;
                if (u->getDistance(p) > 24) cmdAttackMove(u, p);
                continue;
            }
            if (ci == mainCluster && c.units.size() > 3 &&
                u->getDistance(c.center) > 32 * (4 + 1.6 * std::sqrt((double)c.units.size())) &&
                u->getDistance(cgoal) < c.center.getDistance(cgoal) &&
                MapInfo::groundDist(u->getPosition(), cgoal) < MapInfo::groundDist(c.center, cgoal)) {
                cmdMove(u, c.center);
                continue;
            }
            cmdAttackMove(u, cgoal);
        }
    }
}
}  // namespace Combat

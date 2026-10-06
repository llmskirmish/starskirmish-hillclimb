#include "common.h"

namespace Info {
std::map<int, EnemyRec> enemies;
std::set<UnitType> seenTypes;
int firstDTFrame = -1;

void update() {
    for (auto u : Opp()->getUnits()) {
        if (!u->exists() || !u->isVisible()) continue;
        auto &r = enemies[u->getID()];
        r.unit = u;
        r.type = u->getType();
        r.pos = u->getPosition();
        r.lastSeen = Now();
        r.hp = u->getHitPoints();
        r.shields = u->getShields();
        r.completed = u->isCompleted();
        r.posValid = true;
        if (!seenTypes.count(r.type)) {
            seenTypes.insert(r.type);
            LOG("first sighting: enemy %s at (%d,%d)", r.type.c_str(), r.pos.x / 32, r.pos.y / 32);
        }
        if (r.type == UT::Protoss_Dark_Templar && firstDTFrame < 0) firstDTFrame = Now();
    }
    for (auto it = enemies.begin(); it != enemies.end();) {
        auto &r = it->second;
        if (r.lastSeen != Now() && r.posValid && Broodwar->isVisible(TilePosition(r.pos))) {
            // We look at where it was and it's gone (or cloaked).
            if (r.type.isBuilding()) {
                it = enemies.erase(it);
                continue;
            }
            r.posValid = false;
        }
        // Forget mobile units unseen for very long.
        if (!r.type.isBuilding() && Now() - r.lastSeen > 24 * 60 * 4) {
            it = enemies.erase(it);
            continue;
        }
        ++it;
    }
}

int armySupplyKilled = 0;
void onDestroy(Unit u) {
    if (u->getPlayer() == Opp() && isArmyType(u->getType()) && u->isCompleted()) armySupplyKilled += u->getType().supplyRequired() / 2;
    enemies.erase(u->getID());
}

int count(UnitType t) {
    int n = 0;
    for (auto &[id, r] : enemies)
        if (r.type == t) ++n;
    return n;
}

bool dtThreat() {
    return seenTypes.count(UT::Protoss_Dark_Templar) || seenTypes.count(UT::Protoss_Templar_Archives) ||
           seenTypes.count(UT::Protoss_Citadel_of_Adun) || seenTypes.count(UT::Protoss_Arbiter) ||
           seenTypes.count(UT::Protoss_Arbiter_Tribunal);
}

bool rushThreat() {
    static bool latched = false;
    if (latched) return true;
    if (Now() > 24 * 60 * 7) return false;
    int zealots = count(UT::Protoss_Zealot);
    int gates = count(UT::Protoss_Gateway);
    bool gas = seenTypes.count(UT::Protoss_Assimilator) > 0;
    int zealotsHome = 0;
    for (auto &[id, e] : enemies)
        if (e.type == UT::Protoss_Zealot && MapInfo::inMyTerritory(e.pos)) ++zealotsHome;
    // "no gas" only counts once we have actually looked at their geyser late enough
    static bool geyserChecked = false;
    if (!geyserChecked && Now() > 24 * 125 && MapInfo::enemyStart != TilePositions::Unknown) {
        Position es = Position(MapInfo::enemyStart) + Position(64, 48);
        for (auto g : Broodwar->getStaticGeysers())
            if (g->getInitialPosition().getDistance(es) < 400 && Broodwar->isVisible(g->getInitialTilePosition()))
                geyserChecked = true;
    }
    bool r = (zealots >= 4 && Now() < 24 * 60 * 4) || (zealotsHome >= 3 && Now() < 24 * 270) ||
             (gates >= 2 && !gas && geyserChecked && Now() > 24 * 140) ||
             (zealots >= 6 && !seenTypes.count(UT::Protoss_Dragoon));
    if (r) {
        latched = true;
        LOG("rush threat detected (zealots %d gates %d gas %d)", zealots, gates, (int)gas);
    }
    return r;
}

bool cloakSeenNow() {
    for (auto u : Opp()->getUnits())
        if (u->isVisible() && !u->isDetected()) return true;
    return false;
}

double enemyArmyStrength(bool recentOnly) {
    double hp = 0, dps = 0;
    for (auto &[id, r] : enemies) {
        if (!isArmyType(r.type) || r.type.isWorker()) continue;
        if (recentOnly && Now() - r.lastSeen > 24 * 60) continue;
        hp += r.type.maxHitPoints() + r.type.maxShields();
        dps += dpsOf(r.type);
    }
    return std::sqrt(hp * dps);
}

int enemyCombatSupply() {
    int s = 0;
    for (auto &[id, r] : enemies)
        if (isArmyType(r.type) && !r.type.isWorker()) s += r.type.supplyRequired();
    return s / 2;
}
}  // namespace Info

// ---------------------------------------------------------------- unit helpers
bool isArmyType(UnitType t) {
    if (t.isBuilding() || t.isWorker()) return false;
    if (t == UT::Protoss_Observer || t == UT::Protoss_Shuttle || t == UT::Protoss_Scarab ||
        t == UT::Protoss_Interceptor || t == UT::Zerg_Overlord || t == UT::Zerg_Larva || t == UT::Zerg_Egg ||
        t == UT::Terran_Science_Vessel || t == UT::Terran_Dropship)
        return false;
    return t.canAttack() || t == UT::Protoss_High_Templar || t == UT::Protoss_Reaver ||
           t == UT::Protoss_Carrier || t == UT::Protoss_Arbiter || t == UT::Terran_Medic;
}

double unitValue(UnitType t) { return t.mineralPrice() + 1.25 * t.gasPrice() + 1; }

double dpsOf(UnitType t, bool vsAir) {
    if (t == UT::Protoss_Reaver) return 100.0 / 60 * 24;
    if (t == UT::Protoss_Carrier) return 6 * 8 * 2 / 30.0 * 24 / 2;
    if (t == UT::Protoss_High_Templar) return 20;
    auto w = vsAir ? t.airWeapon() : t.groundWeapon();
    if (w == WeaponTypes::None || w.damageCooldown() <= 0) return 0;
    int hits = vsAir ? t.maxAirHits() : t.maxGroundHits();
    return double(w.damageAmount() * std::max(1, hits) * w.damageFactor()) / w.damageCooldown() * 24;
}

static bool recent(Unit u, int frames) { return Now() - u->getLastCommandFrame() < frames; }

bool cmdAttack(Unit u, Unit t) {
    if (!u || !t || !t->exists()) return false;
    if (u->getOrder() == Orders::AttackUnit && u->getOrderTarget() == t) return true;
    auto c = u->getLastCommand();
    if (c.getType() == UnitCommandTypes::Attack_Unit && c.getTarget() == t && recent(u, 12)) return true;
    return u->attack(t);
}

bool cmdAttackMove(Unit u, Position p) {
    if (!u || !p.isValid()) return false;
    auto c = u->getLastCommand();
    if (c.getType() == UnitCommandTypes::Attack_Move && c.getTargetPosition().getDistance(p) < 48 &&
        (recent(u, 48) || u->getOrder() == Orders::AttackMove))
        return true;
    return u->attack(p);
}

bool cmdMove(Unit u, Position p) {
    if (!u || !p.isValid()) return false;
    auto c = u->getLastCommand();
    if (c.getType() == UnitCommandTypes::Move && c.getTargetPosition().getDistance(p) < 32 &&
        (recent(u, 24) || u->getOrder() == Orders::Move))
        return true;
    return u->move(p);
}

bool cmdGather(Unit u, Unit t) {
    if (!u || !t || !t->exists()) return false;
    auto c = u->getLastCommand();
    if (c.getType() == UnitCommandTypes::Gather && c.getTarget() == t && recent(u, 24)) return true;
    return u->gather(t);
}

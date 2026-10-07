#include "world.hpp"
#include <cstdio>
using namespace BWAPI;
using namespace bw_demo;
class Candidate : public AIModule {
  std::map<int, Reservation> builders;
  std::set<int> militia;
  Position home, enemy = Positions::Invalid;
  std::map<int, Position> buildings;
  Unit scout = nullptr;
  bool launched = false, dtSeen = false;
  int frame = 0;
  Position clamp(Position p) {
    return Position(std::clamp(p.x, 0, Broodwar->mapWidth() * 32 - 1),
                    std::clamp(p.y, 0, Broodwar->mapHeight() * 32 - 1));
  }
  void move(Unit u, Position p) {
    p = clamp(p);
    if (u->isIdle() || u->getLastCommand().getType() != UnitCommandTypes::Move ||
        u->getLastCommand().getTargetPosition().getApproxDistance(p) > 48 ||
        frame - u->getLastCommandFrame() > 48)
      u->move(p);
  }
  void attack(Unit u, Unit e) {
    if (e && (!u->isAttacking() || u->getOrderTarget() != e))
      u->attack(e);
  }
  void attackPos(Unit u, Position p) {
    if (u->isIdle() || (frame - u->getLastCommandFrame() > 48 &&
                        u->getLastCommand().getTargetPosition().getApproxDistance(p) > 96))
      u->attack(clamp(p));
  }

public:
  void onStart() override {
    Broodwar->setCommandOptimizationLevel(2);
    home = Position(Broodwar->self()->getStartLocation()) + Position(64, 48);
  }
  void onFrame() override {
    frame = Broodwar->getFrameCount();
    if (frame % 6)
      return;
    for (auto it = builders.begin(); it != builders.end();) {
      Unit u = Broodwar->getUnit(it->first);
      bool started = false;
      for (auto b : Broodwar->self()->getUnits())
        if (b->getType() == it->second.type && b->getTilePosition() == it->second.tile)
          started = true;
      if (!u || !u->exists() || started || frame - it->second.frame > 480 ||
          (!u->isConstructing() && frame - it->second.frame > 30))
        it = builders.erase(it);
      else
        ++it;
    }
    World w{builders, militia};
    for (auto &[id, r] : builders) {
      w.minerals -= r.type.mineralPrice();
      w.gas -= r.type.gasPrice();
    }
    auto have = [&](T t) { return w.count(t) + w.pending(t); };
    std::vector<Unit> foes, threats, army;
    for (auto e : Broodwar->enemy()->getUnits())
      if (e->exists() && e->isVisible()) {
        foes.push_back(e);
        if (e->getType().isBuilding()) {
          buildings[e->getID()] = e->getPosition();
          if (e->getType().isResourceDepot() || !enemy.isValid())
            enemy = e->getPosition();
        }
        if (e->getType() == UnitTypes::Protoss_Dark_Templar ||
            e->getType() == UnitTypes::Protoss_Templar_Archives)
          dtSeen = true;
        if (e->getDistance(home) < 600 && (e->getType().canAttack() || e->getType().isWorker()))
          threats.push_back(e);
      }
    for (auto it = buildings.begin(); it != buildings.end();) {
      if (Broodwar->isVisible(TilePosition(it->second)) &&
          (!Broodwar->getUnit(it->first) || !Broodwar->getUnit(it->first)->exists()))
        it = buildings.erase(it);
      else
        ++it;
    }
    Position target = enemy;
    if (!buildings.empty()) {
      int d = 100000;
      for (auto &[id, p] : buildings)
        if (p.getApproxDistance(home) < d) {
          d = p.getApproxDistance(home);
          target = p;
        }
    }
    if (!target.isValid() || (buildings.empty() && Broodwar->isVisible(TilePosition(target)))) {
      target = Positions::Invalid;
      for (auto t : Broodwar->getStartLocations())
        if (t != Broodwar->self()->getStartLocation() &&
            !Broodwar->isExplored(t + TilePosition(2, 1))) {
          target = Position(t) + Position(64, 48);
          break;
        }
      if (!target.isValid()) {
        auto bs = World::bases();
        if (!bs.empty())
          target = bs[(frame / 720) % bs.size()][0];
        else
          target = home;
      }
    }
    for (auto u : Broodwar->self()->getUnits())
      if (u->isCompleted() && !u->getType().isBuilding() && !u->getType().isWorker())
        army.push_back(u);
    int zealots = w.count(T::Zealot), dragoons = w.count(T::Dragoon);
    if (zealots >= 3 || dragoons >= 6 || w.count(T::DT))
      launched = true;
    // On three-start maps the first scout checks the remaining starts in order.
    if (Broodwar->getStartLocations().size() > 2 && !enemy.isValid() && have(T::Probe) >= 9 &&
        (!scout || !scout->exists()))
      for (auto u : Broodwar->self()->getUnits())
        if (u->getType().isWorker() && !builders.count(u->getID()) && !u->isGatheringGas()) {
          scout = u;
          break;
        }
    if (scout && scout->exists() && !enemy.isValid())
      move(scout, target);
    else
      scout = nullptr;
    militia.clear();
    int threatening = 0;
    for (auto e : threats)
      if (e->isDetected() && !e->getType().isWorker() && !e->getType().isBuilding())
        threatening++;
    int nearArmy = 0;
    for (auto u : army)
      if (u->getDistance(home) < 480)
        nearArmy++;
    if (threatening > 0 && nearArmy < threatening * 2 + 1) {
      std::vector<Unit> ws;
      for (auto u : Broodwar->self()->getUnits())
        if (u->getType().isWorker() && !builders.count(u->getID()) && u != scout &&
            u->getDistance(home) < 500)
          ws.push_back(u);
      std::sort(ws.begin(), ws.end(), [&](Unit a, Unit b) {
        return a->getDistance(threats[0]) < b->getDistance(threats[0]);
      });
      int n = std::min((int)ws.size() - 4, threatening * 3 + 2);
      for (int i = 0; i < n; i++) {
        auto u = ws[i];
        militia.insert(u->getID());
        Unit e = nullptr;
        for (auto v : threats)
          if (v->isDetected() && (!e || u->getDistance(v) < u->getDistance(e)))
            e = v;
        if (u->isAttackFrame())
          continue;
        if (u->getShields() < 5 && u->getHitPoints() < 25) {
          Unit m = u->getClosestUnit(Filter::IsMineralField);
          if (m)
            u->gather(m);
        } else
          attack(u, e);
      }
    }
    // Economy: gas is staffed separately, mineral assignments favor nearby unsaturated patches.
    std::vector<Unit> gases, nexuses;
    std::map<int, int> miners;
    for (auto u : Broodwar->self()->getUnits()) {
      if (u->isCompleted() && u->getType().isRefinery())
        gases.push_back(u);
      if (u->isCompleted() && u->getType().isResourceDepot())
        nexuses.push_back(u);
      if (u->getOrderTarget())
        miners[u->getOrderTarget()->getID()]++;
    }
    for (auto u : Broodwar->self()->getUnits()) {
      if (!u->getType().isWorker() || u == scout || builders.count(u->getID()) ||
          militia.count(u->getID()) || u->isConstructing())
        continue;
      if (u->isGatheringGas())
        continue;
      bool assigned = false;
      for (auto g : gases)
        if (miners[g->getID()] < 3 && !u->isCarryingMinerals() && !u->isCarryingGas()) {
          if (u->gather(g)) {
            miners[g->getID()]++;
            assigned = true;
          }
          break;
        }
      if (assigned)
        continue;
      if (!u->isIdle() && u->getOrder() != Orders::AttackUnit && u->getOrder() != Orders::Move)
        continue;
      Unit best = nullptr;
      double bs = 1e9;
      for (auto m : Broodwar->getMinerals()) {
        int baseDist = 100000;
        for (auto n : nexuses)
          baseDist = std::min(baseDist, n->getDistance(m));
        if (baseDist > 400)
          continue;
        double s = miners[m->getID()] * 110 + u->getDistance(m) + baseDist * 0.3;
        if (s < bs) {
          bs = s;
          best = m;
        }
      }
      if (best && u->gather(best))
        miners[best->getID()]++;
    }
    // Build order and adaptive macro.
    if (w.supplyLeft() < 2 + 2 * have(T::Gateway) && !w.pending(T::Pylon))
      w.build(T::Pylon);
    if (have(T::Pylon) && have(T::Gateway) < 2 && have(T::Probe) >= 9)
      w.build(T::Gateway);
    int cap = have(T::Zealot) < 4
                  ? 13
                  : (frame < 6000 ? 20 : std::min(64, 22 * std::max(1, w.count(T::Nexus))));
    if (have(T::Probe) < cap)
      w.train(T::Probe);
    if (have(T::Zealot) >= 5 || frame > 5200) {
      if (!have(T::Gas))
        w.build(T::Gas);
      if (have(T::Gas) && !have(T::Core))
        w.build(T::Core);
    }
    if (w.count(T::Core)) {
      if (!have(T::Robo))
        w.build(T::Robo);
      if (w.count(T::Robo) && !have(T::Observatory))
        w.build(T::Observatory);
      if (w.count(T::Observatory) && have(T::Observer) < 2)
        w.train(T::Observer);
      if (have(T::Dragoon) >= 1 && !w.rangeStarted() && w.minerals >= 150 && w.gas >= 150) {
        w.researchRange();
        w.minerals -= 150;
        w.gas -= 150;
      }
    }
    if (dtSeen && !have(T::Observer)) {
      if (!have(T::Forge))
        w.build(T::Forge);
      if (w.count(T::Forge) && have(T::Cannon) < 2)
        w.build(T::Cannon);
    }
    for (int i = 0; i < w.count(T::Gateway); i++) {
      if (w.count(T::Core) && w.gas >= 50)
        w.train(T::Dragoon);
      else
        w.train(T::Zealot);
    }
    if (frame > 6500 && have(T::Gateway) < 4 && w.minerals >= 250)
      w.build(T::Gateway);
    if (frame > 8500 && w.minerals >= 450 && have(T::Nexus) < 2)
      w.build(T::Nexus);
    if (w.count(T::Nexus) > 1 && have(T::Gas) < w.count(T::Nexus))
      w.build(T::Gas);
    if (w.minerals > 600 && have(T::Gateway) < std::min(12, 4 * w.count(T::Nexus)))
      w.build(T::Gateway);
    if (frame > 11000 && have(T::Nexus) < 4 && w.minerals > 800)
      w.build(T::Nexus);
    if (frame > 10000 && !have(T::Citadel))
      w.build(T::Citadel);
    if (w.count(T::Citadel))
      for (auto u : Broodwar->self()->getUnits())
        if (u->getType() == type(T::Citadel) && !u->isUpgrading() &&
            !Broodwar->self()->getUpgradeLevel(UpgradeTypes::Leg_Enhancements))
          u->upgrade(UpgradeTypes::Leg_Enhancements);
    if (frame > 10000 && !have(T::Forge))
      w.build(T::Forge);
    if (w.count(T::Forge) && w.minerals > 200 && w.gas > 150)
      for (auto u : Broodwar->self()->getUnits())
        if (u->getType() == type(T::Forge) && !u->isUpgrading())
          u->upgrade(UpgradeTypes::Protoss_Ground_Weapons);
    // Fight nearby threats with individual target choice and Dragoon cooldown movement.
    Position center = home;
    if (!army.empty()) {
      center = Position(0, 0);
      int n = 0;
      for (auto u : army)
        if (u->getType() != type(T::Observer)) {
          center += u->getPosition();
          n++;
        }
      if (n)
        center /= n;
      else
        center = home;
    }
    for (auto u : army) {
      if (u->getType() == type(T::Observer)) {
        move(u, center);
        continue;
      }
      if (u->isAttackFrame() || u->isStartingAttack())
        continue;
      Unit best = nullptr, close = nullptr;
      double bs = 1e9;
      int range = Broodwar->self()->weaponMaxRange(u->getType().groundWeapon());
      for (auto e : foes) {
        if (!e->isDetected() || e->isInvincible() || e->isFlying())
          continue;
        int d = u->getDistance(e);
        if (d > 560)
          continue;
        double s = d + (e->getType().isBuilding() && !e->getType().canAttack() ? 180 : 0) +
                   (e->getType().isWorker() ? 30 : 0);
        if (d <= range + 16)
          s -= 100;
        s += (e->getHitPoints() + e->getShields()) * .13;
        if (s < bs) {
          bs = s;
          best = e;
        }
        if (e->getType().canAttack() && !e->getType().isWorker() &&
            (!close || d < u->getDistance(close)))
          close = e;
      }
      if (best) {
        if (u->getType() == type(T::Dragoon) && u->getGroundWeaponCooldown() > 5 && close &&
            u->getDistance(close) < range && close->getType().groundWeapon().maxRange() < range) {
          auto p = u->getPosition(), q = close->getPosition();
          double d = std::max(1.0, std::hypot(p.x - q.x, p.y - q.y));
          move(u, p + Position(int((p.x - q.x) * 80 / d), int((p.y - q.y) * 80 / d)));
        } else
          attack(u, best);
      } else if (!threats.empty() && u->getDistance(home) < 1000)
        attackPos(u, threats.front()->getPosition());
      else if (launched)
        attackPos(u, target);
      else if (u->getDistance(home) > 160)
        move(u, home);
    }
    if (frame % 1440 == 0)
      std::printf("t=%d p=%d z=%d d=%d gates=%d minerals=%d gas=%d target=%d,%d\n", frame,
                  have(T::Probe), zealots, dragoons, have(T::Gateway), w.minerals, w.gas, target.x,
                  target.y);
  }
};
BWAPI::AIModule *makeCandidateBot() { return new Candidate(); }

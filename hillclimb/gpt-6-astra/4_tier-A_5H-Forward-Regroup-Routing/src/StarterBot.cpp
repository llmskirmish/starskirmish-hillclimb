#include "world.hpp"
#include <FAP.hpp>
#include <bwem.h>
#include <cstdio>
using namespace BWAPI;
using namespace bw_demo;
class Candidate : public AIModule {
  std::map<int, Reservation> builders;
  std::set<int> militia;
  Position home, rally, enemy = Positions::Invalid;
  std::map<int, Position> buildings;
  Unit scout = nullptr;
  Position firstScout = Positions::Invalid, secondScout = Positions::Invalid,
           knownStart = Positions::Invalid;
  bool proxyLocated = false;
  bool launched = false, dtSeen = false, dtReady = false, enemyDetection = false, enemyRobo = false;
  Position chokeRally = Positions::Invalid, workerRally = Positions::Invalid;
  int dtMade = 0;
  int steals = 0, scoutTrips = 0;
  Position searchGoal = Positions::Invalid;
  int searchIndex = 0, searchFrame = 0;
  std::vector<Position> searchPoints;
  std::vector<std::pair<Position, int>> storms;
  int frame = 0;
  std::map<int, bool> retreat;
  std::map<int, int> exposedUntil;
  std::set<int> transferred;
  std::map<int, int> gasWorkers;
  std::map<int, int> mineralWorkers;
  auto simUnit(Unit u) {
    auto t = u->getType();
    auto p = u->getPlayer();
    return FAP::makeUnit()
        .setUnitType(t)
        .setPosition(u->getPosition())
        .setHealth(u->getHitPoints())
        .setShields(u->getShields())
        .setArmorUpgrades(p->getUpgradeLevel(t.armorUpgrade()))
        .setAttackerCount(1)
        .setAttackUpgrades(p->getUpgradeLevel(t.groundWeapon().upgradeType()))
        .setShieldUpgrades(p->getUpgradeLevel(UpgradeTypes::Protoss_Plasma_Shields))
        .setSpeedUpgrade(p->getUpgradeLevel(t == type(T::Zealot) ? UpgradeTypes::Leg_Enhancements
                                                                 : UpgradeTypes::None) > 0)
        .setAttackSpeedUpgrade(false)
        .setStimmed(false)
        .setRangeUpgrade(t == type(T::Dragoon) &&
                         p->getUpgradeLevel(UpgradeTypes::Singularity_Charge) > 0)
        .setAttackCooldownRemaining(u->getGroundWeaponCooldown())
        .setFlying(u->isFlying())
        .setElevation(Broodwar->getGroundHeight(u->getTilePosition()))
        .setData(std::tuple<>{});
  }

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
    if (e && !e->isFlying() &&
        u->getDistance(e) > Broodwar->self()->weaponMaxRange(u->getType().groundWeapon()) + 16 &&
        !World::clearGroundLine(u->getPosition(), e->getPosition())) {
      move(u, World::waypoint(u->getPosition(), e->getPosition(), true));
      return;
    }
    if (e &&
        (u->isIdle() || u->getLastCommand().getType() != UnitCommandTypes::Attack_Unit ||
         u->getLastCommand().getTarget() != e) &&
        frame - u->getLastCommandFrame() >= 6)
      u->attack(e);
  }
  void attackPos(Unit u, Position p) {
    if (!u->isFlying() && u->getDistance(p) > 192 && !World::clearGroundLine(u->getPosition(), p)) {
      move(u, World::waypoint(u->getPosition(), p, true));
      return;
    }
    if (u->isIdle() || (frame - u->getLastCommandFrame() > 48 &&
                        u->getLastCommand().getTargetPosition().getApproxDistance(p) > 96))
      u->attack(clamp(p));
  }
  TilePosition forwardSite(Position enemyMain) {
    const auto &gd = World::groundDistance();
    int mw = Broodwar->mapWidth();
    auto t = TilePosition(enemyMain);
    TilePosition anchor = t;
    for (int i = 0; i < 20; i++) {
      if (!t.isValid() || gd[t.x + t.y * mw] < 0)
        break;
      auto next = t;
      int d = gd[t.x + t.y * mw];
      for (auto v :
           {TilePosition(-1, 0), TilePosition(1, 0), TilePosition(0, -1), TilePosition(0, 1)}) {
        auto q = t + v;
        if (q.isValid() && gd[q.x + q.y * mw] >= 0 && gd[q.x + q.y * mw] < d) {
          d = gd[q.x + q.y * mw];
          next = q;
        }
      }
      if (next == t)
        break;
      t = next;
      anchor = t;
    }
    auto best = TilePositions::Invalid;
    double bs = 1e9;
    for (int dx = -10; dx <= 10; dx++)
      for (int dy = -10; dy <= 10; dy++) {
        auto q = anchor + TilePosition(dx, dy);
        if (!q.isValid() || !World::reachable(q) || !Broodwar->canBuildHere(q, type(T::Pylon)))
          continue;
        int dist = (Position(q) + Position(32, 32)).getApproxDistance(enemyMain);
        if (dist < 500 || dist > 1200)
          continue;
        int free = 0;
        for (int x = -3; x <= 3; x++)
          for (int y = -3; y <= 3; y++) {
            auto v = q + TilePosition(x, y);
            if (v.isValid() && Broodwar->isBuildable(v) && World::reachable(v))
              free++;
          }
        if (free < 40)
          continue;
        double score = dx * dx + dy * dy + gd[q.x + q.y * mw] * .1 + std::abs(dist - 650) * .06;
        if (score < bs) {
          bs = score;
          best = q;
        }
      }
    return best.isValid() ? best : anchor;
  }

public:
  void onUnitDestroy(Unit u) override {
    if (u->getPlayer() == Broodwar->self() && u->getType() == type(T::DT))
      enemyDetection = true;
  }
  void onUnitComplete(Unit u) override {
    if (u->getPlayer() == Broodwar->self() && u->getType() == type(T::DT))
      dtMade++;
  }
  void onStart() override {
    Broodwar->setCommandOptimizationLevel(2);
    home = Position(Broodwar->self()->getStartLocation()) + Position(64, 48);
    BWEM::Map::Instance().Initialize(BroodwarPtr);
    BWEM::Map::Instance().EnableAutomaticPathAnalysis();
    BWEM::Map::Instance().FindBasesForStartingLocations();
    rally = home;
    const auto &gd = World::groundDistance();
    int mw = Broodwar->mapWidth();
    TilePosition t = Broodwar->self()->getStartLocation();
    for (auto v : Broodwar->getStartLocations())
      if (v != t) {
        t = v;
        break;
      }
    for (int i = 0; i < 1000; i++) {
      if (!t.isValid())
        break;
      int d = gd[t.x + t.y * mw];
      if (d < 0)
        break;
      if (d <= 13) {
        rally = Position(t) + Position(16, 16);
        break;
      }
      TilePosition next = t;
      for (auto delta :
           {TilePosition(-1, 0), TilePosition(1, 0), TilePosition(0, -1), TilePosition(0, 1)}) {
        auto q = t + delta;
        if (q.isValid() && gd[q.x + q.y * mw] >= 0 && gd[q.x + q.y * mw] < d) {
          d = gd[q.x + q.y * mw];
          next = q;
        }
      }
      if (next == t)
        break;
      t = next;
    }
    t = Broodwar->self()->getStartLocation();
    for (auto v : Broodwar->getStartLocations())
      if (v != t) {
        t = v;
        break;
      }
    int steps = 0;
    for (int i = 0; i < 1000; i++) {
      if (!t.isValid())
        break;
      int d = gd[t.x + t.y * mw];
      if (d < 0)
        break;
      if (steps >= 20) {
        World::proxy = t;
        break;
      }
      TilePosition next = t;
      for (auto delta :
           {TilePosition(-1, 0), TilePosition(1, 0), TilePosition(0, -1), TilePosition(0, 1)}) {
        auto q = t + delta;
        if (q.isValid() && gd[q.x + q.y * mw] >= 0 && gd[q.x + q.y * mw] < d) {
          d = gd[q.x + q.y * mw];
          next = q;
        }
      }
      if (next == t)
        break;
      t = next;
      steps++;
    }
    if (World::proxy.isValid()) {
      auto anchor = World::proxy;
      TilePosition et;
      for (auto q : Broodwar->getStartLocations())
        if (q != Broodwar->self()->getStartLocation()) {
          et = q;
          break;
        }
      double dx = Position(et).x - home.x, dy = Position(et).y - home.y,
             d = std::max(1.0, std::hypot(dx, dy));
      bool found = false;
      for (int off : {8, -8, 6, -6, 10, -10, 4, -4}) {
        auto q = anchor + TilePosition(int(-dy / d * off), int(dx / d * off));
        if (q.isValid() && gd[q.x + q.y * mw] >= 0 && World::reachable(q) &&
            Broodwar->canBuildHere(q, type(T::Pylon)) &&
            Position(q).getApproxDistance(Position(et)) > 550) {
          World::proxy = q;
          found = true;
          break;
        }
      }
    }
    if (Broodwar->getStartLocations().size() > 2) {
      Position sum(0, 0);
      for (auto q : Broodwar->getStartLocations())
        sum += Position(q);
      auto c = TilePosition(sum / int(Broodwar->getStartLocations().size()));
      int best = 100000;
      for (int x = -15; x <= 15; x++)
        for (int y = -15; y <= 15; y++) {
          auto q = c + TilePosition(x, y);
          int score = x * x + y * y;
          if (q.isValid() && gd[q.x + q.y * mw] >= 0 && score < best && World::reachable(q) &&
              Broodwar->canBuildHere(q, type(T::Pylon))) {
            best = score;
            World::proxy = q;
          }
        }
    }
    for (int y = 4; y < Broodwar->mapHeight(); y += 12)
      for (int i = 4; i < Broodwar->mapWidth(); i += 12) {
        int x = ((y / 12) % 2) ? Broodwar->mapWidth() - 1 - i : i;
        TilePosition q(x, y);
        if (q.isValid() && gd[q.x + q.y * mw] >= 0)
          searchPoints.push_back(Position(q) + Position(16, 16));
      }
    auto viable = [&](TilePosition q) {
      int free = 0;
      for (int dx = -4; dx <= 4; dx++)
        for (int dy = -4; dy <= 4; dy++) {
          auto v = q + TilePosition(dx, dy);
          if (v.isValid() && Broodwar->isBuildable(v) && World::reachable(v))
            free++;
        }
      return free >= 65;
    };
    if (World::proxy.isValid() && !viable(World::proxy)) {
      auto old = World::proxy;
      int best = 999999;
      TilePosition chosen = TilePositions::Invalid;
      for (int dx = -16; dx <= 16; dx++)
        for (int dy = -16; dy <= 16; dy++) {
          auto q = old + TilePosition(dx, dy);
          if (q.isValid() && World::reachable(q) && Broodwar->canBuildHere(q, type(T::Pylon)) &&
              viable(q)) {
            int score = dx * dx + dy * dy;
            if (score < best) {
              best = score;
              chosen = q;
            }
          }
        }
      World::proxy = chosen;
    }
    if (Broodwar->getStartLocations().size() == 2) {
      TilePosition enemyStart;
      for (auto st : Broodwar->getStartLocations())
        if (st != Broodwar->self()->getStartLocation())
          enemyStart = st;
      const auto &ed = World::distanceFrom(enemyStart);
      int old = World::proxy.isValid() ? ed[World::proxy.x + World::proxy.y * mw] : -1;
      if (old < 450 || old > 1200) {
        TilePosition best = TilePositions::Invalid;
        double score = 1e30;
        for (int y = 2; y < Broodwar->mapHeight() - 2; y++)
          for (int x = 2; x < mw - 2; x++) {
            auto q = TilePosition(x, y);
            int e = ed[x + y * mw], h = gd[x + y * mw];
            if (e < 550 || e > 1000 || h < 0 || !Broodwar->canBuildHere(q, type(T::Pylon)) ||
                !viable(q))
              continue;
            double s = h * 8 + e * 1.6;
            if (s < score) {
              score = s;
              best = q;
            }
          }
        if (best.isValid())
          World::proxy = best;
      }
      std::printf("PROXY ground approach %d -> %d\n", old,
                  World::proxy.isValid() ? ed[World::proxy.x + World::proxy.y * mw] : -1);
    }
    std::printf("PROXY %d,%d area-distance=%d\n", World::proxy.x, World::proxy.y,
                World::proxy.isValid() ? gd[World::proxy.x + World::proxy.y * mw] : -1);
    for (auto u : World::own())
      if (u->getType().isWorker()) {
        World::forwardWorker = u;
        break;
      }
    if (Broodwar->getStartLocations().size() > 2) {
      for (auto t : Broodwar->getStartLocations())
        if (t != Broodwar->self()->getStartLocation()) {
          if (!firstScout.isValid())
            firstScout = Position(t) + Position(64, 48);
          else
            secondScout = Position(t) + Position(64, 48);
        }
      for (auto u : World::own())
        if (u->getType().isWorker() && u != World::forwardWorker) {
          scout = u;
          scoutTrips = 1;
          break;
        }
    }

    for (auto start : Broodwar->getStartLocations())
      if (start != Broodwar->self()->getStartLocation()) {
        const auto &path = BWEM::Map::Instance().GetPath(home, Position(start) + Position(64, 48));
        if (!path.empty()) {
          auto pos = Position(path.front()->Center());
          auto v = home - pos;
          double d = std::max(1.0, std::hypot(v.x, v.y));
          rally = pos + Position(int(v.x * 200 / d), int(v.y * 200 / d));
          break;
        }
      }
  }
  void onFrame() override {
    frame = Broodwar->getFrameCount();
    if (frame % 6)
      return;
    for (auto it = builders.begin(); it != builders.end();) {
      Unit u = Broodwar->getUnit(it->first);
      bool started = false;
      for (auto b : World::own())
        if (b->getType() == it->second.type && b->getTilePosition() == it->second.tile)
          started = true;
      if (!u || !u->exists() || started || frame - it->second.frame > 720) {
        if (!started && u && u->exists() && frame - it->second.frame > 720)
          World::failedSites[{it->second.tile.x, it->second.tile.y}] = frame + 2400;
        it = builders.erase(it);
      } else {
        auto &r = it->second;
        Position site =
            Position(r.tile) + Position(r.type.tileWidth() * 16, r.type.tileHeight() * 16);
        if (frame - r.frame >= 12 && frame - u->getLastCommandFrame() >= 18 &&
            !u->isConstructing()) {
          auto next = World::waypoint(u->getPosition(), site);
          if (u->getDistance(site) > 128 || next.getApproxDistance(site) > 96) {
            if (u->isIdle() || u->getLastCommand().getType() != UnitCommandTypes::Move ||
                u->getLastCommand().getTargetPosition().getApproxDistance(next) > 64 ||
                frame - u->getLastCommandFrame() > 120)
              u->move(next);
          } else
            u->build(r.type, r.tile);
        }
        ++it;
      }
    }
    if (frame % 240 == 0)
      for (auto &[id, r] : builders) {
        auto u = Broodwar->getUnit(id);
        if (u)
          std::printf("RES t=%d type=%s site=%d,%d worker=%d,%d order=%s age=%d\n", frame,
                      r.type.c_str(), r.tile.x, r.tile.y, u->getPosition().x, u->getPosition().y,
                      u->getOrder().c_str(), frame - r.frame);
      }
    World::reservedTiles.clear();
    for (auto &[id, r] : builders)
      World::reservedTiles.emplace_back(r.type, r.tile);
    World w{builders, militia};
    for (auto &[id, r] : builders) {
      w.minerals -= r.type.mineralPrice();
      w.gas -= r.type.gasPrice();
    }
    auto have = [&](T t) { return w.count(t) + w.pending(t); };
    std::vector<Unit> foes, threats, army;
    for (auto e : World::ordered(Broodwar->enemy()->getUnits()))
      if (e->exists() && e->isVisible()) {
        foes.push_back(e);
        if (e->getType().isDetector() && e->isCompleted())
          enemyDetection = true;
        if (e->getType() == type(T::Robo) || e->getType() == type(T::Observatory))
          enemyRobo = true;
        if (e->getType().isBuilding()) {
          for (auto st : Broodwar->getStartLocations())
            if (st != Broodwar->self()->getStartLocation() &&
                e->getDistance(Position(st) + Position(64, 48)) < 600)
              knownStart = Position(st) + Position(64, 48);
          buildings[e->getID()] = e->getPosition();
          if (e->getType().isResourceDepot() || !enemy.isValid())
            enemy = e->getPosition();
        }
        if (e->getType() == UnitTypes::Protoss_Dark_Templar ||
            e->getType() == UnitTypes::Protoss_Templar_Archives)
          dtSeen = true;
        bool nearBase = e->getDistance(home) < 500;
        for (auto n : World::own())
          if (n->getType().isResourceDepot() && n->getDistance(e) < 500)
            nearBase = true;
        if (nearBase && (e->getType().canAttack() || e->getType().isWorker()))
          threats.push_back(e);
      }
    for (auto it = buildings.begin(); it != buildings.end();) {
      if (Broodwar->isVisible(TilePosition(it->second)) &&
          (!Broodwar->getUnit(it->first) || !Broodwar->getUnit(it->first)->exists() ||
           Broodwar->getUnit(it->first)->getPlayer() != Broodwar->enemy() ||
           !Broodwar->getUnit(it->first)->getType().isBuilding() ||
           Broodwar->getUnit(it->first)->getPosition().getApproxDistance(it->second) > 64))
        it = buildings.erase(it);
      else
        ++it;
    }
    if (firstScout.isValid() && !proxyLocated && knownStart.isValid()) {
      if (Broodwar->self()->allUnitCount(type(T::Pylon)) == 0) {
        World::proxy = forwardSite(knownStart);
        builders.clear();
        if (scout && scout->exists() &&
            (!World::forwardWorker || !World::forwardWorker->exists() ||
             scout->getDistance(Position(World::proxy)) <
                 World::forwardWorker->getDistance(Position(World::proxy))))
          World::forwardWorker = scout;
        scout = nullptr;
      }
      proxyLocated = true;
    }
    if (proxyLocated && firstScout.isValid() && scout) {
      Unit mineral = nullptr;
      for (auto m : World::ordered(Broodwar->getMinerals()))
        if (m->getDistance(home) < 400 &&
            (!mineral || m->getDistance(home) < mineral->getDistance(home)))
          mineral = m;
      if (mineral && scout->exists())
        scout->gather(mineral);
      scout = nullptr;
      scoutTrips = 2;
    }
    World::danger.clear();
    for (auto &[id, p] : buildings)
      World::danger.push_back(p);
    for (auto e : foes)
      if (e->getType().canAttack() && !e->getType().isWorker())
        World::danger.push_back(e->getPosition());
    if (buildings.empty() && enemy.isValid() && Broodwar->isVisible(TilePosition(enemy)))
      enemy = Positions::Invalid;
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
        if (!searchPoints.empty()) {
          if (!searchGoal.isValid() || Broodwar->isVisible(TilePosition(searchGoal)) ||
              frame - searchFrame > 2400) {
            for (int i = 0; i < (int)searchPoints.size(); i++) {
              searchGoal = searchPoints[searchIndex++ % searchPoints.size()];
              if (!Broodwar->isVisible(TilePosition(searchGoal)))
                break;
            }
            searchFrame = frame;
          }
          target = searchGoal;
        } else
          target = home;
      }
    }
    if (!(World::proxy.isValid() && frame < 6500))
      for (auto e : threats)
        if (!e->getType().isWorker() && !e->getType().isBuilding()) {
          target = e->getPosition();
          break;
        }
    if (World::proxy.isValid() && frame < 6500) {
      Position far = Positions::Invalid;
      int dist = 0;
      for (auto &[id, p] : buildings)
        if (p.getApproxDistance(home) > 1400 && p.getApproxDistance(home) > dist) {
          dist = p.getApproxDistance(home);
          far = p;
        }
      if (far.isValid())
        target = far;
      else
        for (auto t : Broodwar->getStartLocations())
          if (t != Broodwar->self()->getStartLocation() &&
              !Broodwar->isExplored(t + TilePosition(2, 1))) {
            target = Position(t) + Position(64, 48);
            break;
          }
    }
    for (auto u : World::own())
      if (u->isCompleted() && !u->getType().isBuilding() && !u->getType().isWorker())
        army.push_back(u);
    if (!chokeRally.isValid()) {
      chokeRally = rally;
      Position sum(0, 0);
      int n = 0;
      for (auto m : Broodwar->getStaticMinerals())
        if (m->getInitialPosition().getApproxDistance(home) < 320) {
          sum += m->getInitialPosition();
          n++;
        }
      workerRally = n ? (home + sum / n) / 2 : home;
    }
    int fighters = 0;
    for (auto u : army)
      if (u->getType().canAttack())
        fighters++;
    rally = (frame < 6500 && World::proxy.isValid()) ? Position(World::proxy) + Position(32, 32)
                                                     : chokeRally;
    for (auto u : army)
      if (u->getType() == type(T::DT) && u->isUnderAttack()) {
        enemyDetection = true;
        for (auto a : army)
          if (a->getType() == type(T::DT) && a->getDistance(u) < 350)
            exposedUntil[a->getID()] = frame + 240;
      }
    int zealots = w.count(T::Zealot), dragoons = w.count(T::Dragoon);
    if (zealots >= 2 || dragoons >= 6 || w.count(T::DT))
      launched = true;
    if (!zealots && dragoons < 3 && !w.count(T::DT))
      launched = false;
    // On three-start maps the first scout checks the remaining starts in order.
    if (frame < 3600 && Broodwar->getStartLocations().size() > 2 && w.count(T::Probe) >= 7 &&
        scoutTrips < 1 && (!scout || !scout->exists()))
      for (auto u : World::own())
        if (u->getType().isWorker() && u != World::forwardWorker && !builders.count(u->getID()) &&
            !u->isGatheringGas()) {
          scout = u;
          scoutTrips++;
          break;
        }
    if (scout && scout->exists() && frame < 6500) {
      Position goal = (!knownStart.isValid() && secondScout.isValid()) ? secondScout : target;
      if (enemy.isValid() && (!firstScout.isValid() || knownStart.isValid())) {
        Position main = knownStart.isValid() ? knownStart : enemy;
        int bd = 999999;
        for (auto t : Broodwar->getStartLocations())
          if (t != Broodwar->self()->getStartLocation()) {
            auto p = Position(t) + Position(64, 48);
            if (p.getApproxDistance(enemy) < bd) {
              bd = p.getApproxDistance(enemy);
              main = p;
            }
          }
        static int phase = 0;
        static const Position offsets[] = {Position(190, 0), Position(0, 190), Position(-190, 0),
                                           Position(0, -190)};
        goal = clamp(main + offsets[phase % 4]);
        if (scout->getDistance(goal) < 90)
          phase++;
        Unit danger = nullptr;
        for (auto e : foes)
          if (e->getType().canAttack() &&
              (!e->getType().isWorker() || e->getOrderTarget() == scout) &&
              scout->getDistance(e) < (e->getType().isWorker() ? 80 : 230) &&
              (!danger || scout->getDistance(e) < scout->getDistance(danger)))
            danger = e;
        if (danger) {
          auto v = scout->getPosition() - danger->getPosition();
          double d = std::max(1.0, std::hypot(v.x, v.y));
          goal = scout->getPosition() + Position(int(v.x * 128 / d), int(v.y * 128 / d));
        }
      }
      move(scout, goal);
    } else
      scout = nullptr;
    militia.clear();
    if (World::proxy.isValid() && frame < 6500 &&
        (!World::forwardWorker || !World::forwardWorker->exists())) {
      Unit best = nullptr;
      for (auto u : World::own())
        if (u->getType().isWorker() && u != scout && !u->isGatheringGas() &&
            u->getHitPoints() + u->getShields() > 25 &&
            (!best ||
             u->getDistance(Position(World::proxy)) < best->getDistance(Position(World::proxy))))
          best = u;
      World::forwardWorker = best;
    }
    if (World::forwardWorker && World::forwardWorker->exists() && frame < 6500) {
      auto u = World::forwardWorker;
      militia.insert(u->getID());
      Unit danger = nullptr;
      for (auto e : foes)
        if (e->getType().canAttack() && (!e->getType().isWorker() || e->getOrderTarget() == u) &&
            u->getDistance(e) < 220 && (!danger || u->getDistance(e) < u->getDistance(danger)))
          danger = e;
      if (danger && (!builders.count(u->getID()) || u->getHitPoints() + u->getShields() < 18)) {
        builders.erase(u->getID());
        auto v = u->getPosition() - danger->getPosition();
        double angle = std::atan2(v.y, v.x), best = -1e9;
        Position goal = home;
        for (int i = -4; i <= 4; i++) {
          double a = angle + i * .35;
          auto p =
              clamp(u->getPosition() + Position(int(std::cos(a) * 160), int(std::sin(a) * 160)));
          if (!World::reachable(TilePosition(p)))
            continue;
          bool free = true;
          for (auto b : Broodwar->getAllUnits())
            if (b->getType().isBuilding() && b->getDistance(p) < 24)
              free = false;
          if (!free)
            continue;
          double score = std::cos(i * .35) * 100 + danger->getDistance(p) * .5;
          if (score > best) {
            best = score;
            goal = p;
          }
        }
        move(u, goal);
      } else if (!builders.count(u->getID()) && !u->isConstructing()) {
        auto proxy = Position(World::proxy) + Position(32, 32);
        auto v = home - proxy;
        double d = std::max(1.0, std::hypot(v.x, v.y));
        auto goal = (!knownStart.isValid() && firstScout.isValid() && frame < 1800)
                        ? firstScout
                        : proxy + Position(int(v.x * 160 / d), int(v.y * 160 / d));
        move(u, World::waypoint(u->getPosition(), goal));
      }
    }

    int threatening = 0;
    for (auto e : threats)
      if (e->isDetected() && !e->getType().isWorker() && !e->getType().isBuilding() &&
          e->getDistance(home) < 450 && e->getType().groundWeapon().maxRange() < 64)
        threatening++;
    int nearArmy = 0;
    for (auto u : army)
      if (u->getDistance(home) < 480)
        nearArmy++;
    if (threatening > 0 && nearArmy < threatening * 2 + 1) {
      std::vector<Unit> ws;
      for (auto u : World::own())
        if (u->getType().isWorker() && !builders.count(u->getID()) && u != scout &&
            u->getDistance(home) < 500)
          ws.push_back(u);
      std::sort(ws.begin(), ws.end(), [&](Unit a, Unit b) {
        return a->getDistance(threats[0]) < b->getDistance(threats[0]);
      });
      int n = std::min((int)ws.size() - 2, threatening * 4 + 2);
      for (int i = 0; i < n; i++) {
        auto u = ws[i];
        militia.insert(u->getID());
        Unit e = nullptr;
        for (auto v : threats)
          if (v->isDetected() && !v->getType().isWorker() && !v->getType().isBuilding() &&
              (!e || u->getDistance(v) < u->getDistance(e)))
            e = v;
        if (u->isAttackFrame() || u->isStartingAttack())
          continue;
        if (u->getShields() < 8 && u->getHitPoints() < 28) {
          Unit m = nullptr;
          double score = -1e9;
          for (auto v : World::ordered(Broodwar->getMinerals()))
            if (v->getDistance(home) < 600) {
              double val = (e ? v->getDistance(e) : v->getDistance(home)) - .3 * u->getDistance(v);
              if (val > score) {
                score = val;
                m = v;
              }
            }
          if (m)
            u->gather(m);
        } else
          attack(u, e);
      }
    }
    if (scout && scout->exists())
      militia.insert(scout->getID());
    // Economy: gas is staffed separately, mineral assignments favor nearby unsaturated patches.
    std::vector<Unit> gases, nexuses;
    std::map<int, int> miners;
    for (auto u : World::own()) {
      if (u->isCompleted() && u->getType().isRefinery() && World::homeGas(u))
        gases.push_back(u);
      if (u->isCompleted() && u->getType().isResourceDepot())
        nexuses.push_back(u);
      if (u->getOrderTarget() && !u->isGatheringGas())
        miners[u->getOrderTarget()->getID()]++;
    }
    for (auto it = gasWorkers.begin(); it != gasWorkers.end();) {
      Unit u = Broodwar->getUnit(it->first), g = Broodwar->getUnit(it->second);
      if (!u || !u->exists() || !g || !g->exists() || builders.count(it->first) ||
          militia.count(it->first))
        it = gasWorkers.erase(it);
      else
        ++it;
    }
    int gasCap = w.gas > 800 ? 1 : 3;
    for (auto g : gases) {
      std::vector<int> assigned;
      for (auto [id, gid] : gasWorkers)
        if (gid == g->getID())
          assigned.push_back(id);
      while ((int)assigned.size() > gasCap) {
        auto id = assigned.back();
        assigned.pop_back();
        gasWorkers.erase(id);
        auto u = Broodwar->getUnit(id);
        if (u)
          u->stop();
      }
      while ((int)assigned.size() < gasCap) {
        Unit best = nullptr;
        for (auto u : World::own())
          if (u->getType().isWorker() && u != scout && !gasWorkers.count(u->getID()) &&
              !builders.count(u->getID()) && !militia.count(u->getID()) && !u->isConstructing() &&
              !u->isCarryingMinerals() && !u->isCarryingGas() &&
              (!best || u->getDistance(g) < best->getDistance(g)))
            best = u;
        if (!best)
          break;
        gasWorkers[best->getID()] = g->getID();
        assigned.push_back(best->getID());
        best->gather(g);
      }
    }
    miners.clear();
    for (auto it = mineralWorkers.begin(); it != mineralWorkers.end();) {
      Unit u = Broodwar->getUnit(it->first), m = Broodwar->getUnit(it->second);
      if (!u || !u->exists() || !m || !m->exists() || m->getResources() == 0 ||
          gasWorkers.count(it->first) || builders.count(it->first) || militia.count(it->first) ||
          u == scout)
        it = mineralWorkers.erase(it);
      else {
        miners[it->second]++;
        ++it;
      }
    }
    for (auto u : World::own()) {
      if (!u->getType().isWorker() || u == scout || builders.count(u->getID()) ||
          militia.count(u->getID()) || u->isConstructing())
        continue;
      if (gasWorkers.count(u->getID())) {
        auto g = Broodwar->getUnit(gasWorkers[u->getID()]);
        if (g && !u->isGatheringGas() && !u->isCarryingGas())
          u->gather(g);
        continue;
      }
      if (u->isCarryingMinerals() || u->isCarryingGas()) {
        if (u->isIdle())
          u->returnCargo();
        continue;
      }
      Unit old = mineralWorkers.count(u->getID()) ? Broodwar->getUnit(mineralWorkers[u->getID()])
                                                  : nullptr;
      bool rebalance = frame % 120 == 0 && old && miners[old->getID()] > 2;
      if (old && !rebalance) {
        if (u->isIdle() || !u->isGatheringMinerals())
          u->gather(old);
        continue;
      }
      if (old)
        miners[old->getID()]--;
      Unit best = nullptr;
      double bs = 1e9;
      for (auto m : World::ordered(Broodwar->getMinerals())) {
        int baseDist = 100000;
        for (auto n : nexuses)
          baseDist = std::min(baseDist, n->getDistance(m));
        if (baseDist > 360)
          continue;
        double score = miners[m->getID()] * 220 + u->getDistance(m) * .15 + baseDist * .25;
        if (score < bs) {
          bs = score;
          best = m;
        }
      }
      if (best) {
        mineralWorkers[u->getID()] = best->getID();
        miners[best->getID()]++;
        if (best != old || u->isIdle())
          u->gather(best);
      }
    }
    int invaders = 0;
    for (auto e : threats)
      if (!e->getType().isWorker() && !e->getType().isBuilding())
        invaders++;
    if ((!firstScout.isValid() || proxyLocated || frame > 1800) &&
        w.supplyLeft() < (frame < 6000 ? 4 : 6) + std::max(0, have(T::Gateway) - 2) &&
        !w.pending(T::Pylon))
      w.build(T::Pylon);
    if (have(T::Pylon) && have(T::Gateway) < 2 && have(T::Probe) >= 8)
      w.build(T::Gateway);

    if (frame > 3000 && have(T::Gateway) < 3 && have(T::Zealot) >= 6)
      w.build(T::Gateway);
    int cap = have(T::Gateway) < 2
                  ? 9
                  : (frame < 6000 ? 13 : std::min(64, 22 * std::max(1, w.count(T::Nexus))));
    if (frame > 6000) {
      bool homePylon = false;
      for (auto u : World::own())
        if (u->getType() == type(T::Pylon) && u->getDistance(home) < 600)
          homePylon = true;
      for (auto &[id, r] : builders)
        if (r.type == type(T::Pylon) &&
            (Position(r.tile) + Position(32, 32)).getApproxDistance(home) < 600)
          homePylon = true;
      if (!homePylon) {
        w.build(T::Pylon);
        if (!w.pending(T::Pylon))
          w.minerals -= 100;
      }
      if (!have(T::Gas)) {
        w.build(T::Gas);
        if (!have(T::Gas))
          w.minerals -= 100;
      }
      if (w.count(T::Gateway) && have(T::Gas) && !have(T::Core)) {
        w.build(T::Core);
        if (!have(T::Core))
          w.minerals -= 200;
      }
    }
    if (frame > 9500 && invaders < 3 && have(T::Nexus) < 2) {
      w.build(T::Nexus);
      if (have(T::Nexus) < 2)
        w.minerals -= 400;
    }
    for (int i = 0; i < 2; i++)
      if (!w.count(T::Core))
        w.train(T::Zealot);
    if (have(T::Probe) < cap)
      w.train(T::Probe);
    if (w.count(T::Core)) {
      if (have(T::Dragoon) >= 2 && !w.rangeStarted()) {
        if (w.minerals >= 150 && w.gas >= 150)
          w.researchRange();
        w.minerals -= 150;
        w.gas -= 150;
      }
      if ((dtSeen || have(T::Dragoon) >= 3) && !have(T::Robo)) {
        w.build(T::Robo);
        if (!have(T::Robo)) {
          w.minerals -= 200;
          w.gas -= 100;
        }
      }
      if (w.count(T::Robo) && !have(T::Observatory)) {
        w.build(T::Observatory);
        if (!have(T::Observatory)) {
          w.minerals -= 50;
          w.gas -= 100;
        }
      }
      if (w.count(T::Observatory) && have(T::Observer) < 2)
        w.train(T::Observer);
      for (int i = 0; i < w.count(T::Gateway); i++)
        if (!w.train(T::Dragoon) && w.minerals > 200)
          w.train(T::Zealot);
    }
    if (dtSeen && !have(T::Observer)) {
      if (!have(T::Forge))
        w.build(T::Forge);
      if (w.count(T::Forge) && have(T::Cannon) < 2)
        w.build(T::Cannon);
    }
    if (w.minerals > 350 && have(T::Gateway) < 4)
      w.build(T::Gateway);
    if (w.count(T::Nexus) > 1 && have(T::Gas) < w.count(T::Nexus))
      w.build(T::Gas);
    if (w.count(T::Nexus) > 1 && w.minerals > 300 && have(T::Gateway) < 8)
      w.build(T::Gateway);
    // Estimate local engagements; reinforcements should not feed a defended choke one at a time.
    if (frame % 24 == 0) {
      std::vector<std::pair<Position, bool>> evaluations;
      for (auto u : army)
        if (u->getType().canAttack()) {
          bool reused = false;
          for (auto &[p, r] : evaluations)
            if (u->getDistance(p) < 128) {
              retreat[u->getID()] = r;
              reused = true;
              break;
            }
          if (reused)
            continue;
          FAP::FastAPproximation<> sim;
          float a0 = 0, b0 = 0;
          int es = 0;
          for (auto a : army)
            if (a->getType().canAttack() && a->getDistance(u) < 480) {
              sim.addIfCombatUnitPlayer1(simUnit(a));
              a0 += a->getHitPoints() + a->getShields();
            }
          for (auto e : foes)
            if (e->isCompleted() && e->isDetected() && e->getType().canAttack() &&
                (!e->getType().isWorker() || e->isAttacking() ||
                 e->getOrder() == Orders::AttackUnit) &&
                e->getDistance(u) < 560) {
              sim.addIfCombatUnitPlayer2(simUnit(e));
              b0 += e->getHitPoints() + e->getShields();
              es++;
            }
          retreat[u->getID()] = false;
          if (es && u->getDistance(home) > 500) {
            for (auto group : {sim.getState().first, sim.getState().second})
              for (auto &v : *group)
                if (v.unitType == type(T::Reaver)) {
                  v.groundMaxRangeSquared = 256 * 256;
                  v.groundCooldown = 60;
                }
            sim.simulate(192);
            auto st = sim.getState();
            float a1 = 0, b1 = 0;
            for (auto &a : *st.first)
              a1 += (a.health + a.shields) / 256.f;
            for (auto &b : *st.second)
              b1 += (b.health + b.shields) / 256.f;
            retreat[u->getID()] = (a1 < a0 * .60f && a0 - a1 > (b0 - b1) * 1.10f) ||
                                  (a1 < a0 * .15f && b1 > b0 * .20f);
          }
          evaluations.emplace_back(u->getPosition(), retreat[u->getID()]);
        }
    }
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
    Position group = home;
    int most = 0;
    for (auto a : army)
      if (a->getType().canAttack()) {
        int n = 0;
        Position sum(0, 0);
        for (auto b : army)
          if (b->getType().canAttack() && a->getDistance(b) < 320) {
            sum += b->getPosition();
            n++;
          }
        if (n > most) {
          most = n;
          group = sum / n;
        }
      }
    storms.erase(
        std::remove_if(storms.begin(), storms.end(), [&](auto &x) { return x.second < frame; }),
        storms.end());
    std::vector<Position> observerTargets;
    std::map<int, double> committed;
    auto damage = [&](Unit a, Unit b) {
      auto weapon = b->isFlying() ? a->getType().airWeapon() : a->getType().groundWeapon();
      double d = a->getPlayer()->damage(weapon);
      if (b->getShields() <= 0) {
        if (weapon.damageType() == DamageTypes::Explosive) {
          if (b->getType().size() == UnitSizeTypes::Small)
            d *= .5;
          else if (b->getType().size() == UnitSizeTypes::Medium)
            d *= .75;
        }
        d -= b->getPlayer()->armor(b->getType());
      }
      return std::max(1.0, d);
    };
    for (auto b : Broodwar->getBullets())
      if (b->exists() && b->getSource() && b->getTarget() &&
          b->getSource()->getPlayer() == Broodwar->self())
        committed[b->getTarget()->getID()] += damage(b->getSource(), b->getTarget());

    for (auto u : army) {
      if (u->getType() == type(T::Observer)) {
        Position op = center;
        double score = -1e9;
        for (auto a : army)
          if (a->getType().canAttack()) {
            double val = 0;
            for (auto v : army)
              if (v->getType().canAttack() && v->getDistance(a) < 350)
                val += 10;
            for (auto e : foes)
              if (e->getType() == type(T::DT) && e->getDistance(a) < 300)
                val += 35;
            for (auto p : observerTargets)
              if (p.getApproxDistance(a->getPosition()) < 300)
                val -= 100;
            val -= u->getDistance(a) * .003;
            if (val > score) {
              score = val;
              op = a->getPosition();
            }
          }
        for (auto e : threats)
          if (e->getType() == type(T::DT)) {
            double val = 90;
            for (auto p : observerTargets)
              if (p.getApproxDistance(e->getPosition()) < 250)
                val -= 150;
            if (val > score) {
              score = val;
              op = e->getPosition();
            }
          }
        observerTargets.push_back(op);
        move(u, op);
        continue;
      }
      if (invaders > 0 && !(World::proxy.isValid() && frame < 6500) &&
          u->getDistance(target) > 650) {
        attackPos(u, target);
        continue;
      }
      if (u->isUnderStorm()) {
        Position p = home;
        int bd = 200;
        for (auto b : Broodwar->getBullets())
          if (b->getType() == BulletTypes::Psionic_Storm && u->getDistance(b->getPosition()) < bd) {
            bd = u->getDistance(b->getPosition());
            auto q = b->getPosition();
            auto v = u->getPosition() - q;
            double d = std::max(1.0, std::hypot(v.x, v.y));
            p = u->getPosition() + Position(int(v.x * 112 / d), int(v.y * 112 / d));
          }
        move(u, p);
        continue;
      }
      if (u->getType() == type(T::HT)) {
        Position cast = Positions::Invalid;
        double bestScore = 1.5;
        if (u->getEnergy() >= 75 && Broodwar->self()->hasResearched(TechTypes::Psionic_Storm))
          for (auto e : foes)
            if (!e->getType().isBuilding() && u->getDistance(e) < 288) {
              Position p = e->getPosition();
              bool overlap = false;
              for (auto &x : storms)
                if (p.getApproxDistance(x.first) < 96)
                  overlap = true;
              if (overlap)
                continue;
              double score = 0;
              for (auto v : foes)
                if (!v->getType().isBuilding() && v->getDistance(p) < 64)
                  score += std::min(112, v->getHitPoints() + v->getShields()) / 70.0;
              for (auto a : army)
                if (a->getDistance(p) < 64)
                  score -= 1.5;
              if (score > bestScore) {
                bestScore = score;
                cast = p;
              }
            }
        if (cast.isValid() && u->useTech(TechTypes::Psionic_Storm, cast)) {
          storms.emplace_back(cast, frame + 72);
          continue;
        }
        if (frame - u->getLastCommandFrame() < 24)
          continue;
        Unit danger = nullptr;
        for (auto e : foes)
          if (e->getType().canAttack() && u->getDistance(e) < 220 &&
              (!danger || u->getDistance(e) < u->getDistance(danger)))
            danger = e;
        if (danger)
          move(u, rally);
        else if (u->getDistance(center) > 120)
          move(u, center);
        continue;
      }
      if (u->isAttackFrame() || u->isStartingAttack())
        continue;
      if (u->getLastCommand().getType() == UnitCommandTypes::Attack_Unit &&
          frame - u->getLastCommandFrame() < 18 &&
          (u->getType() != type(T::Dragoon) || u->getGroundWeaponCooldown() == 0)) {
        auto t = u->getLastCommand().getTarget();
        if (t && t->exists() && t->isVisible() &&
            u->getDistance(t) <= Broodwar->self()->weaponMaxRange(u->getType().groundWeapon()) + 8)
          continue;
      }
      Unit hidden = nullptr;
      for (auto e : foes)
        if (!e->isDetected() && e->getType().canAttack() && u->getDistance(e) < 192) {
          hidden = e;
          break;
        }
      if (hidden) {
        move(u, home);
        continue;
      }
      Unit best = nullptr, close = nullptr;
      double bs = 1e9;
      int range = Broodwar->self()->weaponMaxRange(u->getType().groundWeapon());
      for (auto e : foes) {
        if (!e->isDetected() || e->isInvincible() ||
            (e->isFlying() && u->getType().airWeapon() == WeaponTypes::None))
          continue;
        int d = u->getDistance(e);
        if (d > 400 || (!launched && u->getType() != type(T::DT) && invaders == 0 && d > range))
          continue;
        if (e->getType().isWorker() && d > range + 48 && e->getDistance(home) > 400 &&
            (!enemy.isValid() || e->getDistance(enemy) > 500))
          continue;
        double s = d + (e->getType().isBuilding() && !e->getType().canAttack() ? 180 : 0) +
                   (e->getType().isWorker() ? 30 : 0);
        if (e->getType() == type(T::Reaver) || e->getType() == type(T::HT))
          s -= 80;
        if (e->getType() == type(T::Observer) && w.count(T::DT) > 0 &&
            u->getType() == type(T::Dragoon))
          s -= 240;
        if (u->getType() == type(T::DT)) {
          if (e->getType().isWorker())
            s -= 100;
          if (e->getType() == type(T::Cannon) && w.count(T::DT) >= 3)
            s -= 200;
        }
        if (d <= range + 16)
          s -= 100;
        s += std::max(0.0, e->getHitPoints() + e->getShields() - committed[e->getID()]) * .25;
        if (committed[e->getID()] >= e->getHitPoints() + e->getShields())
          s += 800;
        if (s < bs) {
          bs = s;
          best = e;
        }
        if (e->getType().canAttack() &&
            (!e->getType().isWorker() || e->isAttacking() || e->getOrder() == Orders::AttackUnit) &&
            (!close || d < u->getDistance(close)))
          close = e;
      }
      if (launched && u->getType() != type(T::DT) && most >= 4 && u->getDistance(group) > 280 &&
          u->getDistance(target) > group.getApproxDistance(target) + 128 &&
          (!best || u->getDistance(best) > range + 16)) {
        attackPos(u, group);
        continue;
      }
      if (best) {
        if (Broodwar->getStartLocations().size() > 2 && u->getType() == type(T::Zealot) &&
            u->getShields() < 12 && u->getHitPoints() < 65 && u->getGroundWeaponCooldown() > 5) {
          bool focused = false;
          for (auto e : foes)
            if (e->getOrderTarget() == u && e->getType().canAttack() && e->getDistance(u) < 120)
              focused = true;
          Unit cover = nullptr;
          for (auto a : army)
            if (a != u && a->getType() == type(T::Zealot) &&
                a->getHitPoints() + a->getShields() > 100 && a->getDistance(u) < 160 &&
                (!cover || a->getDistance(u) < cover->getDistance(u)))
              cover = a;
          if (focused && cover && close) {
            auto v = u->getPosition() - close->getPosition();
            double d = std::max(1.0, std::hypot(v.x, v.y));
            auto p = clamp(u->getPosition() + Position(int(v.x * 96 / d), int(v.y * 96 / d)));
            if (World::reachable(TilePosition(p))) {
              move(u, p);
              continue;
            }
          }
        }
        bool exposed = exposedUntil[u->getID()] > frame;
        if (u->getType() == type(T::DT))
          for (auto e : foes)
            if (e->isCompleted() && e->getType().isDetector() && u->getDistance(e) < 350)
              exposed = true;
        if ((u->getType() != type(T::DT) || exposed) && retreat[u->getID()] && close &&
            u->getDistance(close) < 400 &&
            (u->getDistance(rally) > 100 || close->getType().isWorker())) {
          if ((u->getType() == type(T::Dragoon) || u->getType() == type(T::Zealot)) &&
              u->getDistance(best) <= range + 8 && u->getGroundWeaponCooldown() <= 5) {
            attack(u, best);
            continue;
          }
          Position p = rally;
          if (u->getType() == type(T::Dragoon) || close->getType().isWorker()) {
            auto v = u->getPosition() - close->getPosition();
            double d = std::max(1.0, std::hypot(v.x, v.y));
            p = u->getPosition() + Position(int(v.x * 100 / d), int(v.y * 100 / d));
          }
          move(u, p);
          continue;
        }
        if (u->getType() == type(T::Dragoon) && u->getGroundWeaponCooldown() > 5 && close &&
            u->getDistance(close) < range &&
            (close->getPlayer()->weaponMaxRange(close->getType().groundWeapon()) + 16 < range ||
             (u->getHitPoints() < 50 && u->getShields() < 10))) {
          auto p = u->getPosition(), q = close->getPosition();
          double d = std::max(1.0, std::hypot(p.x - q.x, p.y - q.y));
          move(u, p + Position(int((p.x - q.x) * 80 / d), int((p.y - q.y) * 80 / d)));
        } else {
          attack(u, best);
          if (u->getGroundWeaponCooldown() < 8 && u->getDistance(best) < range + 16)
            committed[best->getID()] += damage(u, best);
        }
      } else if (invaders > 0)
        attackPos(u, target);
      else if (launched || u->getType() == type(T::DT))
        attackPos(u, target);
      else {
        Position hold = rally;
        auto dir = target - rally;
        double len = std::max(1.0, std::hypot(dir.x, dir.y));
        std::vector<Unit> line;
        for (auto a : army)
          if (a->getType() == type(T::Dragoon))
            line.push_back(a);
        auto it = std::find(line.begin(), line.end(), u);
        int index = int(it - line.begin());
        int off = ((index % 5) - std::min(4, int(line.size()) - 1) / 2) * 44;
        if (u->getType() == type(T::Dragoon))
          hold = clamp(rally + Position(int(-dir.y / len * off - dir.x / len * (index / 5) * 44),
                                        int(dir.x / len * off - dir.y / len * (index / 5) * 44)));
        if (!Broodwar->isWalkable(WalkPosition(hold)))
          hold = rally;
        if (u->getDistance(hold) > 32)
          move(u, hold);
        else if (u->getOrder() != Orders::HoldPosition)
          u->holdPosition();
      }
    }
    if (frame % 1440 == 0)
      std::printf("t=%d p=%d z=%d d=%d gates=%d minerals=%d gas=%d target=%d,%d\n", frame,
                  have(T::Probe), zealots, dragoons, have(T::Gateway), w.minerals, w.gas, target.x,
                  target.y);
  }
};
BWAPI::AIModule *makeCandidateBot() { return new Candidate(); }

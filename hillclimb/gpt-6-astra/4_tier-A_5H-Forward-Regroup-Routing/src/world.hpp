#pragma once
#include "types.hpp"
#include <BWAPI.h>
#include <algorithm>
#include <bwem.h>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>
namespace bw_demo {
using namespace BWAPI;
using T = demos::Type;
inline UnitType type(T t) {
  static const UnitType types[] = {UnitTypes::Protoss_Probe,
                                   UnitTypes::Protoss_Zealot,
                                   UnitTypes::Protoss_Dragoon,
                                   UnitTypes::Protoss_Dark_Templar,
                                   UnitTypes::Protoss_Pylon,
                                   UnitTypes::Protoss_Gateway,
                                   UnitTypes::Protoss_Assimilator,
                                   UnitTypes::Protoss_Cybernetics_Core,
                                   UnitTypes::Protoss_Citadel_of_Adun,
                                   UnitTypes::Protoss_Templar_Archives,
                                   UnitTypes::Protoss_Nexus,
                                   UnitTypes::Protoss_Forge,
                                   UnitTypes::Protoss_Photon_Cannon,
                                   UnitTypes::Protoss_Robotics_Facility,
                                   UnitTypes::Protoss_Observatory,
                                   UnitTypes::Protoss_Observer,
                                   UnitTypes::Protoss_High_Templar,
                                   UnitTypes::Protoss_Archon,
                                   UnitTypes::Protoss_Robotics_Support_Bay,
                                   UnitTypes::Protoss_Reaver};
  return types[int(t)];
}
struct Reservation {
  UnitType type;
  TilePosition tile;
  int frame;
};
struct World {
  static std::vector<Unit> ordered(const Unitset &units) {
    std::vector<Unit> v(units.begin(), units.end());
    std::sort(v.begin(), v.end(), [](Unit a, Unit b) { return a->getID() < b->getID(); });
    return v;
  }
  static const std::vector<Unit> &own() {
    static int f = -1;
    static std::vector<Unit> v;
    if (f != Broodwar->getFrameCount()) {
      f = Broodwar->getFrameCount();
      v = ordered(Broodwar->self()->getUnits());
    }
    return v;
  }

  inline static bool homePowerRequest = false;
  inline static bool hiddenProxy = false;
  inline static Position proxyEnemy = Positions::Invalid;
  inline static std::vector<std::pair<Position, Position>> scoutCorridors;
  static double corridorDistance(Position p) {
    double best = 1e9;
    for (auto &[a, b] : scoutCorridors) {
      double dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy,
             t = l ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l, 0.0, 1.0) : 0;
      best = std::min(best, std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy));
    }
    return best;
  }
  inline static std::vector<std::pair<UnitType, TilePosition>> reservedTiles;
  inline static TilePosition proxy = TilePositions::Invalid;
  inline static Unit forwardWorker = nullptr;
  inline static std::vector<Position> danger;
  inline static std::map<std::pair<int, int>, int> failedSites;
  inline static std::vector<unsigned char> walkOpen;
  static bool reachable(TilePosition t) {
    if (!t.isValid())
      return false;
    return groundDistance()[t.x + t.y * Broodwar->mapWidth()] >= 0;
  }
  std::map<int, Reservation> &builders;
  std::set<int> &militia;
  int minerals = Broodwar->self()->minerals(), gas = Broodwar->self()->gas(),
      supply = Broodwar->self()->supplyTotal() - Broodwar->self()->supplyUsed();
  std::map<T, int> queued;
  std::map<int, bool> busy;
  static bool homeGas(Unit u) {
    for (auto n : own())
      if (n->getType().isResourceDepot() && u->getDistance(n) < 400)
        return true;
    return false;
  }
  int count(T t) {
    if (t == T::Gas) {
      int n = 0;
      for (auto u : own())
        if (u->getType() == type(t) && u->isCompleted() && homeGas(u))
          n++;
      return n;
    }
    return Broodwar->self()->completedUnitCount(type(t));
  }
  int pending(T t) {
    int n = Broodwar->self()->incompleteUnitCount(type(t)) + queued[t];
    if (t == T::Gas) {
      n = queued[t];
      for (auto u : own())
        if (u->getType() == type(t) && !u->isCompleted() && homeGas(u))
          n++;
    }
    for (auto &[id, r] : builders)
      if (r.type == type(t))
        ++n;
    // The currently trained unit is already included in incompleteUnitCount.
    // This bot queues one regular unit per producer, so only this-frame orders need adding.
    return n;
  }
  int supplyLeft() { return supply / 2; }
  bool rangeDone() {
    return Broodwar->self()->getUpgradeLevel(UpgradeTypes::Singularity_Charge) > 0;
  }
  bool rangeStarted() {
    return rangeDone() || Broodwar->self()->isUpgrading(UpgradeTypes::Singularity_Charge);
  }
  void researchRange() {
    auto t = UpgradeTypes::Singularity_Charge;
    if (minerals < t.mineralPrice() || gas < t.gasPrice())
      return;
    for (auto b : own())
      if (b->getType() == UnitTypes::Protoss_Cybernetics_Core && b->isCompleted() && b->upgrade(t))
        return;
  }
  bool train(T t) {
    auto ut = type(t);
    if (minerals < ut.mineralPrice() || gas < ut.gasPrice() || supply < ut.supplyRequired())
      return false;
    std::vector<Unit> producers;
    for (auto b : own())
      producers.push_back(b);
    if (t == T::DT && proxy.isValid())
      std::sort(producers.begin(), producers.end(), [&](Unit a, Unit b) {
        return a->getDistance(Position(proxy)) < b->getDistance(Position(proxy));
      });
    for (auto b : producers) {
      if (b->getType() != ut.whatBuilds().first || !b->isCompleted() || b->isTraining() ||
          busy[b->getID()])
        continue;
      if (b->train(ut)) {
        minerals -= ut.mineralPrice();
        gas -= ut.gasPrice();
        supply -= ut.supplyRequired();
        ++queued[t];
        busy[b->getID()] = true;
        return true;
      }
    }
    return false;
  }
  // Placement follows UAlbertaBot: search outward for a site
  // instead of asking BWAPI for one. canBuildHere checks power, creep and unit
  // collision boxes, but a building's collision box is smaller than its
  // footprint, so every footprint tile must also be free. A one-tile gap keeps
  // buildings from walling in the base, and nothing goes between the Nexus
  // and its minerals.
  struct Box {
    int left, top, right, bottom;
  };
  static bool fits(UnitType ut, TilePosition tile, int gap, const Box &box) {
    if (!tile.isValid() ||
        !reachable(tile + TilePosition(ut.tileWidth() / 2, ut.tileHeight() / 2)) ||
        failedSites[{tile.x, tile.y}] > Broodwar->getFrameCount() ||
        !Broodwar->canBuildHere(tile, ut))
      return false;
    for (auto &[t, p] : reservedTiles)
      if (tile.x < p.x + t.tileWidth() + gap && tile.x + ut.tileWidth() + gap > p.x &&
          tile.y < p.y + t.tileHeight() + gap && tile.y + ut.tileHeight() + gap > p.y)
        return false;
    const int right = tile.x + ut.tileWidth();
    const int bottom = tile.y + ut.tileHeight();
    if (tile.x < box.right && right > box.left && tile.y < box.bottom && bottom > box.top)
      return false;
    for (int x = tile.x - gap; x < right + gap; ++x)
      for (int y = tile.y - gap; y < bottom + gap; ++y)
        if (!Broodwar->isBuildable(x, y, true))
          return false;
    return true;
  }
  static Box mineralLine() {
    auto home = Broodwar->self()->getStartLocation();
    auto centre = Position(home) + Position(64, 48);
    Box box{home.x, home.y, home.x + 4, home.y + 3};
    auto add = [&](Unit r) {
      if (r->getInitialPosition().getApproxDistance(centre) > 320)
        return;
      auto t = r->getInitialTilePosition();
      auto type = r->getInitialType();
      box.left = std::min(box.left, t.x);
      box.top = std::min(box.top, t.y);
      box.right = std::max(box.right, t.x + type.tileWidth());
      box.bottom = std::max(box.bottom, t.y + type.tileHeight());
    };
    for (auto m : Broodwar->getStaticMinerals())
      add(m);
    for (auto g : Broodwar->getStaticGeysers())
      add(g);
    return box;
  }
  // The nearest free geyser beside one of our Nexuses.
  static TilePosition geyser(UnitType ut) {
    TilePosition best = TilePositions::Invalid;
    int distance = 1000000;
    for (auto g : Broodwar->getGeysers())
      for (auto n : own()) {
        if (n->getType() != UnitTypes::Protoss_Nexus || !n->isCompleted())
          continue;
        int d = n->getDistance(g);
        if (d < 320 && d < distance && Broodwar->canBuildHere(g->getTilePosition(), ut)) {
          distance = d;
          best = g->getTilePosition();
        }
      }
    return best;
  }
  // A base is a line of adjacent mineral fields plus any geyser beside it.
  static std::vector<std::vector<Position>> bases() {
    std::vector<std::vector<Position>> bases;
    for (auto m : Broodwar->getStaticMinerals()) {
      if (m->getInitialResources() <= 50)
        continue;
      auto p = m->getInitialPosition();
      std::vector<Position> merged{p};
      for (auto b = bases.begin(); b != bases.end();) {
        bool near = false;
        for (auto q : *b)
          near = near || q.getApproxDistance(p) < 128;
        if (near) {
          merged.insert(merged.end(), b->begin(), b->end());
          b = bases.erase(b);
        } else
          ++b;
      }
      bases.push_back(merged);
    }
    for (auto g : Broodwar->getStaticGeysers()) {
      auto p = g->getInitialPosition();
      std::vector<Position> *nearest = nullptr;
      int distance = 320;
      for (auto &b : bases)
        for (auto q : b)
          if (q.getApproxDistance(p) < distance) {
            distance = q.getApproxDistance(p);
            nearest = &b;
          }
      if (nearest)
        nearest->push_back(p);
    }
    return bases;
  }
  // Ground distance in tiles from the main, by flood fill over tiles that
  // are mostly walkable and not covered by resources. Unreachable tiles
  // keep -1.
  static const std::vector<int> &groundDistance() {
    static std::vector<int> distance;
    static std::string key;
    int w = Broodwar->mapWidth(), h = Broodwar->mapHeight(), ww = w * 4, hh = h * 4;
    auto home = Broodwar->self()->getStartLocation();
    auto k = Broodwar->mapHash() + std::to_string(home.x) + "," + std::to_string(home.y);
    if (key == k)
      return distance;
    key = k;
    std::vector<unsigned char> raw(ww * hh), open(ww * hh);
    for (int y = 0; y < hh; y++)
      for (int x = 0; x < ww; x++)
        raw[x + y * ww] = Broodwar->isWalkable(x, y);
    auto block = [&](Unit u) {
      auto t = u->getInitialTilePosition();
      auto type = u->getInitialType();
      if (!type.isBuilding() && !type.isMineralField() && !type.isRefinery() &&
          type != UnitTypes::Resource_Vespene_Geyser)
        return;
      for (int y = std::max(0, t.y * 4); y < std::min(hh, (t.y + type.tileHeight()) * 4); y++)
        for (int x = std::max(0, t.x * 4); x < std::min(ww, (t.x + type.tileWidth()) * 4); x++)
          raw[x + y * ww] = 0;
    };
    for (auto u : Broodwar->getStaticNeutralUnits())
      block(u);
    for (auto u : Broodwar->getStaticMinerals())
      block(u);
    for (auto u : Broodwar->getStaticGeysers())
      block(u);
    for (int y = 2; y < hh - 2; y++)
      for (int x = 2; x < ww - 2; x++) {
        bool good = true;
        for (int dy = -2; dy <= 2; dy++)
          for (int dx = -2; dx <= 2; dx++)
            good = good && raw[x + dx + (y + dy) * ww];
        open[x + y * ww] = good;
      }
    walkOpen = open;
    std::vector<int> steps(ww * hh, -1), queue;
    queue.reserve(ww * hh);
    for (int y = home.y * 4; y < (home.y + 3) * 4; y++)
      for (int x = home.x * 4; x < (home.x + 4) * 4; x++)
        if (open[x + y * ww]) {
          steps[x + y * ww] = 0;
          queue.push_back(x + y * ww);
        }
    for (size_t i = 0; i < queue.size(); i++) {
      int c = queue[i], x = c % ww, y = c / ww;
      for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
        int nx = x + dx, ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= ww || ny >= hh)
          continue;
        int n = nx + ny * ww;
        if (open[n] && steps[n] < 0) {
          steps[n] = steps[c] + 1;
          queue.push_back(n);
        }
      }
    }
    distance.assign(w * h, -1);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        int best = -1;
        for (int dy = 0; dy < 4; dy++)
          for (int dx = 0; dx < 4; dx++) {
            int d = steps[x * 4 + dx + (y * 4 + dy) * ww];
            if (d >= 0 && (best < 0 || d < best))
              best = d;
          }
        if (best >= 0)
          distance[x + y * w] = best;
      }
    return distance;
  }
  static const std::vector<int> &distanceFrom(TilePosition start) {
    groundDistance();
    int w = Broodwar->mapWidth(), h = Broodwar->mapHeight(), ww = w * 4, hh = h * 4;
    static std::map<std::string, std::vector<int>> cache;
    auto key = Broodwar->mapHash() + std::to_string(start.x) + "," + std::to_string(start.y);
    if (cache.count(key))
      return cache[key];
    auto &out = cache[key];
    out.assign(w * h, -1);
    std::vector<int> d(ww * hh, -1), q;
    q.reserve(ww * hh);
    for (int y = start.y * 4; y < (start.y + 3) * 4; y++)
      for (int x = start.x * 4; x < (start.x + 4) * 4; x++)
        if (x >= 0 && y >= 0 && x < ww && y < hh && walkOpen[x + y * ww]) {
          d[x + y * ww] = 0;
          q.push_back(x + y * ww);
        }
    for (size_t i = 0; i < q.size(); i++) {
      int c = q[i], x = c % ww, y = c / ww, t = x / 4 + (y / 4) * w;
      if (out[t] < 0 || d[c] * 8 < out[t])
        out[t] = d[c] * 8;
      for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
        int nx = x + dx, ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= ww || ny >= hh)
          continue;
        int n = nx + ny * ww;
        if (walkOpen[n] && d[n] < 0) {
          d[n] = d[c] + 1;
          q.push_back(n);
        }
      }
    }
    return out;
  }
  static bool clearGroundLine(Position a, Position b) {
    groundDistance();
    int ww = Broodwar->mapWidth() * 4, hh = Broodwar->mapHeight() * 4;
    int n = std::max(abs(a.x - b.x), abs(a.y - b.y)) / 8;
    for (int i = 3; i < n - 3; i++) {
      auto p = a + (b - a) * i / std::max(1, n);
      auto t = WalkPosition(p);
      if (t.x < 0 || t.y < 0 || t.x >= ww || t.y >= hh || !walkOpen[t.x + t.y * ww])
        return false;
    }
    return true;
  }
  static Position waypoint(Position from, Position to, bool combat = false) {
    groundDistance();
    int ww = Broodwar->mapWidth() * 4, hh = Broodwar->mapHeight() * 4;
    static Position goal = Positions::Invalid;
    static std::vector<int> d;
    static int stamp = -1;
    static bool mode = false;
    int currentStamp = 0;
    for (auto u : Broodwar->getAllUnits())
      if (u->exists() && u->getType().isBuilding())
        currentStamp += u->getID() * 7 + u->getTilePosition().x * 31 + u->getTilePosition().y * 131;
    if (goal != to || stamp != currentStamp || mode != combat) {
      mode = combat;
      stamp = currentStamp;
      auto open = walkOpen;
      for (auto u : Broodwar->getAllUnits())
        if (u->exists() && u->getType().isBuilding()) {
          int l = std::max(0, (u->getLeft() - 12) / 8),
              r = std::min(ww - 1, (u->getRight() + 12) / 8),
              t = std::max(0, (u->getTop() - 12) / 8),
              b = std::min(hh - 1, (u->getBottom() + 12) / 8);
          for (int y = t; y <= b; y++)
            for (int x = l; x <= r; x++)
              open[x + y * ww] = 0;
        }
      goal = to;
      d.assign(ww * hh, -1);
      std::vector<int> queue;
      auto g = WalkPosition(to);
      int best = -1, score = 999999;
      for (int dy = -8; dy <= 8; dy++)
        for (int dx = -8; dx <= 8; dx++) {
          int x = g.x + dx, y = g.y + dy;
          if (x < 0 || y < 0 || x >= ww || y >= hh || !open[x + y * ww])
            continue;
          int s = dx * dx + dy * dy;
          if (s < score) {
            score = s;
            best = x + y * ww;
          }
        }
      if (combat) {
        for (int dy = -12; dy <= 12; dy++)
          for (int dx = -12; dx <= 12; dx++) {
            int x = g.x + dx, y = g.y + dy;
            if (x >= 0 && y >= 0 && x < ww && y < hh && open[x + y * ww] &&
                dx * dx + dy * dy <= 144) {
              d[x + y * ww] = 0;
              queue.push_back(x + y * ww);
            }
          }
      } else if (best >= 0) {
        d[best] = 0;
        queue.push_back(best);
      }
      for (size_t i = 0; i < queue.size(); i++) {
        int c = queue[i], x = c % ww, y = c / ww;
        for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
          int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= ww || ny >= hh)
            continue;
          int n = nx + ny * ww;
          if (open[n] && d[n] < 0) {
            d[n] = d[c] + 1;
            queue.push_back(n);
          }
        }
      }
    }
    auto f = WalkPosition(from);
    int c = -1, score = 999999;
    for (int dy = -8; dy <= 8; dy++)
      for (int dx = -8; dx <= 8; dx++) {
        int x = f.x + dx, y = f.y + dy;
        if (x < 0 || y < 0 || x >= ww || y >= hh || d[x + y * ww] < 0)
          continue;
        int s = 20 * (dx * dx + dy * dy) + d[x + y * ww];
        if (s < score) {
          score = s;
          c = x + y * ww;
        }
      }
    if (c < 0)
      return to;
    for (int i = 0; i < 20; i++) {
      int x = c % ww, y = c / ww, next = c;
      for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
        int nx = x + dx, ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= ww || ny >= hh)
          continue;
        int n = nx + ny * ww;
        if (d[n] >= 0 && d[n] < d[next])
          next = n;
      }
      if (next == c)
        break;
      c = next;
    }
    if (combat && d[c] == 0)
      return to;
    return Position((c % ww) * 8 + 4, (c / ww) * 8 + 4);
  }
  // The expansion is the unoccupied base nearest by ground with a site that
  // canBuildHere accepts (which keeps the resource-distance rule), placed as
  // close as possible to its resources.
  static TilePosition expansion(UnitType ut) {
    const auto bases = World::bases();
    const auto &ground = groundDistance();
    std::vector<std::pair<int, size_t>> order;
    for (size_t i = 0; i < bases.size(); ++i) {
      auto &base = bases[i];
      if (base.size() < 5)
        continue;
      Position sum(0, 0);
      for (auto r : base)
        sum += r;
      auto centre = sum / int(base.size());
      bool occupied = false;
      for (auto p : danger)
        if (p.getApproxDistance(centre) < 550)
          occupied = true;
      for (auto u : Broodwar->getAllUnits())
        if (u->getType().isResourceDepot() && u->getDistance(centre) < 300)
          occupied = true;
      // Resources are closed tiles, so measure from the nearest open
      // tile to the centre.
      int d = -1;
      auto c = TilePosition(centre);
      for (int r = 0; r <= 4 && d < 0; ++r)
        for (int dx = -r; dx <= r; ++dx)
          for (int dy = -r; dy <= r; ++dy) {
            auto t = c + TilePosition(dx, dy);
            if (t.isValid()) {
              int g = ground[t.x + t.y * Broodwar->mapWidth()];
              if (g >= 0 && (d < 0 || g < d))
                d = g;
            }
          }
      if (!occupied && d >= 0)
        order.emplace_back(d, i);
    }
    std::sort(order.begin(), order.end(), [](auto &a, auto &b) { return a.first < b.first; });
    const Box none{0, 0, 0, 0};
    for (auto &[d, i] : order) {
      auto &resources = bases[i];
      Position sum(0, 0);
      for (auto r : resources)
        sum += r;
      auto centre = sum / int(resources.size());
      TilePosition best = TilePositions::Invalid;
      int score = 1000000000;
      for (int dx = -12; dx <= 12; ++dx)
        for (int dy = -12; dy <= 12; ++dy) {
          auto tile = TilePosition(centre) + TilePosition(dx - 2, dy - 1);
          if (!fits(ut, tile, 0, none))
            continue;
          auto middle = Position(tile) + Position(64, 48);
          int s = 0;
          for (auto r : resources)
            s += middle.getApproxDistance(r);
          if (s < score) {
            score = s;
            best = tile;
          }
        }
      if (best.isValid())
        return best;
    }
    return TilePositions::Invalid;
  }
  static TilePosition site(UnitType ut) {
    if (ut.isRefinery())
      return geyser(ut);
    if (ut.isResourceDepot())
      return expansion(ut);
    auto start = Broodwar->self()->getStartLocation();
    auto box = mineralLine();
    if (!homePowerRequest && proxy.isValid() && Broodwar->getFrameCount() < 6000 &&
        (ut == UnitTypes::Protoss_Pylon || ut == UnitTypes::Protoss_Gateway))
      start = proxy;
    if (ut == UnitTypes::Protoss_Photon_Cannon) {
      Position sum(0, 0);
      int n = 0;
      Position home = Position(Broodwar->self()->getStartLocation()) + Position(64, 48);
      for (auto m : Broodwar->getStaticMinerals())
        if (m->getInitialPosition().getApproxDistance(home) < 320) {
          sum += m->getInitialPosition();
          n++;
        }
      if (n) {
        start = TilePosition((home + sum / n) / 2);
        box = {0, 0, 0, 0};
      }
    }
    // Keep the gap if any site allows it; cramped mains fall back to none.
    for (int gap = 1; gap >= 0; --gap)
      for (int r = 0; r <= 24; ++r)
        for (int dx = -r; dx <= r; ++dx)
          for (int dy = -r; dy <= r; ++dy) {
            auto tile = start + TilePosition(dx, dy);
            if ((std::abs(dx) == r || std::abs(dy) == r) && fits(ut, tile, gap, box))
              return tile;
          }
    return TilePositions::Invalid;
  }
  bool build(T t) {
    if (t == T::Pylon && Broodwar->self()->supplyTotal() >= 400)
      return false;
    auto ut = type(t);
    if (builders.size() >= 3 || minerals < ut.mineralPrice() || gas < ut.gasPrice())
      return false;
    std::vector<Unit> workers;
    for (auto u : own())
      if (u->getType() == UnitTypes::Protoss_Probe && !builders.count(u->getID()) &&
          !u->isConstructing() && !u->isGatheringGas() && u->getOrder() != Orders::MiningMinerals &&
          (!militia.count(u->getID()) || u == forwardWorker))
        workers.push_back(u);
    if (workers.empty())
      return false;
    auto tile = site(ut);
    if (!tile.isValid())
      return false;
    Position center = Position(tile) + Position(ut.tileWidth() * 16, ut.tileHeight() * 16);
    std::sort(workers.begin(), workers.end(),
              [&](Unit a, Unit b) { return a->getDistance(center) < b->getDistance(center); });
    for (auto u : workers)
      if (u->hasPath(center) && u->stop()) {
        minerals -= ut.mineralPrice();
        gas -= ut.gasPrice();
        builders.emplace(u->getID(), Reservation{ut, tile, Broodwar->getFrameCount()});
        reservedTiles.emplace_back(ut, tile);
        return true;
      }

    return false;
  }
};

} // namespace bw_demo

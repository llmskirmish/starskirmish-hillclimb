#pragma once
#include "types.hpp"
#include <BWAPI.h>
#include <algorithm>
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
 UnitTypes::Protoss_Robotics_Facility, UnitTypes::Protoss_Observatory, UnitTypes::Protoss_Observer, UnitTypes::Protoss_High_Templar, UnitTypes::Protoss_Archon, UnitTypes::Protoss_Robotics_Support_Bay, UnitTypes::Protoss_Reaver};
    return types[int(t)];
}
struct Reservation {
    UnitType type;
    TilePosition tile;
    int frame;
};
struct World {
    std::map<int, Reservation> &builders;
    std::set<int> &militia;
    int minerals = Broodwar->self()->minerals(), gas = Broodwar->self()->gas(),
        supply =
            Broodwar->self()->supplyTotal() - Broodwar->self()->supplyUsed();
    std::map<T, int> queued;
    std::map<int, bool> busy;
    int count(T t) { return Broodwar->self()->completedUnitCount(type(t)); }
    int pending(T t) {
        int n = Broodwar->self()->incompleteUnitCount(type(t)) + queued[t];
        for (auto &[id, r] : builders)
            if (r.type == type(t))
                ++n;
        for (auto u : Broodwar->self()->getUnits()) {
            // Probes can retain stale construction entries after returning to mining.
            // Only producer training queues represent pending trained units.
            if (!u->getType().canProduce())
                continue;
            for (auto q : u->getTrainingQueue())
                if (q == type(t))
                    ++n;
        }
        return n;
    }
    int supplyLeft() { return supply / 2; }
    bool rangeDone() {
        return Broodwar->self()->getUpgradeLevel(
                   UpgradeTypes::Singularity_Charge) > 0;
    }
    bool rangeStarted() {
        return rangeDone() ||
               Broodwar->self()->isUpgrading(UpgradeTypes::Singularity_Charge);
    }
    void researchRange() {
        auto t = UpgradeTypes::Singularity_Charge;
        if (minerals < t.mineralPrice() || gas < t.gasPrice())
            return;
        for (auto b : Broodwar->self()->getUnits())
            if (b->getType() == UnitTypes::Protoss_Cybernetics_Core &&
                b->isCompleted() && b->upgrade(t))
                return;
    }
    bool train(T t) {
        auto ut = type(t);
        if (minerals < ut.mineralPrice() || gas < ut.gasPrice() ||
            supply < ut.supplyRequired())
            return false;
        for (auto b : Broodwar->self()->getUnits()) {
            if (b->getType() != ut.whatBuilds().first ||
                !b->isCompleted() || b->isTraining() || busy[b->getID()])
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
        if (!tile.isValid() || !Broodwar->canBuildHere(tile, ut))
            return false;
        const int right = tile.x + ut.tileWidth();
        const int bottom = tile.y + ut.tileHeight();
        if (tile.x < box.right && right > box.left && tile.y < box.bottom &&
            bottom > box.top)
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
            for (auto n : Broodwar->self()->getUnits()) {
                if (n->getType() != UnitTypes::Protoss_Nexus ||
                    !n->isCompleted())
                    continue;
                int d = n->getDistance(g);
                if (d < 320 && d < distance &&
                    Broodwar->canBuildHere(g->getTilePosition(), ut)) {
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
        const int w = Broodwar->mapWidth(), h = Broodwar->mapHeight();
        auto home = Broodwar->self()->getStartLocation();
        auto k = Broodwar->mapHash() + std::to_string(home.x) + "," +
                 std::to_string(home.y);
        if (key == k)
            return distance;
        key = k;
        std::vector<bool> open(w * h);
        for (int x = 0; x < w; ++x)
            for (int y = 0; y < h; ++y) {
                int walkable = 0;
                for (int i = 0; i < 4; ++i)
                    for (int j = 0; j < 4; ++j)
                        walkable += Broodwar->isWalkable(x * 4 + i, y * 4 + j);
                open[x + y * w] = walkable >= 8;
            }
        auto block = [&](Unit r) {
            auto t = r->getInitialTilePosition();
            auto type = r->getInitialType();
            for (int x = t.x; x < t.x + type.tileWidth(); ++x)
                for (int y = t.y; y < t.y + type.tileHeight(); ++y)
                    if (x >= 0 && y >= 0 && x < w && y < h)
                        open[x + y * w] = false;
        };
        for (auto m : Broodwar->getStaticMinerals())
            block(m);
        for (auto g : Broodwar->getStaticGeysers())
            block(g);
        distance.assign(w * h, -1);
        std::vector<int> queue;
        for (int x = home.x; x < home.x + 4; ++x)
            for (int y = home.y; y < home.y + 3; ++y) {
                distance[x + y * w] = 0;
                queue.push_back(x + y * w);
            }
        for (size_t i = 0; i < queue.size(); ++i) {
            int x = queue[i] % w, y = queue[i] / w;
            for (auto [dx, dy] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
                int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h ||
                    !open[nx + ny * w] || distance[nx + ny * w] >= 0)
                    continue;
                distance[nx + ny * w] = distance[queue[i]] + 1;
                queue.push_back(nx + ny * w);
            }
        }
        return distance;
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
            for (auto u : Broodwar->getAllUnits())
                if (u->getType().isResourceDepot() &&
                    u->getDistance(centre) < 300)
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
        std::sort(order.begin(), order.end(),
                  [](auto &a, auto &b) { return a.first < b.first; });
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
                    auto tile =
                        TilePosition(centre) + TilePosition(dx - 2, dy - 1);
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
        const auto box = mineralLine();
        // Keep the gap if any site allows it; cramped mains fall back to none.
        for (int gap = 1; gap >= 0; --gap)
            for (int r = 0; r <= 24; ++r)
                for (int dx = -r; dx <= r; ++dx)
                    for (int dy = -r; dy <= r; ++dy) {
                        auto tile = start + TilePosition(dx, dy);
                        if ((std::abs(dx) == r || std::abs(dy) == r) &&
                            fits(ut, tile, gap, box))
                            return tile;
                    }
        return TilePositions::Invalid;
    }
    bool build(T t) {
        if (t == T::Pylon && Broodwar->self()->supplyTotal() >= 400)
            return false;
        auto ut = type(t);
        if (!builders.empty() || minerals < ut.mineralPrice() ||
            gas < ut.gasPrice())
            return false;
        // The site search is costly, so it runs once, and only when a
        // Probe is free to take the order.
        TilePosition tile;
        bool searched = false;
        for (auto u : Broodwar->self()->getUnits()) {
            // A Probe mid-harvest drops a build command and resumes mining.
            if (u->getType() != UnitTypes::Protoss_Probe ||
                u->isConstructing() || u->isGatheringGas() ||
                u->getOrder() == Orders::MiningMinerals ||
                militia.count(u->getID()))
                continue;
            if (!searched) {
                tile = site(ut);
                searched = true;
            }
            if (!tile.isValid()) {if(Broodwar->getFrameCount()%240==0)std::printf("BUILD no site type=%s m=%d g=%d\n",ut.c_str(),minerals,gas);return false;}
            if (u->stop()) {
                minerals -= ut.mineralPrice();
                gas -= ut.gasPrice();
                builders.emplace(
                    u->getID(),
                    Reservation{ut, tile, Broodwar->getFrameCount()});
                return true;
            }
            if(Broodwar->getFrameCount()%240==0)std::printf("BUILD failed type=%s site=%d,%d worker=%d error=%s\n",ut.c_str(),tile.x,tile.y,u->getID(),Broodwar->getLastError().c_str());
        }
        return false;
    }
};

}

#include "common.h"

namespace Placement {

struct Box {
    int l, t, r, b;
    bool hit(TilePosition p, UnitType ut) const {
        return p.x < r && p.x + ut.tileWidth() > l && p.y < b && p.y + ut.tileHeight() > t;
    }
};

static Box mineralBox(const BaseLoc &base) {
    Box box{base.tile.x, base.tile.y, base.tile.x + 4, base.tile.y + 3};
    auto add = [&](TilePosition t, UnitType type) {
        box.l = std::min(box.l, t.x);
        box.t = std::min(box.t, t.y);
        box.r = std::max(box.r, t.x + type.tileWidth());
        box.b = std::max(box.b, t.y + type.tileHeight());
    };
    for (auto m : base.bw->Minerals()) add(m->TopLeft(), UT::Resource_Mineral_Field);
    for (auto g : base.bw->Geysers()) add(g->TopLeft(), UT::Resource_Vespene_Geyser);
    return box;
}

static std::vector<Box> forbidden() {
    std::vector<Box> v;
    auto m = MapInfo::mainBase();
    if (m) v.push_back(mineralBox(*m));
    auto n = MapInfo::natBase();
    if (n) {
        v.push_back(mineralBox(*n));
        v.push_back(Box{n->tile.x - 1, n->tile.y - 1, n->tile.x + 5, n->tile.y + 4});
    }
    for (auto &b : MapInfo::bases) {
        if (&b == m || &b == n) continue;
        for (auto u : Self()->getUnits())
            if (u->getType().isResourceDepot() && u->getTilePosition() == b.tile) v.push_back(mineralBox(b));
    }
    return v;
}

static bool completedPylonPowers(TilePosition tile, UnitType ut) {
    Position c = Position(tile) + Position(ut.tileWidth() * 16, ut.tileHeight() * 16);
    for (auto p : Self()->getUnits())
        if (p->getType() == UT::Protoss_Pylon && p->isCompleted()) {
            int dx = std::abs(c.x - p->getPosition().x), dy = std::abs(c.y - p->getPosition().y);
            if (dx <= 224 && dy <= 128) return true;
        }
    return false;
}

static bool fitsBasic(UnitType ut, TilePosition tile, int gap) {
    if (!tile.isValid()) return false;
    int w = Broodwar->mapWidth(), h = Broodwar->mapHeight();
    if (tile.x < 0 || tile.y < 0 || tile.x + ut.tileWidth() > w || tile.y + ut.tileHeight() > h) return false;
    for (int x = tile.x - gap; x < tile.x + ut.tileWidth() + gap; ++x)
        for (int y = tile.y - gap; y < tile.y + ut.tileHeight() + gap; ++y) {
            if (x < 0 || y < 0 || x >= w || y >= h) return false;
            if (!Broodwar->isBuildable(x, y, true)) return false;
        }
    return true;
}

static std::vector<Box> cachedForbidden;
static int cachedForbiddenFrame = -1;

bool goodSpot(UnitType ut, TilePosition tile, int gap, bool inLine) {
    if (!fitsBasic(ut, tile, gap)) return false;
    if (cachedForbiddenFrame != Now()) cachedForbidden = forbidden(), cachedForbiddenFrame = Now();
    if (!inLine)
        for (auto &b : cachedForbidden)
            if (b.hit(tile, ut)) return false;
    Position c = Position(tile) + Position(ut.tileWidth() * 16, ut.tileHeight() * 16);
    if (MapInfo::mainChoke.isValid() && c.getDistance(MapInfo::mainChoke) < 32 * 4) return false;
    if (MapInfo::natChoke.isValid() && MapInfo::myNaturalTaken() && c.getDistance(MapInfo::natChoke) < 32 * 3)
        return false;
    if (!Broodwar->canBuildHere(tile, ut)) return false;
    if (ut.requiresPsi() && !completedPylonPowers(tile, ut)) return false;
    return true;
}

int powerScore(TilePosition t) { return 0; }

static TilePosition findPylon(Position near) {
    auto mb = MapInfo::mainBase();
    if (!mb) return TilePositions::None;
    Position nexus = mb->center;
    bool anchored = near.isValid();
    Position anchor = anchored ? near : nexus;
    int pylons = Self()->allUnitCount(UT::Protoss_Pylon);
    std::vector<Position> existing;
    for (auto u : Self()->getUnits())
        if (u->getType() == UT::Protoss_Pylon) existing.push_back(u->getPosition());
    auto homeArea = mb->area;
    auto natArea = MapInfo::natBase() ? MapInfo::natBase()->area : nullptr;
    bool natOk = MapInfo::myNaturalTaken();
    TilePosition at(anchor);
    int R = anchored ? 7 : 22;
    std::vector<std::pair<double, TilePosition>> cand;
    for (int dx = -R; dx <= R; ++dx)
        for (int dy = -R; dy <= R; ++dy) {
            TilePosition t(at.x + dx, at.y + dy);
            if (!t.isValid()) continue;
            Position c = Position(t) + Position(32, 32);
            double s;
            if (anchored) {
                s = c.getDistance(anchor);
            } else {
                double dn = c.getDistance(nexus) / 32.0;
                if (dn < 4) continue;
                auto a = MapInfo::areaAtTile(t);
                if (a != homeArea && !(natOk && a == natArea)) continue;
                s = std::abs(dn - 7) * 2;
                if (a == natArea) s += 4;
                for (auto &p : existing) {
                    double d = c.getDistance(p) / 32.0;
                    if (d < 6) s += (6 - d) * 6;
                }
                if (pylons == 0 && MapInfo::mainChoke.isValid()) s += c.getDistance(MapInfo::mainChoke) / 32.0;
            }
            cand.push_back({s, t});
        }
    std::sort(cand.begin(), cand.end(), [](auto &a, auto &b) { return a.first < b.first; });
    for (int gap = 1; gap >= 0; --gap)
        for (auto &[s, t] : cand)
            if (goodSpot(UT::Protoss_Pylon, t, gap, anchored)) return t;
    return TilePositions::None;
}

static TilePosition findPowered(UnitType ut, Position near) {
    auto mb = MapInfo::mainBase();
    if (!mb) return TilePositions::None;
    Position anchor = near.isValid() ? near : mb->center;
    std::vector<Unit> pylons;
    for (auto u : Self()->getUnits())
        if (u->getType() == UT::Protoss_Pylon && u->isCompleted()) pylons.push_back(u);
    std::vector<std::pair<double, TilePosition>> cand;
    std::set<std::pair<int, int>> seen;
    for (auto p : pylons) {
        TilePosition pt = p->getTilePosition();
        for (int dx = -9; dx <= 9; ++dx)
            for (int dy = -6; dy <= 6; ++dy) {
                TilePosition t(pt.x + dx, pt.y + dy);
                if (!t.isValid() || !seen.insert({t.x, t.y}).second) continue;
                Position c = Position(t) + Position(ut.tileWidth() * 16, ut.tileHeight() * 16);
                cand.push_back({c.getDistance(anchor), t});
            }
    }
    std::sort(cand.begin(), cand.end(), [](auto &a, auto &b) { return a.first < b.first; });
    int gapMax = (ut == UT::Protoss_Photon_Cannon) ? 0 : 1;
    for (int gap = gapMax; gap >= 0; --gap)
        for (auto &[s, t] : cand)
            if (goodSpot(ut, t, gap, ut == UT::Protoss_Photon_Cannon)) return t;
    return TilePositions::None;
}

static std::map<std::pair<int, int>, int> failedUntil;
void markFailed(TilePosition t) { failedUntil[{t.x, t.y}] = Now() + 24 * 60 * 4; }

static bool pathBlocked(Position a, Position b) {
    int len = -1;
    const auto &path = BWEM::Map::Instance().GetPath(a, b, &len);
    if (len < 0) return true;
    for (auto cp : path)
        if (cp->Blocked()) return true;
    return false;
}

TilePosition expansionSpot() {
    auto mb = MapInfo::mainBase();
    if (!mb) return TilePositions::None;
    Position enemy = MapInfo::enemyMainPos();
    std::vector<std::pair<double, BaseLoc *>> order;
    for (auto &b : MapInfo::bases) {
        if (b.enemyHere || b.mineralCount < 4) continue;
        auto fit = failedUntil.find({b.tile.x, b.tile.y});
        if (fit != failedUntil.end() && fit->second > Now()) continue;
        if (pathBlocked(mb->center, b.center)) continue;
        bool taken = false;
        for (auto u : Broodwar->getAllUnits())
            if (u->getType().isResourceDepot() && u->getPosition().getDistance(b.center) < 200) taken = true;
        if (taken) continue;
        // enemy buildings next to it?
        bool hostile = false;
        for (auto &[id, e] : Info::enemies)
            if (e.type.isBuilding() && e.pos.getDistance(b.center) < 400) hostile = true;
        if (hostile) continue;
        int d = MapInfo::groundDist(mb->center, b.center);
        if (d < 0) continue;
        int de = MapInfo::groundDist(enemy, b.center);
        double s = d - (de > 0 ? de * 0.3 : 0);
        if (de > 0 && d > de) s += 3000;  // on the enemy's side of the map
        if (&b == MapInfo::natBase()) s -= 100000;
        if (b.gasCount == 0) s += 1500;
        order.push_back({s, &b});
    }
    std::sort(order.begin(), order.end(), [](auto &a, auto &b) { return a.first < b.first; });
    static int lastLog = -10000;
    if (Now() - lastLog > 24 * 30) {
        lastLog = Now();
        for (auto &[sc, b] : order)
            LOG("expansion candidate (%d,%d) score %.0f my %d enemy %d", b->tile.x, b->tile.y, sc,
                MapInfo::groundDist(mb->center, b->center), MapInfo::groundDist(enemy, b->center));
    }
    for (auto &[s, b] : order)
        if (Broodwar->canBuildHere(b->tile, UT::Protoss_Nexus)) return b->tile;
    return TilePositions::None;
}

TilePosition gasSpot() {
    for (auto d : Self()->getUnits()) {
        if (!d->getType().isResourceDepot() || !d->isCompleted()) continue;
        for (auto g : Broodwar->getGeysers())
            if (g->getDistance(d) < 320 && g->getType() == UT::Resource_Vespene_Geyser &&
                Broodwar->canBuildHere(g->getTilePosition(), UT::Protoss_Assimilator))
                return g->getTilePosition();
    }
    return TilePositions::None;
}

TilePosition find(UnitType t, Position near) {
    if (t == UT::Protoss_Nexus) return expansionSpot();
    if (t == UT::Protoss_Assimilator) return gasSpot();
    if (t == UT::Protoss_Pylon) return findPylon(near);
    return findPowered(t, near);
}
}  // namespace Placement

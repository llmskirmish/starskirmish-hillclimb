#include "common.h"

namespace MapInfo {
std::vector<BaseLoc> bases;
int mainIdx = -1, natIdx = -1;
Position mainChoke = Positions::None, natChoke = Positions::None;
std::vector<TilePosition> enemyStartCandidates;
TilePosition enemyStart = TilePositions::Unknown;

static BWEM::Map &M() { return BWEM::Map::Instance(); }

const BWEM::Area *areaOf(Position p) {
    if (!p.isValid()) return nullptr;
    auto a = M().GetArea(WalkPosition(p));
    if (!a) a = M().GetNearestArea(WalkPosition(p));
    return a;
}

const BWEM::Area *areaAtTile(TilePosition t) {
    if (!t.isValid()) return nullptr;
    return M().GetArea(t);
}

int groundDist(Position a, Position b) {
    if (!a.isValid() || !b.isValid()) return -1;
    int len = -1;
    M().GetPath(a, b, &len);
    return len;
}

Position nextWaypoint(Position from, Position to) {
    if (!from.isValid() || !to.isValid()) return to;
    int len = -1;
    const auto &path = M().GetPath(from, to, &len);
    if (len < 0) return to;
    for (auto cp : path) {
        Position c(cp->Center());
        if (from.getDistance(c) > 96) return c;
    }
    return to;
}

BaseLoc *mainBase() { return mainIdx >= 0 ? &bases[mainIdx] : nullptr; }
BaseLoc *natBase() { return natIdx >= 0 ? &bases[natIdx] : nullptr; }

void init() {
    auto &m = M();
    m.Initialize(BroodwarPtr);
    m.EnableAutomaticPathAnalysis();
    m.FindBasesForStartingLocations();
    for (auto &area : m.Areas())
        for (auto &b : area.Bases()) {
            BaseLoc bl;
            bl.bw = &b;
            bl.tile = b.Location();
            bl.center = Position(b.Location()) + Position(64, 48);
            bl.start = b.Starting();
            bl.area = &area;
            bl.gasCount = (int)b.Geysers().size();
            bl.mineralCount = (int)b.Minerals().size();
            bases.push_back(bl);
        }
    auto home = Self()->getStartLocation();
    int best = 1 << 30;
    for (int i = 0; i < (int)bases.size(); ++i) {
        int d = bases[i].tile.getApproxDistance(home);
        if (d < best) best = d, mainIdx = i;
    }
    if (mainIdx >= 0) bases[mainIdx].tile = home, bases[mainIdx].center = Position(home) + Position(64, 48);
    for (auto t : Broodwar->getStartLocations())
        if (t != home) enemyStartCandidates.push_back(t);
    if (enemyStartCandidates.size() == 1) enemyStart = enemyStartCandidates[0];

    // natural: nearest non-start base with gas by ground
    best = 1 << 30;
    for (int i = 0; i < (int)bases.size(); ++i) {
        if (i == mainIdx || bases[i].start) continue;
        int d = groundDist(bases[mainIdx].center, bases[i].center);
        if (d < 0) continue;
        if (bases[i].gasCount == 0) d += 2000;
        if (d < best) best = d, natIdx = i;
    }
    if (mainIdx >= 0 && natIdx >= 0) {
        int len = -1;
        const auto &path = m.GetPath(bases[mainIdx].center, bases[natIdx].center, &len);
        if (!path.empty()) mainChoke = Position(path.front()->Center());
        Position far = Position(m.Center());
        if (enemyStart.isValid() && enemyStart != TilePositions::Unknown)
            far = Position(enemyStart) + Position(64, 48);
        const auto &p2 = m.GetPath(bases[natIdx].center, far, &len);
        for (auto cp : p2) {
            Position c(cp->Center());
            if (mainChoke.isValid() && c.getDistance(mainChoke) < 64) continue;
            natChoke = c;
            break;
        }
    }
    if (!mainChoke.isValid()) mainChoke = bases[mainIdx].center;
    if (!natChoke.isValid()) natChoke = mainChoke;
    LOG("map %s bases=%d main=%d nat=%d mainChoke=(%d,%d) natChoke=(%d,%d) candidates=%d",
        Broodwar->mapFileName().c_str(), (int)bases.size(), mainIdx, natIdx, mainChoke.x / 32, mainChoke.y / 32,
        natChoke.x / 32, natChoke.y / 32, (int)enemyStartCandidates.size());
}

void update() {
    for (auto &b : bases)
        if (Broodwar->isVisible(b.tile)) b.lastSeen = Now();
    // Enemy base flags from memory.
    for (auto &b : bases) b.enemyHere = false;
    for (auto &[id, e] : Info::enemies) {
        if (!e.type.isBuilding()) continue;
        for (auto &b : bases)
            if (e.pos.getDistance(b.center) < 320 && (e.type.isResourceDepot() || b.start)) b.enemyHere = true;
    }
    if (enemyStart == TilePositions::Unknown) {
        // Any enemy building tells us which start is theirs.
        for (auto &[id, e] : Info::enemies) {
            if (!e.type.isBuilding()) continue;
            TilePosition bestT = TilePositions::Unknown;
            int bestD = 1 << 30;
            for (auto t : enemyStartCandidates) {
                int d = e.pos.getApproxDistance(Position(t) + Position(64, 48));
                if (d < bestD) bestD = d, bestT = t;
            }
            if (bestD < 32 * 30) {
                enemyStart = bestT;
                LOG("enemy start found at (%d,%d) via %s", bestT.x, bestT.y, e.type.c_str());
                break;
            }
        }
        // Eliminate explored candidates without enemy buildings.
        for (auto it = enemyStartCandidates.begin(); it != enemyStartCandidates.end();) {
            Position c = Position(*it) + Position(64, 48);
            bool seen = Broodwar->isVisible(*it) && Broodwar->isVisible(TilePosition(c));
            bool enemyThere = false;
            for (auto &[id, e] : Info::enemies)
                if (e.type.isBuilding() && e.pos.getDistance(c) < 400) enemyThere = true;
            if (seen && !enemyThere && enemyStart == TilePositions::Unknown) {
                LOG("start (%d,%d) is empty", it->x, it->y);
                it = enemyStartCandidates.erase(it);
            } else
                ++it;
        }
        if (enemyStart == TilePositions::Unknown && enemyStartCandidates.size() == 1) {
            enemyStart = enemyStartCandidates[0];
            LOG("enemy start deduced (%d,%d)", enemyStart.x, enemyStart.y);
        }
    }
}

Position enemyMainPos() {
    if (enemyStart.isValid() && enemyStart != TilePositions::Unknown) return Position(enemyStart) + Position(64, 48);
    if (!enemyStartCandidates.empty()) return Position(enemyStartCandidates[0]) + Position(64, 48);
    return Position(M().Center());
}

bool myNaturalTaken() {
    auto n = natBase();
    if (!n) return false;
    for (auto u : Self()->getUnits())
        if (u->getType().isResourceDepot() && u->getPosition().getDistance(n->center) < 128) return true;
    return false;
}

bool inMyTerritory(Position p) {
    auto a = areaAtTile(TilePosition(p));
    if (a && mainIdx >= 0 && a == bases[mainIdx].area) return true;
    if (a && natIdx >= 0 && a == bases[natIdx].area && myNaturalTaken()) return true;
    for (auto u : Self()->getUnits())
        if (u->getType().isBuilding() && u->getDistance(p) < 32 * 9) return true;
    return false;
}
}  // namespace MapInfo

#include "common.h"

namespace Scout {
static Unit scout = nullptr;
static bool done = false;
static int circleIdx = 0;
static int startFrame = 0;

void update() {
    if (done) return;
    if (!scout) {
        if (Self()->supplyUsed() / 2 < 9 || Self()->allUnitCount(UT::Protoss_Pylon) == 0) return;
        scout = Workers::takeBuilder(MapInfo::enemyMainPos());
        if (!scout) return;
        Workers::setRole(scout, Workers::Role::Scout);
        startFrame = Now();
        LOG("scout %d sent", scout->getID());
    }
    if (!scout->exists()) {
        done = true;
        return;
    }
    auto finish = [&]() {
        Workers::release(scout);
        done = true;
        LOG("scout returning");
    };
    if (scout->getHitPoints() + scout->getShields() < 20 || Now() > 24 * 60 * 6) {
        finish();
        return;
    }
    if (MapInfo::enemyStart == TilePositions::Unknown) {
        // nearest unexplored candidate
        Position best = Positions::None;
        double bd = 1e18;
        for (auto t : MapInfo::enemyStartCandidates) {
            Position p = Position(t) + Position(64, 48);
            double d = scout->getDistance(p);
            if (d < bd) bd = d, best = p;
        }
        if (best.isValid()) cmdMove(scout, best);
        return;
    }
    Position c = Position(MapInfo::enemyStart) + Position(64, 48);
    // dodge enemy fighters
    for (auto e : Opp()->getUnits()) {
        if (!e->isVisible() || e->getType().isBuilding() || !e->getType().canAttack()) continue;
        if (e->getType().isWorker() && !e->isAttacking()) continue;
        if (scout->getDistance(e) < 32 * 4) {
            Position away = scout->getPosition() + (scout->getPosition() - e->getPosition()) * 2;
            // keep circling but away from the threat: advance the waypoint
            circleIdx = (circleIdx + 1) % 10;
            cmdMove(scout, away.makeValid());
            return;
        }
    }
    // circle the enemy main
    const int N = 10;
    double ang = circleIdx * 2 * 3.14159265 / N;
    Position p(int(c.x + std::cos(ang) * 32 * 10), int(c.y + std::sin(ang) * 32 * 10));
    p = p.makeValid();
    if (scout->getDistance(p) < 96 || (!Broodwar->isWalkable(WalkPosition(p)) && scout->getDistance(c) < 32 * 12)) circleIdx = (circleIdx + 1) % N;
    if (scout->getDistance(c) > 32 * 14) cmdMove(scout, c);
    else cmdMove(scout, p);
}
}  // namespace Scout

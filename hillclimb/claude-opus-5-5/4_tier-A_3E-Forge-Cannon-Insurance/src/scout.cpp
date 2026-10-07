#include "common.h"

namespace Scout {
static Unit scout = nullptr;
static bool done = false;
static int circleIdx = 0;
static int startFrame = 0;

// A second look at the enemy main around 4:00, to catch tech (dark templar) the first scout missed.
static Unit scout2 = nullptr;
static bool done2 = false;
static int scout2Start = 0, circle2 = 0, scout2Arrived = 0;

static void secondScout() {
    if (done2 || !done) return;
    if (Info::dtThreat() || MapInfo::enemyStart == TilePositions::Unknown) {
        if (Now() > 24 * 60 * 5) done2 = true;
        if (!scout2) return;
    }
    if (!scout2) {
        if (Now() < 24 * (60 * 3 + 50)) return;
        scout2 = Workers::takeBuilder(Position(Self()->getStartLocation()));  // a fresh probe, not the hurt first scout
        if (!scout2) return;
        Workers::setRole(scout2, Workers::Role::Scout);
        scout2Start = Now();
        LOG("second scout %d sent", scout2->getID());
    }
    if (!scout2->exists()) {
        done2 = true;
        return;
    }
    if (scout2->getHitPoints() + scout2->getShields() < 30 || Now() - scout2Start > 24 * 150 || (scout2Arrived && Now() - scout2Arrived > 24 * 70) || Info::dtThreat()) {
        Workers::release(scout2);
        done2 = true;
        LOG("second scout returning");
        return;
    }
    Position c = Position(MapInfo::enemyStart) + Position(64, 48);
    if (scout2->getDistance(c) > 32 * 12) {
        cmdMove(scout2, c);
        return;
    }
    if (!scout2Arrived) scout2Arrived = Now();
    const int N = 8;
    double ang = circle2 * 2 * 3.14159265 / N;
    Position p = Position(int(c.x + std::cos(ang) * 32 * 7), int(c.y + std::sin(ang) * 32 * 7)).makeValid();
    if (scout2->getDistance(p) < 96 || !Broodwar->isWalkable(WalkPosition(p))) circle2 = (circle2 + 1) % N;
    cmdMove(scout2, p);
}

void update() {
    secondScout();
    if (done) return;
    if (!scout) {
        // with the enemy start already known, the scout only looks at their build: send it later
        bool known = MapInfo::enemyStart != TilePositions::Unknown;
        if (Self()->supplyUsed() / 2 < (known ? 10 : 9) || Self()->allUnitCount(UT::Protoss_Pylon) == 0) return;
        if (known && Self()->allUnitCount(UT::Protoss_Gateway) == 0) return;
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
    // blocked (e.g. probes holding the ramp): stop wasting the probe
    static Position lastPos = Positions::None;
    static int lastMoveFrame = 0;
    if (!lastPos.isValid() || scout->getDistance(lastPos) > 96) lastPos = scout->getPosition(), lastMoveFrame = Now();
    if (Now() - lastMoveFrame > 24 * 12 && scout->getDistance(c) > 32 * 12) {
        LOG("scout blocked");
        finish();
        return;
    }
    // dodge enemy fighters
    for (auto e : Opp()->getUnits()) {
        if (!e->isVisible() || e->getType().isBuilding() || !e->getType().canAttack()) continue;
        if (e->getType().isWorker() && !e->isAttacking()) continue;
        if (scout->getDistance(e) < 32 * 4) {
            Position away = scout->getPosition() + (scout->getPosition() - e->getPosition()) * 2;
            // keep circling but away from the threat: advance the waypoint
            circleIdx = (circleIdx + 1) % 10;
            lastMoveFrame = Now();  // chased, not blocked
            cmdMove(scout, away.makeValid());
            return;
        }
    }
    // first look at their geyser: gas or no gas tells a lot about the build
    static bool geyserSeen = false;
    if (!geyserSeen && scout->getDistance(c) < 32 * 14) {
        Unit gy = nullptr;
        for (auto g : Broodwar->getStaticGeysers())
            if (g->getInitialPosition().getDistance(c) < 400 && (!gy || g->getInitialPosition().getDistance(c) < gy->getInitialPosition().getDistance(c)))
                gy = g;
        if (!gy || (Broodwar->isVisible(gy->getInitialTilePosition()) && Now() > 24 * 135)) geyserSeen = true;
        else if (!Broodwar->isVisible(gy->getInitialTilePosition())) {
            cmdMove(scout, gy->getInitialPosition());
            return;
        }
    }
    // a look at their natural now and then: an early expansion changes everything
    if (geyserSeen && Now() > 24 * 150) {
        auto en = MapInfo::enemyNatBase();
        if (en && Now() - en->lastSeen > 24 * 50) {
            cmdMove(scout, en->center);
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

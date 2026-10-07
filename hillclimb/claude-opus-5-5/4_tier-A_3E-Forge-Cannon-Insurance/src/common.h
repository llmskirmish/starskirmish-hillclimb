// Shared declarations for the bot's modules.
#pragma once
#include <BWAPI.h>
#include <bwem.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace BWAPI;

#define LOG(...)                                                       \
    do {                                                               \
        std::printf("[%5d] ", BWAPI::Broodwar->getFrameCount());       \
        std::printf(__VA_ARGS__);                                      \
        std::printf("\n");                                             \
        std::fflush(stdout);                                           \
    } while (0)

inline Player Self() { return Broodwar->self(); }
inline Player Opp() { return Broodwar->enemy(); }
inline int Now() { return Broodwar->getFrameCount(); }

namespace UT = UnitTypes;

// ---------------------------------------------------------------- map
struct BaseLoc {
    const BWEM::Base *bw = nullptr;
    TilePosition tile;
    Position center;
    bool start = false;
    const BWEM::Area *area = nullptr;
    int gasCount = 0;
    int mineralCount = 0;
    int lastSeen = -1;     // frame the nexus spot was last visible
    bool enemyHere = false; // enemy depot believed here
};

namespace MapInfo {
extern std::vector<BaseLoc> bases;
extern int mainIdx, natIdx;
extern Position mainChoke, natChoke;
extern std::vector<TilePosition> enemyStartCandidates;
extern TilePosition enemyStart;  // TilePositions::Unknown until found
void init();
void update();
int groundDist(Position a, Position b);  // pixels, -1 if unreachable
Position nextWaypoint(Position from, Position to);
BaseLoc *mainBase();
BaseLoc *natBase();
BaseLoc *enemyNatBase();  // null until their start is known
Position enemyMainPos();  // best guess
bool myNaturalTaken();
bool inMyTerritory(Position p);
const BWEM::Area *areaOf(Position p);
const BWEM::Area *areaAtTile(TilePosition t);  // no nearest-area fallback
}  // namespace MapInfo

// ---------------------------------------------------------------- enemy memory
struct EnemyRec {
    Unit unit = nullptr;
    UnitType type;
    Position pos;
    int lastSeen = 0;
    int hp = 0, shields = 0;
    bool completed = true;
    bool posValid = true;
};

namespace Info {
extern std::map<int, EnemyRec> enemies;
extern std::set<UnitType> seenTypes;
extern int firstDTFrame;
void update();
void onDestroy(Unit u);
int count(UnitType t);
bool dtThreat();       // enemy has or will soon have dark templar
bool cloakSeenNow();   // undetected enemy cloaked unit visible now
bool techUnknown();    // the scout never saw their tech buildings
bool rushThreat();     // early mass-zealot aggression expected
double enemyArmyStrength(bool recentOnly = false);
int enemyCombatSupply();
extern int armySupplyKilled;  // enemy army supply we destroyed (whole units)
}  // namespace Info

// ---------------------------------------------------------------- workers
namespace Workers {
enum class Role { Mine, Gas, Build, Scout, Fight };
void update();
Unit takeBuilder(Position near);  // removes from mining
Unit takeAny(Position near, int minLife = 0);
void release(Unit u);
void setRole(Unit u, Role r);
bool hasRole(Unit u, Role r);
int count();
int mineralWorkers();
int gasWorkers();
int baseNeed();  // how many more workers the bases can use
void onDestroy(Unit u);
extern int gasPerGeyser;
}  // namespace Workers

// ---------------------------------------------------------------- build placement
namespace Placement {
TilePosition find(UnitType t, Position near = Positions::None);
bool goodSpot(UnitType t, TilePosition tile, int gap, bool inLine = false);
TilePosition expansionSpot();
void markFailed(TilePosition t);
TilePosition gasSpot();
int powerScore(TilePosition t);
}  // namespace Placement

// ---------------------------------------------------------------- macro
namespace Macro {
void update();
void onUnitCreate(Unit u);
int planned(UnitType t);  // completed + in progress + queued
int pendingBuild(UnitType t);
extern int reservedMin, reservedGas;
}  // namespace Macro

// ---------------------------------------------------------------- combat
namespace Combat {
void update();
extern std::string mode;
extern double myStrength, theirStrength, theirEstimate;  // estimate: what they probably have, seen or not
bool homeThreat();
void onOwnDeath(Unit u);
void logDeaths();
}  // namespace Combat

// ---------------------------------------------------------------- scouting
namespace Scout {
void update();
}

// ---------------------------------------------------------------- strategy flags
namespace Strat {
extern bool expandNow;
extern bool wantCannons;
extern int lastRetreatFrame;
// one-base dragoon timing: build up on one base, hit before their expansion pays off
bool timingActive();
extern bool timingOver;
}

// ---------------------------------------------------------------- unit helpers
bool isArmyType(UnitType t);
double unitValue(UnitType t);
double dpsOf(UnitType t, bool vsAir = false);
extern int dbgCmds[3];  // army commands issued: attack unit, attack move, move
bool cmdAttack(Unit u, Unit t);
bool cmdAttackMove(Unit u, Position p);
bool cmdMove(Unit u, Position p);
bool cmdGather(Unit u, Unit t);

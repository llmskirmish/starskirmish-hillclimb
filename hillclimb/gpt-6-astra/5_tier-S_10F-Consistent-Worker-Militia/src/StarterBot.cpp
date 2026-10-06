#include "world.hpp"
#include <cstdio>
#include <FAP.hpp>
#include <bwem.h>
using namespace BWAPI;
using namespace bw_demo;
class Candidate : public AIModule {
 std::map<int,Reservation> builders;
 std::set<int> militia;
 Position home, rally, enemy=Positions::Invalid;
 std::map<int,Position> buildings;
 Unit scout=nullptr,homeGuard=nullptr,secondProbeScout=nullptr;bool guardSent=false,secondProbeSent=false;
 Position firstScout=Positions::Invalid,secondScout=Positions::Invalid,knownStart=Positions::Invalid;bool proxyLocated=false;
 bool enemyHomeGate=false,cannonRush=false,enemyGasSeen=false,earlyMelee=false;std::set<int> enemyGatesSeen;
 std::set<int> scoutTechLogged;bool fastGas=false;
 bool expansionFailed=false;
 bool launched=false, dtSeen=false, dtReady=false, enemyDetection=false, enemyRobo=false;
 Position chokeRally=Positions::Invalid, workerRally=Positions::Invalid;
 int dtMade=0;int steals=0, scoutTrips=0;
 Position searchGoal=Positions::Invalid;int searchIndex=0,searchFrame=0;
 std::vector<Position> searchPoints,airSearchPoints;
 std::vector<std::pair<Position,int>> storms;
 Position scoutCheckpoint=Positions::Invalid;bool scoutCheckpointDone=false;
 int frame=0;
 std::vector<Position> stormFields;
 std::map<int,std::pair<Position,int>> stormEscapes;
 std::set<int> observedStormBullets;
 struct PendingStorm {int frame,energy;};
 std::map<int,PendingStorm> pendingStorms;
 int proxyPowerLost=-10000;bool proxyDisarmed=false;std::set<int> localPylons;
 std::map<int,bool> retreat;
 struct ReaverMemory{FAP::FAPUnit<int> unit;int frame,shuttle;};
 std::map<int,ReaverMemory> reaverMemory;std::set<int> visibleReavers;int memoryLogFrame=-1;
 std::map<int,int> exposedUntil;
 std::map<int,int> observerSearch;
 std::set<int> transferred;
 std::map<int,int> gasWorkers;
 std::map<int,int> mineralWorkers;
 auto simUnit(Unit u) {
  auto t=u->getType(); auto p=u->getPlayer();
  return FAP::makeUnit<int>().setUnitType(t).setPosition(u->getPosition()).setHealth(u->getHitPoints()).setShields(u->getShields()).setArmorUpgrades(p->getUpgradeLevel(t.armorUpgrade())).setAttackerCount(1).setAttackUpgrades(p->getUpgradeLevel(t==type(T::Reaver)?UpgradeTypes::Scarab_Damage:t.groundWeapon().upgradeType())).setShieldUpgrades(p->getUpgradeLevel(UpgradeTypes::Protoss_Plasma_Shields)).setSpeedUpgrade(p->getUpgradeLevel(t==type(T::Zealot)?UpgradeTypes::Leg_Enhancements:UpgradeTypes::None)>0).setAttackSpeedUpgrade(false).setStimmed(false).setRangeUpgrade(t==type(T::Dragoon)&&p->getUpgradeLevel(UpgradeTypes::Singularity_Charge)>0).setAttackCooldownRemaining(u->getGroundWeaponCooldown()).setFlying(u->isFlying()).setElevation(Broodwar->getGroundHeight(u->getTilePosition())).setData(u->getID());
 }

 void simulateCombat(FAP::FastAPproximation<int>& sim,int frames){
  auto initial=sim.getState();bool reavers=false;for(auto group:{initial.first,initial.second})for(auto &u:*group)if(u.unitType==type(T::Reaver))reavers=true;
  if(!reavers){sim.simulate(frames);return;}
  struct Blast{int side,shooter,target,x,y,damage;};
  auto scarab=UnitTypes::Protoss_Scarab.groundWeapon();
  for(int tick=0;tick<frames;tick++){
   auto state=sim.getState();if(state.first->empty()||state.second->empty())break;
   std::vector<Blast> blasts;
   for(int side=0;side<2;side++){
    auto own=side?state.second:state.first,foes=side?state.first:state.second;
    for(auto &r:*own)if(r.unitType==type(T::Reaver)&&r.attackCooldownRemaining==0){
     FAP::FAPUnit<int>* target=nullptr;int nearest=1000000000;
     for(auto &e:*foes)if(!e.flying){int dx=e.x-r.x,dy=e.y-r.y,d=dx*dx+dy*dy;if(d<nearest){nearest=d;target=&e;}}
     if(target&&nearest<=r.groundMaxRangeSquared)blasts.push_back({side,r.data,target->data,target->x,target->y,r.groundDamage});
    }
   }
   sim.simulate(1);state=sim.getState();
   for(auto &b:blasts){
    auto own=b.side?state.second:state.first,foes=b.side?state.first:state.second;bool fired=false;
    for(auto &r:*own)if(r.data==b.shooter&&r.attackCooldownRemaining>0)fired=true;
    if(!fired)continue;
    for(auto &e:*foes)if(!e.flying&&e.data!=b.target){
     int dx=std::max(0,std::abs(e.x-b.x)-e.unitType.width()/2),dy=std::max(0,std::abs(e.y-b.y)-e.unitType.height()/2),d=dx*dx+dy*dy;
     int damage=d<=scarab.innerSplashRadius()*scarab.innerSplashRadius()?b.damage:d<=scarab.medianSplashRadius()*scarab.medianSplashRadius()?b.damage/2:d<=scarab.outerSplashRadius()*scarab.outerSplashRadius()?b.damage/4:0;
     if(!damage)continue;int fixed=damage*256;
     if(e.shields>0){int shieldDamage=std::max(128,fixed-e.shieldArmor*256);if(e.shields>=shieldDamage){e.shields-=shieldDamage;continue;}fixed=shieldDamage-e.shields;e.shields=0;}
     e.health-=std::max(128,fixed-e.armor*256);
    }
   }
   for(auto group:{state.first,state.second})group->erase(std::remove_if(group->begin(),group->end(),[](const auto &u){return u.health<1;}),group->end());
  }
 }

 Position clamp(Position p) {return Position(std::clamp(p.x,0,Broodwar->mapWidth()*32-1),std::clamp(p.y,0,Broodwar->mapHeight()*32-1));}
 bool clearMicroLine(Unit u,Position goal){
  auto from=u->getPosition();
  if(!World::reachable(TilePosition(goal))||!World::clearGroundLine(from,goal))return false;
  int px=u->getType().width()/2+2,py=u->getType().height()/2+2;
  int steps=std::max(1,from.getApproxDistance(goal)/8);
  for(auto b:Broodwar->getAllUnits())if(b->exists()&&b->getType().isBuilding()&&b->getDistance(u)<256){
   int l=b->getLeft()-px,r=b->getRight()+px,t=b->getTop()-py,bt=b->getBottom()+py;
   auto inside=[&](Position p){return p.x>l&&p.x<r&&p.y>t&&p.y<bt;};
   if(inside(goal))return false;
   bool exited=!inside(from);
   for(int i=1;i<=steps;i++){bool in=inside(from+(goal-from)*i/steps);if(in&&exited)return false;if(!in)exited=true;}
  }
  return true;
 }
 Position dragoonKite(Unit u,Unit foe,Position group){
  auto p=u->getPosition(),v=p-foe->getPosition();double angle=std::atan2(v.y,v.x);
  Position straight=clamp(p+Position(int(std::cos(angle)*80),int(std::sin(angle)*80)));
  if(clearMicroLine(u,straight))return straight;
  Position best=p;double bestScore=-1e30;
  for(int i=0;i<16;i++){
   double a=angle+i*6.28318530718/16;Position q=clamp(p+Position(int(std::cos(a)*80),int(std::sin(a)*80)));
   if(!clearMicroLine(u,q)||foe->getDistance(q)<u->getDistance(foe)-8)continue;
   double score=foe->getDistance(q)-q.getApproxDistance(group)*.15;
   if(score>bestScore){bestScore=score;best=q;}
  }
  return best;
 }
 Position constructionSafe(Unit u,Position p){
  if(u->getType().isWorker()||u->isFlying())return p;
  int pad=std::max(u->getType().width(),u->getType().height())/2+16;
  for(int pass=0;pass<4;pass++){
   bool changed=false;
   for(auto &[id,r]:builders){
    int left=r.tile.x*32-pad,top=r.tile.y*32-pad,right=(r.tile.x+r.type.tileWidth())*32+pad,bottom=(r.tile.y+r.type.tileHeight())*32+pad;
    if(p.x<=left||p.x>=right||p.y<=top||p.y>=bottom)continue;
    Position best=Positions::Invalid;double score=1e30;
    for(auto q:{Position(left-4,p.y),Position(right+4,p.y),Position(p.x,top-4),Position(p.x,bottom+4)}){
     q=clamp(q);if(!World::reachable(TilePosition(q))||!World::clearGroundLine(u->getPosition(),q))continue;
     bool blocked=false;for(auto b:World::own())if(b->getType().isBuilding()&&b->getDistance(q)<pad){blocked=true;break;}if(blocked)continue;
     double value=q.getApproxDistance(p)+.1*u->getDistance(q);if(value<score){score=value;best=q;}
    }
    if(best.isValid()){p=best;changed=true;}
   }
   if(!changed)break;
  }
  return p;
 }
 void move(Unit u,Position p) {p=constructionSafe(u,p);p=clamp(p); if(u->isIdle() || u->getLastCommand().getType()!=UnitCommandTypes::Move || u->getLastCommand().getTargetPosition().getApproxDistance(p)>48 || frame-u->getLastCommandFrame()>48) u->move(p);}
 void attack(Unit u,Unit e) {if(e&&!e->isFlying()&&u->getDistance(e)>Broodwar->self()->weaponMaxRange(u->getType().groundWeapon())+16&&!World::clearGroundLine(u->getPosition(),e->getPosition())){move(u,World::waypoint(u->getPosition(),e->getPosition(),true));return;}if(e && (u->isIdle() || u->getLastCommand().getType()!=UnitCommandTypes::Attack_Unit || u->getLastCommand().getTarget()!=e) && frame-u->getLastCommandFrame()>=6) u->attack(e);}
 void attackPos(Unit u,Position p) {p=constructionSafe(u,p);if(!u->isFlying()&&u->getDistance(p)>192&&!World::clearGroundLine(u->getPosition(),p)){auto next=World::waypoint(u->getPosition(),p,true);if(next.getApproxDistance(p)>32){move(u,next);return;}}if(u->isIdle() || (frame-u->getLastCommandFrame()>48 && u->getLastCommand().getTargetPosition().getApproxDistance(p)>96)) u->attack(clamp(p));}
 bool evadeStorm(Unit u){
  if(u->isLoaded()||u->getType().isBuilding()||stormFields.empty())return false;
  bool worker=u->getType().isWorker(),inside=u->isUnderStorm();
  auto clearance=[&](Position p){double d=10000;for(auto q:stormFields)d=std::min(d,p.getDistance(q));return d;};
  inside=inside||clearance(u->getPosition())<64;
  auto it=stormEscapes.find(u->getID());
  if(it!=stormEscapes.end()&&it->second.second>frame&&clearance(it->second.first)>=80){
   if(u->getDistance(it->second.first)>24){move(u,it->second.first);return true;}
   if(worker){if(u->getOrder()!=Orders::HoldPosition)u->holdPosition();return true;}
   if(!inside)return false;
  }
  if(!inside)return false;
  Position best=Positions::Invalid;double value=-1e30;
  for(int radius:{80,128,176})for(int i=0;i<24;i++){
   double a=i*6.28318530718/24;auto q=clamp(u->getPosition()+Position(int(std::cos(a)*radius),int(std::sin(a)*radius)));
   if(!u->isFlying()){
    if(!clearMicroLine(u,q))continue;
    bool blocked=false;for(auto b:Broodwar->getAllUnits())if(b->exists()&&b->getType().isBuilding()&&b->getDistance(q)<u->getType().width()/2+8){blocked=true;break;}if(blocked)continue;
   }
   double score=std::min(160.0,clearance(q))*3-radius*.4;
   for(int k=1;k<=4;k++)score-=std::max(0.0,64-clearance(u->getPosition()+(q-u->getPosition())*k/4))*.5;
   for(auto e:Broodwar->enemy()->getUnits())if(e->exists()&&e->isVisible()&&e->getType().canAttack()){
    auto weapon=u->isFlying()?e->getType().airWeapon():e->getType().groundWeapon();
    if(weapon!=WeaponTypes::None&&e->getDistance(q)<e->getPlayer()->weaponMaxRange(weapon)+32)score-=20;
   }
   for(auto &[id,escape]:stormEscapes)if(id!=u->getID()&&escape.second>frame&&escape.first.getApproxDistance(q)<48)score-=20;
   if(score>value){value=score;best=q;}
  }
  if(!best.isValid())return false;
  stormEscapes[u->getID()]={best,frame+(worker?96:24)};if(frame%24==0)std::printf("STORM EVADE f=%d unit=%d type=%s\n",frame,u->getID(),u->getType().c_str());move(u,best);return true;
 }
 TilePosition forwardSite(Position enemyMain){
  const auto &gd=World::groundDistance();int mw=Broodwar->mapWidth();auto t=TilePosition(enemyMain);TilePosition anchor=t;
  for(int i=0;i<20;i++){if(!t.isValid()||gd[t.x+t.y*mw]<0)break;auto next=t;int d=gd[t.x+t.y*mw];for(auto v:{TilePosition(-1,0),TilePosition(1,0),TilePosition(0,-1),TilePosition(0,1)}){auto q=t+v;if(q.isValid()&&gd[q.x+q.y*mw]>=0&&gd[q.x+q.y*mw]<d){d=gd[q.x+q.y*mw];next=q;}}if(next==t)break;t=next;anchor=t;}
  auto best=TilePositions::Invalid;double bs=1e9;
  for(int dx=-10;dx<=10;dx++)for(int dy=-10;dy<=10;dy++){auto q=anchor+TilePosition(dx,dy);if(!q.isValid()||!World::reachable(q)||!Broodwar->canBuildHere(q,type(T::Pylon)))continue;
   int dist=(Position(q)+Position(32,32)).getApproxDistance(enemyMain);if(dist<500||dist>1200)continue;
   int free=0;for(int x=-3;x<=3;x++)for(int y=-3;y<=3;y++){auto v=q+TilePosition(x,y);if(v.isValid()&&Broodwar->isBuildable(v)&&World::reachable(v))free++;}if(free<40)continue;
   double score=dx*dx+dy*dy+gd[q.x+q.y*mw]*.1+std::abs(dist-650)*.06;if(score<bs){bs=score;best=q;}
  }
  return best.isValid()?best:anchor;
 }
 public:
 void onUnitDestroy(Unit u) override {
  if(u->getType().isMineralField())World::mineralRemaining[u->getID()]=0;
  if(u->getPlayer()==Broodwar->enemy())reaverMemory.erase(u->getID());
  if(u->getPlayer()==Broodwar->self()&&u->getType().isWorker()&&builders.count(u->getID())){auto r=builders[u->getID()];if(r.type==type(T::Nexus)&&(Position(r.tile)+Position(64,48)).getApproxDistance(home)>600){bool started=false;for(auto n:World::own())if(n->getType()==type(T::Nexus)&&n->getTilePosition()==r.tile)started=true;if(!started){expansionFailed=true;World::failedSites[{r.tile.x,r.tile.y}]=frame+3600;std::printf("EXPANSION BUILDER LOST f=%d tile=%d,%d\n",frame,r.tile.x,r.tile.y);}}}
if(localPylons.count(u->getID())&&frame<4500){proxyPowerLost=frame;std::printf("PROXY POWER LOST f=%d id=%d\n",frame,u->getID());}if(u->getPlayer()==Broodwar->self()&&u->getType()==type(T::DT))enemyDetection=true;if(u->getPlayer()==Broodwar->self()&&u->getType()==type(T::Nexus)&&u->getDistance(home)>600){expansionFailed=true;auto t=u->getTilePosition();World::failedSites[{t.x,t.y}]=frame+5000;}}
 void onUnitComplete(Unit u) override {if(u->getPlayer()==Broodwar->self()&&u->getType()==type(T::DT))dtMade++;}
 void onStart() override {Broodwar->setCommandOptimizationLevel(2); home=Position(Broodwar->self()->getStartLocation())+Position(64,48);
  BWEM::Map::Instance().Initialize(BroodwarPtr);BWEM::Map::Instance().EnableAutomaticPathAnalysis();BWEM::Map::Instance().FindBasesForStartingLocations();
  rally=home; const auto &gd=World::groundDistance();int mw=Broodwar->mapWidth();TilePosition t=Broodwar->self()->getStartLocation();for(auto v:Broodwar->getStartLocations())if(v!=t){t=v;break;}
  for(int i=0;i<1000;i++){if(!t.isValid())break;int d=gd[t.x+t.y*mw];if(d<0)break;if(d<=13){rally=Position(t)+Position(16,16);break;}TilePosition next=t;for(auto delta:{TilePosition(-1,0),TilePosition(1,0),TilePosition(0,-1),TilePosition(0,1)}){auto q=t+delta;if(q.isValid()&&gd[q.x+q.y*mw]>=0&&gd[q.x+q.y*mw]<d){d=gd[q.x+q.y*mw];next=q;}}if(next==t)break;t=next;}
  t=Broodwar->self()->getStartLocation();for(auto v:Broodwar->getStartLocations())if(v!=t){t=v;break;}
  int steps=0;for(int i=0;i<1000;i++){if(!t.isValid())break;int d=gd[t.x+t.y*mw];if(d<0)break;if(steps>=20){World::proxy=t;break;}TilePosition next=t;for(auto delta:{TilePosition(-1,0),TilePosition(1,0),TilePosition(0,-1),TilePosition(0,1)}){auto q=t+delta;if(q.isValid()&&gd[q.x+q.y*mw]>=0&&gd[q.x+q.y*mw]<d){d=gd[q.x+q.y*mw];next=q;}}if(next==t)break;t=next;steps++;}
  if(World::proxy.isValid()) {
   auto anchor=World::proxy;TilePosition et;for(auto q:Broodwar->getStartLocations())if(q!=Broodwar->self()->getStartLocation()){et=q;break;}
   double dx=Position(et).x-home.x,dy=Position(et).y-home.y,d=std::max(1.0,std::hypot(dx,dy));
   bool found=false;for(int off:{8,-8,6,-6,10,-10,4,-4}){auto q=anchor+TilePosition(int(-dy/d*off),int(dx/d*off));if(q.isValid()&&gd[q.x+q.y*mw]>=0&&World::reachable(q)&&Broodwar->canBuildHere(q,type(T::Pylon))&&Position(q).getApproxDistance(Position(et))>550){World::proxy=q;found=true;break;}}
  }
  if(Broodwar->getStartLocations().size()>2){Position sum(0,0);for(auto q:Broodwar->getStartLocations())sum+=Position(q);auto c=TilePosition(sum/int(Broodwar->getStartLocations().size()));int best=100000;for(int x=-15;x<=15;x++)for(int y=-15;y<=15;y++){auto q=c+TilePosition(x,y);int score=x*x+y*y;if(q.isValid()&&gd[q.x+q.y*mw]>=0&&score<best&&World::reachable(q)&&Broodwar->canBuildHere(q,type(T::Pylon))){best=score;World::proxy=q;}}}
  for(const auto &area:BWEM::Map::Instance().Areas())for(const auto &base:area.Bases())if(base.Minerals().size()>=4)searchPoints.push_back(base.Center());
  for(int y=4;y<Broodwar->mapHeight();y+=12)for(int i=4;i<Broodwar->mapWidth();i+=12){int x=((y/12)%2)?Broodwar->mapWidth()-1-i:i;TilePosition q(x,y);if(q.isValid()&&gd[q.x+q.y*mw]>=0)searchPoints.push_back(Position(q)+Position(16,16));}
  for(int y=2;y<Broodwar->mapHeight();y+=12)for(int i=2;i<Broodwar->mapWidth();i+=12){int x=((y/12)%2)?Broodwar->mapWidth()-1-i:i;airSearchPoints.push_back(Position(x*32+16,y*32+16));}
  for(int x=2;x<Broodwar->mapWidth();x+=8){airSearchPoints.push_back(Position(x*32,64));airSearchPoints.push_back(Position(x*32,Broodwar->mapHeight()*32-64));}
  for(int y=2;y<Broodwar->mapHeight();y+=8){airSearchPoints.push_back(Position(64,y*32));airSearchPoints.push_back(Position(Broodwar->mapWidth()*32-64,y*32));}
  auto viable=[&](TilePosition q){int free=0;for(int dx=-4;dx<=4;dx++)for(int dy=-4;dy<=4;dy++){auto v=q+TilePosition(dx,dy);if(v.isValid()&&Broodwar->isBuildable(v)&&World::reachable(v))free++;}return free>=65;};
  if(World::proxy.isValid()&&!viable(World::proxy)){
   auto old=World::proxy;int best=999999;TilePosition chosen=TilePositions::Invalid;
   for(int dx=-16;dx<=16;dx++)for(int dy=-16;dy<=16;dy++){auto q=old+TilePosition(dx,dy);if(q.isValid()&&World::reachable(q)&&Broodwar->canBuildHere(q,type(T::Pylon))&&viable(q)){int score=dx*dx+dy*dy;if(score<best){best=score;chosen=q;}}}
   World::proxy=chosen;
  }
  if(Broodwar->getStartLocations().size()==2){TilePosition enemyStart;for(auto st:Broodwar->getStartLocations())if(st!=Broodwar->self()->getStartLocation())enemyStart=st;
   const auto &ed=World::distanceFrom(enemyStart);int old=World::proxy.isValid()?ed[World::proxy.x+World::proxy.y*mw]:-1;
   if(old<450||old>1200){TilePosition best=TilePositions::Invalid;double score=1e30;
    for(int y=2;y<Broodwar->mapHeight()-2;y++)for(int x=2;x<mw-2;x++){auto q=TilePosition(x,y);int e=ed[x+y*mw],h=gd[x+y*mw];if(e<550||e>1000||h<0||!Broodwar->canBuildHere(q,type(T::Pylon))||!viable(q))continue;double s=h*8+e*1.6;if(s<score){score=s;best=q;}}
    if(best.isValid())World::proxy=best;
   }
   std::printf("PROXY ground approach %d -> %d\n",old,World::proxy.isValid()?ed[World::proxy.x+World::proxy.y*mw]:-1);
  }
  std::printf("PROXY %d,%d area-distance=%d\n",World::proxy.x,World::proxy.y,World::proxy.isValid()?gd[World::proxy.x+World::proxy.y*mw]:-1);
  for(auto u:World::own())if(u->getType().isWorker()){World::forwardWorker=u;break;}
  if(Broodwar->getStartLocations().size()>2){
   for(auto t:Broodwar->getStartLocations())if(t!=Broodwar->self()->getStartLocation()){if(!firstScout.isValid())firstScout=Position(t)+Position(64,48);else secondScout=Position(t)+Position(64,48);}
   for(auto u:World::own())if(u->getType().isWorker()&&u!=World::forwardWorker){scout=u;scoutTrips=1;break;}
  }

  
  
  for(auto start:Broodwar->getStartLocations())if(start!=Broodwar->self()->getStartLocation()){
   const auto &path=BWEM::Map::Instance().GetPath(home,Position(start)+Position(64,48));
   if(!path.empty()){auto pos=Position(path.front()->Center());auto v=home-pos;double d=std::max(1.0,std::hypot(v.x,v.y));rally=pos+Position(int(v.x*200/d),int(v.y*200/d));break;}
  }

  scout=nullptr;World::forwardWorker=nullptr;World::proxy=TilePositions::Invalid;proxyLocated=true;scoutTrips=0;
  auto naturalTile=World::expansion(type(T::Nexus));Position natural=naturalTile.isValid()?Position(naturalTile)+Position(64,48):home;
  Position far=Position(Broodwar->mapWidth()*16,Broodwar->mapHeight()*16);if(Broodwar->getStartLocations().size()==2)for(auto st:Broodwar->getStartLocations())if(st!=Broodwar->self()->getStartLocation())far=Position(st)+Position(64,48);
  auto v=far-natural;double len=std::max(1.0,std::hypot(v.x,v.y));Position ideal=natural+Position(int(v.x*512/len),int(v.y*512/len));int best=999999;
  for(int dx=-8;dx<=8;dx++)for(int dy=-8;dy<=8;dy++){auto q=TilePosition(ideal)+TilePosition(dx,dy);if(!q.isValid()||!World::reachable(q)||!Broodwar->isWalkable(WalkPosition(q)+WalkPosition(2,2))||gd[q.x+q.y*mw]*8>2400)continue;Position p=Position(q)+Position(16,16);int d=p.getApproxDistance(ideal);if(d<best){best=d;scoutCheckpoint=p;}}
  std::printf("LOCAL SCOUT %d,%d\n",scoutCheckpoint.x,scoutCheckpoint.y);




 }
 void onFrame() override {
 frame=Broodwar->getFrameCount(); if(frame%6) return;
 stormFields.clear();for(auto b:Broodwar->getBullets())if(b->exists()&&b->getType()==BulletTypes::Psionic_Storm)stormFields.push_back(b->getPosition());
 for(auto it=stormEscapes.begin();it!=stormEscapes.end();)if(it->second.second<frame)it=stormEscapes.erase(it);else ++it;
 for(auto b:Broodwar->getBullets())if(b->exists()&&b->getType()==BulletTypes::Psionic_Storm&&b->getPlayer()==Broodwar->self()&&observedStormBullets.insert(b->getID()).second){storms.emplace_back(b->getPosition(),frame+72);std::printf("STORM BULLET f=%d id=%d pos=%d,%d\n",frame,b->getID(),b->getPosition().x,b->getPosition().y);}

 for(auto it=builders.begin();it!=builders.end();) {
  Unit u=Broodwar->getUnit(it->first); bool started=false;
  for(auto b:World::own()) if(b->getType()==it->second.type && b->getTilePosition()==it->second.tile) started=true;
  bool remoteResource=(it->second.type.isResourceDepot()||it->second.type.isRefinery())&&(Position(it->second.tile)+Position(64,48)).getApproxDistance(home)>500;int buildTimeout=remoteResource?1440:720;
  if(!u||!u->exists()||started||frame-it->second.frame>buildTimeout){if(!started&&u&&u->exists()&&frame-it->second.frame>buildTimeout)World::failedSites[{it->second.tile.x,it->second.tile.y}]=frame+2400;it=builders.erase(it);}
  else {auto &r=it->second;Position site=Position(r.tile)+Position(r.type.tileWidth()*16,r.type.tileHeight()*16);
   if(frame-r.frame>=12 && frame-u->getLastCommandFrame()>=18 && !u->isConstructing()) {
    if(remoteResource){
     if(u->isIdle()||u->getLastCommand().getType()!=UnitCommandTypes::Build||frame-u->getLastCommandFrame()>240){if(!u->build(r.type,r.tile)&&(u->isIdle()||u->getLastCommand().getType()!=UnitCommandTypes::Move||frame-u->getLastCommandFrame()>120))u->move(site);}
    }else{
    auto next=World::waypoint(u->getPosition(),site);if(u->getDistance(site)>128||next.getApproxDistance(site)>96){if(u->isIdle()||u->getLastCommand().getType()!=UnitCommandTypes::Move||u->getLastCommand().getTargetPosition().getApproxDistance(next)>64||frame-u->getLastCommandFrame()>120)u->move(next);}else u->build(r.type,r.tile);
    }
   }
   ++it;
  }
 }
 if(frame%240==0)for(auto &[id,r]:builders){auto u=Broodwar->getUnit(id);if(u)std::printf("RES t=%d type=%s site=%d,%d worker=%d,%d order=%s age=%d\n",frame,r.type.c_str(),r.tile.x,r.tile.y,u->getPosition().x,u->getPosition().y,u->getOrder().c_str(),frame-r.frame);}
 World::reservedTiles.clear();for(auto &[id,r]:builders)World::reservedTiles.emplace_back(r.type,r.tile);
 World w{builders,militia}; for(auto &[id,r]:builders){w.minerals-=r.type.mineralPrice();w.gas-=r.type.gasPrice();}
 auto have=[&](T t){return w.count(t)+w.pending(t);};
 std::vector<Unit> foes, threats, army;
 for(auto e:World::ordered(Broodwar->enemy()->getUnits())) if(e->exists()&&e->isVisible()) {
  foes.push_back(e);if(e->getType()==type(T::Pylon)&&e->getDistance(home)<1200)localPylons.insert(e->getID());
  if(frame<5000&&(e->getType()==type(T::Gas)||e->getType()==type(T::Core))&&scoutTechLogged.insert(e->getID()).second)std::printf("SCOUT TECH f=%d type=%s complete=%d remain=%d hp=%d shields=%d\n",frame,e->getType().c_str(),e->isCompleted(),e->getRemainingBuildTime(),e->getHitPoints(),e->getShields());
  if((e->getType()==type(T::Forge)&&frame<4200)||(e->getType()==type(T::Cannon)&&e->getDistance(home)<1600))cannonRush=true;
  if(frame<3500&&e->getType()==type(T::Gateway)&&e->getDistance(home)<1200)earlyMelee=true;
  if(frame<3500&&e->getType()==type(T::Gateway)&&e->getDistance(home)<2000){bool proxy=true;for(auto st:Broodwar->getStartLocations())if(st!=Broodwar->self()->getStartLocation()&&e->getDistance(Position(st)+Position(64,48))<800)proxy=false;if(proxy)earlyMelee=true;}
  if(e->getType()==type(T::Gateway)){enemyGatesSeen.insert(e->getID());for(auto st:Broodwar->getStartLocations())if(st!=Broodwar->self()->getStartLocation()&&e->getDistance(Position(st)+Position(64,48))<600)enemyHomeGate=true;}
  if(e->getType()==type(T::Core)&&((frame<3120)||(e->isCompleted()&&frame<4080))&&!fastGas){fastGas=true;std::printf("EARLY CORE f=%d\n",frame);}
  if(e->getType()==type(T::Gas)){enemyGasSeen=true;if(e->isCompleted()&&frame<3420&&!fastGas){fastGas=true;std::printf("EARLY GAS f=%d\n",frame);}}
  if(e->getType().isDetector()&&e->isCompleted())enemyDetection=true;
  if(e->getType()==type(T::Robo)||e->getType()==type(T::Observatory))enemyRobo=true;
  if(e->getType().isBuilding()) {for(auto st:Broodwar->getStartLocations())if(st!=Broodwar->self()->getStartLocation()&&e->getDistance(Position(st)+Position(64,48))<600)knownStart=Position(st)+Position(64,48);buildings[e->getID()]=e->getPosition(); if(e->getType().isResourceDepot() || !enemy.isValid()) enemy=e->getPosition();}
  if(e->getType()==UnitTypes::Protoss_Dark_Templar || e->getType()==UnitTypes::Protoss_Templar_Archives) dtSeen=true;
  bool nearBase=e->getDistance(home)<500||(e->getType()==type(T::Cannon)&&e->getDistance(home)<850);for(auto n:World::own())if(n->getType().isResourceDepot()&&n->getDistance(e)<(n->isCompleted()?500:650))nearBase=true;
  if(nearBase && (e->getType().canAttack()||e->getType().isWorker()))threats.push_back(e);
 }
 visibleReavers.clear();
 for(auto e:foes)if(e->getType()==type(T::Reaver)){
  visibleReavers.insert(e->getID());FAP::FastAPproximation<int> snapshot;snapshot.addIfCombatUnitPlayer2(simUnit(e));int carrier=-1,nearest=193;
  for(auto t:foes)if(t->getType()==type(T::Shuttle)&&t->getDistance(e)<nearest){nearest=t->getDistance(e);carrier=t->getID();}
  if(!snapshot.getState().second->empty())reaverMemory.insert_or_assign(e->getID(),ReaverMemory{snapshot.getState().second->front(),frame,carrier});
 }
 for(auto it=reaverMemory.begin();it!=reaverMemory.end();)if(frame-it->second.frame>480)it=reaverMemory.erase(it);else ++it;
 for(auto it=buildings.begin();it!=buildings.end();) {if(Broodwar->isVisible(TilePosition(it->second)) && (!Broodwar->getUnit(it->first)||!Broodwar->getUnit(it->first)->exists()||Broodwar->getUnit(it->first)->getPlayer()!=Broodwar->enemy()||!Broodwar->getUnit(it->first)->getType().isBuilding()||Broodwar->getUnit(it->first)->getPosition().getApproxDistance(it->second)>64)) it=buildings.erase(it);else ++it;}
 if(firstScout.isValid()&&!proxyLocated&&knownStart.isValid()){
  if(Broodwar->self()->allUnitCount(type(T::Pylon))==0){
   World::proxy=forwardSite(knownStart);builders.clear();
   if(scout&&scout->exists()&&(!World::forwardWorker||!World::forwardWorker->exists()||scout->getDistance(Position(World::proxy))<World::forwardWorker->getDistance(Position(World::proxy))))World::forwardWorker=scout;
   scout=nullptr;
  }
  proxyLocated=true;
 }
 if(scout&&(earlyMelee||cannonRush||(knownStart.isValid()&&(fastGas||frame>4000))||frame>8000)){Unit mineral=nullptr;for(auto m:World::ordered(Broodwar->getMinerals()))if(m->getDistance(home)<400&&(!mineral||m->getDistance(home)<mineral->getDistance(home)))mineral=m;if(mineral&&scout->exists())scout->gather(mineral);scout=nullptr;scoutTrips=2;}
 if(knownStart.isValid())World::enemyStart=TilePosition(knownStart-Position(64,48));
 World::cannonDanger.clear();for(auto e:foes)if(e->getType()==type(T::Cannon))World::cannonDanger.push_back(e->getPosition());
 World::danger.clear();for(auto &[id,p]:buildings)World::danger.push_back(p);for(auto e:foes)if(e->getType().canAttack()&&!e->getType().isWorker())World::danger.push_back(e->getPosition());
 if(buildings.empty()&&enemy.isValid()&&Broodwar->isVisible(TilePosition(enemy)))enemy=Positions::Invalid;
 Position target=enemy;
 if(!buildings.empty()) { int d=100000; for(auto &[id,p]:buildings) if(p.getApproxDistance(home)<d){d=p.getApproxDistance(home); target=p;} }
 if(!target.isValid() || (buildings.empty()&&Broodwar->isVisible(TilePosition(target)))) {
  target=Positions::Invalid;
  for(auto t:Broodwar->getStartLocations()) if(t!=Broodwar->self()->getStartLocation() && !Broodwar->isExplored(t+TilePosition(2,1))) {target=Position(t)+Position(64,48);break;}
  if(!target.isValid()) {
   if(!searchPoints.empty()) {if(!searchGoal.isValid()||Broodwar->isVisible(TilePosition(searchGoal))||frame-searchFrame>2400){for(int i=0;i<(int)searchPoints.size();i++){searchGoal=searchPoints[searchIndex++%searchPoints.size()];if(!Broodwar->isVisible(TilePosition(searchGoal)))break;}searchFrame=frame;}target=searchGoal;}else target=home;
  }
 }
 if(!(World::proxy.isValid()&&frame<6500)){Unit emergency=nullptr;double best=1e30;for(auto e:threats)if(!e->getType().isWorker()&&(!e->getType().isBuilding()||e->getType()==type(T::Cannon))){int d=e->getDistance(home);for(auto n:World::own())if(n->getType()==type(T::Nexus))d=std::min(d,e->getDistance(n));double score=d+(e->getType().isBuilding()?600:0);if(score<best){best=score;emergency=e;}}if(emergency)target=emergency->getPosition();}
 if(World::proxy.isValid()&&frame<6500){
  Position far=Positions::Invalid;int dist=0;
  for(auto &[id,p]:buildings)if(p.getApproxDistance(home)>1400&&p.getApproxDistance(home)>dist){dist=p.getApproxDistance(home);far=p;}
  if(far.isValid())target=far;
  else for(auto t:Broodwar->getStartLocations())if(t!=Broodwar->self()->getStartLocation()&&!Broodwar->isExplored(t+TilePosition(2,1))){target=Position(t)+Position(64,48);break;}
 }
 for(auto u:World::own()) if(u->isCompleted()&&!u->getType().isBuilding()&&!u->getType().isWorker()&&u->getType()!=UnitTypes::Protoss_Scarab) army.push_back(u);
 if(!chokeRally.isValid()){rally=home+(rally-home)/3;chokeRally=rally;Position sum(0,0);int n=0;for(auto m:Broodwar->getStaticMinerals())if(m->getInitialPosition().getApproxDistance(home)<320){sum+=m->getInitialPosition();n++;}workerRally=n?(home+sum/n)/2:home;}
 int fighters=0;for(auto u:army)if(u->getType().canAttack())fighters++;
 rally=(frame<6500&&World::proxy.isValid())?Position(World::proxy)+Position(32,32):chokeRally;
 for(auto u:army)if(u->getType()==type(T::DT)&&u->isUnderAttack()){enemyDetection=true;for(auto a:army)if(a->getType()==type(T::DT)&&a->getDistance(u)<350)exposedUntil[a->getID()]=frame+240;}
 if(!launched){Position pos=Positions::Invalid;int nearest=999999;for(auto n:World::own())if(n->getType()==type(T::Nexus)&&n->getDistance(home)>600&&n->getDistance(home)<nearest){pos=n->getPosition();nearest=n->getDistance(home);}for(auto &[id,r]:builders)if(r.type==type(T::Nexus)){auto p=Position(r.tile)+Position(64,48);if(p.getApproxDistance(home)<nearest){pos=p;nearest=p.getApproxDistance(home);}}if(pos.isValid()){auto v=target-pos;double d=std::max(1.0,std::hypot(v.x,v.y));rally=pos+Position(int(v.x*144/d),int(v.y*144/d));}}
 if(fastGas&&!earlyMelee&&!cannonRush&&!launched&&w.count(T::Reaver)){
  Unit threatenedBase=nullptr;double urgency=0;
  for(auto n:World::own())if(n->getType()==type(T::Nexus)){
   double value=0;
   for(auto e:foes)if(!e->getType().isWorker()&&(e->getType().canAttack()||e->getType()==type(T::HT))&&n->getDistance(e)<600){
    value+=std::max(0,600-n->getDistance(e))*(e->getType()==type(T::Reaver)?3:e->getType()==type(T::HT)?2:1);
   }
   if(value>urgency){urgency=value;threatenedBase=n;}
  }
  if(threatenedBase){auto v=target-threatenedBase->getPosition();double d=std::max(1.0,std::hypot(v.x,v.y));rally=threatenedBase->getPosition()+Position(int(v.x*112/d),int(v.y*112/d));}
 }
 if(earlyMelee&&have(T::Nexus)<2)rally=workerRally;
 int zealots=w.count(T::Zealot), dragoons=w.count(T::Dragoon);
 if(((dragoons>=28&&w.count(T::HT)>=4&&Broodwar->self()->hasResearched(TechTypes::Psionic_Storm))||dragoons>=40)&&w.rangeDone())launched=true;
 if(zealots+dragoons<14)launched=false;
 // On three-start maps the first scout checks the remaining starts in order.
 if(frame<3600 && have(T::Gateway) && scoutTrips<1 && (!scout||!scout->exists()))
 for(auto u:World::own()) if(u->getType().isWorker()&&u!=World::forwardWorker&&!builders.count(u->getID())&&!u->isGatheringGas()){scout=u;scoutTrips++;break;}
 if(Broodwar->getStartLocations().size()>2&&have(T::Gateway)&&!secondProbeSent){
  for(auto u:World::own())if(u->getType().isWorker()&&u!=scout&&u!=homeGuard&&!builders.count(u->getID())&&!u->isGatheringGas()){secondProbeScout=u;secondProbeSent=true;break;}
 }
 if(secondProbeScout&&secondProbeScout->exists()){
  if(frame>4600||fastGas||earlyMelee||cannonRush||(knownStart.isValid()&&knownStart.getApproxDistance(secondScout)>500)){secondProbeScout=nullptr;}
  else{Position goal=secondScout;if(secondProbeScout->getDistance(goal)<240){int phase=(frame/144)%4;static const Position offsets[]={Position(160,0),Position(0,160),Position(-160,0),Position(0,-160)};goal=clamp(goal+offsets[phase]);}move(secondProbeScout,goal);}
 }
 if(scout&&scout->exists()&&frame<8000){
  Position goal=target;
  if(Broodwar->getStartLocations().size()>2&&!knownStart.isValid()){
   if(!Broodwar->isExplored(TilePosition(firstScout)))goal=firstScout;else goal=secondScout;
  }

  bool localScout=Broodwar->getStartLocations().size()<=2&&!scoutCheckpointDone&&scoutCheckpoint.isValid()&&frame<3400&&scout->getDistance(scoutCheckpoint)>96;
  if(!localScout)scoutCheckpointDone=true;
  if(localScout)goal=World::waypoint(scout->getPosition(),scoutCheckpoint);
  if(!localScout&&enemy.isValid()&&(!firstScout.isValid()||knownStart.isValid())){
   Position main=knownStart.isValid()?knownStart:enemy;int bd=999999;for(auto t:Broodwar->getStartLocations())if(t!=Broodwar->self()->getStartLocation()){auto p=Position(t)+Position(64,48);if(p.getApproxDistance(enemy)<bd){bd=p.getApproxDistance(enemy);main=p;}}
   static int phase=0;static const Position offsets[]={Position(190,0),Position(0,190),Position(-190,0),Position(0,-190)};
   goal=clamp(main+offsets[phase%4]);if(scout->getDistance(goal)<90)phase++;
   Unit danger=nullptr;for(auto e:foes)if(e->getType().canAttack()&&(!e->getType().isWorker()||e->getOrderTarget()==scout)&&scout->getDistance(e)<(e->getType().isWorker()?160:260)&&(!danger||scout->getDistance(e)<scout->getDistance(danger)))danger=e;
   if(danger){double best=-1e30;Position bestPos=scout->getPosition();
    for(int i=0;i<16;i++){double a=i*6.28318530718/16;Position q=clamp(scout->getPosition()+Position(int(std::cos(a)*128),int(std::sin(a)*128)));
     if(!World::clearGroundLine(scout->getPosition(),q)||!World::reachable(TilePosition(q)))continue;
     double safe=1000;for(auto e:foes)if(e->getType().canAttack()&&(!e->getType().isWorker()||e->getOrderTarget()==scout))safe=std::min(safe,double(e->getDistance(q))-(e->getType().isWorker()?0:100));
     double val=safe-q.getApproxDistance(goal)*.1;if(val>best){best=val;bestPos=q;}
    }goal=bestPos;
   }
  }
  move(scout,goal);
 }else scout=nullptr;
 militia.clear();
 if(!guardSent&&frame<3000&&have(T::Probe)>=11&&have(T::Gateway)){for(auto u:World::own())if(u->getType().isWorker()&&u!=scout&&u!=secondProbeScout&&!builders.count(u->getID())&&!u->isGatheringGas()){homeGuard=u;guardSent=true;break;}}
 if(homeGuard&&homeGuard->exists()&&!fastGas&&!earlyMelee&&!cannonRush&&frame<((enemyHomeGate||fastGas)?3000:4300)){
  auto v=Position(Broodwar->mapWidth()*16,Broodwar->mapHeight()*16)-home;double d=std::max(1.0,std::hypot(v.x,v.y));auto goal=clamp(home+Position(int(v.x*1056/d),int(v.y*1056/d)));bool danger=false;for(auto e:foes)if(e->getType().canAttack()&&!e->getType().isWorker()&&e->isCompleted()&&homeGuard->getDistance(e)<256)danger=true;
  if(danger){move(homeGuard,home);homeGuard=nullptr;}else{militia.insert(homeGuard->getID());move(homeGuard,World::waypoint(homeGuard->getPosition(),goal));}
 }else homeGuard=nullptr;
 for(auto e:foes)if(e->getType().isWorker()&&e->getOrderTarget()&&e->getOrderTarget()->getPlayer()==Broodwar->self()&&e->getOrderTarget()->getType().isWorker()){
  bool nearBase=false;for(auto n:World::own())if(n->getType()==type(T::Nexus)&&n->getDistance(e)<320)nearBase=true;if(!nearBase)continue;
  std::vector<Unit> defenders;for(auto u:World::own())if(u->getType().isWorker()&&u!=scout&&!builders.count(u->getID())&&!u->isGatheringGas()&&u->getDistance(e)<240&&u->getHitPoints()+u->getShields()>20)defenders.push_back(u);
  std::sort(defenders.begin(),defenders.end(),[&](Unit a,Unit b){return a->getDistance(e)<b->getDistance(e);});
  for(int i=0;i<std::min(3,int(defenders.size()));i++){auto u=defenders[i];militia.insert(u->getID());if(!u->isAttackFrame()&&!u->isStartingAttack()&&frame-u->getLastCommandFrame()>=8&&(u->getOrder()!=Orders::AttackUnit||u->getOrderTarget()!=e))u->attack(e);}
 }
 if(cannonRush&&frame<8000){
  Unit intruder=nullptr;
  for(auto e:foes)if(e->getType().isWorker()&&e->getDistance(home)<1200){
   bool covered=false;for(auto c:foes)if(c->getType()==type(T::Cannon)&&c->isCompleted()&&c->isPowered()&&c->getDistance(e)<256)covered=true;
   if(!covered&&(!intruder||e->getDistance(home)<intruder->getDistance(home)))intruder=e;
  }
  if(intruder){
   std::vector<Unit> chasers;
   for(auto u:World::own())if(u->getType().isWorker()&&!builders.count(u->getID())&&!u->isGatheringGas()&&u!=scout&&u->getHitPoints()+u->getShields()>20&&u->getDistance(intruder)<600)chasers.push_back(u);
   std::sort(chasers.begin(),chasers.end(),[&](Unit a,Unit b){return a->getDistance(intruder)<b->getDistance(intruder);});
   int count=std::min(2,std::max(0,int(chasers.size())-4));
   for(int i=0;i<count;i++){auto u=chasers[i];militia.insert(u->getID());if(!u->isAttackFrame()&&!u->isStartingAttack())attack(u,intruder);}
  }
 }
 if(frame<8000){
  Unit targetBuilding=nullptr;int score=999999;
  bool cannonThreat=false;for(auto e:foes)if(e->getType()==type(T::Cannon)&&e->getDistance(home)<1000)cannonThreat=true;
  for(auto e:foes)if((e->getType()==type(T::Cannon)||((cannonThreat||e->getDistance(home)<900)&&e->getType()==type(T::Pylon)))&&e->getDistance(home)<1000){
   bool danger=false;for(auto c:foes)if(c->getType()==type(T::Cannon)&&c->isCompleted()&&c->getDistance(e)<280)danger=true;
   if(danger)continue;int d=e->getDistance(home)+(e->getType()==type(T::Cannon)?-200:0);if(d<score){score=d;targetBuilding=e;}
  }
  if(targetBuilding){std::vector<Unit> workers;for(auto u:World::own())if(u->getType().isWorker()&&u!=scout&&!builders.count(u->getID())&&!militia.count(u->getID())&&!u->isGatheringGas())workers.push_back(u);
   std::sort(workers.begin(),workers.end(),[&](Unit a,Unit b){return a->getDistance(targetBuilding)<b->getDistance(targetBuilding);});
   int support=0;for(auto a:army)if((a->getType()==type(T::Zealot)||a->getType()==type(T::Dragoon))&&a->getDistance(targetBuilding)<320)support++;int count=std::min(std::max(2,6-support*2),std::max(0,int(workers.size())-4));for(int i=0;i<count;i++){auto u=workers[i];militia.insert(u->getID());if(!u->isAttackFrame()&&!u->isStartingAttack()&&frame-u->getLastCommandFrame()>=8&&(u->getOrder()!=Orders::AttackUnit||u->getOrderTarget()!=targetBuilding))u->attack(targetBuilding);}
  }
 }

 if(World::proxy.isValid()&&frame<6500&&(!World::forwardWorker||!World::forwardWorker->exists())){Unit best=nullptr;for(auto u:World::own())if(u->getType().isWorker()&&u!=scout&&!u->isGatheringGas()&&u->getHitPoints()+u->getShields()>25&&(!best||u->getDistance(Position(World::proxy))<best->getDistance(Position(World::proxy))))best=u;World::forwardWorker=best;}
 if(World::forwardWorker&&World::forwardWorker->exists()&&frame<6500){
  auto u=World::forwardWorker;militia.insert(u->getID());Unit danger=nullptr;
  for(auto e:foes)if(e->getType().canAttack()&&(!e->getType().isWorker()||e->getOrderTarget()==u)&&u->getDistance(e)<220&&(!danger||u->getDistance(e)<u->getDistance(danger)))danger=e;
  if(danger&&(!builders.count(u->getID())||u->getHitPoints()+u->getShields()<18)){
   builders.erase(u->getID());auto v=u->getPosition()-danger->getPosition();double angle=std::atan2(v.y,v.x),best=-1e9;Position goal=home;
   for(int i=-4;i<=4;i++){double a=angle+i*.35;auto p=clamp(u->getPosition()+Position(int(std::cos(a)*160),int(std::sin(a)*160)));if(!World::reachable(TilePosition(p)))continue;bool free=true;for(auto b:Broodwar->getAllUnits())if(b->getType().isBuilding()&&b->getDistance(p)<24)free=false;if(!free)continue;double score=std::cos(i*.35)*100+danger->getDistance(p)*.5;if(score>best){best=score;goal=p;}}
   move(u,goal);
  }else if(!builders.count(u->getID())&&!u->isConstructing()){
   auto proxy=Position(World::proxy)+Position(32,32);auto v=home-proxy;double d=std::max(1.0,std::hypot(v.x,v.y));auto goal=(!knownStart.isValid()&&firstScout.isValid()&&frame<1800)?firstScout:proxy+Position(int(v.x*160/d),int(v.y*160/d));move(u,World::waypoint(u->getPosition(),goal));
  }
 }


 int enemyZealots=0;for(auto e:foes)if(e->getType()==type(T::Zealot))enemyZealots++;
 if(frame<5000&&enemyGatesSeen.size()>=2&&!enemyGasSeen)earlyMelee=true;
 if(!fastGas&&frame<7200&&enemyZealots>=2) {int close=0;for(auto e:foes)if(e->getType()==type(T::Zealot)&&e->getDistance(home)<1400)close++;if(close>=(frame<3800?2:3))earlyMelee=true;}

 if(frame<6000&&proxyPowerLost>0){
  int unpowered=0,powered=0,closeZealots=0;
  for(auto e:foes){
   if(e->getType()==type(T::Gateway)&&e->getDistance(home)<1600){if(e->isPowered())powered++;else unpowered++;}
   if(e->getType()==type(T::Zealot)&&e->getDistance(home)<1400)closeZealots++;
  }
  if(frame%240==0)std::printf("PROXY POWER CHECK f=%d unpowered=%d powered=%d zealots=%d\n",frame,unpowered,powered,closeZealots);
  if(powered||closeZealots>=2)proxyDisarmed=false;
  else if(unpowered&&closeZealots==0&&!proxyDisarmed){proxyDisarmed=true;std::printf("PROXY DEPOWERED f=%d gates=%d\n",frame,unpowered);}
  if(proxyDisarmed)earlyMelee=false;
 }
 if(cannonRush)earlyMelee=false;
 bool imminentRush=false;for(auto e:foes)if((e->getType()==type(T::Gateway)&&e->getDistance(home)<1600)||(e->getType()==type(T::Zealot)&&e->getDistance(home)<700))imminentRush=true;
 if(earlyMelee&&imminentRush&&frame<4000&&have(T::Cannon)<2)for(auto u:World::own())if(u->getType()==type(T::Core)&&!u->isCompleted())u->cancelConstruction();
 World::mineralDefense=earlyMelee||dtSeen;
 int defenseZealots=(fastGas||earlyMelee||cannonRush)?2:1;
 int threatening=0; for(auto e:threats) if(e->isDetected()&&!e->isInvincible()&&!e->getType().isWorker()&&!e->getType().isBuilding()&&e->getDistance(earlyMelee?workerRally:home)<(earlyMelee?192:300)&&e->getType().groundWeapon().maxRange()<64) threatening++;
 int homeCannons=0;for(auto u:World::own())if(u->getType()==type(T::Cannon)&&u->isCompleted()&&u->isPowered()&&u->getDistance(home)<300)homeCannons++;
 if(homeCannons){threatening=0;for(auto e:threats)if(e->isDetected()&&!e->isInvincible()&&!e->getType().isWorker()&&!e->getType().isBuilding()&&e->getDistance(earlyMelee?workerRally:home)<128&&e->getType().groundWeapon().maxRange()<64)threatening++;}
 std::vector<Unit> workerThreats;
 for(auto e:threats)if(e->isDetected()&&!e->isInvincible()&&!e->getType().isWorker()&&!e->getType().isBuilding()&&e->getType().groundWeapon().maxRange()<64&&e->getDistance(earlyMelee?workerRally:home)<(homeCannons?128:earlyMelee?192:300))workerThreats.push_back(e);
 int nearArmy=0;for(auto u:army) if(u->getDistance(home)<480) nearArmy++;
 if(threatening>0 && nearArmy< threatening*2+1) {
  std::vector<Unit> ws; for(auto u:World::own()) if(u->getType().isWorker()&&!builders.count(u->getID())&&u!=scout&&u->getDistance(home)<500) ws.push_back(u);
  std::sort(ws.begin(),ws.end(),[&](Unit a,Unit b){int da=99999,db=99999;for(auto e:workerThreats){da=std::min(da,a->getDistance(e));db=std::min(db,b->getDistance(e));}return da<db;});
  int n=std::min(std::max(0,(int)ws.size()-4),threatening*4+2);
  for(int i=0;i<n;i++) {auto u=ws[i];militia.insert(u->getID());Unit e=nullptr;for(auto v:workerThreats)if(!e||u->getDistance(v)<u->getDistance(e))e=v;
   if(u->isAttackFrame()||u->isStartingAttack())continue;
   if(u->getHitPoints()<=10&&u->getShields()<5) {Unit m=nullptr;double score=-1e9;for(auto v:World::ordered(Broodwar->getMinerals()))if(v->getDistance(home)<600){double val=(e?v->getDistance(e):v->getDistance(home))-.3*u->getDistance(v);if(val>score){score=val;m=v;}}if(m)u->gather(m);}else if(e&&frame-u->getLastCommandFrame()>=8&&(u->getOrder()!=Orders::AttackUnit||u->getOrderTarget()!=e))u->attack(e);
  }
 }
 if(scout&&scout->exists())militia.insert(scout->getID());
 if(secondProbeScout&&secondProbeScout->exists())militia.insert(secondProbeScout->getID());
 for(auto u:World::own())if(u->getType().isWorker()&&evadeStorm(u))militia.insert(u->getID());
 // Economy: gas is staffed separately, mineral assignments favor nearby unsaturated patches.
 std::vector<Unit> gases,nexuses; std::map<int,int> miners;
 for(auto u:World::own()) {if(u->isCompleted()&&u->getType().isRefinery()&&World::homeGas(u))gases.push_back(u);if(u->isCompleted()&&u->getType().isResourceDepot())nexuses.push_back(u);if(u->getOrderTarget()&&!u->isGatheringGas())miners[u->getOrderTarget()->getID()]++;}
 for(auto it=gasWorkers.begin();it!=gasWorkers.end();) {
  Unit u=Broodwar->getUnit(it->first),g=Broodwar->getUnit(it->second);
  if(!u||!u->exists()||!g||!g->exists()||!World::homeGas(g)||builders.count(it->first)||militia.count(it->first))it=gasWorkers.erase(it);else ++it;
 }
 int availableWorkers=0;for(auto u:World::own())if(u->getType().isWorker()&&u!=scout&&!builders.count(u->getID())&&!militia.count(u->getID()))availableWorkers++;
 int gasCap=std::min(w.gas>800?1:3,std::max(0,(availableWorkers-4)/std::max(1,int(gases.size()))));
 if(earlyMelee&&!cannonRush&&!have(T::Core))gasCap=0;
 for(auto g:gases) {
  std::vector<int> assigned;for(auto [id,gid]:gasWorkers)if(gid==g->getID())assigned.push_back(id);
  while((int)assigned.size()>gasCap){auto id=assigned.back();assigned.pop_back();gasWorkers.erase(id);auto u=Broodwar->getUnit(id);if(u)u->stop();}
  while((int)assigned.size()<gasCap){Unit best=nullptr;for(auto u:World::own())if(u->getType().isWorker()&&u!=scout&&!gasWorkers.count(u->getID())&&!builders.count(u->getID())&&!militia.count(u->getID())&&!u->isConstructing()&&!u->isCarryingMinerals()&&!u->isCarryingGas()&&(!best||u->getDistance(g)<best->getDistance(g)))best=u;
   if(!best)break;gasWorkers[best->getID()]=g->getID();assigned.push_back(best->getID());best->gather(g);
  }
 }
 miners.clear();
 for(auto it=mineralWorkers.begin();it!=mineralWorkers.end();) {
  Unit u=Broodwar->getUnit(it->first),m=Broodwar->getUnit(it->second);
  bool activePatch=false;if(m)for(auto n:nexuses)if(n->getDistance(m)<360)activePatch=true;
  if(!u||!u->exists()||!m||!m->exists()||!activePatch||m->getResources()==0||gasWorkers.count(it->first)||builders.count(it->first)||militia.count(it->first)||u==scout)it=mineralWorkers.erase(it);else {miners[it->second]++;++it;}
 }
 for(auto u:World::own()) {
  if(!u->getType().isWorker()||u==scout||builders.count(u->getID())||militia.count(u->getID())||u->isConstructing())continue;
  if(gasWorkers.count(u->getID())){auto g=Broodwar->getUnit(gasWorkers[u->getID()]);if(g && !u->isGatheringGas()&&!u->isCarryingGas())u->gather(g);continue;}
  if(u->isCarryingMinerals()||u->isCarryingGas()){
   auto o=u->getOrder();if(u->isIdle()||o==Orders::AttackUnit||o==Orders::AttackMove||o==Orders::Move||o==Orders::HoldPosition||o==Orders::Patrol)u->returnCargo();continue;
  }
  Unit old=mineralWorkers.count(u->getID())?Broodwar->getUnit(mineralWorkers[u->getID()]):nullptr;
  bool rebalance=frame%120==0 && old && miners[old->getID()]>2;
  if(old&&!rebalance) {if(u->isIdle()||!u->isGatheringMinerals()||(u->getOrderTarget()!=old&&u->getOrder()!=Orders::MiningMinerals))u->gather(old);continue;}
  if(old)miners[old->getID()]--;
  Unit best=nullptr;double bs=1e9;
  for(auto m:World::ordered(Broodwar->getMinerals())) {int baseDist=100000;for(auto n:nexuses)baseDist=std::min(baseDist,n->getDistance(m));if(baseDist>360)continue;
   double score=miners[m->getID()]*220+u->getDistance(m)*.15+baseDist*.25;if(score<bs){bs=score;best=m;}}
  if(best){mineralWorkers[u->getID()]=best->getID();miners[best->getID()]++;if(best!=old||u->isIdle())u->gather(best);}
 }
 int invaders=0;for(auto e:threats)if(!e->getType().isWorker()&&(!e->getType().isBuilding()||e->getType()==type(T::Cannon)))invaders++;
 int ammoBank=Broodwar->self()->minerals();for(auto u:World::own())if(u->getType()==type(T::Reaver)&&u->isCompleted()&&u->getScarabCount()<5&&!u->isTraining()&&ammoBank>=15&&(u->getScarabCount()==0||w.minerals>=15))if(u->train(UnitTypes::Protoss_Scarab)){w.minerals-=15;ammoBank-=15;}
 if(frame>20000&&have(T::Probe)>=30&&w.pending(T::Nexus)==0){
  int patches=0,resources=0,usefulBases=0;
  for(auto n:nexuses){int remaining=0;for(auto m:Broodwar->getMinerals())if(m->getResources()>64&&n->getDistance(m)<360){patches++;resources+=m->getResources();remaining+=m->getResources();}if(remaining>1800)usefulBases++;}
  if(usefulBases<3&&(have(T::Probe)>2*patches+3*have(T::Gas)+4||resources<6000)&&World::expansion(type(T::Nexus)).isValid()){w.build(T::Nexus);if(w.pending(T::Nexus)==0)w.minerals-=400;}
 }
 bool reaverPlan=fastGas&&!earlyMelee&&!cannonRush&&!proxyDisarmed;
 w.allowMineralBuildings=reaverPlan;World::allowEarlyTemplarGas=!reaverPlan&&!earlyMelee&&!cannonRush;
 bool loadedReaver=false;for(auto u:World::own())if(u->getType()==type(T::Reaver)&&u->isCompleted()&&u->getScarabCount()>0)loadedReaver=true;

 if(reaverPlan){
  int buffer=have(T::Pylon)?2+2*w.count(T::Gateway)+2*w.count(T::Nexus):6;
  if(w.supplyLeft()+8*w.pending(T::Pylon)<buffer)w.build(T::Pylon);
  if(have(T::Pylon)&&!have(T::Gateway)&&have(T::Probe)>=9)w.build(T::Gateway);
  if(have(T::Gateway)&&have(T::Probe)>=10&&!have(T::Gas))w.build(T::Gas);
  if(have(T::Gas)&&!have(T::Core)){w.build(T::Core);if(!have(T::Core))w.minerals-=200;}
  if(w.count(T::Gateway)&&!w.count(T::Core)&&!have(T::Zealot))w.train(T::Zealot);
  if(w.count(T::Core)&&!w.rangeStarted()){
   if(w.minerals>=150&&w.gas>=150)w.researchRange();w.minerals-=150;w.gas-=150;
  }
  if(w.count(T::Core)&&have(T::Dragoon)<1)w.train(T::Dragoon);
  if(w.rangeStarted()&&have(T::Robo)&&have(T::Gateway)<2){w.build(T::Gateway);if(have(T::Gateway)<2)w.minerals-=150;}
  if((have(T::Dragoon)>=2||dtSeen)&&!have(T::Robo)){w.build(T::Robo);if(!have(T::Robo)){w.minerals-=200;w.gas-=100;}}
  if(w.count(T::Robo)&&!have(T::Observatory)){w.build(T::Observatory);if(!have(T::Observatory)){w.minerals-=50;w.gas-=100;}}
  if(w.count(T::Observatory)&&have(T::Observer)<1){w.train(T::Observer);if(!have(T::Observer)){w.minerals-=25;w.gas-=75;}}
  if(have(T::Dragoon)>=4&&w.count(T::Robo)&&!have(T::ReaverBay)){w.build(T::ReaverBay);if(!have(T::ReaverBay)){w.minerals-=150;w.gas-=100;}}
  if(w.count(T::ReaverBay)&&have(T::Reaver)<1){w.train(T::Reaver);if(!have(T::Reaver)){w.minerals-=200;w.gas-=100;}}
  if(have(T::Nexus)<2&&w.count(T::Dragoon)>=4&&have(T::Observer)&&have(T::Reaver)&&w.rangeDone()){
   w.build(T::Nexus);if(have(T::Nexus)<2&&World::expansion(type(T::Nexus)).isValid())w.minerals-=400;
  }
  if(w.count(T::Nexus)>1&&have(T::Gas)<w.count(T::Nexus)){w.build(T::Gas);if(have(T::Gas)<w.count(T::Nexus))w.minerals-=100;}
  if(have(T::Nexus)>=2&&have(T::Reaver)>=1&&!have(T::Shuttle)){w.train(T::Shuttle);if(!have(T::Shuttle))w.minerals-=200;}
  if(have(T::Nexus)>=2&&have(T::Shuttle)&&w.count(T::ReaverBay)&&have(T::Reaver)>=1&&have(T::Reaver)<2){w.train(T::Reaver);if(have(T::Reaver)<2){w.minerals-=200;w.gas-=100;}}
  if(w.count(T::Nexus)>=2&&w.count(T::Dragoon)>=8&&!have(T::Citadel)){w.build(T::Citadel);if(!have(T::Citadel)){w.minerals-=150;w.gas-=100;}}
  if(w.count(T::Citadel)&&!have(T::Archives)){w.build(T::Archives);if(!have(T::Archives)){w.minerals-=150;w.gas-=200;}}
  if(w.count(T::Archives)){
   if(!Broodwar->self()->hasResearched(TechTypes::Psionic_Storm)&&!Broodwar->self()->isResearching(TechTypes::Psionic_Storm)){
    if(w.minerals>=200&&w.gas>=200)for(auto u:World::own())if(u->getType()==type(T::Archives)&&u->research(TechTypes::Psionic_Storm))break;
    w.minerals-=200;w.gas-=200;
   }
   int want=w.count(T::Dragoon)<12?2:w.count(T::Nexus)>=3?6:4;if(have(T::HT)<want){w.train(T::HT);if(have(T::HT)<want){w.minerals-=50;w.gas-=150;}}
  }
  if(have(T::HT)>=2&&have(T::Dragoon)>=12&&have(T::Probe)>=36&&have(T::Nexus)<3){w.build(T::Nexus);if(have(T::Nexus)<3&&World::expansion(type(T::Nexus)).isValid())w.minerals-=400;}
  if((have(T::Nexus)>=2||dtSeen)&&!have(T::Forge))w.build(T::Forge);
  if(w.count(T::Forge)&&have(T::Cannon)<have(T::Nexus))w.build(T::Cannon);
  if(w.count(T::Observatory)&&have(T::Observer)<(launched?3:1))w.train(T::Observer);
  for(int i=0;i<w.count(T::Gateway);i++)if(w.count(T::Core)&&!w.train(T::Dragoon)&&w.minerals>300)w.train(T::Zealot);
  if(have(T::Probe)<std::min(64,22*std::max(1,w.count(T::Nexus))))w.train(T::Probe);
  if(have(T::Nexus)>=2&&w.minerals>=200&&have(T::Gateway)<std::min(10,3*w.count(T::Nexus)))w.build(T::Gateway);
  if(have(T::Reaver)>=2&&w.count(T::Nexus)>=2&&!have(T::Shuttle))w.train(T::Shuttle);
  if(have(T::HT)>=2&&w.minerals>200&&w.gas>150&&w.count(T::Forge))for(auto u:World::own())if(u->getType()==type(T::Forge)&&!u->isUpgrading())u->upgrade(UpgradeTypes::Protoss_Ground_Weapons);
  if(have(T::HT)>=4&&have(T::Dragoon)>=24&&w.minerals>600&&have(T::Nexus)<4)w.build(T::Nexus);
 }else{
 // Dark Templars cover a second base while the army gains Storm.
 bool standardTemplars=!reaverPlan&&!earlyMelee&&!cannonRush;
 int initialDTs=standardTemplars?1:2;
 if(standardTemplars){int gasBases=0;for(auto n:World::own())if(n->getType()==type(T::Nexus)&&(n->isCompleted()||n->getRemainingBuildTime()<=720))gasBases++;
  if(have(T::Gas)<gasBases&&have(T::Gas)>=1){w.build(T::Gas);if(have(T::Gas)<gasBases)w.minerals-=100;}
 }
 if(frame>2500&&have(T::Probe)<8)w.train(T::Probe);
 if(w.supplyLeft()+8*w.pending(T::Pylon)<4+2*have(T::Gateway)+2*have(T::Nexus))w.build(T::Pylon);
 if(have(T::Pylon)&&have(T::Gateway)<1&&have(T::Probe)>=9)w.build(T::Gateway);
 if(cannonRush&&w.count(T::Core)&&have(T::Dragoon)>=2&&!w.rangeStarted()){if(w.minerals>=150&&w.gas>=150)w.researchRange();w.minerals-=150;w.gas-=150;}
 if(earlyMelee){
  if(w.count(T::Core)&&have(T::Reaver)&&!w.rangeStarted()){if(w.minerals>=150&&w.gas>=150)w.researchRange();w.minerals-=150;w.gas-=150;}
  if(w.count(T::Gateway)&&!w.count(T::Core)&&have(T::Zealot)<1){w.train(T::Zealot);if(have(T::Zealot)<1)w.minerals-=100;}
  if(!have(T::Forge)){w.build(T::Forge);if(!have(T::Forge))w.minerals-=150;}
  if(have(T::Forge)&&have(T::Cannon)<2){if(w.count(T::Forge))w.build(T::Cannon);if(have(T::Cannon)<2)w.minerals-=150;}
 }
 if(have(T::Probe)>=10&&have(T::Gateway)&&(!earlyMelee||have(T::Cannon)>=2)){
  if(!have(T::Gas))w.build(T::Gas);
  if(have(T::Gas)&&!have(T::Core)){w.build(T::Core);if(!have(T::Core))w.minerals-=200;}
 }
 
 if(w.count(T::Gateway)&&have(T::Zealot)<defenseZealots&&(!(earlyMelee||cannonRush)||!w.count(T::Core))&&(!have(T::Citadel)||earlyMelee)){w.train(T::Zealot);if(earlyMelee&&have(T::Zealot)<defenseZealots)w.minerals-=100;}
 
 
 bool expansionPower=false;for(auto n:World::own())if(n->getType()==type(T::Nexus)&&n->getDistance(home)>500){bool powered=false;for(auto p:World::own())if(p->getType()==type(T::Pylon)&&p->getDistance(n)<256)powered=true;for(auto &[id,r]:builders)if(r.type==type(T::Pylon)&&(Position(r.tile)+Position(32,32)).getApproxDistance(n->getPosition())<320)powered=true;if(!powered)expansionPower=true;}
 if(expansionPower){w.build(T::Pylon);}
 if(!reaverPlan&&!cannonRush&&w.count(T::Archives)&&dtMade+w.pending(T::DT)>=initialDTs&&(have(T::Nexus)>=2||expansionFailed)){
  if(!Broodwar->self()->hasResearched(TechTypes::Psionic_Storm)&&!Broodwar->self()->isResearching(TechTypes::Psionic_Storm)){
   if(w.minerals>=200&&w.gas>=200)for(auto u:World::own())if(u->getType()==type(T::Archives)&&u->isCompleted())if(u->research(TechTypes::Psionic_Storm))break;
   w.minerals-=200;w.gas-=200;
  }
  if(have(T::HT)<2){w.train(T::HT);if(have(T::HT)<2){w.minerals-=50;w.gas-=150;}}
 }
 if(!reaverPlan&&!cannonRush&&have(T::HT)>=2&&have(T::Dragoon)>=4&&!w.rangeStarted()){
  if(w.minerals>=150&&w.gas>=150)w.researchRange();w.minerals-=150;w.gas-=150;
 }
 if((have(T::Nexus)>=2||dtSeen)&&!have(T::Forge)){w.build(T::Forge);if(!have(T::Forge))w.minerals-=150;}
 if(w.count(T::Forge)&&have(T::Cannon)<(have(T::Nexus)+(earlyMelee?1:0))){w.build(T::Cannon);if(have(T::Cannon)<(have(T::Nexus)+(earlyMelee?1:0)))w.minerals-=150;}
 
 if(reaverPlan&&w.count(T::Core)){
  if(have(T::Dragoon)<1)w.train(T::Dragoon);
  if(have(T::Dragoon)>=1&&!have(T::Robo)){w.build(T::Robo);if(!have(T::Robo)){w.minerals-=200;w.gas-=100;}}
  if(w.count(T::Robo)&&!have(T::ReaverBay)){w.build(T::ReaverBay);if(!have(T::ReaverBay)){w.minerals-=150;w.gas-=100;}}
  if(w.count(T::ReaverBay)&&have(T::Reaver)<2&&(have(T::Reaver)<1||have(T::Observer))){w.train(T::Reaver);if(!have(T::Reaver)){w.minerals-=200;w.gas-=100;}}
  if(have(T::ReaverBay)&&have(T::Gateway)<2)w.build(T::Gateway);
  if(have(T::Reaver)>=2&&have(T::Observer)&&!have(T::Shuttle))w.train(T::Shuttle);
 }
 if(dtSeen||(!reaverPlan&&w.count(T::Nexus)>=2&&have(T::Dragoon)>=6)){
  if(!have(T::Robo)){w.build(T::Robo);if(!have(T::Robo)){w.minerals-=200;w.gas-=100;}}
  if(w.count(T::Robo)&&!have(T::Observatory)){w.build(T::Observatory);if(!have(T::Observatory)){w.minerals-=50;w.gas-=100;}}
  if(w.count(T::Observatory)&&!have(T::Observer)){w.train(T::Observer);if(!have(T::Observer)){w.minerals-=25;w.gas-=75;}}
 }
 int cap=have(T::Gateway)<1?9:(earlyMelee&&have(T::Cannon)<2)?13:(!have(T::Archives)&&!cannonRush&&!reaverPlan)?16:std::min(64,22*std::max(1,w.count(T::Nexus)));
 if(have(T::Probe)<cap)w.train(T::Probe);
 if((!cannonRush&&!reaverPlan||(have(T::Nexus)>=2&&have(T::Dragoon)>=8))&&w.count(T::Core)&&!have(T::Citadel)){w.build(T::Citadel);if(!have(T::Citadel)){w.minerals-=150;w.gas-=100;}}
 if(w.count(T::Citadel)&&!have(T::Archives)){w.build(T::Archives);if(!have(T::Archives)){w.minerals-=150;w.gas-=200;}}
 if((have(T::Archives)||(cannonRush&&have(T::Core)))&&have(T::Gateway)<2)w.build(T::Gateway);
 if(!reaverPlan&&!cannonRush&&w.count(T::Archives)&&dtMade+w.pending(T::DT)<initialDTs){w.train(T::DT);if(dtMade+w.pending(T::DT)<initialDTs){w.minerals-=125;w.gas-=100;}}
 if(((reaverPlan&&((!earlyMelee&&w.count(T::Reaver)>=1&&loadedReaver&&have(T::Dragoon)>=6&&invaders<=2&&(!dtSeen||w.count(T::Observer)>=1))||(earlyMelee&&w.count(T::Reaver)>=2&&have(T::Dragoon)>=8)))||(cannonRush&&have(T::Dragoon)>=8&&invaders==0)||dtMade+w.pending(T::DT)>=initialDTs)&&have(T::Nexus)<2){w.build(T::Nexus);if(have(T::Nexus)<2&&World::expansion(type(T::Nexus)).isValid())w.minerals-=400;}
 if(w.count(T::Nexus)>1&&have(T::Gas)<w.count(T::Nexus)){w.build(T::Gas);if(have(T::Gas)<w.count(T::Nexus))w.minerals-=100;}
 if(w.count(T::Forge)&&w.count(T::Nexus)>=2&&have(T::Dragoon)>=2&&!Broodwar->self()->getUpgradeLevel(UpgradeTypes::Protoss_Ground_Weapons)&&!Broodwar->self()->isUpgrading(UpgradeTypes::Protoss_Ground_Weapons)){
  if(w.minerals>=100&&w.gas>=100)for(auto u:World::own())if(u->getType()==type(T::Forge)&&u->upgrade(UpgradeTypes::Protoss_Ground_Weapons))break;
  w.minerals-=100;w.gas-=100;
 }
 if(w.count(T::Archives)&&have(T::Dragoon)>=10&&w.count(T::Nexus)>=2){
  if(!Broodwar->self()->hasResearched(TechTypes::Psionic_Storm)&&!Broodwar->self()->isResearching(TechTypes::Psionic_Storm)){
   if(w.minerals>=200&&w.gas>=200)for(auto u:World::own())if(u->getType()==type(T::Archives)&&u->isCompleted())if(u->research(TechTypes::Psionic_Storm))break;
   w.minerals-=200;w.gas-=200;
  }
  int desiredHT=frame<11500?2:(w.count(T::Nexus)>=3?6:4);if(have(T::HT)<desiredHT){w.train(T::HT);if(have(T::HT)<desiredHT){w.minerals-=50;w.gas-=150;}}
 }
 if(frame>14000&&have(T::Probe)>=36&&have(T::HT)>=2&&have(T::Dragoon)>=10&&w.count(T::Nexus)>=2&&have(T::Nexus)<3){w.build(T::Nexus);if(have(T::Nexus)<3&&World::expansion(type(T::Nexus)).isValid())w.minerals-=400;}

 
 if(have(T::HT)>=2&&have(T::Zealot)>=8&&w.count(T::Citadel)&&w.minerals>=150&&w.gas>=150&&!Broodwar->self()->getUpgradeLevel(UpgradeTypes::Leg_Enhancements)&&!Broodwar->self()->isUpgrading(UpgradeTypes::Leg_Enhancements))for(auto u:World::own())if(u->getType()==type(T::Citadel)&&u->upgrade(UpgradeTypes::Leg_Enhancements)){w.minerals-=150;w.gas-=150;break;}
 if((dtSeen||have(T::Dragoon)>=6)&&!have(T::Robo))w.build(T::Robo);
 if(w.count(T::Robo)&&(!reaverPlan||have(T::Reaver))&&!have(T::Observatory))w.build(T::Observatory);
 if(w.count(T::Observatory)&&have(T::Observer)<((launched&&frame>20000)?3:1))w.train(T::Observer);
 if(((reaverPlan&&have(T::Reaver))||dtMade+w.pending(T::DT)>=initialDTs||cannonRush||(earlyMelee&&have(T::Reaver)))&&!w.rangeStarted()&&w.minerals>=150&&w.gas>=150){w.researchRange();w.minerals-=150;w.gas-=150;}
 if(have(T::Dragoon)>=(standardTemplars?4:1)&&(!reaverPlan||have(T::Nexus)>=2)&&have(T::Gateway)<3)w.build(T::Gateway);
 for(int i=0;i<w.count(T::Gateway);i++){
  if((reaverPlan&&(have(T::Dragoon)<1||have(T::Robo)))||dtMade+w.pending(T::DT)>=initialDTs||cannonRush){if(!w.train(T::Dragoon)&&w.minerals>300)w.train(T::Zealot);}
 }
 if((!standardTemplars||have(T::Dragoon)>=4)&&have(T::Nexus)>=2&&w.minerals>=200&&have(T::Gateway)<std::min(10,4*w.count(T::Nexus)))w.build(T::Gateway);
 if(have(T::HT)>=4&&have(T::Dragoon)>=24&&w.minerals>600&&have(T::Nexus)<4)w.build(T::Nexus);
 if(have(T::HT)>=2&&w.minerals>200&&w.gas>150&&w.count(T::Forge))for(auto u:World::own())if(u->getType()==type(T::Forge)&&!u->isUpgrading())u->upgrade(UpgradeTypes::Protoss_Ground_Weapons);

 }
 // Estimate local engagements; reinforcements should not feed a defended choke one at a time.
 if(frame%24==0){std::vector<std::pair<Position,bool>> evaluations;
 for(auto u:army)if(u->getType().canAttack()) {
  bool reused=false;for(auto &[p,r]:evaluations)if(u->getDistance(p)<128){retreat[u->getID()]=r;reused=true;break;}if(reused)continue;
  FAP::FastAPproximation<int> sim; float a0=0,b0=0; int es=0;
  for(auto a:army)if(a->getType().canAttack()&&!a->isLoaded()&&a->getDistance(u)<480){sim.addIfCombatUnitPlayer1(simUnit(a));a0+=a->getHitPoints()+a->getShields();}
  for(auto a:World::own())if(a->getType()==type(T::Cannon)&&a->isCompleted()&&a->isPowered()&&a->getDistance(u)<448){sim.addIfCombatUnitPlayer1(simUnit(a));a0+=a->getHitPoints()+a->getShields();}
  for(auto e:foes)if(e->isCompleted()&&e->isDetected()&&e->getType().canAttack()&&(e->getType()!=type(T::Cannon)||e->isPowered())&&(!e->getType().isWorker()||e->isAttacking()||e->getOrder()==Orders::AttackUnit)&&e->getDistance(u)<560){sim.addIfCombatUnitPlayer2(simUnit(e));b0+=e->getHitPoints()+e->getShields();es++;}
  int remembered=0;std::map<int,int> cargoCount;
  for(auto &[id,m]:reaverMemory)if(!visibleReavers.count(id)&&m.shuttle>=0&&cargoCount[m.shuttle]<2){
   Unit carrier=nullptr;for(auto e:foes)if(e->getID()==m.shuttle&&e->getType()==type(T::Shuttle))carrier=e;
   if(!carrier||carrier->getDistance(u)>=560)continue;auto v=m.unit;v.x=carrier->getPosition().x;v.y=carrier->getPosition().y;v.elevation=-1;v.attackCooldownRemaining=std::max(12,v.attackCooldownRemaining-(frame-m.frame));
   sim.getState().second->push_back(v);b0+=(v.health+v.shields)/256.f;es++;remembered++;cargoCount[m.shuttle]++;
  }
  if(remembered&&frame%240==0&&memoryLogFrame!=frame){std::printf("REAVER CARGO ESTIMATE f=%d count=%d\n",frame,remembered);memoryLogFrame=frame;}
  retreat[u->getID()]=false;
  int spellSupport=0;for(auto a:army)if(Broodwar->self()->hasResearched(TechTypes::Psionic_Storm)&&a->getType()==type(T::HT)&&a->getEnergy()>=75&&a->getDistance(u)<600)spellSupport++;
  for(auto &[p,until]:storms)if(until>frame&&u->getDistance(p)<500)spellSupport++;
  bool staticThreat=false;for(auto e:foes)if(e->getType()==type(T::Cannon)&&e->isCompleted()&&e->isPowered()&&e->getDistance(u)<400)staticThreat=true;
  if(es&&(u->getDistance(home)>500||staticThreat)){for(auto group:{sim.getState().first,sim.getState().second})for(auto &v:*group)if(v.unitType==type(T::Reaver)){v.groundMaxRangeSquared=256*256;v.groundCooldown=60;}simulateCombat(sim,192);auto st=sim.getState();float a1=0,b1=0;for(auto &a:*st.first)a1+=(a.health+a.shields)/256.f;for(auto &b:*st.second)b1+=(b.health+b.shields)/256.f;retreat[u->getID()]=((a1<a0*.60f && a0-a1>(b0-b1)*1.10f) || (a1<a0*.15f && b1>b0*.20f));}
  evaluations.emplace_back(u->getPosition(),retreat[u->getID()]);
 }
 }
 bool clearingContain=cannonRush&&have(T::Nexus)<2&&w.count(T::Dragoon)>=8&&w.rangeDone()&&target.isValid()&&target.getApproxDistance(home)<1800;
 // Fight nearby threats with individual target choice and Dragoon cooldown movement.
 Position center=home; if(!army.empty()){center=Position(0,0);int n=0;for(auto u:army)if(u->getType()!=type(T::Observer)){center+=u->getPosition();n++;}if(n)center/=n;else center=home;}
 Position group=home;int most=0;
 for(auto a:army)if(a->getType().canAttack()){int n=0;Position sum(0,0);for(auto b:army)if(b->getType().canAttack()&&a->getDistance(b)<320){sum+=b->getPosition();n++;}if(n>most){most=n;group=sum/n;}}
 storms.erase(std::remove_if(storms.begin(),storms.end(),[&](auto &x){return x.second<frame;}),storms.end());
 std::vector<Position> observerTargets;
 std::map<int,double> committed;
 auto damage=[&](Unit a,Unit b){auto weapon=b->isFlying()?a->getType().airWeapon():a->getType().groundWeapon();double d=a->getPlayer()->damage(weapon);if(b->getShields()<=0){if(weapon.damageType()==DamageTypes::Explosive){if(b->getType().size()==UnitSizeTypes::Small)d*=.5;else if(b->getType().size()==UnitSizeTypes::Medium)d*=.75;}d-=b->getPlayer()->armor(b->getType());}return std::max(1.0,d)*(b->isFlying()?a->getType().maxAirHits():a->getType().maxGroundHits());};
 for(auto b:Broodwar->getBullets())if(b->exists()&&b->getSource()&&b->getTarget()&&b->getSource()->getPlayer()==Broodwar->self())committed[b->getTarget()->getID()]+=damage(b->getSource(),b->getTarget());

 std::map<int,Position> baseDefenders;
 if(w.count(T::Nexus)>=2&&w.count(T::Dragoon)>=10){
  Position guardBase=Positions::Invalid;int nearest=100000;
  for(auto &[id,r]:builders)if(r.type==type(T::Nexus)){auto p=Position(r.tile)+Position(64,48);int d=p.getApproxDistance(home);if(d>600&&d<nearest){guardBase=p;nearest=d;}}
  for(auto n:World::own())if(n->getType()==type(T::Nexus)&&!n->isCompleted()&&n->getDistance(home)>600&&n->getDistance(home)<nearest){guardBase=n->getPosition();nearest=n->getDistance(home);}
  if(guardBase.isValid()){
   std::vector<Unit> ds,hs;for(auto a:army){if(a->getType()==type(T::Dragoon))ds.push_back(a);if(a->getType()==type(T::HT))hs.push_back(a);}
   auto closer=[&](Unit a,Unit b){return a->getDistance(guardBase)<b->getDistance(guardBase);};std::sort(ds.begin(),ds.end(),closer);std::sort(hs.begin(),hs.end(),closer);
   for(int i=0;i<std::min(4,int(ds.size()));i++)baseDefenders[ds[i]->getID()]=guardBase;
   if(!hs.empty())baseDefenders[hs[0]->getID()]=guardBase;
  }
 }

 if(frame>14000&&w.count(T::Dragoon)>=6){
  for(auto n:World::own())if(n->getType()==type(T::Nexus)&&n->isCompleted()){
   Unit darkThreat=nullptr;for(auto e:threats)if(e->getType()==type(T::DT)&&e->getDistance(n)<500)darkThreat=e;
   if(!darkThreat)continue;
   std::vector<Unit> candidates;for(auto a:army)if(a->getType()==type(T::Dragoon)&&!baseDefenders.count(a->getID()))candidates.push_back(a);
   std::sort(candidates.begin(),candidates.end(),[&](Unit a,Unit b){return a->getDistance(darkThreat)<b->getDistance(darkThreat);});
   for(int i=0;i<std::min(4,int(candidates.size()));i++)baseDefenders[candidates[i]->getID()]=darkThreat->getPosition();
  }
 }
 std::set<int> rescuing;
 for(auto shuttle:army)if(shuttle->getType()==type(T::Shuttle)){
  if(evadeStorm(shuttle))continue;
  Unit nearEnemy=nullptr;for(auto e:foes)if(e->isDetected()&&!e->isFlying()&&!e->getType().isBuilding()&&e->getType().canAttack()&&(!nearEnemy||shuttle->getDistance(e)<shuttle->getDistance(nearEnemy)))nearEnemy=e;
  auto loaded=shuttle->getLoadedUnits();
  if(!loaded.empty()){
   Unit cargo=*loaded.begin();Position drop=launched?group:rally;
   if(nearEnemy){double bs=-1e30;for(int i=0;i<16;i++){double a=i*6.28318530718/16;auto q=clamp(nearEnemy->getPosition()+Position(int(std::cos(a)*256),int(std::sin(a)*256)));if(!World::reachable(TilePosition(q))||!Broodwar->isWalkable(WalkPosition(q)))continue;double value=-q.getApproxDistance(group)*.6-shuttle->getDistance(q)*.1;for(auto e:foes)if(e->getType().canAttack()&&e->getDistance(q)<224)value-=500;for(auto u:army)if(u->getType()==type(T::Dragoon)&&u->getDistance(q)<192)value+=30;if(value>bs){bs=value;drop=q;}}
   }
   bool safe=true;for(auto e:foes)if(e->getType().canAttack()&&e->getDistance(shuttle)<208)safe=false;
   if(safe&&shuttle->getDistance(drop)<96&&cargo->getGroundWeaponCooldown()<8&&cargo->getScarabCount()>0&&shuttle->canUnload(cargo))shuttle->unload(cargo);
   else move(shuttle,drop);
  }else{
   Unit reaver=nullptr;double best=1e30;for(auto u:army)if(u->getType()==type(T::Reaver)&&!u->isLoaded()){double v=shuttle->getDistance(u)+(u->getHitPoints()+u->getShields())*.3;if(v<best){best=v;reaver=u;}}
   if(reaver){bool danger=false;for(auto e:foes)if(e->getType().canAttack()&&!e->isFlying()&&reaver->getDistance(e)<224)danger=true;
    if(danger&&(reaver->getGroundWeaponCooldown()>12||reaver->getScarabCount()==0||reaver->getHitPoints()+reaver->getShields()<100)&&!reaver->isAttackFrame()&&!reaver->isStartingAttack()){rescuing.insert(reaver->getID());if(frame-shuttle->getLastCommandFrame()>=6)shuttle->load(reaver);}
    else if(shuttle->getDistance(reaver)>32)move(shuttle,reaver->getPosition());
   }else move(shuttle,group);
  }
 }
 for(auto u:army) {
  if(u->getType()==type(T::HT)){
   auto pending=pendingStorms.find(u->getID());
   if(pending!=pendingStorms.end()){
    int age=frame-pending->second.frame;
    if(age<60&&u->getEnergy()>pending->second.energy-50)continue;
    std::printf("STORM END f=%d unit=%d age=%d spent=%d\n",frame,u->getID(),age,pending->second.energy-u->getEnergy());
    pendingStorms.erase(pending);
   }
  }
  if(u->getType()==type(T::Shuttle)||u->isLoaded()||rescuing.count(u->getID()))continue;
  if(evadeStorm(u))continue;
  if(baseDefenders.count(u->getID())&&u->getDistance(baseDefenders[u->getID()])>320){
   bool localFight=false;for(auto e:foes)if(e->getType().canAttack()&&u->getDistance(e)<480)localFight=true;
   if(!localFight){move(u,World::waypoint(u->getPosition(),baseDefenders[u->getID()],true));continue;}
  }
  auto freePosition=constructionSafe(u,u->getPosition());if(freePosition!=u->getPosition()&&!u->isAttackFrame()&&!u->isStartingAttack()){move(u,freePosition);continue;}
  if(u->getType()==type(T::Reaver)){
   if(u->isAttackFrame()||u->isStartingAttack())continue;
   Unit best=nullptr;double score=-1e9;for(auto e:foes)if(!e->isFlying()&&e->isDetected()&&!e->isInvincible()&&u->getDistance(e)<=256){double s=0;for(auto v:foes)if(!v->isFlying()&&v->getDistance(e)<72)s+=v->getType().isBuilding()?30:std::min(100,v->getHitPoints()+v->getShields());s-=u->getDistance(e)*.2;if(s>score){score=s;best=e;}}
   Unit hazard=nullptr;for(auto e:foes)if(!e->isFlying()&&e->getType().canAttack()&&!e->getType().isWorker()&&u->getDistance(e)<240&&(!hazard||u->getDistance(e)<u->getDistance(hazard)))hazard=e;
   if(hazard&&u->getGroundWeaponCooldown()>8){
    Position escape=u->getPosition();double value=-1e30;for(int i=0;i<16;i++){double angle=i*6.28318530718/16;auto q=clamp(u->getPosition()+Position(int(std::cos(angle)*80),int(std::sin(angle)*80)));if(!World::reachable(TilePosition(q))||!World::clearGroundLine(u->getPosition(),q))continue;double score=hazard->getDistance(q)-q.getApproxDistance(group)*.4-q.getApproxDistance(rally)*.1;for(auto e:foes)if(e->getType().canAttack()&&!e->getType().isWorker()&&e->getDistance(q)<192)score-=200;if(score>value){value=score;escape=q;}}
    if(escape!=u->getPosition())move(u,escape);continue;
   }
   if(best&&u->getScarabCount()==0){auto v=u->getPosition()-best->getPosition();double d=std::max(1.0,std::hypot(v.x,v.y));auto p=clamp(u->getPosition()+Position(int(v.x*112/d),int(v.y*112/d)));if(World::clearGroundLine(u->getPosition(),p))move(u,p);continue;}
   if(best&&u->getScarabCount()>0){if(u->getLastCommand().getType()!=UnitCommandTypes::Attack_Unit||u->getLastCommand().getTarget()!=best||u->isIdle())u->attack(best);}
   else {
    Position support=launched?group:rally;Unit front=nullptr;int escorts=0;
    for(auto a:army)if(a->getType()==type(T::Dragoon)&&a->getDistance(u)<600)escorts++;
    if(escorts>=2&&u->getScarabCount()>0)for(auto e:foes)if(!e->isFlying()&&e->isDetected()&&e->getType().canAttack()&&!e->getType().isWorker()&&u->getDistance(e)<640&&(!front||u->getDistance(e)<u->getDistance(front)))front=e;
    if(front){auto v=group-front->getPosition();double d=std::max(1.0,std::hypot(v.x,v.y));support=clamp(front->getPosition()+Position(int(v.x*240/d),int(v.y*240/d)));}
    if(!launched){auto v=support-rally;double d=std::hypot(v.x,v.y);if(d>288)support=rally+Position(int(v.x*288/d),int(v.y*288/d));}
    if(u->getDistance(support)>48)move(u,World::waypoint(u->getPosition(),support,true));else if(u->getOrder()!=Orders::HoldPosition)u->holdPosition();
   }
   continue;
  }
  if(u->getType()==type(T::Observer)){
   Unit darkThreat=nullptr;double darkScore=1e30;
   for(auto e:threats)if(e->getType()==type(T::DT)){
    bool covered=false;for(auto p:observerTargets)if(p.getApproxDistance(e->getPosition())<256)covered=true;if(covered)continue;
    double score=u->getDistance(e);if(score<darkScore){darkScore=score;darkThreat=e;}
   }
   if(darkThreat){observerTargets.push_back(darkThreat->getPosition());move(u,darkThreat->getPosition());continue;}
   bool firstObserver=true;for(auto a:army)if(a->getType()==type(T::Observer)&&a->getID()<u->getID())firstObserver=false;
   if(launched&&(buildings.empty()||(!firstObserver&&frame>20000))&&!airSearchPoints.empty()){bool unexplored=false;for(auto p:airSearchPoints)if(!Broodwar->isExplored(TilePosition(p)))unexplored=true;auto [it,added]=observerSearch.emplace(u->getID(),u->getID()*17);auto &index=it->second;for(int i=0;i<int(airSearchPoints.size());i++){auto p=airSearchPoints[index%airSearchPoints.size()];if(unexplored?!Broodwar->isExplored(TilePosition(p)):!Broodwar->isVisible(TilePosition(p)))break;index++;}move(u,airSearchPoints[index%airSearchPoints.size()]);continue;}
   Position op=center; double score=-1e9;
   for(auto a:army)if(a->getType().canAttack()) {
    double val=0;for(auto v:army)if(v->getType().canAttack()&&v->getDistance(a)<350)val+=10;
    for(auto e:foes)if(e->getType()==type(T::DT)&&e->getDistance(a)<300)val+=35;
    for(auto p:observerTargets)if(p.getApproxDistance(a->getPosition())<300)val-=100;
    val-=u->getDistance(a)*.003;
    if(val>score){score=val;op=a->getPosition();}
   }
   for(auto e:threats)if(e->getType()==type(T::DT)) {double val=90;for(auto p:observerTargets)if(p.getApproxDistance(e->getPosition())<250)val-=150;if(val>score){score=val;op=e->getPosition();}}
   observerTargets.push_back(op);if(launched&&target.isValid()){auto v=target-op;double d=std::max(1.0,std::hypot(v.x,v.y));op=clamp(op+Position(int(v.x*128/d),int(v.y*128/d)));}move(u,op);continue;
  }

  if(u->getType()==type(T::HT)) {
   Position cast=Positions::Invalid;double bestScore=1.8;
   if(u->getEnergy()>=75&&Broodwar->self()->hasResearched(TechTypes::Psionic_Storm))for(auto e:foes)if(!e->getType().isBuilding()&&u->getDistance(e)<640){
    Position p=e->getPosition();bool overlap=false;for(auto &x:storms)if(p.getApproxDistance(x.first)<80)overlap=true;if(overlap)continue;
    double score=0;for(auto v:foes)if(!v->getType().isBuilding()&&v->getDistance(p)<64)score+=std::min(112,v->getHitPoints()+v->getShields())/70.0*(v->getType()==type(T::Reaver)?2.5:(v->getType()==type(T::HT)||v->getType()==type(T::Shuttle))?2.0:1.0);
    for(auto a:army)if(a->getDistance(p)<64)score-=1.5;
    score-=u->getDistance(p)*.001;
    if(score>bestScore){bestScore=score;cast=p;}
   }
   if(cast.isValid()){
    if(u->getPosition().getDistance(cast)<=284){if(u->useTech(TechTypes::Psionic_Storm,cast)){storms.emplace_back(cast,frame+60);pendingStorms[u->getID()]={frame,u->getEnergy()};std::printf("STORM f=%d unit=%d energy=%d at=%d,%d\n",frame,u->getID(),u->getEnergy(),cast.x,cast.y);}continue;}
    auto v=u->getPosition()-cast;double d=std::max(1.0,std::hypot(v.x,v.y));Position p=cast+Position(int(v.x*280/d),int(v.y*280/d));move(u,World::waypoint(u->getPosition(),p,true));continue;
   }
   Unit danger=nullptr;for(auto e:foes)if(e->getType().canAttack()&&u->getDistance(e)<288&&(!danger||u->getDistance(e)<u->getDistance(danger)))danger=e;
   if(danger){
    Position escape=u->getPosition();double best=-1e30;
    for(int radius:{96,144})for(int i=0;i<24;i++){
     double angle=i*6.28318530718/24;auto q=clamp(u->getPosition()+Position(int(std::cos(angle)*radius),int(std::sin(angle)*radius)));
     if(!clearMicroLine(u,q))continue;
     double clearance=1000;for(auto e:foes)if(e->getType().canAttack()&&e->getType().groundWeapon()!=WeaponTypes::None)clearance=std::min(clearance,double(e->getDistance(q)-e->getPlayer()->weaponMaxRange(e->getType().groundWeapon())));
     double score=clearance-q.getApproxDistance(group)*.08-radius*.1;
     for(auto c:World::own())if(c->getType()==type(T::Cannon)&&c->isCompleted()&&c->isPowered()&&c->getDistance(q)<192)score+=16;
     if(score>best){best=score;escape=q;}
    }
    if(escape!=u->getPosition())move(u,escape);
   }
   else {auto hold=baseDefenders.count(u->getID())?baseDefenders[u->getID()]:group;if(u->getDistance(hold)>80)move(u,World::waypoint(u->getPosition(),hold,true));}
   continue;
  }
  if(u->isAttackFrame()||u->isStartingAttack())continue;
  if(earlyMelee&&have(T::Nexus)<2&&u->getType()==type(T::Zealot)&&!w.count(T::DT)){
   Unit near=nullptr;for(auto e:foes)if(e->isDetected()&&e->getType().canAttack()&&!e->getType().isBuilding()&&e->getDistance(workerRally)<224&&(!near||u->getDistance(e)<u->getDistance(near)))near=e;
   if(near&&u->getDistance(workerRally)<288)attack(u,near);
   else {auto v=target-home;double d=std::max(1.0,std::hypot(v.x,v.y));auto p=workerRally;if(u->getDistance(p)>48)move(u,p);else if(u->getOrder()!=Orders::HoldPosition)u->holdPosition();}
   continue;
  }
  if(u->getLastCommand().getType()==UnitCommandTypes::Attack_Unit&&frame-u->getLastCommandFrame()<18&&(u->getType()!=type(T::Dragoon)||u->getGroundWeaponCooldown()==0)){auto t=u->getLastCommand().getTarget();if(t&&t->exists()&&t->isVisible()&&u->getDistance(t)<=Broodwar->self()->weaponMaxRange(u->getType().groundWeapon())+8)continue;}
  Unit hidden=nullptr; for(auto e:foes)if(!e->isDetected()&&e->getType().canAttack()&&u->getDistance(e)<192){hidden=e;break;}
  if(hidden){move(u,home);continue;}
  bool holdStatic=earlyMelee&&have(T::Nexus)<2&&u->getType()==type(T::Dragoon);
  Unit best=nullptr, close=nullptr; double bs=1e9;
  int range=Broodwar->self()->weaponMaxRange(u->getType().groundWeapon());
  for(auto e:foes) {
   if(!e->isDetected()||e->isInvincible()||(e->isFlying()&&u->getType().airWeapon()==WeaponTypes::None))continue;
   int d=u->getDistance(e);if(!e->isFlying()&&d>range+16&&!World::clearGroundLine(u->getPosition(),e->getPosition()))continue;if(holdStatic&&d>range+16)continue;if(d>400 || (!launched&&!clearingContain&&u->getType()!=type(T::DT)&&invaders==0&&d>range))continue;
   if(e->getType().isWorker()&&d>range+48&&e->getDistance(home)>400&&(!enemy.isValid()||e->getDistance(enemy)>500))continue;
   double s=d + (e->getType().isBuilding()&&!e->getType().canAttack()?180:0) + (e->getType().isWorker()?30:0);
   if(e->getType()==type(T::Reaver)||e->getType()==type(T::HT))s-=80;
   if(e->getType()==type(T::Observer)&&w.count(T::DT)>0&&u->getType()==type(T::Dragoon)&&d<=range+16)s-=240;
   if(e->getType()==UnitTypes::Protoss_Shuttle&&d<=range+16)s-=100;
   if(u->getType()==type(T::DT)){if(e->getType().isWorker())s-=100;if(e->getType()==type(T::Cannon)&&w.count(T::DT)>=3)s-=200;}
   if(d<=range+16)s-=100;
   s+=std::max(0.0,e->getHitPoints()+e->getShields()-committed[e->getID()])*.25; if(committed[e->getID()]>=e->getHitPoints()+e->getShields())s+=800;
   if(s<bs){bs=s;best=e;}
   if(e->getType().canAttack()&&(!e->getType().isWorker()||e->isAttacking()||e->getOrder()==Orders::AttackUnit)&&(!close||d<u->getDistance(close)))close=e;
  }
  if(holdStatic&&!best){if(u->getDistance(workerRally)>48)move(u,workerRally);else if(u->getOrder()!=Orders::HoldPosition)u->holdPosition();continue;}
  if(launched&&foes.size()>=4&&(!best||u->getDistance(best)>range+100)&&u->getType()==type(T::Zealot)){
   Unit support=nullptr;for(auto a:army)if(a->getType()==type(T::HT)&&a->getEnergy()>=65&&(!support||a->getDistance(target)<support->getDistance(target)))support=a;
   if(support&&u->getDistance(support)>360&&u->getDistance(target)+200<support->getDistance(target)){
    auto v=target-support->getPosition();double d=std::max(1.0,std::hypot(v.x,v.y));auto p=support->getPosition()+Position(int(v.x*280/d),int(v.y*280/d));attackPos(u,p);continue;
   }
  }
  if(u->getType()==type(T::Dragoon)&&w.count(T::Reaver)&&frame<18000&&(!best||u->getDistance(best)>range+16)){
   Unit escort=nullptr;for(auto a:army)if(a->getType()==type(T::Reaver)&&!a->isLoaded()&&a->getDistance(u)<640&&(!escort||a->getDistance(target)<escort->getDistance(target)))escort=a;
   if(escort&&u->getDistance(target)+144<escort->getDistance(target)){auto v=target-escort->getPosition();double d=std::max(1.0,std::hypot(v.x,v.y));auto p=escort->getPosition()+Position(int(v.x*112/d),int(v.y*112/d));if(u->getDistance(p)>64)attackPos(u,p);else if(u->getOrder()!=Orders::HoldPosition)u->holdPosition();continue;}
  }
  if(launched&&u->getType()!=type(T::DT)&&most>=4&&u->getDistance(group)>280&&u->getDistance(target)>group.getApproxDistance(target)+128&&(!best||u->getDistance(best)>range+16)){attackPos(u,group);continue;}
  if(best) {
   if(Broodwar->getStartLocations().size()>2&&u->getType()==type(T::Zealot)&&u->getShields()<12&&u->getHitPoints()<65&&u->getGroundWeaponCooldown()>5){
    bool focused=false;for(auto e:foes)if(e->getOrderTarget()==u&&e->getType().canAttack()&&e->getDistance(u)<120)focused=true;
    Unit cover=nullptr;for(auto a:army)if(a!=u&&a->getType()==type(T::Zealot)&&a->getHitPoints()+a->getShields()>100&&a->getDistance(u)<160&&(!cover||a->getDistance(u)<cover->getDistance(u)))cover=a;
    if(focused&&cover&&close){auto v=u->getPosition()-close->getPosition();double d=std::max(1.0,std::hypot(v.x,v.y));auto p=clamp(u->getPosition()+Position(int(v.x*96/d),int(v.y*96/d)));if(World::reachable(TilePosition(p))){move(u,p);continue;}}
   }
   bool exposed=exposedUntil[u->getID()]>frame;if(u->getType()==type(T::DT))for(auto e:foes)if(e->isCompleted()&&e->getType().isDetector()&&u->getDistance(e)<350)exposed=true;
   if((u->getType()!=type(T::DT)||exposed) && retreat[u->getID()] && close && u->getDistance(close)<400 && (u->getDistance(rally)>100||close->getType().isWorker()||close->getType()==type(T::Cannon))) {
    if((u->getType()==type(T::Dragoon)||u->getType()==type(T::Zealot))&&u->getDistance(best)<=range+8&&u->getGroundWeaponCooldown()<=5){attack(u,best);continue;}
    Position p=rally;if(u->getType()==type(T::Dragoon)||close->getType().isWorker()){auto v=u->getPosition()-close->getPosition();double d=std::max(1.0,std::hypot(v.x,v.y));p=u->getPosition()+Position(int(v.x*100/d),int(v.y*100/d));}move(u,p);continue;
   }
   if(u->getType()==type(T::Dragoon) && u->getGroundWeaponCooldown()>5 && close && u->getDistance(close)<range) {
    auto back=dragoonKite(u,close,group);if(back!=u->getPosition())move(u,back);else if(u->getOrder()!=Orders::HoldPosition)u->holdPosition();
   }else {attack(u,best);if(u->getGroundWeaponCooldown()<8&&u->getDistance(best)<range+16)committed[best->getID()]+=damage(u,best);}
  }else if(baseDefenders.count(u->getID()))attackPos(u,baseDefenders[u->getID()]);
  else if(invaders>0)attackPos(u,target);
  else if(launched||clearingContain)attackPos(u,target);
  else {
   Position hold=rally;auto dir=target-rally;double len=std::max(1.0,std::hypot(dir.x,dir.y));
   std::vector<Unit> line;for(auto a:army)if(a->getType()==type(T::Dragoon))line.push_back(a);
   auto it=std::find(line.begin(),line.end(),u);int index=int(it-line.begin());
   int off=((index%5)-std::min(4,int(line.size())-1)/2)*44;
   if(u->getType()==type(T::Dragoon))hold=clamp(rally+Position(int(-dir.y/len*off-dir.x/len*(index/5)*44),int(dir.x/len*off-dir.y/len*(index/5)*44)));
   if(!Broodwar->isWalkable(WalkPosition(hold)))hold=rally;
   if(u->getDistance(hold)>32)move(u,hold);else if(u->getOrder()!=Orders::HoldPosition)u->holdPosition();
  }
 }
 if(frame%1440==0)std::printf("t=%d p=%d z=%d d=%d gates=%d minerals=%d gas=%d target=%d,%d\n",frame,have(T::Probe),zealots,dragoons,have(T::Gateway),w.minerals,w.gas,target.x,target.y);
 }
};
BWAPI::AIModule* makeCandidateBot(){return new Candidate();}

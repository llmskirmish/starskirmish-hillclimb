#include "world.hpp"
#include <cstdio>
#include <FAP.hpp>
using namespace BWAPI;
using namespace bw_demo;
class Candidate : public AIModule {
 std::map<int,Reservation> builders;
 std::set<int> militia;
 Position home, rally, enemy=Positions::Invalid;
 std::map<int,Position> buildings;
 Unit scout=nullptr;
 bool launched=false, dtSeen=false;
 int frame=0;
 std::map<int,bool> retreat;
 std::set<int> transferred;
 std::map<int,int> gasWorkers;
 std::map<int,int> mineralWorkers;
 auto simUnit(Unit u) {
  auto t=u->getType(); auto p=u->getPlayer();
  return FAP::makeUnit().setUnitType(t).setPosition(u->getPosition()).setHealth(u->getHitPoints()).setShields(u->getShields()).setArmorUpgrades(p->getUpgradeLevel(t.armorUpgrade())).setAttackerCount(1).setAttackUpgrades(p->getUpgradeLevel(t.groundWeapon().upgradeType())).setShieldUpgrades(p->getUpgradeLevel(UpgradeTypes::Protoss_Plasma_Shields)).setSpeedUpgrade(p->getUpgradeLevel(t==type(T::Zealot)?UpgradeTypes::Leg_Enhancements:UpgradeTypes::None)>0).setAttackSpeedUpgrade(false).setStimmed(false).setRangeUpgrade(t==type(T::Dragoon)&&p->getUpgradeLevel(UpgradeTypes::Singularity_Charge)>0).setAttackCooldownRemaining(u->getGroundWeaponCooldown()).setFlying(u->isFlying()).setElevation(Broodwar->getGroundHeight(u->getTilePosition())).setData(std::tuple<>{});
 }

 Position clamp(Position p) {return Position(std::clamp(p.x,0,Broodwar->mapWidth()*32-1),std::clamp(p.y,0,Broodwar->mapHeight()*32-1));}
 void move(Unit u,Position p) {p=clamp(p); if(u->isIdle() || u->getLastCommand().getType()!=UnitCommandTypes::Move || u->getLastCommand().getTargetPosition().getApproxDistance(p)>48 || frame-u->getLastCommandFrame()>48) u->move(p);}
 void attack(Unit u,Unit e) {if(e && (!u->isAttacking() || u->getOrderTarget()!=e)) u->attack(e);}
 void attackPos(Unit u,Position p) {if(u->isIdle() || (frame-u->getLastCommandFrame()>48 && u->getLastCommand().getTargetPosition().getApproxDistance(p)>96)) u->attack(clamp(p));}
 public:
 void onStart() override {Broodwar->setCommandOptimizationLevel(2); home=Position(Broodwar->self()->getStartLocation())+Position(64,48);
  rally=home; const auto &gd=World::groundDistance();int mw=Broodwar->mapWidth();TilePosition t=Broodwar->self()->getStartLocation();for(auto v:Broodwar->getStartLocations())if(v!=t){t=v;break;}
  for(int i=0;i<1000;i++){if(!t.isValid())break;int d=gd[t.x+t.y*mw];if(d<0)break;if(d<=13){rally=Position(t)+Position(16,16);break;}TilePosition next=t;for(auto delta:{TilePosition(-1,0),TilePosition(1,0),TilePosition(0,-1),TilePosition(0,1)}){auto q=t+delta;if(q.isValid()&&gd[q.x+q.y*mw]>=0&&gd[q.x+q.y*mw]<d){d=gd[q.x+q.y*mw];next=q;}}if(next==t)break;t=next;}
 }
 void onFrame() override {
 frame=Broodwar->getFrameCount(); if(frame%6) return;
 for(auto it=builders.begin();it!=builders.end();) {
  Unit u=Broodwar->getUnit(it->first); bool started=false;
  for(auto b:Broodwar->self()->getUnits()) if(b->getType()==it->second.type && b->getTilePosition()==it->second.tile) started=true;
  if(!u||!u->exists()||started||frame-it->second.frame>720) it=builders.erase(it);
  else {auto &r=it->second;Position site=Position(r.tile)+Position(r.type.tileWidth()*16,r.type.tileHeight()*16);
   if(frame-r.frame>=12 && frame-u->getLastCommandFrame()>=18 && !u->isConstructing()) {if(u->getDistance(site)>128)u->move(site);else u->build(r.type,r.tile);}
   ++it;
  }
 }
 if(frame%240==0)for(auto &[id,r]:builders){auto u=Broodwar->getUnit(id);if(u)std::printf("RES t=%d type=%s site=%d,%d worker=%d,%d order=%s age=%d\n",frame,r.type.c_str(),r.tile.x,r.tile.y,u->getPosition().x,u->getPosition().y,u->getOrder().c_str(),frame-r.frame);}
 World w{builders,militia}; for(auto &[id,r]:builders){w.minerals-=r.type.mineralPrice();w.gas-=r.type.gasPrice();}
 auto have=[&](T t){return w.count(t)+w.pending(t);};
 std::vector<Unit> foes, threats, army;
 for(auto e:Broodwar->enemy()->getUnits()) if(e->exists()&&e->isVisible()) {
  foes.push_back(e);
  if(e->getType().isBuilding()) {buildings[e->getID()]=e->getPosition(); if(e->getType().isResourceDepot() || !enemy.isValid()) enemy=e->getPosition();}
  if(e->getType()==UnitTypes::Protoss_Dark_Templar || e->getType()==UnitTypes::Protoss_Templar_Archives) dtSeen=true;
  bool nearBase=e->getDistance(home)<500;for(auto n:Broodwar->self()->getUnits())if(n->getType().isResourceDepot()&&n->getDistance(e)<500)nearBase=true;
  if(nearBase && (e->getType().canAttack()||e->getType().isWorker()))threats.push_back(e);
 }
 for(auto it=buildings.begin();it!=buildings.end();) {if(Broodwar->isVisible(TilePosition(it->second)) && (!Broodwar->getUnit(it->first)||!Broodwar->getUnit(it->first)->exists())) it=buildings.erase(it);else ++it;}
 Position target=enemy;
 if(!buildings.empty()) { int d=100000; for(auto &[id,p]:buildings) if(p.getApproxDistance(home)<d){d=p.getApproxDistance(home); target=p;} }
 if(!target.isValid() || (buildings.empty()&&Broodwar->isVisible(TilePosition(target)))) {
  target=Positions::Invalid;
  for(auto t:Broodwar->getStartLocations()) if(t!=Broodwar->self()->getStartLocation() && !Broodwar->isExplored(t+TilePosition(2,1))) {target=Position(t)+Position(64,48);break;}
  if(!target.isValid()) {auto bs=World::bases(); if(!bs.empty()) target=bs[(frame/720)%bs.size()][0];else target=home;}
 }
 for(auto e:threats)if(!e->getType().isWorker()&&!e->getType().isBuilding()){target=e->getPosition();break;}
 for(auto u:Broodwar->self()->getUnits()) if(u->isCompleted()&&!u->getType().isBuilding()&&!u->getType().isWorker()) army.push_back(u);
 int zealots=w.count(T::Zealot), dragoons=w.count(T::Dragoon);
 if((dragoons>=6 && w.count(T::Observer)) || w.count(T::DT)) launched=true;
 if(dragoons<3&&!w.count(T::DT))launched=false;
 // On three-start maps the first scout checks the remaining starts in order.
 if(Broodwar->getStartLocations().size()>2 && !enemy.isValid() && have(T::Probe)>=9 && (!scout||!scout->exists()))
 for(auto u:Broodwar->self()->getUnits()) if(u->getType().isWorker()&&!builders.count(u->getID())&&!u->isGatheringGas()){scout=u;break;}
 if(scout&&scout->exists()&&!enemy.isValid()) move(scout,target); else scout=nullptr;
 militia.clear();
 int threatening=0; for(auto e:threats) if(e->isDetected()&&!e->getType().isWorker()&&!e->getType().isBuilding()&&e->getDistance(home)<288&&e->getType().groundWeapon().maxRange()<64) threatening++;
 int nearArmy=0;for(auto u:army) if(u->getDistance(home)<480) nearArmy++;
 if(threatening>0 && nearArmy< threatening*2+1) {
  std::vector<Unit> ws; for(auto u:Broodwar->self()->getUnits()) if(u->getType().isWorker()&&!builders.count(u->getID())&&u!=scout&&u->getDistance(home)<500) ws.push_back(u);
  std::sort(ws.begin(),ws.end(),[&](Unit a,Unit b){return a->getDistance(threats[0])<b->getDistance(threats[0]);});
  int n=std::min((int)ws.size()-4,threatening*3+2);
  for(int i=0;i<n;i++) {auto u=ws[i];militia.insert(u->getID());Unit e=nullptr;for(auto v:threats)if(v->isDetected()&&(!e||u->getDistance(v)<u->getDistance(e)))e=v;
   if(u->isAttackFrame())continue; if(u->getShields()<5 && u->getHitPoints()<25) {Unit m=u->getClosestUnit(Filter::IsMineralField);if(m)u->gather(m);} else attack(u,e);
  }
 }
 if(scout&&scout->exists())militia.insert(scout->getID());
 // Economy: gas is staffed separately, mineral assignments favor nearby unsaturated patches.
 std::vector<Unit> gases,nexuses; std::map<int,int> miners;
 for(auto u:Broodwar->self()->getUnits()) {if(u->isCompleted()&&u->getType().isRefinery())gases.push_back(u);if(u->isCompleted()&&u->getType().isResourceDepot())nexuses.push_back(u);if(u->getOrderTarget()&&!u->isGatheringGas())miners[u->getOrderTarget()->getID()]++;}
 for(auto it=gasWorkers.begin();it!=gasWorkers.end();) {
  Unit u=Broodwar->getUnit(it->first),g=Broodwar->getUnit(it->second);
  if(!u||!u->exists()||!g||!g->exists()||builders.count(it->first)||militia.count(it->first))it=gasWorkers.erase(it);else ++it;
 }
 int gasCap=w.gas>800?1:3;
 for(auto g:gases) {
  std::vector<int> assigned;for(auto [id,gid]:gasWorkers)if(gid==g->getID())assigned.push_back(id);
  while((int)assigned.size()>gasCap){auto id=assigned.back();assigned.pop_back();gasWorkers.erase(id);auto u=Broodwar->getUnit(id);if(u)u->stop();}
  while((int)assigned.size()<gasCap){Unit best=nullptr;for(auto u:Broodwar->self()->getUnits())if(u->getType().isWorker()&&u!=scout&&!gasWorkers.count(u->getID())&&!builders.count(u->getID())&&!militia.count(u->getID())&&!u->isConstructing()&&!u->isCarryingMinerals()&&!u->isCarryingGas()&&(!best||u->getDistance(g)<best->getDistance(g)))best=u;
   if(!best)break;gasWorkers[best->getID()]=g->getID();assigned.push_back(best->getID());best->gather(g);
  }
 }
 miners.clear();
 for(auto it=mineralWorkers.begin();it!=mineralWorkers.end();) {
  Unit u=Broodwar->getUnit(it->first),m=Broodwar->getUnit(it->second);
  if(!u||!u->exists()||!m||!m->exists()||m->getResources()==0||gasWorkers.count(it->first)||builders.count(it->first)||militia.count(it->first)||u==scout)it=mineralWorkers.erase(it);else {miners[it->second]++;++it;}
 }
 for(auto u:Broodwar->self()->getUnits()) {
  if(!u->getType().isWorker()||u==scout||builders.count(u->getID())||militia.count(u->getID())||u->isConstructing())continue;
  if(gasWorkers.count(u->getID())){auto g=Broodwar->getUnit(gasWorkers[u->getID()]);if(g && !u->isGatheringGas()&&!u->isCarryingGas())u->gather(g);continue;}
  if(u->isCarryingMinerals()||u->isCarryingGas()){if(u->isIdle())u->returnCargo();continue;}
  Unit old=mineralWorkers.count(u->getID())?Broodwar->getUnit(mineralWorkers[u->getID()]):nullptr;
  bool rebalance=frame%120==0 && old && miners[old->getID()]>2;
  if(old&&!rebalance) {if(u->isIdle()||!u->isGatheringMinerals())u->gather(old);continue;}
  if(old)miners[old->getID()]--;
  Unit best=nullptr;double bs=1e9;
  for(auto m:Broodwar->getMinerals()) {int baseDist=100000;for(auto n:nexuses)baseDist=std::min(baseDist,n->getDistance(m));if(baseDist>360)continue;
   double score=miners[m->getID()]*220+u->getDistance(m)*.15+baseDist*.25;if(score<bs){bs=score;best=m;}}
  if(best){mineralWorkers[u->getID()]=best->getID();miners[best->getID()]++;if(best!=old||u->isIdle())u->gather(best);}
 }
 // Build order and adaptive macro.
 if(w.supplyLeft()<2+2*have(T::Gateway) && !w.pending(T::Pylon)) w.build(T::Pylon);
 if(have(T::Pylon) && have(T::Gateway)<1 && have(T::Probe)>=9) w.build(T::Gateway);
 if(have(T::Core)&&have(T::Gateway)<2&&have(T::Probe)>=16)w.build(T::Gateway);
 int cap=std::min(64,22*std::max(1,w.count(T::Nexus)));
 if(have(T::Probe)<cap)w.train(T::Probe);
 if(have(T::Probe)>=12 && have(T::Gateway)) {
  if(!have(T::Gas))w.build(T::Gas);
  if(have(T::Gas)&&!have(T::Core)){w.build(T::Core);if(!have(T::Core))w.minerals-=200;}
 }
 if(w.count(T::Core) && !have(T::Citadel)) {w.build(T::Citadel);if(!have(T::Citadel)){w.minerals-=150;w.gas-=100;}}
 if(w.count(T::Citadel)&&!have(T::Archives)) {w.build(T::Archives);if(!have(T::Archives)){w.minerals-=150;w.gas-=200;}}
 if(w.count(T::Archives)&&have(T::DT)<(frame<11000?4:2)) {w.train(T::DT);if(have(T::DT)<2){w.minerals-=125;w.gas-=100;}}
 if(w.count(T::Core) && (have(T::DT)>=2 || frame>9500)) {
  if(have(T::Dragoon)>=2&&!have(T::Robo)){w.build(T::Robo);if(have(T::Dragoon)>=2&&!have(T::Robo)){w.minerals-=200;w.gas-=100;}}
  if(w.count(T::Robo)&&!have(T::Observatory)){w.build(T::Observatory);if(!have(T::Observatory)){w.minerals-=50;w.gas-=100;}}
  if(w.count(T::Observatory)&&have(T::Observer)<(frame>9500?2:1))w.train(T::Observer);
  if(have(T::Dragoon)>=1 && !w.rangeStarted() && w.minerals>=150 && w.gas>=150){w.researchRange();w.minerals-=150;w.gas-=150;}
 }
 if(dtSeen) {if(!have(T::Forge))w.build(T::Forge);if(w.count(T::Forge)&&have(T::Cannon)<1)w.build(T::Cannon);}
 if(w.count(T::Observer)&&frame>8500) {
  if(!have(T::ReaverBay))w.build(T::ReaverBay);
  if(w.count(T::ReaverBay)&&have(T::Reaver)<2){w.train(T::Reaver);if(!have(T::Reaver)){w.minerals-=200;w.gas-=100;}}
 }
 for(auto u:army)if(u->getType()==type(T::Reaver)&&u->getScarabCount()<5&&!u->isTraining()&&w.minerals>=15){if(u->train(UnitTypes::Protoss_Scarab))w.minerals-=15;}
 if(frame>8500 && have(T::Nexus)<2 && army.size()>=8 && threats.empty()){w.build(T::Nexus);if(!have(T::Nexus) || have(T::Nexus)<2)w.minerals-=400;}
 for(int i=0;i<w.count(T::Gateway);i++) {
  if(Broodwar->self()->getUpgradeLevel(UpgradeTypes::Leg_Enhancements)>0 && have(T::Zealot)*3<have(T::Dragoon))w.train(T::Zealot);
  else if(w.count(T::Core)&&w.gas>=50 && (have(T::Dragoon)<1 || have(T::DT)>=2 || frame>10000))w.train(T::Dragoon);
  else if((!have(T::Core)&&have(T::Zealot)<2)||(have(T::Core)&&w.minerals>300))w.train(T::Zealot);
 }
 if(frame>6500 && have(T::Gateway)<4 && w.minerals>=250)w.build(T::Gateway);
 if(frame>8500 && w.minerals>=450 && have(T::Nexus)<2)w.build(T::Nexus);
 if(w.count(T::Nexus)>1 && have(T::Gas)<w.count(T::Nexus))w.build(T::Gas);
 if(w.minerals>600 && have(T::Gateway)<std::min(12,4*w.count(T::Nexus)))w.build(T::Gateway);
 if(frame>11000 && have(T::Nexus)<4 && w.minerals>800)w.build(T::Nexus);
 if(frame>10000 && !have(T::Citadel))w.build(T::Citadel);
 if(w.count(T::Citadel)&&frame>10000)for(auto u:Broodwar->self()->getUnits())if(u->getType()==type(T::Citadel)&&!u->isUpgrading()&&!Broodwar->self()->getUpgradeLevel(UpgradeTypes::Leg_Enhancements))u->upgrade(UpgradeTypes::Leg_Enhancements);
 if(frame>10000&&!have(T::Forge))w.build(T::Forge);
 if(w.count(T::Forge)&&w.minerals>200&&w.gas>150)for(auto u:Broodwar->self()->getUnits())if(u->getType()==type(T::Forge)&&!u->isUpgrading())u->upgrade(UpgradeTypes::Protoss_Ground_Weapons);
 // Estimate local engagements; reinforcements should not feed a defended choke one at a time.
 if(frame%24==0)for(auto u:army)if(u->getType().canAttack()) {
  FAP::FastAPproximation<> sim; float a0=0,b0=0; int es=0;
  for(auto a:army)if(a->getType().canAttack()&&a->getDistance(u)<480){sim.addIfCombatUnitPlayer1(simUnit(a));a0+=a->getHitPoints()+a->getShields();}
  for(auto e:foes)if(e->isCompleted()&&e->isDetected()&&e->getType().canAttack()&&e->getDistance(u)<560){sim.addIfCombatUnitPlayer2(simUnit(e));b0+=e->getHitPoints()+e->getShields();es++;}
  retreat[u->getID()]=false;
  if(es&&u->getDistance(home)>500){for(auto group:{sim.getState().first,sim.getState().second})for(auto &v:*group)if(v.unitType==type(T::Reaver)){v.groundMaxRangeSquared=256*256;v.groundCooldown=60;}sim.simulate(192);auto st=sim.getState();float a1=0,b1=0;for(auto &a:*st.first)a1+=(a.health+a.shields)/256.f;for(auto &b:*st.second)b1+=(b.health+b.shields)/256.f;retreat[u->getID()]=(a1<a0*.60f && a0-a1>(b0-b1)*1.10f) || (a1<a0*.15f && b1>b0*.20f);}
 }
 // Fight nearby threats with individual target choice and Dragoon cooldown movement.
 Position center=home; if(!army.empty()){center=Position(0,0);int n=0;for(auto u:army)if(u->getType()!=type(T::Observer)){center+=u->getPosition();n++;}if(n)center/=n;else center=home;}
 std::vector<Position> observerTargets;
 for(auto u:army) {
  if(u->getType()==type(T::Observer)){
   Position op=center; double score=-1e9;
   for(auto a:army)if(a->getType().canAttack()) {
    double val=0;for(auto v:army)if(v->getType().canAttack()&&v->getDistance(a)<350)val+=10;
    for(auto e:foes)if(e->getType()==type(T::DT)&&e->getDistance(a)<300)val+=35;
    for(auto p:observerTargets)if(p.getApproxDistance(a->getPosition())<300)val-=100;
    val-=u->getDistance(a)*.003;
    if(val>score){score=val;op=a->getPosition();}
   }
   for(auto e:threats)if(e->getType()==type(T::DT)) {double val=90;for(auto p:observerTargets)if(p.getApproxDistance(e->getPosition())<250)val-=150;if(val>score){score=val;op=e->getPosition();}}
   observerTargets.push_back(op);move(u,op);continue;
  }
  if(u->isAttackFrame()||u->isStartingAttack())continue;
  Unit hidden=nullptr; for(auto e:foes)if(!e->isDetected()&&e->getType().canAttack()&&u->getDistance(e)<192){hidden=e;break;}
  if(hidden){move(u,home);continue;}
  Unit best=nullptr, close=nullptr; double bs=1e9;
  int range=Broodwar->self()->weaponMaxRange(u->getType().groundWeapon());
  for(auto e:foes) {
   if(!e->isDetected()||e->isInvincible()||(e->isFlying()&&u->getType().airWeapon()==WeaponTypes::None))continue;
   int d=u->getDistance(e);if(d>560)continue;
   if(e->getType().isWorker()&&d>range+48&&e->getDistance(home)>400&&(!enemy.isValid()||e->getDistance(enemy)>500))continue;
   double s=d + (e->getType().isBuilding()&&!e->getType().canAttack()?180:0) + (e->getType().isWorker()?30:0);
   if(e->getType()==type(T::Reaver)||e->getType()==type(T::HT))s-=80;
   if(d<=range+16)s-=100;
   s+=(e->getHitPoints()+e->getShields())*.13;
   if(s<bs){bs=s;best=e;}
   if(e->getType().canAttack()&&!e->getType().isWorker()&&(!close||d<u->getDistance(close)))close=e;
  }
  if(best) {
   if(u->getType()!=type(T::DT) && retreat[u->getID()] && close && u->getDistance(close)<400) {move(u,rally);continue;}
   if(u->getType()==type(T::Dragoon) && u->getGroundWeaponCooldown()>5 && close && u->getDistance(close)<range && close->getType().groundWeapon().maxRange()<range) {
    auto p=u->getPosition(),q=close->getPosition();double d=std::max(1.0,std::hypot(p.x-q.x,p.y-q.y));move(u,p+Position(int((p.x-q.x)*80/d),int((p.y-q.y)*80/d)));
   }else attack(u,best);
  }else if(!threats.empty())attackPos(u,target);
  else if(launched)attackPos(u,target);
  else if(u->getDistance(rally)>96)move(u,rally);
 }
 if(frame%1440==0)std::printf("t=%d p=%d z=%d d=%d gates=%d minerals=%d gas=%d target=%d,%d\n",frame,have(T::Probe),zealots,dragoons,have(T::Gateway),w.minerals,w.gas,target.x,target.y);
 }
};
BWAPI::AIModule* makeCandidateBot(){return new Candidate();}

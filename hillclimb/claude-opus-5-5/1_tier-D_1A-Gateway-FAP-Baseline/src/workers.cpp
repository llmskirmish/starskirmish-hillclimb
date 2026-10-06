#include "common.h"

namespace Workers {
int gasPerGeyser = 3;
static std::map<Unit, Role> roles;
static std::map<Unit, Unit> mineralOf;
static std::map<Unit, Unit> gasOf;
static int lastBalance = 0;

void setRole(Unit u, Role r) {
    roles[u] = r;
    mineralOf.erase(u);
    gasOf.erase(u);
}
bool hasRole(Unit u, Role r) {
    auto it = roles.find(u);
    return it != roles.end() && it->second == r;
}
void release(Unit u) {
    if (!u) return;
    setRole(u, Role::Mine);
}
void onDestroy(Unit u) {
    roles.erase(u);
    mineralOf.erase(u);
    gasOf.erase(u);
}
int count() { return (int)roles.size(); }
int mineralWorkers() {
    int n = 0;
    for (auto &[u, r] : roles)
        if (r == Role::Mine) ++n;
    return n;
}
int gasWorkers() {
    int n = 0;
    for (auto &[u, r] : roles)
        if (r == Role::Gas) ++n;
    return n;
}

static std::vector<Unit> depots() {
    std::vector<Unit> d;
    for (auto u : Self()->getUnits())
        if (u->getType().isResourceDepot() && u->isCompleted()) d.push_back(u);
    return d;
}

static std::vector<Unit> mineralsNear(Unit depot) {
    std::vector<Unit> v;
    for (auto m : Broodwar->getMinerals())
        if (m->exists() && m->getDistance(depot) < 300 && m->getResources() > 0) v.push_back(m);
    return v;
}

int baseNeed() {
    int need = 0;
    std::map<Unit, int> cnt;
    for (auto &[u, m] : mineralOf) ++cnt[m];
    for (auto d : depots())
        for (auto m : mineralsNear(d)) need += std::max(0, 2 - cnt[m]);
    for (auto u : Self()->getUnits())
        if (u->getType().isRefinery() && u->isCompleted()) {
            int g = 0;
            for (auto &[w, r] : gasOf)
                if (r == u) ++g;
            need += std::max(0, gasPerGeyser - g);
        }
    return need;
}

static Unit pickMineral(Unit w, std::map<Unit, int> &cnt) {
    Unit best = nullptr;
    double bestScore = 1e18;
    for (auto d : depots()) {
        double dd = w->getDistance(d);
        for (auto m : mineralsNear(d)) {
            int c = cnt[m];
            double s = dd + c * 100000.0 + (c >= 2 ? 1e7 : 0) + m->getDistance(d) * 3;
            if (s < bestScore) bestScore = s, best = m;
        }
    }
    return best;
}

Unit takeBuilder(Position near) {
    Unit best = nullptr;
    double bestD = 1e18;
    for (auto &[u, r] : roles) {
        if (r != Role::Mine || !u->exists() || !u->isCompleted()) continue;
        double d = near.isValid() ? u->getDistance(near) : 0;
        if (u->isCarryingMinerals()) d += 160;
        if (u->getOrder() == Orders::MiningMinerals) d += 100;
        if (d < bestD) bestD = d, best = u;
    }
    if (best) setRole(best, Role::Build);
    return best;
}

Unit takeAny(Position near) {
    Unit best = nullptr;
    double bestD = 1e18;
    for (auto &[u, r] : roles) {
        if ((r != Role::Mine && r != Role::Gas) || !u->exists() || !u->isCompleted()) continue;
        double d = near.isValid() ? u->getDistance(near) : 0;
        if (r == Role::Gas) d += 1000;
        if (d < bestD) bestD = d, best = u;
    }
    return best;
}

void update() {
    // register / prune
    for (auto u : Self()->getUnits())
        if (u->getType().isWorker() && u->isCompleted() && !roles.count(u)) roles[u] = Role::Mine;
    for (auto it = roles.begin(); it != roles.end();) {
        if (!it->first->exists()) {
            mineralOf.erase(it->first);
            gasOf.erase(it->first);
            it = roles.erase(it);
        } else
            ++it;
    }
    auto ds = depots();

    // gas assignment
    std::vector<Unit> refineries;
    for (auto u : Self()->getUnits())
        if (u->getType().isRefinery() && u->isCompleted()) {
            bool nearDepot = false;
            for (auto d : ds)
                if (d->getDistance(u) < 400) nearDepot = true;
            if (nearDepot) refineries.push_back(u);
        }
    for (auto it = gasOf.begin(); it != gasOf.end();) {
        if (!it->second->exists() || std::find(refineries.begin(), refineries.end(), it->second) == refineries.end()) {
            roles[it->first] = Role::Mine;
            it = gasOf.erase(it);
        } else
            ++it;
    }
    for (auto r : refineries) {
        std::vector<Unit> on;
        for (auto &[w, g] : gasOf)
            if (g == r) on.push_back(w);
        int want = gasPerGeyser;
        if ((int)roles.size() < 10) want = std::min(want, 1);
        while ((int)on.size() > want) {
            Unit w = on.back();
            on.pop_back();
            setRole(w, Role::Mine);
        }
        while ((int)on.size() < want) {
            Unit best = nullptr;
            double bd = 1e18;
            for (auto &[w, role] : roles) {
                if (role != Role::Mine || w->isCarryingMinerals()) continue;
                double d = w->getDistance(r);
                if (d < bd) bd = d, best = w;
            }
            if (!best) break;
            setRole(best, Role::Gas);
            gasOf[best] = r;
            on.push_back(best);
        }
    }

    // mineral assignment
    std::map<Unit, int> cnt;
    for (auto it = mineralOf.begin(); it != mineralOf.end();) {
        Unit m = it->second;
        bool ok = m && m->exists() && m->getResources() > 0;
        if (ok) {
            ok = false;
            for (auto d : ds)
                if (m->getDistance(d) < 300) ok = true;
        }
        if (!ok || !hasRole(it->first, Role::Mine)) {
            it = mineralOf.erase(it);
            continue;
        }
        ++cnt[m];
        ++it;
    }
    // rebalance oversaturated patches every few seconds
    if (Now() - lastBalance > 24 * 4) {
        lastBalance = Now();
        for (auto it = mineralOf.begin(); it != mineralOf.end();) {
            if (cnt[it->second] > 2) {
                --cnt[it->second];
                it = mineralOf.erase(it);
            } else
                ++it;
        }
        // move workers from saturated depots to ones with free patches
        for (auto d : ds) {
            int freeHere = 0;
            for (auto m : mineralsNear(d)) freeHere += std::max(0, 2 - cnt[m]);
            if (freeHere < 3) continue;
            for (auto d2 : ds) {
                if (d2 == d) continue;
                int freeThere = 0;
                for (auto m : mineralsNear(d2)) freeThere += std::max(0, 2 - cnt[m]);
                if (freeThere > 0) continue;
                // d2 saturated; move up to 'freeHere' workers
                int moved = 0;
                for (auto it = mineralOf.begin(); it != mineralOf.end() && moved < freeHere - 1;) {
                    if (it->second->getDistance(d2) < 300 && cnt[it->second] >= 2 && !it->first->isCarryingMinerals()) {
                        --cnt[it->second];
                        it = mineralOf.erase(it);
                        ++moved;
                    } else
                        ++it;
                }
            }
        }
    }

    for (auto &[w, r] : roles) {
        if (!w->isCompleted()) continue;
        if (r == Role::Gas) {
            auto g = gasOf[w];
            if (!g) continue;
            if (w->isCarryingMinerals()) {
                if (w->getOrder() != Orders::ReturnMinerals) w->returnCargo();
                continue;
            }
            if (!w->isGatheringGas()) cmdGather(w, g);
            continue;
        }
        if (r != Role::Mine) continue;
        if (!mineralOf.count(w)) {
            Unit m = ds.empty() ? nullptr : pickMineral(w, cnt);
            if (m) {
                mineralOf[w] = m;
                ++cnt[m];
            }
        }
        auto it = mineralOf.find(w);
        if (it == mineralOf.end()) {
            if (w->isIdle() && !ds.empty()) w->move(ds[0]->getPosition());
            continue;
        }
        Unit m = it->second;
        if (w->isCarryingGas() || (w->isCarryingMinerals() && !w->isGatheringMinerals())) {
            if (w->getOrder() != Orders::ReturnGas && w->getOrder() != Orders::ReturnMinerals) w->returnCargo();
            continue;
        }
        if (w->isIdle() || !w->isGatheringMinerals()) {
            cmdGather(w, m);
            continue;
        }
        // gathering, but heading to a patch at another base?  re-target
        if ((w->getOrder() == Orders::MoveToMinerals || w->getOrder() == Orders::WaitForMinerals) &&
            w->getOrderTarget() && w->getOrderTarget() != m && w->getOrderTarget()->getDistance(m) > 320)
            cmdGather(w, m);
        else if (w->getOrder() == Orders::MoveToMinerals && w->getOrderTarget() && w->getOrderTarget() != m &&
                 Now() % 48 == 0)
            cmdGather(w, m);
    }
}
}  // namespace Workers

#include "common.h"

#include <chrono>

namespace {
class ProtossBot : public AIModule {
    double maxMs = 0;

public:
    void onStart() override {
        Broodwar->setCommandOptimizationLevel(1);
        auto t0 = std::chrono::steady_clock::now();
        MapInfo::init();
        auto t1 = std::chrono::steady_clock::now();
        LOG("init %.0f ms", std::chrono::duration<double, std::milli>(t1 - t0).count());
    }

    void onFrame() override {
        if (Broodwar->isReplay()) return;
        auto t0 = std::chrono::steady_clock::now();
        try {
            auto timed = [&](const char *name, auto fn) {
                auto a = std::chrono::steady_clock::now();
                fn();
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
                if (ms > 15) LOG("slow %s %.1f ms", name, ms);
            };
            timed("info", [] { Info::update(); });
            timed("map", [] { MapInfo::update(); });
            timed("workers", [] { Workers::update(); });
            timed("combat", [] { Combat::update(); });
            timed("macro", [] { Macro::update(); });
            timed("scout", [] { Scout::update(); });
        } catch (const std::exception &e) {
            LOG("exception: %s", e.what());
        } catch (...) {
            LOG("unknown exception");
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ms > maxMs) {
            maxMs = ms;
            if (ms > 20) LOG("slow frame %.1f ms", ms);
        }
    }

    void onUnitCreate(Unit u) override { Macro::onUnitCreate(u); }
    void onUnitMorph(Unit u) override { Macro::onUnitCreate(u); }

    void onUnitDestroy(Unit u) override {
        try {
            if (u->getPlayer() == Opp()) Info::onDestroy(u);
            if (u->getPlayer() == Self()) Workers::onDestroy(u);
            if (u->getType().isMineralField()) BWEM::Map::Instance().OnMineralDestroyed(u);
            else if (u->getType().isSpecialBuilding()) BWEM::Map::Instance().OnStaticBuildingDestroyed(u);
        } catch (...) {
        }
    }

    void onEnd(bool isWinner) override { LOG("game over, won=%d", (int)isWinner); }
};
}  // namespace

BWAPI::AIModule *makeCandidateBot() { return new ProtossBot(); }

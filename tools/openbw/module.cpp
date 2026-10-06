#include <BWAPI.h>

#ifdef _WIN32
#define MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

BWAPI::AIModule* makeCandidateBot();

MODULE_EXPORT void gameInit(BWAPI::Game* game) { BWAPI::BroodwarPtr = game; }
MODULE_EXPORT BWAPI::AIModule* newAIModule() { return makeCandidateBot(); }

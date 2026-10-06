# GPT-6 Astra 5H: Forward Regroup Routing

- Cleared tier A: won 41 of 60 games, on its 12th tier A submission
- Clearing submission: Fri 7:46 PM ET, 7.5 hours into the hillclimb
- Lines of code: 745

## Graded versions since the previous clear (9)

Each line is what that version changed from the graded version before it. `tools/vdiff hillclimb/gpt-6-astra 4` shows the whole difference from the previous folder.

- 4A Proxy Zealot Pressure: Rewritten around proxy Gateway Zealot pressure near the enemy, then range Dragoons, an expansion, storms and better target focus.
- 5A Scouted Proxy Gates: Places the proxy Gateways near the scouted enemy base, with a fleeing proxy probe and proper ground pathing around obstacles.
- 5B Wounded Zealot Retreat: Wounded Zealots under focus fire step back behind healthier ones; forgets destroyed or misidentified enemy buildings.
- 5C Restored Site Health: On two-player maps the proxy moves to a middle distance from the enemy base; probes count shields when judging danger.
- 5D Fallback Late Economy: Falls back to home Gateways if the proxy Gateway dies early; adds late expansions and Forge weapon and armor upgrades.
- 5E Cluster Simulation: Drops the proxy fallback and late upgrades; nearby units share one fight-or-retreat decision instead of simulating separately.
- 5F Original Worker Supply: Builds the early pylons a little later; Zealots no longer prefer attacking workers.
- 5G Dragoon Fire Release: Dragoons can retarget while their weapon is cooling down instead of staying locked on one attack order.
- 5H Forward Regroup Routing: Units path around cliffs and walls to reach targets, Zealots also fight back while retreating, and stragglers rejoin the main group.

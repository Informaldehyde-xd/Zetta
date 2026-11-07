#pragma once

namespace zb {

// Tells Android which of the game's threads are the hot ones (Android's PerformanceHint API, from
// Android 13), so the system can give them a faster core and a higher clock for a while. A game that
// runs through the translator is not known to the system as a game, so its short, heavy bursts on
// one or two threads are often met with a clock that is still ramping up. Switch: perf_hint=0 in
// zb-options.txt. Does nothing on a phone without the API.
void start_perf_hint();

}  // namespace zb

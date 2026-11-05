#pragma once

namespace zb {

// Switches for behaviours that trade stability against speed or fidelity. They are read once from
// zb-options.txt in the launcher's external files folder (lines "name=0" or "name=1"; "#" starts a
// comment). A switch that is not listed keeps the default the caller passes.
//
//   interruptible_syscalls  (default 0)  Let a guest signal end a wait inside any blocking system
//                                        call. Fixes some garbage-collector stalls, but changes every
//                                        blocking call; keep off unless a freeze needs it.
//   jni_clear_exceptions    (default 1)  Clear a Java exception left by a JNI method call, as the
//                                        old Dalvik did not abort on one. Prevents crashes in games
//                                        that never check for exceptions.
//   egl_force_current       (default 0)  Make the game's context current before every eglSwapBuffers.
//                                        Helps a game that swaps from the wrong thread; costs time.
//   native_zlib             (default 1)  Run the game's zlib (inflate, uncompress, crc32, adler32) as native
//                                        arm64 code instead of translating it. Faster loading.
//   code_cache_mb           (default 32) Size of the translated-code cache of every guest thread, in MB
//                                        (8 to 120). When the cache is full the translator throws all
//                                        of it away and translates the game again, which shows as a
//                                        stutter. A bigger cache uses more RAM only when it is needed.
//   gc_unfreeze             (default 0)  Freeze rescue, for testing: when two or more threads sleep in one
//                                        retry loop for 10 s, clear the lock word they share. Only done
//                                        when exactly one shared place holds 1. May crash the game, but
//                                        the game was stopped anyway.
//   atomic_swp              (default 1)  Make the ARM "swp" instruction atomic (the translator does it as a
//                                        read then a write). The old garbage collector in some games
//                                        locks with swp; without this two threads can break its lock
//                                        and the game freezes for good.
//   atomic_exclusive        (default 1)  Make store-exclusive (strex) one atomic compare-and-swap. Without it a
//                                        lock release can be overwritten by a waiter and the lock stays
//                                        set for good (a mid-game freeze with the video stopped).
//   meter                   (default 1)  Debug build only: write zb-meter.txt (FPS, slowest frame, CPU, RAM,
//                                        busiest threads) twice a second for the on-screen readout.
bool option_enabled(const char* name, bool default_value);
// A whole-number setting from the same file ("name=96"); the default when it is missing or not a number.
int option_number(const char* name, int default_value);

}  // namespace zb

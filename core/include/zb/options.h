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
bool option_enabled(const char* name, bool default_value);

}  // namespace zb

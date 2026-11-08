#pragma once

#include <string>

// JavaVM is declared as in <jni.h> so this header does not need the JNI headers (host test builds
// include it too).
struct _JavaVM;
typedef _JavaVM JavaVM;

namespace zb {

// The launcher's own external files folder, where the runtime report, the diagnostic dumps and the
// optional setting files (zb-options.txt, zb-tuning.txt, zb-dpi.txt, ...) live. The Java side tells
// the runtime where it is (the folder of the runtime report file), so the path is right on every
// phone, for every Android user and on every storage layout, and never has to be guessed. Before
// that call the folder is worked out from the process name (/proc/self/cmdline); if that fails the
// usual single-user path is returned. No trailing slash.
std::string get_external_files_dir();

// get_external_files_dir() + "/" + name. The first call also copies the default setting files
// (zb-options.txt, zb-tuning.txt) out of the launcher's assets when the folder does not have them yet.
std::string external_file_path(const char* name);

// Called once, early, with the directory. The first value wins; later calls are ignored.
void set_external_files_dir(const std::string& directory);

// Called from setReportFile to capture the JavaVM, which the first-run copy of the default
// setting files needs.
void set_java_vm(JavaVM* vm);

}  // namespace zb

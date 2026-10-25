#pragma once

#include <string>

namespace zb {

// The launcher's own external files folder, where the runtime report, the diagnostic dumps and the
// optional setting files (zb-tuning.txt, zb-dpi.txt, ...) live. The Java side tells the runtime
// where it is (the folder of the runtime report file), so the path is right on every phone, for
// every Android user and on every storage layout, and never has to be guessed.
// Without that call the usual single-user path is returned. No trailing slash.
std::string get_external_files_dir();

// get_external_files_dir() + "/" + name.
std::string external_file_path(const char* name);

// Called once, early, with the directory. Safe to call again with the same value.
void set_external_files_dir(const std::string& directory);

}  // namespace zb

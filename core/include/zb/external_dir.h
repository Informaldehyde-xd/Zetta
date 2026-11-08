#pragma once

#include <string>
#include <jni.h>

namespace zb {

std::string get_external_files_dir();

// get_external_files_dir() + "/" + name.
std::string external_file_path(const char* name);

// Called once, early, with the directory. Safe to call again with the same value.
void set_external_files_dir(const std::string& directory);

// Called from JNI_OnLoad to capture the JavaVM for extracting assets on boot.
void set_java_vm(JavaVM* vm);

}  // namespace zb
 is returned. No trailing slash.
std::string get_external_files_dir();

// get_external_files_dir() + "/" + name.
std::string external_file_path(const char* name);

// Called once, early, with the directory. Safe to call again with the same value.
void set_external_files_dir(const std::string& directory);

}  // namespace zb

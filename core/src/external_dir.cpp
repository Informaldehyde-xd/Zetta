#include "zb/external_dir.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

// Android NDK headers for JNI and Assets
#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

char g_directory[512];
std::atomic<bool> g_ready{false};
std::once_flag g_assets_extracted;

// Dynamically find the package name via /proc/self/cmdline (from the handover doc)
std::string get_dynamic_fallback_dir() {
    std::ifstream cmdline("/proc/self/cmdline");
    if (cmdline.is_open()) {
        std::string process_name;
        std::getline(cmdline, process_name, '\0');
        
        // Process name might be "com.ix32psl.trialx:guest", we only want the package name
        size_t colon = process_name.find(':');
        if (colon != std::string::npos) {
            process_name = process_name.substr(0, colon);
        }
        
        if (!process_name.empty()) {
            return "/storage/emulated/0/Android/data/" + process_name + "/files";
        }
    }
    // Ultimate fallback if /proc/self/cmdline fails
    return "/storage/emulated/0/Android/data/";
}

void extract_asset(JNIEnv* env, jobject context, const char* asset_name, const std::string& dest_dir) {
    std::string out_path = dest_dir + "/" + asset_name;
    
    // If the file already exists (user placed it manually), don't overwrite it
    if (std::ifstream(out_path).good()) return;

    jclass context_cls = env->GetObjectClass(context);
    jmethodID get_assets_meth = env->GetMethodID(context_cls, "getAssets", "()Landroid/content/res/AssetManager;");
    jobject asset_mgr_obj = env->CallObjectMethod(context, get_assets_meth);
    AAssetManager* mgr = AAssetManager_fromJava(env, asset_mgr_obj);

    if (mgr) {
        AAsset* asset = AAssetManager_open(mgr, asset_name, AASSET_MODE_BUFFER);
        if (asset) {
            off_t len = AAsset_getLength(asset);
            std::vector<char> buf(len);
            AAsset_read(asset, buf.data(), len);
            AAsset_close(asset);

            std::ofstream out(out_path, std::ios::binary);
            if (out.is_open()) {
                out.write(buf.data(), len);
                out.close();
                log("Extracted %s from assets to %s", asset_name, out_path.c_str());
                runtime_report().note_jni_detail("asset-extract", std::string(asset_name) + " -> " + out_path, true);
            }
        }
    }
    env->DeleteLocalRef(asset_mgr_obj);
    env->DeleteLocalRef(context_cls);
}

void check_and_extract_assets() {
    JavaVM* vm = nullptr;
    jsize count = 0;
    if (JNI_GetCreatedJavaVMs(&vm, 1, &count) != JNI_OK || count == 0) return;

    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;

    jclass activity_thread_cls = env->FindClass("android/app/ActivityThread");
    if (activity_thread_cls) {
        jmethodID current_app_meth = env->GetStaticMethodID(activity_thread_cls, "currentApplication", "()Landroid/app/Application;");
        if (current_app_meth) {
            jobject context = env->CallStaticObjectMethod(activity_thread_cls, current_app_meth);
            if (context) {
                // Ask Android for the correct path dynamically (handles com.ix32psl.xxx)
                jclass context_cls = env->GetObjectClass(context);
                jmethodID get_dir_meth = env->GetMethodID(context_cls, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
                jobject file_obj = env->CallObjectMethod(context, get_dir_meth, nullptr);
                
                if (file_obj) {
                    jclass file_cls = env->GetObjectClass(file_obj);
                    jmethodID get_path_meth = env->GetMethodID(file_cls, "getAbsolutePath", "()Ljava/lang/String;");
                    jstring path_str = (jstring)env->CallObjectMethod(file_obj, get_path_meth);
                    
                    if (path_str) {
                        const char* path_chars = env->GetStringUTFChars(path_str, 0);
                        std::string dynamic_dir(path_chars);
                        env->ReleaseStringUTFChars(path_str, path_chars);
                        
                        // Ensure directory exists
                        mkdir(dynamic_dir.c_str(), 0777);
                        
                        // Extract the files
                        extract_asset(env, context, "zb-options.txt", dynamic_dir);
                        extract_asset(env, context, "zb-tuning.txt", dynamic_dir);
                        
                        // Save it so get_external_files_dir() uses it
                        set_external_files_dir(dynamic_dir);
                        
                        env->DeleteLocalRef(path_str);
                    }
                    env->DeleteLocalRef(file_cls);
                    env->DeleteLocalRef(file_obj);
                }
                env->DeleteLocalRef(context_cls);
                env->DeleteLocalRef(context);
            }
        }
        env->DeleteLocalRef(activity_thread_cls);
    }
    vm->DetachCurrentThread();
}

}  // namespace

void set_external_files_dir(const std::string& directory) {
    std::string clean = directory;
    while (clean.size() > 1 && clean.back() == '/') clean.pop_back();
    if (clean.empty() || clean.size() >= sizeof g_directory) return;
    if (g_ready.load(std::memory_order_acquire)) return;  // first value wins; it never changes
    std::memcpy(g_directory, clean.c_str(), clean.size() + 1);
    g_ready.store(true, std::memory_order_release);
    runtime_report().note_jni_detail("external-files-dir", clean, true);
}

std::string get_external_files_dir() {
    if (g_ready.load(std::memory_order_acquire)) return std::string(g_directory);
    // If Java hasn't set it yet, use the dynamic /proc/self/cmdline fallback
    return get_dynamic_fallback_dir();
}

std::string external_file_path(const char* name) {
    // Check assets exactly once on the first file request
    std::call_once(g_assets_extracted, check_and_extract_assets);
    
    std::string path = get_external_files_dir();
    path += '/';
    path += name;
    return path;
}

}  // namespace zb

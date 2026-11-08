#include "zb/external_dir.h"

#include <dlfcn.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>
#endif

#include "zb/log.h"
#include "zb/runtime_report.h"

namespace zb {

namespace {

// Last resort if nothing better is known.
constexpr const char* kDefaultDirectory = "/storage/emulated/0/Android/data/com.ix32psl.launcher/files";

// Written once, before the guest runs, and read from many threads and from signal handlers, so it
// is a fixed buffer published through an atomic flag rather than a string behind a mutex.
char g_directory[512];
std::atomic<bool> g_ready{false};
std::once_flag g_assets_extracted;
JavaVM* g_vm = nullptr;

// The launcher is the process named "<package>" or "<package>:guest"; its files folder follows from that.
std::string dynamic_fallback_directory() {
    char name[256] = "";
    if (std::FILE* file = std::fopen("/proc/self/cmdline", "re")) {
        const std::size_t got = std::fread(name, 1, sizeof name - 1, file);
        name[got] = 0;  // the name ends at the first NUL
        std::fclose(file);
    }
    std::string package(name);
    const std::size_t colon = package.find(':');
    if (colon != std::string::npos) package.resize(colon);
    if (package.find('.') == std::string::npos) return kDefaultDirectory;
    return "/storage/emulated/0/Android/data/" + package + "/files";
}

#ifdef __ANDROID__

// libandroid is looked up at run time: the helper programs that link this code do not link it.
struct AssetApi {
    AAssetManager* (*from_java)(JNIEnv*, jobject) = nullptr;
    AAsset* (*open)(AAssetManager*, const char*, int) = nullptr;
    off_t (*length)(AAsset*) = nullptr;
    int (*read)(AAsset*, void*, size_t) = nullptr;
    void (*close)(AAsset*) = nullptr;
    bool ok() const { return from_java && open && length && read && close; }
};

AssetApi load_asset_api() {
    AssetApi api;
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (lib == nullptr) return api;
    api.from_java = reinterpret_cast<decltype(api.from_java)>(dlsym(lib, "AAssetManager_fromJava"));
    api.open = reinterpret_cast<decltype(api.open)>(dlsym(lib, "AAssetManager_open"));
    api.length = reinterpret_cast<decltype(api.length)>(dlsym(lib, "AAsset_getLength"));
    api.read = reinterpret_cast<decltype(api.read)>(dlsym(lib, "AAsset_read"));
    api.close = reinterpret_cast<decltype(api.close)>(dlsym(lib, "AAsset_close"));
    return api;
}

// A Java exception left pending would break the next JNI call of this thread.
bool exception_cleared(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

void extract_asset(JNIEnv* env, const AssetApi& api, jobject context, const char* asset_name, const std::string& dest_dir) {
    const std::string out_path = dest_dir + "/" + asset_name;

    // A file the user already has there is never overwritten.
    if (std::ifstream(out_path).good()) return;

    jclass context_cls = env->GetObjectClass(context);
    jmethodID get_assets = env->GetMethodID(context_cls, "getAssets", "()Landroid/content/res/AssetManager;");
    if (get_assets == nullptr || exception_cleared(env)) {
        env->DeleteLocalRef(context_cls);
        return;
    }
    jobject asset_manager_object = env->CallObjectMethod(context, get_assets);
    if (asset_manager_object != nullptr && !exception_cleared(env)) {
        AAssetManager* manager = api.from_java(env, asset_manager_object);
        if (manager != nullptr) {
            if (AAsset* asset = api.open(manager, asset_name, AASSET_MODE_BUFFER)) {
                const off_t length = api.length(asset);
                std::vector<char> buffer(length > 0 ? static_cast<std::size_t>(length) : 0);
                const int got = length > 0 ? api.read(asset, buffer.data(), buffer.size()) : 0;
                api.close(asset);
                if (length > 0 && got == length) {
                    std::ofstream out(out_path, std::ios::binary);
                    if (out.is_open()) {
                        out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                        out.close();
                        log("copied %s from the launcher's assets to %s", asset_name, out_path.c_str());
                    }
                }
            }
        }
    }
    if (asset_manager_object != nullptr) env->DeleteLocalRef(asset_manager_object);
    env->DeleteLocalRef(context_cls);
}

void check_and_extract_assets() {
    if (g_vm == nullptr) return;
    const AssetApi api = load_asset_api();
    if (!api.ok()) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }

    jclass activity_thread = env->FindClass("android/app/ActivityThread");
    if (activity_thread != nullptr && !exception_cleared(env)) {
        jmethodID current_application =
            env->GetStaticMethodID(activity_thread, "currentApplication", "()Landroid/app/Application;");
        jobject context = current_application != nullptr && !exception_cleared(env)
                              ? env->CallStaticObjectMethod(activity_thread, current_application)
                              : nullptr;
        exception_cleared(env);
        if (context != nullptr) {
            jclass context_cls = env->GetObjectClass(context);
            jmethodID get_dir =
                env->GetMethodID(context_cls, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
            jobject file = get_dir != nullptr && !exception_cleared(env) ? env->CallObjectMethod(context, get_dir, nullptr) : nullptr;
            exception_cleared(env);
            if (file != nullptr) {
                jclass file_cls = env->GetObjectClass(file);
                jmethodID get_path = env->GetMethodID(file_cls, "getAbsolutePath", "()Ljava/lang/String;");
                jstring path = get_path != nullptr && !exception_cleared(env)
                                   ? static_cast<jstring>(env->CallObjectMethod(file, get_path))
                                   : nullptr;
                exception_cleared(env);
                if (path != nullptr) {
                    const char* chars = env->GetStringUTFChars(path, nullptr);
                    if (chars != nullptr) {
                        const std::string directory(chars);
                        env->ReleaseStringUTFChars(path, chars);
                        mkdir(directory.c_str(), 0777);
                        extract_asset(env, api, context, "zb-options.txt", directory);
                        extract_asset(env, api, context, "zb-tuning.txt", directory);
                        set_external_files_dir(directory);  // only counts if Java has not set it yet
                    }
                    env->DeleteLocalRef(path);
                }
                env->DeleteLocalRef(file_cls);
                env->DeleteLocalRef(file);
            }
            env->DeleteLocalRef(context_cls);
            env->DeleteLocalRef(context);
        }
        env->DeleteLocalRef(activity_thread);
    }
    if (attached) g_vm->DetachCurrentThread();
}

#else

void check_and_extract_assets() {}  // host test builds have no launcher assets

#endif

}  // namespace

void set_java_vm(JavaVM* vm) { g_vm = vm; }

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
    return dynamic_fallback_directory();  // Java has not said yet
}

std::string external_file_path(const char* name) {
    std::call_once(g_assets_extracted, check_and_extract_assets);  // the first request copies the default setting files
    std::string path = get_external_files_dir();
    path += '/';
    path += name;
    return path;
}

}  // namespace zb

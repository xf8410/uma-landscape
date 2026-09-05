// uma-landscape v0.2 — Hachimi v3 plugin (libhachimi_landscape.so)
// 强制横屏：走 UmaPatcher + Hachimi 插件路线（与 hlpatch/小黑板同载体，无需 root）
//
// 机制：
//  1. IL2CPP 层：game-initialized 后用 Hachimi 插件 API 拿 interceptor，hook 两个 icall：
//     UnityEngine.Screen::RequestOrientation / set_orientation → 竖屏请求一律改写为 Landscape
//  2. Android 层：JNI 拿 UnityPlayer.currentActivity，直接 setRequestedOrientation(LANDSCAPE)
//     （manifest 锁竖屏的运行时等价反制）；后台线程每 5s 重申一次
//  3. 开关：Hachimi 菜单内注册勾选框（小黑板同一个菜单），状态持久化到数据目录 cfg
//
// 不与 hlpatch 冲突：不碰 HTTP:18765、不碰任何 hlpatch hook 目标、不改 Hachimi 代码。

#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "uma-landscape", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "uma-landscape", __VA_ARGS__)

namespace {

// android.view.Surface.  ROTATION_180? no: ActivityInfo values used by setRequestedOrientation:
constexpr int ORI_LANDSCAPE = 0; // SCREEN_ORIENTATION_LANDSCAPE
constexpr int ORI_PORTRAIT = 1;  // SCREEN_ORIENTATION_PORTRAIT

std::atomic<bool> g_enabled{true};

// ---- Hachimi v3 plugin API (resolved by name, no struct layout dependency) ----
using GetApiFn = void *(*)(const char *);
GetApiFn g_api = nullptr;

using ResolveIcallFn = void *(*)(const char *);
using HachimiInstanceFn = void *(*)(void);
using GetInterceptorFn = void *(*)(void *);
using HookFn = void *(*)(void *interceptor, void *orig_addr, void *hook_addr);
using LogFn = void (*)(int level, const char *target, const char *message);
using RegisterSectionFn = bool (*)(void (*callback)(void *ui, void *userdata), void *userdata);
using CheckboxFn = bool (*)(void *ui, const char *text, bool *value);
using RegisterGameInitFn = bool (*)(void (*callback)(void *userdata), void *userdata);
using GetDataPathFn = const char *(*)(void);
using NotifyFn = bool (*)(const char *message);

void hlog(const char *msg) {
    LOGI("%s", msg);
    if (g_api) {
        auto log = (LogFn)g_api("log");
        if (log) log(3, "uma-landscape", msg); // 3 = Info
    }
}

// ---------------------------------------------------------------- config

std::string config_path() {
    if (!g_api) return "/data/local/tmp/uma_landscape.cfg";
    auto get_path = (GetDataPathFn)g_api("hachimi_get_data_path");
    if (!get_path) return "/data/local/tmp/uma_landscape.cfg";
    const char *p = get_path();
    return std::string(p ? p : "/data/local/tmp") + "/uma_landscape.cfg";
}

void persist() {
    std::ofstream f(config_path(), std::ios::trunc);
    f << (g_enabled.load() ? "on" : "off");
}

void load_config() {
    std::ifstream f(config_path());
    if (!f) return;
    std::string line;
    std::getline(f, line);
    if (line.find("off") != std::string::npos) g_enabled = false;
    hlog(g_enabled ? "config: landscape on" : "config: landscape off");
}

// ---------------------------------------------------------------- android layer

JavaVM *java_vm() {
    static JavaVM *cached = nullptr;
    if (cached) return cached;
    using GetVMsFn = jint (*)(JavaVM **, jsize, jsize *);
    auto get_vms = (GetVMsFn)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
    if (!get_vms) return nullptr;
    JavaVM *vms[4] = {nullptr, nullptr, nullptr, nullptr};
    jsize n = 0;
    get_vms(vms, 4, &n);
    if (n > 0) cached = vms[0];
    return cached;
}

// Activity.setRequestedOrientation may be invoked off the UI thread on modern
// Android (ClientTransaction is posted to the main handler internally).
// If a device misbehaves, v0.3 will post via Handler(mainLooper) instead.
bool request_orientation_java(int orientation) {
    JavaVM *vm = java_vm();
    if (!vm) return false;
    JNIEnv *env = nullptr;
    bool attached = false;
    if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attached = true;
    }

    bool ok = false;
    jclass up = env->FindClass("com/unity3d/player/UnityPlayer");
    if (up) {
        jfieldID fid = env->GetStaticFieldID(up, "currentActivity", "Landroid/app/Activity;");
        if (fid) {
            jobject act = env->GetStaticObjectField(up, fid);
            if (act) {
                jclass cls = env->GetObjectClass(act);
                jmethodID mid = env->GetMethodID(cls, "setRequestedOrientation", "(I)V");
                if (mid) {
                    env->CallVoidMethod(act, mid, orientation);
                    if (env->ExceptionCheck()) {
                        env->ExceptionDescribe();
                        env->ExceptionClear();
                    } else {
                        ok = true;
                    }
                }
            }
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) vm->DetachCurrentThread();
    return ok;
}

void enforcer_loop() {
    // UnityPlayer.currentActivity is null until Unity boots; retry until found.
    for (;;) {
        if (g_enabled.load()) {
            request_orientation_java(ORI_LANDSCAPE);
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

// ---------------------------------------------------------------- il2cpp layer

void (*g_orig_request_orientation)(int) = nullptr;
void (*g_orig_set_orientation)(int) = nullptr;

void my_request_orientation(int orientation) {
    if (g_enabled.load(std::memory_order_relaxed) && orientation != ORI_LANDSCAPE) {
        orientation = ORI_LANDSCAPE;
    }
    if (g_orig_request_orientation) g_orig_request_orientation(orientation);
}

void my_set_orientation(int orientation) {
    if (g_enabled.load(std::memory_order_relaxed) && orientation != ORI_LANDSCAPE) {
        orientation = ORI_LANDSCAPE;
    }
    if (g_orig_set_orientation) g_orig_set_orientation(orientation);
}

void install_il2cpp_hooks() {
    if (!g_api) return;
    auto instance = (HachimiInstanceFn)g_api("hachimi_instance");
    auto get_interceptor = (GetInterceptorFn)g_api("hachimi_get_interceptor");
    auto hook = (HookFn)g_api("interceptor_hook");
    auto resolve = (ResolveIcallFn)g_api("il2cpp_resolve_icall");
    if (!instance || !get_interceptor || !hook || !resolve) {
        LOGE("plugin api incomplete, cannot install icall hooks");
        return;
    }

    void *interceptor = get_interceptor(instance());
    void *req = resolve("UnityEngine.Screen::RequestOrientation(UnityEngine.ScreenOrientation)");
    void *set = resolve("UnityEngine.Screen::set_orientation(UnityEngine.ScreenOrientation)");

    if (req) {
        g_orig_request_orientation = (void (*)(int))hook(interceptor, req, (void *)my_request_orientation);
        hlog("hooked Screen::RequestOrientation");
    }
    if (set) {
        g_orig_set_orientation = (void (*)(int))hook(interceptor, set, (void *)my_set_orientation);
        hlog("hooked Screen::set_orientation");
    }
}

void on_game_initialized(void * /*userdata*/) {
    install_il2cpp_hooks();
}

// ---------------------------------------------------------------- gui switch

void menu_section(void *ui, void * /*userdata*/) {
    if (!g_api || !ui) return;
    auto checkbox = (CheckboxFn)g_api("gui_ui_checkbox");
    auto notify = (NotifyFn)g_api("gui_show_notification");
    if (!checkbox) return;

    bool v = g_enabled.load();
    if (checkbox(ui, "强制横屏 (Landscape)", &v) && v != g_enabled.load()) {
        g_enabled.store(v);
        request_orientation_java(v ? ORI_LANDSCAPE : ORI_PORTRAIT);
        persist();
        if (notify) notify(v ? "uma-landscape: 横屏开" : "uma-landscape: 横屏关");
        hlog(v ? "switch: enabled" : "switch: disabled");
    }
}

} // namespace

// ---------------------------------------------------------------- plugin entry

extern "C" __attribute__((visibility("default")))
int hachimi_init_v3(GetApiFn get_api, int version) {
    if (!get_api || version < 3) return 0;
    g_api = get_api;

    load_config();

    auto register_section = (RegisterSectionFn)get_api("gui_register_menu_section");
    auto register_init = (RegisterGameInitFn)get_api("hachimi_register_on_game_initialized");
    if (register_section) register_section(menu_section, nullptr);
    if (register_init) register_init(on_game_initialized, nullptr);

    std::thread(enforcer_loop).detach();
    hlog("uma-landscape v0.2 initialized (Hachimi plugin)");
    return 1; // InitResult::Ok
}

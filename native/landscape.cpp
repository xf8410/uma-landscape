// uma-landscape v0.1 — force landscape for Uma Musume (JP) via Zygisk + IL2CPP icall hook.
// Independent of Hachimi / hlpatch: no files of theirs are touched, and the hooked
// functions (Screen::RequestOrientation / Screen::set_orientation) are not hooked by
// either of them on Android (verified 2026-09-06: Hachimi-Edge only hooks them under
// #[cfg(target_os = "windows")]).

#include "zygisk.hpp"
#include <dobby.h>

#include <android/log.h>
#include <dlfcn.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "uma-landscape", __VA_ARGS__)

using zygisk::Api;
using zygisk::AppSpecializeArgs;
using zygisk::ServerSpecializeArgs;

// Unity ScreenOrientation: Unknown=0, Portrait=1, PortraitUpsideDown=2,
// Landscape=3 (== LandscapeLeft), LandscapeRight=4, AutoRotation=5.
constexpr int ORI_LANDSCAPE_LEFT = 3;

static std::atomic<bool>  g_enabled{true};
static std::atomic<int>   g_target{ORI_LANDSCAPE_LEFT};
static std::atomic<bool>  g_in_target{false};
static std::string        g_pkg;
static std::string        g_cfg_path;

static void *(*g_resolve_icall)(const char *) = nullptr;
static void (*g_orig_request_orientation)(int) = nullptr;
static void (*g_orig_set_orientation)(int) = nullptr;

// ---------------------------------------------------------------- icall hooks

static void my_request_orientation(int orientation) {
    if (g_enabled.load(std::memory_order_relaxed) && orientation != g_target.load()) {
        orientation = g_target.load();
    }
    if (g_orig_request_orientation) g_orig_request_orientation(orientation);
}

static void my_set_orientation(int orientation) {
    if (g_enabled.load(std::memory_order_relaxed) && orientation != g_target.load()) {
        orientation = g_target.load();
    }
    if (g_orig_set_orientation) g_orig_set_orientation(orientation);
}

// ---------------------------------------------------------------- config / switch

// Config: one line, read every 2 s. "on" / "off" / "left" / "right".
// Default path: /sdcard/Android/data/<pkg>/files/uma_landscape.cfg (editable with any
// file manager). HTTP switch on 127.0.0.1:18766 additionally mirrors this state.
static void apply_cfg_line(const std::string &line) {
    if (line == "on")  g_enabled = true;
    if (line == "off") g_enabled = false;
    if (line == "left")  { g_target = ORI_LANDSCAPE_LEFT; }
    if (line == "right") { g_target = 4; } // LandscapeRight
}

static void config_poll_loop() {
    for (;;) {
        std::ifstream f(g_cfg_path);
        if (f) { std::string line; std::getline(f, line); apply_cfg_line(line); }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// ---------------------------------------------------------------- http switch

static void http_thread_fn() {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return;
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(18766);
    if (bind(srv, (sockaddr *)&addr, sizeof(addr)) != 0) { close(srv); return; }
    listen(srv, 4);
    char buf[512];
    for (;;) {
        int c = accept(srv, nullptr, nullptr);
        if (c < 0) continue;
        int n = recv(c, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = 0;
            std::string req(buf);
            std::string resp = "uma-landscape: status=";
            resp += g_enabled ? "on" : "off";
            resp += g_target == ORI_LANDSCAPE_LEFT ? " left" : " right";
            if (req.find("GET /on") != std::string::npos)  { g_enabled = true;  resp = "switched on";  }
            if (req.find("GET /off") != std::string::npos) { g_enabled = false; resp = "switched off"; }
            if (req.find("GET /left") != std::string::npos)  { g_target = ORI_LANDSCAPE_LEFT; resp = "target left";  }
            if (req.find("GET /right") != std::string::npos) { g_target = 4; resp = "target right"; }
            send(c, resp.c_str(), resp.size(), 0);
        }
        close(c);
    }
}

// ---------------------------------------------------------------- hook install

static bool wait_for_il2cpp() {
    for (int i = 0; i < 600; ++i) { // up to ~5 min
        std::ifstream maps("/proc/self/maps");
        std::string line;
        while (std::getline(maps, line)) {
            if (line.find("libil2cpp.so") != std::string::npos) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

static void install_hooks() {
    if (!wait_for_il2cpp()) { LOGI("libil2cpp.so never appeared, giving up"); return; }

    g_resolve_icall = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "il2cpp_resolve_icall");
    if (!g_resolve_icall) { LOGI("il2cpp_resolve_icall not found"); return; }

    auto req_addr = g_resolve_icall(
        "UnityEngine.Screen::RequestOrientation(UnityEngine.ScreenOrientation)");
    auto set_addr = g_resolve_icall(
        "UnityEngine.Screen::set_orientation(UnityEngine.ScreenOrientation)");

    if (req_addr) {
        DobbyHook(req_addr, (void *)my_request_orientation, (void **)&g_orig_request_orientation);
        LOGI("hooked RequestOrientation @ %p", req_addr);
    }
    if (set_addr) {
        DobbyHook(set_addr, (void *)my_set_orientation, (void **)&g_orig_set_orientation);
        LOGI("hooked set_orientation @ %p", set_addr);
    }

    // Active enforcer: the game re-asserts portrait on scene changes; re-request
    // landscape periodically while enabled so the Activity orientation follows.
    std::thread([] {
        for (;;) {
            if (g_enabled && g_orig_request_orientation) {
                g_orig_request_orientation(g_target.load());
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
    }).detach();

    LOGI("uma-landscape hooks installed, pkg=%s", g_pkg.c_str());
}

// ---------------------------------------------------------------- zygisk module

class UmaLandscapeModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        const char *name = env_->GetStringUTFChars(args->nice_name, nullptr);
        if (name) {
            g_pkg = name;
            // VERIFY the real JP package id on your device before first release;
            // override via /data/local/tmp/uma_landscape.pkg if needed.
            const char *expected = std::getenv("UMA_PKG") ? std::getenv("UMA_PKG")
                                                          : "jp.co.cygames.umamusume";
            g_in_target = (g_pkg == expected);
            env_->ReleaseStringUTFChars(args->nice_name, name);
        }
        g_cfg_path = "/sdcard/Android/data/" + g_pkg + "/files/uma_landscape.cfg";
    }

    void postAppSpecialize(const AppSpecializeArgs *args) override {
        if (!g_in_target) return;
        std::thread(install_hooks).detach();
        std::thread(config_poll_loop).detach();
        std::thread(http_thread_fn).detach();
    }

private:
    Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
};

REGISTER_ZYGISK_MODULE(UmaLandscapeModule)

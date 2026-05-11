// Android NativeActivity entry point — Phase 9.
//
// This is the function that android_native_app_glue's ANativeActivity_onCreate
// calls on its dedicated thread. Our job:
//   1. Drive the ALooper event loop, draining lifecycle and input events.
//   2. Route motion events into banjo_android::touch::process_motion_event.
//   3. Once APP_CMD_INIT_WINDOW gives us an ANativeWindow*, spawn a worker
//      thread that runs banjo_android::run_game() — the recomp::start()
//      call blocks for the lifetime of the game and would otherwise prevent
//      us from continuing to pump lifecycle events here.

#ifdef __ANDROID__

#include <android_native_app_glue.h>
#include <android/log.h>
#include <android/window.h>
#include <jni.h>

#include <atomic>
#include <thread>

#include "android_touch.h"
#include "android_audio.h"
#include "android_run_game.h"

#define LOG_TAG "BK64-Main"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

ANativeWindow* g_native_window = nullptr;
std::atomic<bool> g_running{true};
std::atomic<bool> g_game_thread_started{false};
std::thread g_game_thread;

int32_t handle_input(struct android_app* /*app*/, AInputEvent* event) {
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        return banjo_android::touch::process_motion_event(event);
    }
    return 0;
}

struct android_app* g_app = nullptr;

void maybe_spawn_game_thread() {
    if (g_game_thread_started.load(std::memory_order_acquire)) return;
    if (g_native_window == nullptr) return;
    if (g_app == nullptr) return;

    g_game_thread_started.store(true, std::memory_order_release);
    LOGI("spawning game thread (window=%p, internal=%s)",
         g_native_window, g_app->activity->internalDataPath);
    banjo_android::AppPaths paths{
        .internal_data_path = g_app->activity->internalDataPath,
        .external_data_path = g_app->activity->externalDataPath,
    };
    g_game_thread = std::thread([w = g_native_window, paths]() {
        banjo_android::run_game(w, paths);
        LOGI("game thread exited");
    });
}

void handle_cmd(struct android_app* app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        g_native_window = app->window;
        if (g_native_window) {
            int w = ANativeWindow_getWidth(g_native_window);
            int h = ANativeWindow_getHeight(g_native_window);
            LOGI("APP_CMD_INIT_WINDOW: %dx%d", w, h);
            banjo_android::touch::set_viewport(w, h);
            maybe_spawn_game_thread();
        }
        break;

    case APP_CMD_TERM_WINDOW:
        LOGI("APP_CMD_TERM_WINDOW");
        // We deliberately don't tear down the game thread here — surface
        // recreation while the process keeps running is a Phase 10
        // concern. For now we'll stale-pointer if Android destroys the
        // window, which is fine for the boot-flow validation we want.
        g_native_window = nullptr;
        break;

    case APP_CMD_GAINED_FOCUS:
        LOGI("APP_CMD_GAINED_FOCUS");
        break;

    case APP_CMD_LOST_FOCUS:
        LOGI("APP_CMD_LOST_FOCUS");
        break;

    case APP_CMD_PAUSE:
        LOGI("APP_CMD_PAUSE");
        break;

    case APP_CMD_RESUME:
        LOGI("APP_CMD_RESUME");
        break;

    case APP_CMD_DESTROY:
        LOGI("APP_CMD_DESTROY");
        g_running = false;
        break;
    }
}

}  // anonymous namespace

extern "C" void banjo_android_set_jvm(JavaVM* vm);

extern "C" void android_main(struct android_app* app) {
    LOGI("BK64-Online native entry — android_main()");

    g_app = app;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;

    // NativeActivity loads our .so via dlopen, so JNI_OnLoad never fires.
    // Hand the render context the JavaVM directly so it can attach a
    // background thread later for the first-frame Java callback.
    if (app->activity != nullptr) {
        banjo_android_set_jvm(app->activity->vm);
    }

    // Start the audio output stream early so it's ready when the game
    // produces samples. Safe to call before the window exists.
    if (!banjo_android::audio::start()) {
        LOGW("Oboe audio failed to open — falling back to silent playback");
    }

    // Standard NativeActivity event loop. The game thread (spawned on the
    // first APP_CMD_INIT_WINDOW) runs recomp::start() in parallel — that
    // call blocks for the lifetime of the game, so we can't run it here
    // without starving lifecycle/input events.
    while (g_running.load()) {
        int events;
        struct android_poll_source* source;
        // Block waiting for events; once the game thread is running it'll
        // be the one driving frame pacing.
        int ident = ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void**>(&source));
        if (ident < 0) continue;

        if (source != nullptr) {
            source->process(app, source);
        }

        if (app->destroyRequested != 0) {
            LOGI("destroyRequested — exiting android_main");
            break;
        }
    }

    if (g_game_thread.joinable()) {
        // recomp::start has its own quit signaling; we'll detach for now to
        // avoid hanging the activity on shutdown. Phase 10 should wire a
        // proper ultramodern::quit() bridge.
        g_game_thread.detach();
    }
    banjo_android::audio::stop();
    LOGI("android_main exiting");
}

#endif  // __ANDROID__

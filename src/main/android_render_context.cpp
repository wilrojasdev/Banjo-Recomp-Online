// Android render context — minimal RT64::Application driver for the
// NativeActivity / ANativeWindow path.
//
// Mirrors the bits of recompui::renderer::RT64Context (see
// lib/RecompFrontend/recompui/src/renderer/rt64_render_context.cpp) we
// actually need on Android. Texture pack hot-swap, config-change
// downsampling, refresh-rate publishing, etc. are intentionally omitted —
// they're tied to recompui (not built on Android) and not needed to push
// pixels.

#ifdef __ANDROID__

#include "android_render_context.h"

#include <android/log.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <jni.h>

#include "hle/rt64_application.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"

// NativeActivity loads our .so via dlopen, NOT System.loadLibrary, so the
// JNI_OnLoad hook never fires and we can't capture the JavaVM that way.
// Instead, android_main.cpp calls banjo_android_set_jvm() once the
// android_app struct is wired up — it has app->activity->vm.
//
// Separately, FindClass from a native-thread JNIEnv doesn't see app classes
// (it uses the system classloader). The Java side calls nativeInit() right
// after loadLibrary in MainActivity's static initializer; we use that
// JNIEnv (which DOES have the app classloader) to cache a global ref to
// MainActivity. Background threads invoke cached methods without FindClass.
static JavaVM* g_jvm = nullptr;
static jclass g_main_activity_class = nullptr;
static jmethodID g_notify_rom_gate_method = nullptr;
static jmethodID g_notify_game_started_method = nullptr;
static jmethodID g_notify_return_to_launcher_method = nullptr;
static jmethodID g_notify_first_game_frame_method = nullptr;
static std::atomic<bool> g_expecting_first_game_frame{false};
// The hook below fires on every present, including launcher transition
// presents that happen BEFORE the game thread has produced any DLs. To keep
// the shader splash from blinking off the moment Start Game is pressed, we
// require both a minimum number of presents AND a minimum elapsed time
// between arm and dismiss. The 45 s timeout on the Java side is still the
// upper bound, so if the game-thread bug suppresses real frames we still
// give the user some visible feedback rather than a soft hang.
static std::atomic<int>     g_presents_since_arm{0};
static std::atomic<int64_t> g_arm_time_ms{0};
static constexpr int        kMinPresentsBeforeDismiss = 30;     // ~0.5 s @ 60 Hz
static constexpr int64_t    kMinElapsedMsBeforeDismiss = 1500;  // safety floor

static int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// recompui::RT64Context::update_screen (rt64_render_context.cpp) calls this
// pointer after each present when linked into BanjoRecompiled.
extern "C" void (*recompui_android_after_present_hook)(void) = nullptr;

static bool call_main_activity_method(const char* tag, jmethodID method);
static bool call_main_activity_void_bool(const char* tag, jmethodID method, bool arg);

extern "C" __attribute__((visibility("default")))
void banjo_android_set_jvm(JavaVM* vm) {
    g_jvm = vm;
    __android_log_print(ANDROID_LOG_INFO, "BK64-Render",
        "banjo_android_set_jvm: vm=%p", (void*)vm);
}

extern "C" __attribute__((visibility("default")))
void banjo_android_mark_expecting_first_game_frame() {
    g_presents_since_arm.store(0, std::memory_order_release);
    g_arm_time_ms.store(now_ms(), std::memory_order_release);
    g_expecting_first_game_frame.store(true, std::memory_order_release);
}

extern "C" __attribute__((visibility("default")))
void banjo_android_cancel_expecting_first_game_frame() {
    g_expecting_first_game_frame.store(false, std::memory_order_release);
}

extern "C" __attribute__((visibility("default")))
void banjo_android_notify_rom_gate(bool rom_present) {
    call_main_activity_void_bool("notify_rom_gate", g_notify_rom_gate_method, rom_present);
}

// Start Game pressed: Java shows shader splash. We only arm the first-frame
// latch *after* recomp::start_game() returns so a leftover launcher present
// cannot dismiss the splash early (see android_run_game.cpp).
extern "C" __attribute__((visibility("default")))
void banjo_android_notify_game_started() {
    call_main_activity_method("notify_game_started", g_notify_game_started_method);
}

extern "C" __attribute__((visibility("default")))
void banjo_android_notify_return_to_launcher() {
    banjo_android_cancel_expecting_first_game_frame();
    call_main_activity_method("notify_return_to_launcher",
                              g_notify_return_to_launcher_method);
}

extern "C" void banjo_android_on_present_after_rt64_update() {
    if (!g_expecting_first_game_frame.load(std::memory_order_acquire)) {
        return;
    }
    int n = g_presents_since_arm.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (n < kMinPresentsBeforeDismiss) return;
    int64_t elapsed = now_ms() - g_arm_time_ms.load(std::memory_order_acquire);
    if (elapsed < kMinElapsedMsBeforeDismiss) return;
    if (!g_expecting_first_game_frame.exchange(false, std::memory_order_acq_rel)) return;
    call_main_activity_method("notify_first_game_frame", g_notify_first_game_frame_method);
}

// Called from MainActivity's static initializer (Java thread, app classloader
// reachable). Stash a global ref to the class + the methodID for later.
extern "C" JNIEXPORT void JNICALL
Java_com_banjorecomp_online_MainActivity_nativeInit(JNIEnv* env, jclass clazz) {
    if (env == nullptr) return;
    g_main_activity_class = static_cast<jclass>(env->NewGlobalRef(clazz));
    g_notify_rom_gate_method =
        env->GetStaticMethodID(clazz, "nativeNotifyRomGate", "(Z)V");
    g_notify_game_started_method =
        env->GetStaticMethodID(clazz, "nativeNotifyGameStarted", "()V");
    g_notify_return_to_launcher_method =
        env->GetStaticMethodID(clazz, "nativeNotifyReturnToLauncher", "()V");
    g_notify_first_game_frame_method =
        env->GetStaticMethodID(clazz, "nativeNotifyFirstGameFrame", "()V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    recompui_android_after_present_hook = banjo_android_on_present_after_rt64_update;
    __android_log_print(ANDROID_LOG_INFO, "BK64-Render",
        "nativeInit: class=%p romGate=%p gameStarted=%p returnToLauncher=%p firstGameFrame=%p hook=%p",
        (void*)g_main_activity_class,
        (void*)g_notify_rom_gate_method,
        (void*)g_notify_game_started_method,
        (void*)g_notify_return_to_launcher_method,
        (void*)g_notify_first_game_frame_method,
        (void*)recompui_android_after_present_hook);
}

#define LOG_TAG "BK64-Render"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Heartbeat counters for the gfx-pipeline entry points (AndroidRenderContext).
static std::atomic<uint64_t> g_dl_count{0};
static std::atomic<uint64_t> g_dummy_count{0};
static std::atomic<uint64_t> g_screen_update_count{0};
static std::atomic<std::chrono::steady_clock::time_point> g_last_log{std::chrono::steady_clock::now()};

static bool call_main_activity_method(const char* tag, jmethodID method) {
    if (g_jvm == nullptr || g_main_activity_class == nullptr || method == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, "BK64-Render",
            "%s: missing jvm/class/method (jvm=%p cls=%p mid=%p)",
            tag, (void*)g_jvm, (void*)g_main_activity_class, (void*)method);
        return false;
    }

    JavaVM* vm = g_jvm;
    JNIEnv* env = nullptr;
    bool attached = false;
    jint getEnvResult = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvResult == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            __android_log_print(ANDROID_LOG_WARN, "BK64-Render",
                "%s: AttachCurrentThread failed", tag);
            return false;
        }
        attached = true;
    } else if (getEnvResult != JNI_OK) {
        __android_log_print(ANDROID_LOG_WARN, "BK64-Render",
            "%s: GetEnv returned %d", tag, (int)getEnvResult);
        return false;
    }

    env->CallStaticVoidMethod(g_main_activity_class, method);
    __android_log_print(ANDROID_LOG_INFO, "BK64-Render",
        "%s: signalled MainActivity", tag);

    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) vm->DetachCurrentThread();
    return true;
}

static bool call_main_activity_void_bool(const char* tag, jmethodID method, bool arg) {
    if (g_jvm == nullptr || g_main_activity_class == nullptr || method == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, "BK64-Render",
            "%s: missing jvm/class/method (jvm=%p cls=%p mid=%p)",
            tag, (void*)g_jvm, (void*)g_main_activity_class, (void*)method);
        return false;
    }

    JavaVM* vm = g_jvm;
    JNIEnv* env = nullptr;
    bool attached = false;
    jint getEnvResult = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (getEnvResult == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            __android_log_print(ANDROID_LOG_WARN, "BK64-Render",
                "%s: AttachCurrentThread failed", tag);
            return false;
        }
        attached = true;
    } else if (getEnvResult != JNI_OK) {
        __android_log_print(ANDROID_LOG_WARN, "BK64-Render",
            "%s: GetEnv returned %d", tag, (int)getEnvResult);
        return false;
    }

    env->CallStaticVoidMethod(g_main_activity_class, method, arg ? JNI_TRUE : JNI_FALSE);
    __android_log_print(ANDROID_LOG_INFO, "BK64-Render",
        "%s: signalled MainActivity (bool=%d)", tag, (int)arg);

    if (env->ExceptionCheck()) env->ExceptionClear();
    if (attached) vm->DetachCurrentThread();
    return true;
}

static void heartbeat(const char* who) {
    auto now = std::chrono::steady_clock::now();
    auto last = g_last_log.load();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count() >= 5 &&
        g_last_log.compare_exchange_strong(last, now)) {
        __android_log_print(ANDROID_LOG_INFO, "BK64-Render",
            "heartbeat[%s] dl=%llu dummy=%llu screen_update=%llu",
            who,
            (unsigned long long)g_dl_count.load(),
            (unsigned long long)g_dummy_count.load(),
            (unsigned long long)g_screen_update_count.load());
    }
}

// RT64 expects pointers to RDP / VI registers it can read/write directly. We
// own them here for the lifetime of the process.
unsigned int g_MI_INTR_REG = 0;

unsigned int g_DPC_START_REG = 0;
unsigned int g_DPC_END_REG = 0;
unsigned int g_DPC_CURRENT_REG = 0;
unsigned int g_DPC_STATUS_REG = 0;
unsigned int g_DPC_CLOCK_REG = 0;
unsigned int g_DPC_BUFBUSY_REG = 0;
unsigned int g_DPC_PIPEBUSY_REG = 0;
unsigned int g_DPC_TMEM_REG = 0;

uint8_t g_DMEM[0x1000];
uint8_t g_IMEM[0x1000];

static void dummy_check_interrupts() {}

static ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult r) {
    switch (r) {
        case RT64::Application::SetupResult::Success:
            return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound:
            return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:
            return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound:
            return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:
            return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }
    return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
}

class AndroidRenderContext final : public ultramodern::renderer::RendererContext {
public:
    AndroidRenderContext(uint8_t* rdram, ANativeWindow* window, bool developer_mode) {
        RT64::Application::Core appCore{};
        appCore.window = window;
        appCore.checkInterrupts = dummy_check_interrupts;

        static unsigned char dummy_rom_header[0x40];
        appCore.HEADER = dummy_rom_header;
        appCore.RDRAM = rdram;
        appCore.DMEM = g_DMEM;
        appCore.IMEM = g_IMEM;

        appCore.MI_INTR_REG = &g_MI_INTR_REG;

        appCore.DPC_START_REG = &g_DPC_START_REG;
        appCore.DPC_END_REG = &g_DPC_END_REG;
        appCore.DPC_CURRENT_REG = &g_DPC_CURRENT_REG;
        appCore.DPC_STATUS_REG = &g_DPC_STATUS_REG;
        appCore.DPC_CLOCK_REG = &g_DPC_CLOCK_REG;
        appCore.DPC_BUFBUSY_REG = &g_DPC_BUFBUSY_REG;
        appCore.DPC_PIPEBUSY_REG = &g_DPC_PIPEBUSY_REG;
        appCore.DPC_TMEM_REG = &g_DPC_TMEM_REG;

        ultramodern::renderer::ViRegs* vi_regs = ultramodern::renderer::get_vi_regs();
        appCore.VI_STATUS_REG = &vi_regs->VI_STATUS_REG;
        appCore.VI_ORIGIN_REG = &vi_regs->VI_ORIGIN_REG;
        appCore.VI_WIDTH_REG = &vi_regs->VI_WIDTH_REG;
        appCore.VI_INTR_REG = &vi_regs->VI_INTR_REG;
        appCore.VI_V_CURRENT_LINE_REG = &vi_regs->VI_V_CURRENT_LINE_REG;
        appCore.VI_TIMING_REG = &vi_regs->VI_TIMING_REG;
        appCore.VI_V_SYNC_REG = &vi_regs->VI_V_SYNC_REG;
        appCore.VI_H_SYNC_REG = &vi_regs->VI_H_SYNC_REG;
        appCore.VI_LEAP_REG = &vi_regs->VI_LEAP_REG;
        appCore.VI_H_START_REG = &vi_regs->VI_H_START_REG;
        appCore.VI_V_START_REG = &vi_regs->VI_V_START_REG;
        appCore.VI_V_BURST_REG = &vi_regs->VI_V_BURST_REG;
        appCore.VI_X_SCALE_REG = &vi_regs->VI_X_SCALE_REG;
        appCore.VI_Y_SCALE_REG = &vi_regs->VI_Y_SCALE_REG;

        RT64::ApplicationConfiguration appConfig;
        appConfig.useConfigurationFile = false;
        // Pin RT64's working dir to a writable spot under the app's private
        // storage. Without this, detectDataPath falls through to "/" on
        // Android (cwd) and create_directories("/.rt64") throws because /
        // is read-only. The path was registered earlier by the run_game
        // boot via recomp::register_config_path; we mirror it here so RT64
        // and recomp share the same storage root.
        appConfig.dataPath = recomp::get_config_path();
        appConfig.detectDataPath = false;

        app = std::make_unique<RT64::Application>(appCore, appConfig);

        // Force Vulkan — Android has no D3D12/Metal anyway, but Auto would
        // probe libraries we haven't shipped.
        app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
        app->userConfig.developerMode = developer_mode;
        app->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
        app->userConfig.antialiasing = RT64::UserConfiguration::Antialiasing::None;
        app->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Display;
        app->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
        // Force gbi depth branches to prevent LODs from kicking in (matches
        // the desktop default in rt64_render_context.cpp).
        app->enhancementConfig.f3dex.forceBranch = true;
        app->enhancementConfig.textureLOD.scale = true;
        app->enhancementConfig.presentation.mode =
            RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;

        LOGI("AndroidRenderContext: calling app->setup()");
        setup_result = map_setup_result(app->setup(0));
        chosen_api = ultramodern::renderer::GraphicsApi::Vulkan;
        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            LOGE("AndroidRenderContext: app->setup() failed (%d)", static_cast<int>(setup_result));
            app = nullptr;
            return;
        }

        // Block here until the ubershader pipelines BK64's intro needs are
        // ready (idx=0 and idx=3, pinned to thread 0 by RasterShaderUber's
        // constructor). recomp::start blocks on this setup() returning, so
        // pausing inside it stalls the game loop AND the audio scheduler
        // until rendering can actually keep up. Without this, the splash
        // hides 15 s of game time the user can hear but not see.
        if (app->rasterShaderCache && app->rasterShaderCache->shaderUber) {
            LOGI("AndroidRenderContext: waiting on ubershader pipelines 0+3");
            app->rasterShaderCache->shaderUber->waitForPipelineCreation();
            LOGI("AndroidRenderContext: ubershader pipelines 0+3 ready");
        }

        LOGI("AndroidRenderContext: ready");
    }

    bool valid() override { return static_cast<bool>(app); }

    bool update_config(const ultramodern::renderer::GraphicsConfig& /*old_config*/,
                       const ultramodern::renderer::GraphicsConfig& /*new_config*/) override {
        // Live config updates aren't wired on Android yet — there's no
        // in-game settings UI to drive them.
        return false;
    }

    void enable_instant_present() override {
        if (!app) return;
        app->enhancementConfig.presentation.mode =
            RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        app->updateEnhancementConfig();
    }

    void send_dl(const OSTask* task) override {
        if (!app) return;
        g_dl_count.fetch_add(1, std::memory_order_relaxed);
        heartbeat("send_dl");
        app->state->rsp->reset();
        app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
        app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
    }

    void send_dummy_workload(uint32_t fb_address) override {
        if (!app) return;
        g_dummy_count.fetch_add(1, std::memory_order_relaxed);
        heartbeat("send_dummy_workload");
        app->state->listProcessBegin();
        app->state->rdp->setColorImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, fb_address);
        app->state->rdp->setOtherMode(0x382C30, 0);
        app->state->rdp->fillRect(0, 0, 320 << 2, 240 << 2);
        app->state->fullSync();
        app->state->listProcessEnd();
    }

    void update_screen() override {
        if (!app) return;
        g_screen_update_count.fetch_add(1, std::memory_order_relaxed);
        heartbeat("update_screen");
        app->updateScreen();
        banjo_android_on_present_after_rt64_update();
    }

    void shutdown() override {
        if (app) {
            app->end();
        }
    }

    uint32_t get_display_framerate() const override {
        if (!app) return 60;
        return app->presentQueue->ext.sharedResources->swapChainRate;
    }

    float get_resolution_scale() const override {
        // Mali Valhall G57 on the A24 can't sustain 60 fps at the 1080p
        // WindowIntegerScale (5×) the desktop path picks. Cap at 2× until
        // we have a perf-driven setting or a faster device.
        return 2.0f;
    }

private:
    std::unique_ptr<RT64::Application> app;
};

namespace banjo_android::renderer {

std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
    uint8_t* rdram,
    ultramodern::renderer::WindowHandle window_handle,
    bool developer_mode) {
    if (window_handle == nullptr) {
        LOGE("create_render_context called with nullptr window");
        return nullptr;
    }
    return std::make_unique<AndroidRenderContext>(rdram, window_handle, developer_mode);
}

}  // namespace banjo_android::renderer

#endif  // __ANDROID__

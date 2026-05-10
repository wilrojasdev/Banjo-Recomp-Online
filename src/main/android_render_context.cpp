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

#include "hle/rt64_application.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"

#define LOG_TAG "BK64-Render"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// Heartbeat counters for the gfx-pipeline entry points. Logged every ~5s so
// logcat carries a clear "frames are flowing" signal during Phase 10 bringup.
// Stays cheap (relaxed atomics + a single log line) and harmless once the
// port is stable.
std::atomic<uint64_t> g_dl_count{0};
std::atomic<uint64_t> g_dummy_count{0};
std::atomic<uint64_t> g_screen_update_count{0};
std::atomic<std::chrono::steady_clock::time_point> g_last_log{std::chrono::steady_clock::now()};

void heartbeat(const char* who) {
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

void dummy_check_interrupts() {}

ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult r) {
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
        constexpr int ReferenceHeight = 240;
        if (!app) return 1.0f;
        if (app->userConfig.resolution == RT64::UserConfiguration::Resolution::WindowIntegerScale &&
            app->sharedQueueResources->swapChainHeight > 0) {
            return std::max(float((app->sharedQueueResources->swapChainHeight + ReferenceHeight - 1) / ReferenceHeight), 1.0f);
        }
        return 1.0f;
    }

private:
    std::unique_ptr<RT64::Application> app;
};

}  // namespace

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

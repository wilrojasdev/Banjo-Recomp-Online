// Android game runner — minimum boot to drive recomp::start() into rt64.
//
// The desktop int main() is monolithic (SDL, NFD, recompui launcher, mod
// language packs, chat input, playerlist UI, etc.). For Phase 9 we want a
// single goal: get pixels on screen on a real Android device. Everything
// else (launcher, multiplayer, settings UI, language picker) is layered in
// later iterations.
//
// Symbols we reuse from main.cpp via extern (compiled but never invoked
// because int main() never runs on Android):
//   - supported_games (the BK GameEntry vector)
//   - get_rsp_microcode  (audio task → ucode dispatcher)
//   - banjo::get_game_thread_name  (thread naming for debugger)
//   - All recomp_* / recomp_net_* / bknet_debug_log REGISTER_FUNC targets
//   - banjo::register_bk_overlays / register_bk_patches

#ifdef __ANDROID__

#include "android_run_game.h"

#include <android/log.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <filesystem>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/rsp.hpp"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/error_handling.hpp"

#include "ovl_patches.hpp"
#include "android_audio.h"
#include "android_touch.h"
#include "android_render_context.h"

// Object-extension init lives in src/game/recomp_extension_api.cpp.
// Forward-declared here to avoid pulling in the full recomputil header tree.
namespace recomputil {
    void init_extended_object_data(size_t num_types);
    void register_data_api_exports();
}

#define LOG_TAG "BK64-Run"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// --- Symbols owned by main.cpp ---
extern std::vector<recomp::GameEntry> supported_games;
extern RspUcodeFunc* get_rsp_microcode(const OSTask* task);

namespace banjo {
    std::string get_game_thread_name(const OSThread* t);
}

// REGISTER_FUNC targets — defined elsewhere in the codebase, exposed via
// recomp::overlays::register_base_export. Declarations come from the same
// headers main.cpp uses.
extern "C" {
    // recomp_api / recomp_extension_api
    void recomp_get_window_resolution(uint8_t* rdram, recomp_context* ctx);
    void recomp_get_target_aspect_ratio(uint8_t* rdram, recomp_context* ctx);
    void recomp_get_target_framerate(uint8_t* rdram, recomp_context* ctx);
    void recomp_get_cutscene_aspect_ratio(uint8_t* rdram, recomp_context* ctx);
    void recomp_get_analog_cam_enabled(uint8_t* rdram, recomp_context* ctx);
    void recomp_get_right_analog_inputs(uint8_t* rdram, recomp_context* ctx);
    void recomp_get_bgm_volume(uint8_t* rdram, recomp_context* ctx);
    void recomp_get_inverted_axes(uint8_t* rdram, recomp_context* ctx);
    void recomp_get_analog_inverted_axes(uint8_t* rdram, recomp_context* ctx);

    // net_recomp_api
    void recomp_net_push_full_state(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_push_local_state(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_is_connected(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_get_remote_state(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_get_remote_count(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_get_local_player_id(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_send_collectible(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_send_enemy_death(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_send_flag_change(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_pop_world_event(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_is_host(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_send_enemy_positions(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_get_enemy_positions(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_should_send_full_sync(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_send_world_state_full(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_pop_full_state(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_am_i_world_owner(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_push_level_id(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_is_online_mode(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_get_save_slot(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_is_join_mode(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_push_camera_state(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_send_conga_orange(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_pop_conga_orange(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_should_send_host_eeprom(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_send_host_eeprom(uint8_t* rdram, recomp_context* ctx);
    void recomp_net_pop_host_eeprom(uint8_t* rdram, recomp_context* ctx);
    void bknet_debug_log(uint8_t* rdram, recomp_context* ctx);
}

#define REGISTER_FUNC(name) recomp::overlays::register_base_export(#name, name)

namespace {

// The window we hand out to recomp::start via the create_window callback.
// android_main owns the lifetime; we just borrow.
ANativeWindow* g_window = nullptr;

// Latched once we've kicked off recomp::start_game() so we don't restart it
// every frame in the update_gfx callback.
std::atomic<bool> g_game_started{false};

ultramodern::renderer::WindowHandle android_create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    LOGI("android_create_window -> %p", g_window);
    return g_window;
}

// Phase 10: auto-start the first supported game on the first gfx update.
// vi_thread_func now null-guards update_vi(), so it's safe to flip
// is_game_started before the game thread publishes a VI mode — the early
// ticks just no-op until osViSetMode lands.
void android_update_gfx(void*) {
    bool expected = false;
    if (!g_game_started.compare_exchange_strong(expected, true)) {
        return;
    }
    if (supported_games.empty()) {
        LOGE("android_update_gfx: no supported games registered");
        return;
    }
    const auto& game = supported_games.front();
    LOGI("android_update_gfx: starting game id='%s'",
         reinterpret_cast<const char*>(game.game_id.c_str()));
    recomp::start_game(game.game_id, {});
}

void android_message_box(const char* msg) {
    LOGE("[message_box] %s", msg);
}

}  // namespace

namespace banjo_android {

void run_game(ANativeWindow* window, AppPaths paths) {
    LOGI("run_game: entered with window=%p internal=%s", window, paths.internal_data_path);
    g_window = window;

    recomp::Version project_version{1, 6, 0};

    // Anchor recomp's config / saves / stored-rom paths in the app's
    // *external* private storage when available
    // (/storage/emulated/0/Android/data/<pkg>/files). That dir is reachable
    // via adb push without root, so the user can drop in a BK ROM during
    // bring-up. Falls back to internal storage if the device isn't mounted.
    // Without either, recomp would try to write to "/" (cwd) and the boot
    // throws a filesystem_error in check_all_stored_roms.
    const char* chosen = nullptr;
    if (paths.external_data_path && *paths.external_data_path) {
        chosen = paths.external_data_path;
    } else if (paths.internal_data_path && *paths.internal_data_path) {
        chosen = paths.internal_data_path;
    }
    if (chosen != nullptr) {
        std::filesystem::path config_dir = chosen;
        std::error_code ec;
        std::filesystem::create_directories(config_dir, ec);
        if (ec) {
            LOGW("create_directories(%s) failed: %s",
                 config_dir.c_str(), ec.message().c_str());
        }
        recomp::register_config_path(config_dir);
        LOGI("config_path registered: %s", config_dir.c_str());
    } else {
        LOGW("no internal/external data path provided — recomp will use cwd");
    }

    // Register supported games (BK).
    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }

    recomp::mods::register_deprecated_mod(
        "bk_recomp_mod_fov_slider",
        recomp::mods::DeprecationStatus::BrokenVersion,
        recomp::Version(1, 1, 0));

    // Register all the recomp_* / recomp_net_* exports the patches expect.
    REGISTER_FUNC(recomp_get_window_resolution);
    REGISTER_FUNC(recomp_get_target_aspect_ratio);
    REGISTER_FUNC(recomp_get_target_framerate);
    REGISTER_FUNC(recomp_get_cutscene_aspect_ratio);
    REGISTER_FUNC(recomp_get_analog_cam_enabled);
    REGISTER_FUNC(recomp_get_right_analog_inputs);
    REGISTER_FUNC(recomp_get_bgm_volume);
    REGISTER_FUNC(recomp_get_inverted_axes);
    REGISTER_FUNC(recomp_get_analog_inverted_axes);
    REGISTER_FUNC(recomp_net_push_full_state);
    REGISTER_FUNC(recomp_net_push_local_state);
    REGISTER_FUNC(recomp_net_is_connected);
    REGISTER_FUNC(recomp_net_get_remote_state);
    REGISTER_FUNC(recomp_net_get_remote_count);
    REGISTER_FUNC(recomp_net_get_local_player_id);
    REGISTER_FUNC(recomp_net_send_collectible);
    REGISTER_FUNC(recomp_net_send_enemy_death);
    REGISTER_FUNC(recomp_net_send_flag_change);
    REGISTER_FUNC(recomp_net_pop_world_event);
    REGISTER_FUNC(recomp_net_is_host);
    REGISTER_FUNC(recomp_net_send_enemy_positions);
    REGISTER_FUNC(recomp_net_get_enemy_positions);
    REGISTER_FUNC(recomp_net_should_send_full_sync);
    REGISTER_FUNC(recomp_net_send_world_state_full);
    REGISTER_FUNC(recomp_net_pop_full_state);
    REGISTER_FUNC(recomp_net_am_i_world_owner);
    REGISTER_FUNC(recomp_net_push_level_id);
    REGISTER_FUNC(recomp_net_is_online_mode);
    REGISTER_FUNC(recomp_net_get_save_slot);
    REGISTER_FUNC(recomp_net_is_join_mode);
    REGISTER_FUNC(recomp_net_push_camera_state);
    REGISTER_FUNC(recomp_net_send_conga_orange);
    REGISTER_FUNC(recomp_net_pop_conga_orange);
    REGISTER_FUNC(recomp_net_should_send_host_eeprom);
    REGISTER_FUNC(recomp_net_send_host_eeprom);
    REGISTER_FUNC(recomp_net_pop_host_eeprom);
    REGISTER_FUNC(bknet_debug_log);

    // Hashmap / hashset / slotmap exports that patches use.
    recomputil::register_data_api_exports();

    banjo::register_bk_overlays();
    banjo::register_bk_patches();

    // Patches register object extensions in their core1_init path. Without
    // this, type_contexts is empty and recomp_register_object_extension_*
    // segfaults indexing it. Two types: Props (0) and ActorMarkers (1).
    recomputil::init_extended_object_data(2);

    LOGI("callbacks setup");

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };

    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = [](uint8_t* rdram,
                                    ultramodern::renderer::WindowHandle window_handle,
                                    bool developer_mode) {
            return banjo_android::renderer::create_render_context(rdram, window_handle, developer_mode);
        },
    };

    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = nullptr,
        .create_window = android_create_window,
        .update_gfx = android_update_gfx,
    };

    // Audio: Oboe was opened by android_main before we got here; just hand
    // recomp the ring-buffer callbacks and it'll start producing samples.
    ultramodern::audio_callbacks_t audio_callbacks =
        banjo_android::audio::make_audio_callbacks();

    // Touch overlay maps screen taps into N64 controller state.
    ultramodern::input::callbacks_t input_callbacks =
        banjo_android::touch::make_input_callbacks();

    ultramodern::events::callbacks_t thread_callbacks{
        .vi_callback = nullptr,
        .gfx_init_callback = nullptr,
    };

    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = android_message_box,
    };

    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = banjo::get_game_thread_name,
    };

    LOGI("calling recomp::start (will block until quit)");
    recomp::start(
        project_version,
        ultramodern::renderer::WindowHandle{},  // empty → create_window callback fires
        rsp_callbacks,
        renderer_callbacks,
        audio_callbacks,
        input_callbacks,
        gfx_callbacks,
        thread_callbacks,
        error_handling_callbacks,
        threads_callbacks
    );

    LOGI("recomp::start returned — game loop ended");
}

}  // namespace banjo_android

#endif  // __ANDROID__

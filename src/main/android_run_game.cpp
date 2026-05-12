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

// recompui — launcher / UI state on the same code path as desktop. The render
// context is now provided by recompui::renderer::create_render_context which
// installs the UI's draw hooks into RT64.
#include "recompui/recompui.h"
#include "recompui/renderer.h"
#include "recompui/program_config.h"
#include "banjo_config.h"
#include "banjo_launcher.h"  // banjo::launcher_animation_{setup,update}
#include "file.h"  // recompui::file::set_program_path_override

// banjo::init_config builds all config tabs the launcher renders.
namespace banjo {
    void init_config();
    namespace locale {
        void init();
    }
}

// JNI trampolines exposed by android_render_context.cpp. Forward-declared
// here so we can poke MainActivity from the launcher's Start Game callback
// without dragging <jni.h> in.
extern "C" void banjo_android_notify_game_started();
extern "C" void banjo_android_notify_return_to_launcher();

#include <cerrno>
#include <cstdio>
#include <cstdlib>  // setenv
#include <fstream>
#include <unistd.h>  // chdir

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

// recomp_printf and most patch-side printf calls write to stdout. On a vanilla
// Android NDK app stdout/stderr are wired to /dev/null, so the entire patch
// diagnostic surface is invisible. Pipe both descriptors into a logcat-pumping
// thread so every recomp_printf / fprintf reaches `adb logcat -s BK64-Recomp`.
static void start_stdio_to_logcat_pump() {
    static std::atomic<bool> s_started{false};
    bool expected = false;
    if (!s_started.compare_exchange_strong(expected, true)) return;

    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    int fds[2];
    if (pipe(fds) != 0) {
        LOGW("stdio pump: pipe() failed errno=%d — recomp_printf will go to /dev/null", errno);
        return;
    }
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);

    std::thread([read_fd = fds[0]]() {
        char buf[512];
        std::string line;
        while (true) {
            ssize_t n = read(read_fd, buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = '\0';
            line.append(buf, n);
            size_t pos;
            while ((pos = line.find('\n')) != std::string::npos) {
                std::string out = line.substr(0, pos);
                __android_log_print(ANDROID_LOG_INFO, "BK64-Recomp", "%s", out.c_str());
                line.erase(0, pos + 1);
            }
        }
    }).detach();
}

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

// Copy the contents of one APK asset directory to `dest_dir`. AAssetDir
// only lists regular files — subdirectories must be opened by name, so the
// caller passes the subdir list explicitly. Skips files that already exist
// at the destination with the same byte size (APK assets are immutable per
// install, so a size match means a previous boot already extracted them).
void extract_asset_dir(AAssetManager* am,
                       const std::string& asset_subdir,
                       const std::filesystem::path& dest_dir) {
    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) {
        LOGE("create_directories(%s) failed: %s", dest_dir.c_str(), ec.message().c_str());
        return;
    }

    AAssetDir* dir = AAssetManager_openDir(am, asset_subdir.c_str());
    if (dir == nullptr) {
        LOGW("openDir failed for asset subdir '%s'", asset_subdir.c_str());
        return;
    }

    int extracted = 0;
    int skipped = 0;
    for (const char* name; (name = AAssetDir_getNextFileName(dir)) != nullptr; ) {
        std::string asset_path = asset_subdir.empty()
            ? std::string{name}
            : asset_subdir + "/" + name;
        std::filesystem::path out_path = dest_dir / name;

        AAsset* asset = AAssetManager_open(am, asset_path.c_str(), AASSET_MODE_BUFFER);
        if (asset == nullptr) {
            LOGW("AAssetManager_open failed for '%s'", asset_path.c_str());
            continue;
        }

        off_t asset_size = AAsset_getLength(asset);
        if (std::filesystem::exists(out_path, ec)) {
            uintmax_t existing = std::filesystem::file_size(out_path, ec);
            if (!ec && existing == static_cast<uintmax_t>(asset_size)) {
                AAsset_close(asset);
                ++skipped;
                continue;
            }
        }

        const void* buf = AAsset_getBuffer(asset);
        if (buf != nullptr && asset_size > 0) {
            std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(buf), asset_size);
            ++extracted;
            LOGI("extract: %s -> %s (%lld bytes)",
                 asset_path.c_str(), out_path.c_str(),
                 static_cast<long long>(asset_size));
        }
        AAsset_close(asset);
    }

    LOGI("extract_asset_dir(%s): extracted=%d skipped=%d",
         asset_subdir.empty() ? "<root>" : asset_subdir.c_str(),
         extracted, skipped);

    AAssetDir_close(dir);
}

void extract_apk_assets(AAssetManager* am, const std::filesystem::path& dest_root) {
    if (am == nullptr) {
        LOGE("extract_apk_assets: AAssetManager is null — APK assets unreachable");
        return;
    }
    // gradle's sourceSets points the APK assets root directly at the repo's
    // assets/ directory, so files live at the APK's asset root (e.g.
    // `Suplexmentary Comic NC.ttf`, not `assets/Suplexmentary…`). Mirror the
    // subdir layout into internal storage with `assets/` prepended on disk
    // so recompui's get_asset_path (which prepends `assets/`) finds them.
    // AAssetManager has no recursive listing API; update this list when
    // new subdirs land under assets/.
    extract_asset_dir(am, "",            dest_root / "assets");
    extract_asset_dir(am, "icons",       dest_root / "assets" / "icons");
    extract_asset_dir(am, "promptfont",  dest_root / "assets" / "promptfont");
    LOGI("extract_apk_assets: extracted under %s/assets", dest_root.c_str());
}

ultramodern::renderer::WindowHandle android_create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    LOGI("android_create_window -> %p", g_window);
    return g_window;
}

// Phase 10 used to auto-start the first supported game on the first gfx
// update. With recompui now driving the launcher on Android, the launcher's
// "start game" button is the trigger instead — leave update_gfx as a no-op
// so we never bypass the user's ROM selection.
void android_update_gfx(void*) {
    // intentionally empty: recompui's launcher menu calls recomp::start_game
    // via the same path desktop uses.
}

void android_message_box(const char* msg) {
    LOGE("[message_box] %s", msg);
}

// Android-side launcher init. Mirrors the bits of desktop's on_launcher_init
// (src/main/main.cpp:2025) that are not bknet-specific:
//   - Initialize the game_options_menu with the BK GameEntry.
//   - Add the default options (Start Game / Controls / Settings / Mods / Exit).
//   - Override Start Game's callback so it pings MainActivity to show the
//     touch overlay BEFORE recomp::start_game returns control to the game thread.
//   - Run banjo::launcher_animation_setup so the sky-blue background + animated
//     Banjo/Kazooie/Jiggy/Cloud SVGs are visible (it's the only thing that
//     touches background_container + the wrapper for the SVG art).
void android_on_launcher_init(recompui::LauncherMenu* menu) {
    if (supported_games.empty()) {
        LOGE("on_launcher_init: supported_games is empty — abort");
        return;
    }
    const auto& g = supported_games[0];

    auto* options = menu->init_game_options_menu(
        g.game_id, g.mod_game_id, g.display_name, g.thumbnail_bytes,
        recompui::GameOptionsMenuLayout::Center);
    recompui::update_game_mod_id(g.mod_game_id);
    options->add_default_options();

    // Replace the Start Game callback. We mirror the body of the default in
    // ui_launcher.cpp:457 (the no-rom branch is unreachable here — Android
    // currently has no ROM picker) but inject the notify before the call so
    // MainActivity mounts START/A in time for the first frame.
    if (auto* start_opt = options->get_start_game_option()) {
        start_opt->set_callback([game_id = g.game_id,
                                 mod_game_id = g.mod_game_id,
                                 display_name = g.display_name,
                                 thumbnail_bytes = g.thumbnail_bytes]() {
            recompui::update_game_mod_id(mod_game_id);
            if (recomp::mods::game_mode_count(mod_game_id, /*include_disabled=*/false) > 0) {
                recompui::get_launcher_menu()->show_game_mode_menu(
                    game_id, display_name, thumbnail_bytes);
            } else {
                LOGI("Start Game pressed -> notify Java + recomp::start_game");
                banjo_android_notify_game_started();
                recomp::start_game(game_id, {});
                recompui::hide_all_contexts();
                banjo_android_mark_expecting_first_game_frame();
            }
        });
    } else {
        LOGW("on_launcher_init: no start_game_option to override");
    }

    banjo::launcher_animation_setup(menu);
}

}  // namespace

namespace banjo_android {

void run_game(ANativeWindow* window, AppPaths paths) {
    start_stdio_to_logcat_pump();
    LOGI("run_game: entered with window=%p internal=%s", window, paths.internal_data_path);
    g_window = window;

    // Extract the APK assets/ tree into internal storage on first boot so
    // that RmlUi (FreeType + SVG plugin) can fopen() font + icon paths the
    // recompui code constructs via recompui::file::get_asset_path. Done
    // before any UIState construction; idempotent across reboots.
    std::filesystem::path internal_root = paths.internal_data_path
        ? std::filesystem::path{paths.internal_data_path}
        : std::filesystem::path{};
    if (!internal_root.empty()) {
        extract_apk_assets(paths.asset_manager, internal_root);
        recompui::file::set_program_path_override(internal_root);

        // RT64's UserPaths::detectDataPath uses the __linux__ branch on
        // Android (we're a Linux-flavored toolchain) and builds
        // `$HOME/.rt64`. The default $HOME on Android is `/data` which the
        // app sandbox cannot write to, so the constructor aborts with
        // `Permission denied ["/data/.rt64"]`. Pointing HOME at internal
        // storage moves the dir under our writable sandbox without forking
        // RT64.
        setenv("HOME", paths.internal_data_path, /*overwrite=*/1);

        // RmlUi's default FileInterface uses fopen() and only joins paths
        // against a document source URL — when the SVG element is created
        // programmatically (as launcher_animation does for Banjo.svg /
        // Kazooie.svg / Cloud*.svg etc.) the resolved path is just the bare
        // filename. On Windows the binary runs from a directory that has the
        // assets next to it; on Android cwd defaults to "/" so fopen fails
        // silently and the launcher renders without any background art.
        // chdir to the extracted assets dir so relative loads resolve.
        std::filesystem::path assets_dir = internal_root / "assets";
        if (chdir(assets_dir.c_str()) == 0) {
            LOGI("chdir(%s) ok — relative SVG/RCSS loads will resolve", assets_dir.c_str());
        } else {
            LOGE("chdir(%s) FAILED errno=%d — launcher SVGs will not load",
                 assets_dir.c_str(), errno);
        }

        // Smoke test: probe a couple of launcher SVGs by fopen + RmlUi's
        // get_asset_path. Helps tell apart "asset never extracted" from
        // "extracted but path resolution wrong" in logcat.
        for (const char* probe : { "Banjo.svg", "Kazooie.svg", "Logo.svg",
                                   "Suplexmentary Comic NC.ttf" }) {
            FILE* f = std::fopen(probe, "rb");
            if (f != nullptr) {
                std::fseek(f, 0, SEEK_END);
                long sz = std::ftell(f);
                std::fclose(f);
                LOGI("asset probe (cwd): '%s' -> ok (%ld bytes)", probe, sz);
            } else {
                LOGW("asset probe (cwd): '%s' -> MISSING (errno=%d)", probe, errno);
            }
            auto abs_probe = recompui::file::get_asset_path(probe);
            LOGI("asset probe (get_asset_path): '%s' -> '%s' exists=%d",
                 probe, abs_probe.c_str(),
                 std::filesystem::exists(abs_probe) ? 1 : 0);
        }
    } else {
        LOGE("internal_data_path is empty — recompui assets will not be found");
    }

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

    // Same validation as recomp::start() — notify Java before the launcher so
    // we can show a ROM hint screen without blocking native boot.
    recomp::check_all_stored_roms();
    if (!supported_games.empty()) {
        std::u8string gid = supported_games[0].game_id;
        banjo_android_notify_rom_gate(recomp::is_rom_valid(gid));
    } else {
        banjo_android_notify_rom_gate(false);
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

    // Program identity — recompui's LauncherMenu reads it for the window
    // title bar / about screen, and recompui::file::get_app_folder_path
    // appends program_id when deriving the per-user config dir.
    recompui::programconfig::set_program_name(banjo::program_name);
    recompui::programconfig::set_program_id(banjo::program_id);

    // Register the recompui exports the patches expect (mod menu helpers,
    // texture pack hooks, etc.). Mirrors main.cpp's call site.
    recompui::register_ui_exports();

    // Launcher fonts. Names must match files under <assets>/ — the APK
    // extractor above populates internal_root/assets/ with the same layout
    // as the desktop build's assets/ directory.
    recompui::register_primary_font("Suplexmentary Comic NC.ttf", "Suplexmentary Comic NC");
    recompui::register_extra_font("InterVariable.ttf");

    banjo::register_bk_overlays();
    banjo::register_bk_patches();

    // Locale + config: build the option tabs the launcher reads. Locale must
    // come first because init_config's add_*_options() use tr() with the
    // current language when registering option titles.
    banjo::locale::init();
    banjo::init_config();

    // Wire the launcher: without this Android falls back to recompui's
    // default callback (which only adds the default options) and the
    // launcher renders against a black background. Our callback also paints
    // the sky-blue bg + animated SVGs via banjo::launcher_animation_setup,
    // and overrides Start Game to mount the touch overlay.
    recompui::register_launcher_init_callback(android_on_launcher_init);
    recompui::register_launcher_update_callback([](recompui::LauncherMenu* menu) {
        banjo::launcher_animation_update(menu);
    });

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
            // recompui's render context installs the UI's init/draw/deinit
            // hooks into RT64 so the launcher composites on top of the N64
            // framebuffer (or fills the whole window when no game is running
            // yet). Same call site as desktop main.cpp.
            return recompui::renderer::create_render_context(
                rdram, window_handle,
                ultramodern::renderer::PresentationMode::PresentEarly,
                developer_mode);
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
        // recompui::message_box wraps SDL_ShowSimpleMessageBox; on Android
        // our shim logs to logcat. Use that path so launcher and game share
        // the same error reporting surface.
        .message_box = recompui::message_box,
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

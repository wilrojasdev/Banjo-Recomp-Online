#include "banjo_config.h"
#include "../locale/locale.h"
#include "banjo_launcher.h"
#include "recompui/recompui.h"
#include "recompui/config.h"
#include "recompinput/recompinput.h"
#include "banjo_sound.h"
#include "banjo_support.h"
#include "ultramodern/config.hpp"
#include "librecomp/files.hpp"
#include "librecomp/config.hpp"
#include "util/file.h"
#include <filesystem>
#include <fstream>
#include <iomanip>

#if defined(_WIN32)
#include <Shlobj.h>
#elif defined(__linux__)
#include <unistd.h>
#include <pwd.h>
#elif defined(__APPLE__)
#include "apple/rt64_apple.h"
#endif

static void add_general_options(recomp::config::Config &config) {
    using EnumOptionVector = std::vector<recomp::config::ConfigOptionEnumOption>;

    EnumOptionVector language_options = {
        {banjo::Language::English,    "English",    "English"},
        {banjo::Language::Spanish,    "Spanish",    "Espanol"},
        {banjo::Language::French,     "French",     "Francais"},
        {banjo::Language::German,     "German",     "Deutsch"},
        {banjo::Language::Portuguese, "Portuguese", "Portugues (BR)"},
    };
    config.add_enum_option(
        banjo::configkeys::general::language,
        banjo::locale::tr("settings.language.title"),
        banjo::locale::tr("settings.language.desc"),
        language_options,
        banjo::Language::English
    );
    // Show the restart prompt the moment the user picks a different language.
    // The General tab uses requires_confirmation=false (see create_general_tab in
    // ui_config_tab_general.cpp), so radio clicks fire Permanent — NOT Temporary.
    // We accept either, just skipping Load (initial config read at startup).
    //
    // Two-button choice prompt: confirm (Restart Now) commits both files and
    // relaunches; cancel (button OR X / ESC / click-outside) reverts the picker
    // selection back to the previous language so the UI stays consistent and the
    // user's saved config doesn't drift from language.txt.
    //
    // Re-entrancy guard `s_in_revert` prevents the revert's own set_option_value
    // call from re-firing this callback and showing a second prompt.
    {
        static bool s_lang_prompt_shown = false;
        static bool s_in_revert = false;
        config.add_option_change_callback(
            banjo::configkeys::general::language,
            [](recomp::config::ConfigValueVariant cur, recomp::config::ConfigValueVariant prev,
               recomp::config::OptionChangeContext ctx) {
                if (ctx == recomp::config::OptionChangeContext::Load) return;
                if (s_in_revert) return;
                if (s_lang_prompt_shown) return;
                s_lang_prompt_shown = true;

                auto new_lang = static_cast<banjo::Language>(std::get<uint32_t>(cur));
                uint32_t prev_raw = std::get<uint32_t>(prev);

                recompui::open_choice_prompt(
                    banjo::locale::tr("settings.language_restart_title"),
                    banjo::locale::tr("settings.language_restart_body"),
                    banjo::locale::tr("settings.language_restart_btn"),  // confirm
                    banjo::locale::tr("common.cancel"),                  // cancel
                    [new_lang]() {
                        // Confirm: persist BOTH files and relaunch. The general.json
                        // save is what makes the picker remember the new selection on
                        // next launch; without it the picker would show the old value
                        // even though language.txt (and so the in-game text) advanced.
                        banjo::locale::commit_language_to_disk(new_lang);
                        recompui::config::get_general_config().save_config();
                        banjo::request_return_to_launcher();
                    },
                    [prev_raw]() {
                        // Cancel (button, X, ESC, click-outside): roll the picker
                        // back to its previous value so the on-screen state matches
                        // the language that's actually still in effect.
                        s_in_revert = true;
                        recompui::config::get_general_config().set_option_value(
                            banjo::configkeys::general::language,
                            recomp::config::ConfigValueVariant{ prev_raw }
                        );
                        s_in_revert = false;
                        s_lang_prompt_shown = false;
                    },
                    recompui::ButtonStyle::Primary,    // confirm style
                    recompui::ButtonStyle::Secondary,  // cancel style (neutral)
                    true                               // focus on cancel — safer default
                );
            }
        );
    }

    EnumOptionVector note_saving_mode_options = {
        {banjo::NoteSavingMode::Off, "Off", banjo::locale::tr("opt.off")},
        {banjo::NoteSavingMode::On,  "On",  banjo::locale::tr("opt.on")},
    };
    config.add_enum_option(
        banjo::configkeys::general::note_saving_mode,
        banjo::locale::tr("settings.note_saving.title"),
        banjo::locale::tr("settings.note_saving.desc"),
        note_saving_mode_options,
        banjo::NoteSavingMode::On
    );
    EnumOptionVector analog_cam_mode_options = {
        {banjo::AnalogCamMode::Off, "Off", banjo::locale::tr("opt.off")},
        {banjo::AnalogCamMode::On,  "On",  banjo::locale::tr("opt.on")},
    };
    config.add_enum_option(
        banjo::configkeys::general::analog_cam_mode,
        banjo::locale::tr("settings.analog_cam.title"),
        banjo::locale::tr("settings.analog_cam.desc"),
        analog_cam_mode_options,
        banjo::AnalogCamMode::Off
    );
    config.add_number_option(
        banjo::configkeys::general::analog_camera_sensitivity,
        banjo::locale::tr("settings.analog_cam_sens.title"),
        banjo::locale::tr("settings.analog_cam_sens.desc"),
        1, 10, 1, 0, false, 3
    );
    config.add_option_hidden_dependency(
        banjo::configkeys::general::analog_camera_sensitivity,
        banjo::configkeys::general::analog_cam_mode,
        banjo::AnalogCamMode::Off
    );
    EnumOptionVector camera_invert_mode_options = {
        {banjo::CameraInvertMode::InvertNone, "InvertNone", banjo::locale::tr("opt.invert.none")},
        {banjo::CameraInvertMode::InvertX,    "InvertX",    banjo::locale::tr("opt.invert.x")},
        {banjo::CameraInvertMode::InvertY,    "InvertY",    banjo::locale::tr("opt.invert.y")},
        {banjo::CameraInvertMode::InvertBoth, "InvertBoth", banjo::locale::tr("opt.invert.both")}
    };
    config.add_enum_option(
        banjo::configkeys::general::third_person_camera_invert_mode,
        banjo::locale::tr("settings.invert_cam.title"),
        banjo::locale::tr("settings.invert_cam.desc"),
        camera_invert_mode_options,
        banjo::CameraInvertMode::InvertX
    );
    EnumOptionVector first_person_invert_mode_options = {
        {banjo::CameraInvertMode::InvertNone, "InvertNone", banjo::locale::tr("opt.invert.none")},
        {banjo::CameraInvertMode::InvertX,    "InvertX",    banjo::locale::tr("opt.invert.x")},
        {banjo::CameraInvertMode::InvertY,    "InvertY",    banjo::locale::tr("opt.invert.y")},
        {banjo::CameraInvertMode::InvertBoth, "InvertBoth", banjo::locale::tr("opt.invert.both")}
    };
    config.add_enum_option(
        banjo::configkeys::general::first_person_invert_mode,
        banjo::locale::tr("settings.invert_fp.title"),
        banjo::locale::tr("settings.invert_fp.desc"),
        first_person_invert_mode_options,
        banjo::CameraInvertMode::InvertY
    );
    EnumOptionVector flying_and_swimming_invert_options = {
        {banjo::CameraInvertMode::InvertNone, "InvertNone", banjo::locale::tr("opt.invert.none")},
        {banjo::CameraInvertMode::InvertX,    "InvertX",    banjo::locale::tr("opt.invert.x")},
        {banjo::CameraInvertMode::InvertY,    "InvertY",    banjo::locale::tr("opt.invert.y")},
        {banjo::CameraInvertMode::InvertBoth, "InvertBoth", banjo::locale::tr("opt.invert.both")}
    };
    config.add_enum_option(
        banjo::configkeys::general::flying_and_swimming_invert_mode,
        banjo::locale::tr("settings.invert_fs.title"),
        banjo::locale::tr("settings.invert_fs.desc"),
        flying_and_swimming_invert_options,
        banjo::CameraInvertMode::InvertY
    );
}

template <typename T = uint32_t>
T get_general_config_enum_value(const std::string& option_id) {
    return static_cast<T>(std::get<uint32_t>(recompui::config::get_general_config().get_option_value(option_id)));
}

template <typename T = uint32_t>
T get_general_config_number_value(const std::string& option_id) {
    return static_cast<T>(std::get<double>(recompui::config::get_general_config().get_option_value(option_id)));
}

banjo::NoteSavingMode banjo::get_note_saving_mode() {
    return get_general_config_enum_value<banjo::NoteSavingMode>(banjo::configkeys::general::note_saving_mode);
}

banjo::CameraInvertMode banjo::get_camera_invert_mode() {
    return get_general_config_enum_value<banjo::CameraInvertMode>(banjo::configkeys::general::camera_invert_mode);
}

banjo::CameraInvertMode banjo::get_third_person_camera_mode() {
    return get_general_config_enum_value<banjo::CameraInvertMode>(banjo::configkeys::general::third_person_camera_invert_mode);
}

banjo::CameraInvertMode banjo::get_flying_and_swimming_invert_mode() {
    return get_general_config_enum_value<banjo::CameraInvertMode>(banjo::configkeys::general::flying_and_swimming_invert_mode);
}

banjo::CameraInvertMode banjo::get_first_person_invert_mode() {
    return get_general_config_enum_value<banjo::CameraInvertMode>(banjo::configkeys::general::first_person_invert_mode);
}

banjo::AnalogCamMode banjo::get_analog_cam_mode() {
    return get_general_config_enum_value<banjo::AnalogCamMode>(banjo::configkeys::general::analog_cam_mode);
}

uint32_t banjo::get_analog_cam_sensitivity() {
    return get_general_config_number_value(banjo::configkeys::general::analog_camera_sensitivity);
}

template <typename T = uint32_t>
T get_graphics_config_enum_value(const std::string& option_id) {
    return static_cast<T>(std::get<uint32_t>(recompui::config::get_graphics_config().get_option_value(option_id)));
}

static void add_sound_options(recomp::config::Config &config) {
    config.add_percent_number_option(
        banjo::configkeys::sound::bgm_volume,
        banjo::locale::tr("settings.bgm_volume.title"),
        banjo::locale::tr("settings.bgm_volume.desc"),
        100.0f
    );
}
template <typename T = uint32_t>
T get_sound_config_number_value(const std::string& option_id) {
    return static_cast<T>(std::get<double>(recompui::config::get_sound_config().get_option_value(option_id)));
}

int banjo::get_bgm_volume() {
    return get_sound_config_number_value<int>(banjo::configkeys::sound::bgm_volume);
}

static void add_graphics_options(recomp::config::Config &config) {
    using EnumOptionVector = std::vector<recomp::config::ConfigOptionEnumOption>;
    EnumOptionVector cutscene_aspect_ratio_mode_options = {
        {banjo::CutsceneAspectRatioMode::Original,  "Original",  banjo::locale::tr("opt.aspect.original")},
        {banjo::CutsceneAspectRatioMode::Clamp16x9, "Clamp16x9", banjo::locale::tr("opt.aspect.16x9")},
        {banjo::CutsceneAspectRatioMode::Full,      "Expand",    banjo::locale::tr("opt.aspect.expand")},
    };
    config.add_enum_option(
        banjo::configkeys::graphics::cutscene_aspect_ratio_mode,
        banjo::locale::tr("settings.cutscene_aspect.title"),
        banjo::locale::tr("settings.cutscene_aspect.desc"),
        cutscene_aspect_ratio_mode_options,
        banjo::CutsceneAspectRatioMode::Clamp16x9
    );
}

static void set_control_defaults() {
    using namespace recompinput;

    // Left shoulder -> C Down | Backwards eggs / zoom out
    set_default_mapping_for_controller(
        GameInput::C_DOWN,
        { 
            InputField::controller_analog(SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTY, true),
            InputField::controller_digital(SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
        }
    );

    // Right shoulder -> C Up | Forwards eggs / first person
    set_default_mapping_for_controller(
        GameInput::C_UP,
        { 
            InputField::controller_analog(SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTY, false),
            InputField::controller_digital(SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
        }
    );

    // North button -> C Left | Talon trot / camera left
    set_default_mapping_for_controller(
        GameInput::C_LEFT,
        { 
            InputField::controller_analog(SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTX, false),
            InputField::controller_digital(SDL_CONTROLLER_BUTTON_NORTH)
        }
    );

    // East button -> C Right | Wonderwing / camera right
    set_default_mapping_for_controller(
        GameInput::C_RIGHT,
        { 
            InputField::controller_analog(SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTX, true),
            InputField::controller_digital(SDL_CONTROLLER_BUTTON_EAST)
        }
    );

    // R3 -> L | Unused in BK but can be used in mods
    set_default_mapping_for_controller(GameInput::L, { InputField::controller_digital(SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_RIGHTSTICK) });
}

static void set_control_names_and_descriptions() {
    using namespace recompinput;
    auto T = [](const char* k) { return banjo::locale::tr(k); };

    // --- Names (left column in the Controls tab) ---
    set_game_input_name(GameInput::Y_AXIS_POS, T("recompui.input.up"));
    set_game_input_name(GameInput::Y_AXIS_NEG, T("recompui.input.down"));
    set_game_input_name(GameInput::X_AXIS_NEG, T("recompui.input.left"));
    set_game_input_name(GameInput::X_AXIS_POS, T("recompui.input.right"));
    set_game_input_name(GameInput::A,           T("recompui.input.a"));
    set_game_input_name(GameInput::B,           T("recompui.input.b"));
    set_game_input_name(GameInput::Z,           T("recompui.input.z"));
    set_game_input_name(GameInput::L,           T("recompui.input.l"));
    set_game_input_name(GameInput::R,           T("recompui.input.r"));
    set_game_input_name(GameInput::START,       T("recompui.input.start"));
    set_game_input_name(GameInput::C_UP,        T("recompui.input.c_up"));
    set_game_input_name(GameInput::C_DOWN,      T("recompui.input.c_down"));
    set_game_input_name(GameInput::C_LEFT,      T("recompui.input.c_left"));
    set_game_input_name(GameInput::C_RIGHT,     T("recompui.input.c_right"));
    set_game_input_name(GameInput::DPAD_UP,     T("recompui.input.dpad_up"));
    set_game_input_name(GameInput::DPAD_DOWN,   T("recompui.input.dpad_down"));
    set_game_input_name(GameInput::DPAD_LEFT,   T("recompui.input.dpad_left"));
    set_game_input_name(GameInput::DPAD_RIGHT,  T("recompui.input.dpad_right"));

    // --- Descriptions (right pane when an input is selected) ---
    set_game_input_description(GameInput::Y_AXIS_POS, T("banjo.input_desc.move"));
    set_game_input_description(GameInput::Y_AXIS_NEG, T("banjo.input_desc.move"));
    set_game_input_description(GameInput::X_AXIS_NEG, T("banjo.input_desc.move"));
    set_game_input_description(GameInput::X_AXIS_POS, T("banjo.input_desc.move"));
    set_game_input_description(GameInput::A,          T("banjo.input_desc.a"));
    set_game_input_description(GameInput::B,          T("banjo.input_desc.b"));
    set_game_input_description(GameInput::Z,          T("banjo.input_desc.z"));
    set_game_input_description(GameInput::L,          T("banjo.input_desc.unused"));
    set_game_input_description(GameInput::R,          T("banjo.input_desc.r"));
    set_game_input_description(GameInput::START,      T("banjo.input_desc.start"));
    set_game_input_description(GameInput::C_UP,       T("banjo.input_desc.c_up"));
    set_game_input_description(GameInput::C_DOWN,     T("banjo.input_desc.c_down"));
    set_game_input_description(GameInput::C_LEFT,     T("banjo.input_desc.c_left"));
    set_game_input_description(GameInput::C_RIGHT,    T("banjo.input_desc.c_right"));
    set_game_input_description(GameInput::DPAD_UP,    T("banjo.input_desc.unused"));
    set_game_input_description(GameInput::DPAD_DOWN,  T("banjo.input_desc.unused"));
    set_game_input_description(GameInput::DPAD_LEFT,  T("banjo.input_desc.unused"));
    set_game_input_description(GameInput::DPAD_RIGHT, T("banjo.input_desc.unused"));
}

banjo::CutsceneAspectRatioMode banjo::get_cutscene_aspect_ratio_mode() {
    return get_graphics_config_enum_value<banjo::CutsceneAspectRatioMode>(banjo::configkeys::graphics::cutscene_aspect_ratio_mode);
}

banjo::NetworkMode banjo::get_network_mode() {
    return get_general_config_enum_value<banjo::NetworkMode>(banjo::configkeys::network::mode);
}

banjo::Language banjo::get_language() {
    return get_general_config_enum_value<banjo::Language>(banjo::configkeys::general::language);
}

uint32_t banjo::get_network_port() {
    return get_general_config_number_value<uint32_t>(banjo::configkeys::network::port);
}

static void add_network_options(recomp::config::Config &config) {
    using EnumOptionVector = const std::vector<recomp::config::ConfigOptionEnumOption>;

    static EnumOptionVector network_mode_options = {
        {banjo::NetworkMode::Off, "Off", "Off"},
        {banjo::NetworkMode::Host, "Host", "Host Game"},
        {banjo::NetworkMode::Join, "Join", "Join Game"},
    };
    config.add_enum_option(
        banjo::configkeys::network::mode,
        "Network Mode",
        "Enable online multiplayer. <recomp-color primary>Host</recomp-color> creates a game, <recomp-color primary>Join</recomp-color> connects to a host.",
        network_mode_options,
        banjo::NetworkMode::Off
    );
    config.add_number_option(
        banjo::configkeys::network::port,
        "Port",
        "Port number for hosting or joining. Default is 7777.",
        1024, 65535, 1, 0, false, 7777
    );
}

void banjo::init_config() {
    std::filesystem::path recomp_dir = recompui::file::get_app_folder_path();

    if (!recomp_dir.empty()) {
        std::filesystem::create_directories(recomp_dir);
    }

    recompui::config::GeneralTabOptions general_options{};
    general_options.has_rumble_strength = true;
    general_options.has_gyro_sensitivity = false;
    general_options.has_mouse_sensitivity = false;

    auto &general_config = recompui::config::create_general_tab(general_options, banjo::locale::tr("recompui.tab.general"));
    add_general_options(general_config);
    // Network options removed from Settings — handled by launcher Host/Join buttons

    auto &graphics_config = recompui::config::create_graphics_tab(banjo::locale::tr("recompui.tab.graphics"));
    add_graphics_options(graphics_config);

    set_control_defaults();
    set_control_names_and_descriptions();
    recompui::config::create_controls_tab(banjo::locale::tr("recompui.tab.controls"));

    auto &sound_config = recompui::config::create_sound_tab(banjo::locale::tr("recompui.tab.sound"));
    add_sound_options(sound_config);

    recompui::config::create_mods_tab(banjo::locale::tr("recompui.tab.mods"));

    recompui::config::finalize();

}

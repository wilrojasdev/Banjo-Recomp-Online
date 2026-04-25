#include "locale.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#include "util/file.h"

namespace {
    using banjo::Language;

    Language g_startup = Language::English;
    Language g_current = Language::English;

    // Standalone language store, read BEFORE the recompui::config system is initialized
    // so that add_general_options / add_*_options can call tr() with the correct language.
    // The recompui::config copy is the runtime authoritative one while the app runs;
    // on change, we mirror it here so the next launch picks up the choice early enough.
    std::filesystem::path language_file_path() {
        auto dir = recompui::file::get_app_folder_path();
        if (dir.empty()) return {};
        return dir / "language.txt";
    }

    Language language_from_string(const std::string& s) {
        if (s == "Spanish")    return Language::Spanish;
        if (s == "French")     return Language::French;
        if (s == "German")     return Language::German;
        if (s == "Portuguese") return Language::Portuguese;
        return Language::English;
    }

    const char* language_to_string(Language lang) {
        switch (lang) {
            case Language::Spanish:    return "Spanish";
            case Language::French:     return "French";
            case Language::German:     return "German";
            case Language::Portuguese: return "Portuguese";
            case Language::English:
            default:                   return "English";
        }
    }

    Language read_language_file() {
        auto path = language_file_path();
        if (path.empty() || !std::filesystem::exists(path)) return Language::English;
        std::ifstream f(path);
        if (!f.is_open()) return Language::English;
        std::string line;
        std::getline(f, line);
        // Trim whitespace / line endings.
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        return language_from_string(line);
    }

    void write_language_file(Language lang) {
        auto path = language_file_path();
        if (path.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream f(path, std::ios::trunc);
        if (!f.is_open()) return;
        f << language_to_string(lang) << "\n";
    }

    using Dict = std::unordered_map<std::string, std::string>;

    // English acts as the base / fallback dictionary. All keys MUST exist here.
    const Dict& english_dict() {
        static const Dict d = {
            {"menu.host",               "Host"},
            {"menu.join",               "Join"},
            {"menu.settings",           "Settings"},
            {"menu.exit",               "Exit"},
            {"menu.load_rom",           "Load ROM"},

            {"common.back",             "Back"},
            {"common.cancel",           "Cancel"},
            {"common.ok",               "OK"},
            {"common.start",            "Start"},
            {"common.retry",            "Retry"},
            {"common.erase",            "Erase"},
            {"common.copy",             "Copy"},
            {"common.refresh",          "Refresh"},
            {"common.search",           "Search"},

            {"host.title",              "Host Game"},
            {"host.player_name",        "Player Name"},
            {"host.save_slot",          "Save Slot"},
            {"host.connection_mode",    "Connection Mode"},
            {"host.direct_connection",  "Direct Connection"},
            {"host.coopnet",            "CoopNet"},
            {"host.password",           "Password"},
            {"host.password_optional",  "Password (optional)"},
            {"host.port",               "Port"},
            {"host.your_ip_address",    "Your IP Address"},
            {"host.network_system",     "Network System"},
            {"host.start_hosting",      "Start Hosting"},
            {"host.erase_slot_title",   "Erase Save"},
            {"host.erase_slot_body",    "All progress in this slot will be permanently deleted."},

            {"join.title",              "Join Game"},
            {"join.private_lobbies",    "Private Lobbies"},
            {"join.direct_connection",  "Direct Connection"},
            {"join.password_prompt",    "Enter the private lobby's password:"},
            {"join.direct_prompt",      "Enter direct connection IP and port:"},
            {"join.ip_address",         "IP Address"},
            {"join.port",               "Port"},
            {"join.connecting",         "Connecting..."},
            {"join.connected",          "Connected"},
            {"join.failed",             "Connection failed"},
            {"join.no_lobbies",         "No lobbies found."},
            {"join.join_btn",           "Join"},
            {"join.searching",          "Searching..."},
            {"join.host_ip_address",    "Host IP Address"},
            {"join.search",             "Search"},
            {"join.connecting_server",  "Connecting to server..."},
            {"join.connecting_to",      "Connecting to "},
            {"join.joining_lobby",      "Joining lobby..."},
            {"join.host_not_found",     "Host not found or not responding."},

            {"coopnet.online",          "CoopNet: Online"},
            {"coopnet.offline",         "CoopNet: Offline"},
            {"coopnet.checking",        "CoopNet: Checking..."},

            {"quit.title_online",       "Leave Session?"},
            {"quit.body_online",        "You will be disconnected from the current session."},
            {"quit.leave",              "Disconnect"},
            {"quit.stay",               "Stay"},

            {"settings.language_restart_title", "Restart Required"},
            {"settings.language_restart_body",  "The language change requires restarting the game. The app will restart now."},
            {"settings.language_restart_btn",   "Restart Now"},

            {"settings.language.title",         "Language"},
            {"settings.language.desc",          "Selects the language for the launcher, settings UI and in-game text. <recomp-color primary>The matching translation pack is enabled automatically.</recomp-color><br /><br />Changing this will restart the game."},
            {"settings.note_saving.title",      "Note Saving"},
            {"settings.note_saving.desc",       "Saves collected notes so that you don't need to collect them again when revisiting a level. <recomp-color primary>On</recomp-color> is the default, while <recomp-color primary>off</recomp-color> matches the original game."},
            {"settings.analog_cam.title",       "Analog Camera"},
            {"settings.analog_cam.desc",        "Enables the analog camera."},
            {"settings.analog_cam_sens.title",  "Analog Camera Sensitivity"},
            {"settings.analog_cam_sens.desc",   "Sets the sensitivity of the right stick analog camera, if enabled."},
            {"settings.invert_cam.title",       "Invert Camera"},
            {"settings.invert_cam.desc",        "Inverts the camera controls for the third person camera if it's enabled. <recomp-color primary>Invert X</recomp-color> is the default and matches the original game.<br /><br />If analog camera is off, only the <recomp-color primary>Invert X</recomp-color> setting will take effect."},
            {"settings.invert_fp.title",        "Invert First Person View"},
            {"settings.invert_fp.desc",         "Inverts the camera controls in first person view. <recomp-color primary>Invert Y</recomp-color> is the default and matches the original game."},
            {"settings.invert_fs.title",        "Invert Flying & Swimming"},
            {"settings.invert_fs.desc",         "Inverts the controls for swimming and flying. <recomp-color primary>Invert Y</recomp-color> is the default and matches the original game."},
            {"settings.bgm_volume.title",       "Background Music Volume"},
            {"settings.bgm_volume.desc",        "Controls the overall volume of background music."},
            {"settings.cutscene_aspect.title",  "Cutscene Aspect Ratio"},
            {"settings.cutscene_aspect.desc",   "Sets the aspect ratio limit for cutscenes. Cutscenes have been adjusted to work in <recomp-color primary>16:9</recomp-color>, which is the default option. Wider aspect ratios may show details that weren't meant to be on-screen."},

            {"opt.on",                          "On"},
            {"opt.off",                         "Off"},
            {"opt.invert.none",                 "None"},
            {"opt.invert.x",                    "Invert X"},
            {"opt.invert.y",                    "Invert Y"},
            {"opt.invert.both",                 "Invert Both"},
            {"opt.aspect.original",             "Original"},
            {"opt.aspect.16x9",                 "16:9"},
            {"opt.aspect.expand",               "Expand"},

            {"recompui.tab.general",            "General"},
            {"recompui.tab.graphics",           "Graphics"},
            {"recompui.tab.controls",           "Controls"},
            {"recompui.tab.sound",              "Sound"},
            {"recompui.tab.mods",               "Mods"},

            {"recompui.btn.apply",              "Apply"},
            {"recompui.btn.reset",              "Reset"},
            {"recompui.btn.discard",            "Discard"},
            {"recompui.btn.continue",           "Continue"},
            {"recompui.btn.save",               "Save"},
            {"recompui.btn.cancel",             "Cancel"},
            {"recompui.btn.close",              "Close"},
            {"recompui.btn.yes",                "Yes"},
            {"recompui.btn.no",                 "No"},
            {"recompui.btn.ok",                 "OK"},

            {"recompui.prompt.unsaved.title",   "Unsaved Changes"},
            {"recompui.prompt.unsaved.body",    "You have unsaved changes. Would you like to apply or discard them?"},
            {"recompui.prompt.reset.title",     "Reset to Defaults?"},
            {"recompui.prompt.reset.body",      "All settings in this tab will be reset to their default values."},

            {"recompui.mods.install",           "Install Mod"},
            {"recompui.mods.uninstall",         "Uninstall"},
            {"recompui.mods.enable",            "Enable"},
            {"recompui.mods.disable",           "Disable"},
            {"recompui.mods.no_mods",           "No mods installed."},
            {"recompui.mods.error_install",     "Error Installing Mods"},
            {"recompui.mods.unknown_error",     "An unknown error has occurred."},
            {"recompui.mods.installing_title",  "Installing Mods"},
            {"recompui.mods.installing_body",   "Please Wait"},
            {"recompui.mods.overwrite_title",   "Overwrite Mods?"},
            {"recompui.mods.overwrite_btn",     "Overwrite"},
            {"recompui.mods.empty",             "You have no mods. Go get some!"},
            {"recompui.mods.install_mods",      "Install Mods"},
            {"recompui.mods.open_folder",       "Open Mods Folder"},

            {"recompui.quit.title_online",      "Leave Session?"},
            {"recompui.quit.title_offline",     "Are you sure you want to quit?"},
            {"recompui.quit.body_online",       "You will be disconnected from the game."},
            {"recompui.quit.body_offline",      "Any progress since your last save will be lost."},
            {"recompui.quit.btn_disconnect",    "Disconnect"},
            {"recompui.quit.btn_quit",          "Quit"},

            {"recompui.controls.go_back",       "Go back"},
            {"recompui.controls.assign_players","Assign players"},
            {"recompui.controls.reset_defaults","Reset to defaults"},

            {"recompui.tab.unapplied_suffix",   "options have unapplied changes."},

            {"recompui.general.rumble_strength.title", "Rumble Strength"},
            {"recompui.general.rumble_strength.desc",  "Controls the strength of rumble when using a controller that supports it. <b>Setting this to zero will disable rumble.</b>"},
            {"recompui.general.joystick_deadzone.title", "Joystick Deadzone"},
            {"recompui.general.joystick_deadzone.desc",  "Applies a deadzone to joystick inputs."},
            {"recompui.general.bg_input.title",          "Background Input Mode"},
            {"recompui.general.bg_input.desc",           "Allows the game to read controller input when out of focus.<br/><b>This setting does not affect keyboard input.</b>"},

            {"recompui.graphics.resolution.title",  "Resolution"},
            {"recompui.graphics.resolution.desc",   "Sets the output resolution of the game. <recomp-color primary>Original</recomp-color> matches the game's original 240p resolution. <recomp-color primary>Original 2x</recomp-color> will render at 480p. <recomp-color primary>Auto</recomp-color> will scale based on the game window's resolution."},
            {"recompui.graphics.downsampling.title", "Downsampling Quality"},
            {"recompui.graphics.downsampling.desc",  "Renders at a higher resolution and scales it down to the output resolution for increased quality. Only available in <recomp-color primary>Original</recomp-color> and <recomp-color primary>Original 2x</recomp-color> resolution.<br /><br />Note: <recomp-color primary>4x</recomp-color> downsampling quality at <recomp-color primary>Original 2x</recomp-color> resolution may cause performance issues on low end devices, as it will cause the game to render <recomp-color warning>at almost 4k internal resolution</recomp-color>."},
            {"recompui.graphics.aspect_ratio.title",  "Aspect Ratio"},
            {"recompui.graphics.aspect_ratio.desc",   "Sets the horizontal aspect ratio. <recomp-color primary>Original</recomp-color> uses the game's original 4:3 aspect ratio. <recomp-color primary>Expand</recomp-color> will adjust to match the game window's aspect ratio."},
            {"recompui.graphics.window_mode.title",   "Window Mode"},
            {"recompui.graphics.window_mode.desc",    "Sets whether the game should display <recomp-color primary>Windowed</recomp-color> or <recomp-color primary>Fullscreen</recomp-color>. You can also use <recomp-color primary>F11</recomp-color> or <recomp-color primary>Alt + Enter</recomp-color> to toggle this option."},
            {"recompui.graphics.framerate.title",     "Framerate"},
            {"recompui.graphics.msaa.title",          "MS Anti-Aliasing"},
            {"recompui.graphics.msaa.desc",           "Sets the multisample anti-aliasing (MSAA) quality level. This reduces jagged edges in the final image at the expense of rendering performance.<br /><br /><recomp-color primary>Note: This option won't be available if your GPU does not support programmable MSAA sample positions, as it is currently required to avoid rendering glitches.</recomp-color>"},
            {"recompui.graphics.hud_placement.title", "HUD Placement"},
            {"recompui.graphics.hud_placement.desc",  "Adjusts the placement of HUD elements to fit the selected aspect ratio. <recomp-color primary>Expand</recomp-color> will use the aspect ratio of the game's output window."},

            {"recompui.sound.main_volume.title", "Main Volume"},
            {"recompui.sound.main_volume.desc",  "Controls the main volume of the game."},

            {"recompui.opt.original",     "Original"},
            {"recompui.opt.original_2x",  "Original 2x"},
            {"recompui.opt.auto",         "Auto"},
            {"recompui.opt.x2",           "2x"},
            {"recompui.opt.x4",           "4x"},
            {"recompui.opt.windowed",     "Windowed"},
            {"recompui.opt.fullscreen",   "Fullscreen"},
            {"recompui.opt.display",      "Display"},
            {"recompui.opt.manual",       "Manual"},
            {"recompui.opt.expand",       "Expand"},

            {"recompui.input.up",         "Up"},
            {"recompui.input.down",       "Down"},
            {"recompui.input.left",       "Left"},
            {"recompui.input.right",      "Right"},
            {"recompui.input.a",          "A"},
            {"recompui.input.b",          "B"},
            {"recompui.input.z",          "Z"},
            {"recompui.input.l",          "L"},
            {"recompui.input.r",          "R"},
            {"recompui.input.start",      "Start"},
            {"recompui.input.c_up",       "C Up"},
            {"recompui.input.c_down",     "C Down"},
            {"recompui.input.c_left",     "C Left"},
            {"recompui.input.c_right",    "C Right"},
            {"recompui.input.dpad_up",    "D-Pad Up"},
            {"recompui.input.dpad_down",  "D-Pad Down"},
            {"recompui.input.dpad_left",  "D-Pad Left"},
            {"recompui.input.dpad_right", "D-Pad Right"},

            {"recompui.rom.browse",             "Browse for ROM"},
            {"recompui.rom.wrong_version",      "This ROM is the correct game, but the wrong version.\nThis project requires the NTSC-U N64 version of the game."},
            {"recompui.rom.wrong_game",         "This ROM is not the correct game."},
            {"recompui.rom.invalid",            "Invalid ROM file."},

            {"banjo.input_desc.move",   "Used to move and for steering while flying and swimming. Axis inversion for flying and swimming can be configured in the General tab."},
            {"banjo.input_desc.a",      "Used to jump and select options in menus. Also used for flying upwards."},
            {"banjo.input_desc.b",      "Used for attacks, which change depending on whether you are stationary, moving, in the air, or crouching."},
            {"banjo.input_desc.z",      "Used to crouch, which enables A, B and the C-Buttons to perform different actions."},
            {"banjo.input_desc.r",      "Used to center the camera behind Banjo on the ground, and to perform tighter turns while flying or swimming."},
            {"banjo.input_desc.start",  "Used for pausing and for skipping certain cutscenes."},
            {"banjo.input_desc.c_up",   "Used to enter first-person mode, and to shoot eggs while holding Z."},
            {"banjo.input_desc.c_down", "Used to toggle between the different camera zoom levels, and to shoot eggs backwards while holding Z."},
            {"banjo.input_desc.c_left", "Used to rotate the camera sideways. Axis inversion can be configured in the General tab. Also used to enter Talon Trot while holding Z."},
            {"banjo.input_desc.c_right","Used to rotate the camera sideways. Axis inversion can be configured in the General tab. Also used to enter Wonderwing while holding Z."},
            {"banjo.input_desc.unused", "Unused. Mods may use it for additional features."},
        };
        return d;
    }

    const Dict& spanish_dict() {
        static const Dict d = {
            {"menu.host",               "Crear"},
            {"menu.join",               "Unirse"},
            {"menu.settings",           "Ajustes"},
            {"menu.exit",               "Salir"},
            {"menu.load_rom",           "Cargar ROM"},

            {"common.back",             "Atras"},
            {"common.cancel",           "Cancelar"},
            {"common.ok",               "Aceptar"},
            {"common.start",            "Iniciar"},
            {"common.retry",            "Reintentar"},
            {"common.erase",            "Borrar"},
            {"common.copy",             "Copiar"},
            {"common.refresh",          "Actualizar"},
            {"common.search",           "Buscar"},

            {"host.title",              "Crear Partida"},
            {"host.player_name",        "Nombre del Jugador"},
            {"host.save_slot",          "Ranura de Guardado"},
            {"host.connection_mode",    "Modo de Conexion"},
            {"host.direct_connection",  "Conexion Directa"},
            {"host.coopnet",            "CoopNet"},
            {"host.password",           "Contrasena"},
            {"host.password_optional",  "Contrasena (opcional)"},
            {"host.port",               "Puerto"},
            {"host.your_ip_address",    "Tu Direccion IP"},
            {"host.network_system",     "Sistema de Red"},
            {"host.start_hosting",      "Iniciar Partida"},
            {"host.erase_slot_title",   "Borrar Partida"},
            {"host.erase_slot_body",    "Se eliminara permanentemente todo el progreso de esta ranura."},

            {"join.title",              "Unirse a Partida"},
            {"join.private_lobbies",    "Salas Privadas"},
            {"join.direct_connection",  "Conexion Directa"},
            {"join.password_prompt",    "Introduce la contrasena de la sala privada:"},
            {"join.direct_prompt",      "Introduce la IP y el puerto de conexion directa:"},
            {"join.ip_address",         "Direccion IP"},
            {"join.port",               "Puerto"},
            {"join.connecting",         "Conectando..."},
            {"join.connected",          "Conectado"},
            {"join.failed",             "Error de conexion"},
            {"join.no_lobbies",         "No se encontraron salas."},
            {"join.join_btn",           "Unirse"},
            {"join.searching",          "Buscando..."},
            {"join.host_ip_address",    "Direccion IP del Anfitrion"},
            {"join.search",             "Buscar"},
            {"join.connecting_server",  "Conectando al servidor..."},
            {"join.connecting_to",      "Conectando a "},
            {"join.joining_lobby",      "Uniendose a la sala..."},
            {"join.host_not_found",     "Anfitrion no encontrado o no responde."},

            {"coopnet.online",          "CoopNet: En linea"},
            {"coopnet.offline",         "CoopNet: Desconectado"},
            {"coopnet.checking",        "CoopNet: Comprobando..."},

            {"quit.title_online",       "Salir de la Sesion?"},
            {"quit.body_online",        "Se te desconectara de la sesion actual."},
            {"quit.leave",              "Desconectar"},
            {"quit.stay",               "Quedarse"},

            {"settings.language_restart_title", "Reinicio Requerido"},
            {"settings.language_restart_body",  "El cambio de idioma requiere reiniciar el juego. La aplicacion se reiniciara ahora."},
            {"settings.language_restart_btn",   "Reiniciar Ahora"},

            {"settings.language.title",         "Idioma"},
            {"settings.language.desc",          "Selecciona el idioma del launcher, los ajustes y el texto dentro del juego. <recomp-color primary>El paquete de traduccion correspondiente se activa automaticamente.</recomp-color><br /><br />Al cambiarlo el juego se reiniciara."},
            {"settings.note_saving.title",      "Guardar Notas"},
            {"settings.note_saving.desc",       "Guarda las notas recogidas para que no tengas que recolectarlas de nuevo al volver a un nivel. <recomp-color primary>Activado</recomp-color> es el valor por defecto; <recomp-color primary>desactivado</recomp-color> replica el juego original."},
            {"settings.analog_cam.title",       "Camara Analogica"},
            {"settings.analog_cam.desc",        "Activa la camara analogica."},
            {"settings.analog_cam_sens.title",  "Sensibilidad de Camara Analogica"},
            {"settings.analog_cam_sens.desc",   "Ajusta la sensibilidad del stick derecho si la camara analogica esta activa."},
            {"settings.invert_cam.title",       "Invertir Camara"},
            {"settings.invert_cam.desc",        "Invierte los controles de la camara en tercera persona. <recomp-color primary>Invertir X</recomp-color> es el valor por defecto y replica el juego original.<br /><br />Si la camara analogica esta apagada, solo el ajuste <recomp-color primary>Invertir X</recomp-color> tendra efecto."},
            {"settings.invert_fp.title",        "Invertir Vista en Primera Persona"},
            {"settings.invert_fp.desc",         "Invierte los controles de la camara en primera persona. <recomp-color primary>Invertir Y</recomp-color> es el valor por defecto y replica el juego original."},
            {"settings.invert_fs.title",        "Invertir Vuelo y Natacion"},
            {"settings.invert_fs.desc",         "Invierte los controles de nado y vuelo. <recomp-color primary>Invertir Y</recomp-color> es el valor por defecto y replica el juego original."},
            {"settings.bgm_volume.title",       "Volumen de Musica de Fondo"},
            {"settings.bgm_volume.desc",        "Controla el volumen general de la musica de fondo."},
            {"settings.cutscene_aspect.title",  "Relacion de Aspecto en Cinematicas"},
            {"settings.cutscene_aspect.desc",   "Establece el limite de relacion de aspecto para las cinematicas. Las cinematicas se han ajustado para funcionar en <recomp-color primary>16:9</recomp-color>, que es el valor por defecto. Relaciones mas anchas pueden mostrar detalles no pensados para verse."},

            {"opt.on",                          "Activado"},
            {"opt.off",                         "Apagado"},
            {"opt.invert.none",                 "Ninguno"},
            {"opt.invert.x",                    "Invertir X"},
            {"opt.invert.y",                    "Invertir Y"},
            {"opt.invert.both",                 "Invertir Ambos"},
            {"opt.aspect.original",             "Original"},
            {"opt.aspect.16x9",                 "16:9"},
            {"opt.aspect.expand",               "Expandir"},

            {"recompui.tab.general",            "General"},
            {"recompui.tab.graphics",           "Graficos"},
            {"recompui.tab.controls",           "Controles"},
            {"recompui.tab.sound",              "Sonido"},
            {"recompui.tab.mods",               "Mods"},

            {"recompui.btn.apply",              "Aplicar"},
            {"recompui.btn.reset",              "Restablecer"},
            {"recompui.btn.discard",            "Descartar"},
            {"recompui.btn.continue",           "Continuar"},
            {"recompui.btn.save",               "Guardar"},
            {"recompui.btn.cancel",             "Cancelar"},
            {"recompui.btn.close",              "Cerrar"},
            {"recompui.btn.yes",                "Si"},
            {"recompui.btn.no",                 "No"},
            {"recompui.btn.ok",                 "OK"},

            {"recompui.prompt.unsaved.title",   "Cambios sin Guardar"},
            {"recompui.prompt.unsaved.body",    "Tienes cambios sin guardar. Quieres aplicarlos o descartarlos?"},
            {"recompui.prompt.reset.title",     "Restablecer a valores por defecto?"},
            {"recompui.prompt.reset.body",      "Todos los ajustes de esta pestana se restableceran a sus valores por defecto."},

            {"recompui.mods.install",           "Instalar Mod"},
            {"recompui.mods.uninstall",         "Desinstalar"},
            {"recompui.mods.enable",            "Activar"},
            {"recompui.mods.disable",           "Desactivar"},
            {"recompui.mods.no_mods",           "Sin mods instalados."},
            {"recompui.mods.error_install",     "Error al Instalar Mods"},
            {"recompui.mods.unknown_error",     "Ha ocurrido un error desconocido."},
            {"recompui.mods.installing_title",  "Instalando Mods"},
            {"recompui.mods.installing_body",   "Por favor espera"},
            {"recompui.mods.overwrite_title",   "Sobrescribir Mods?"},
            {"recompui.mods.overwrite_btn",     "Sobrescribir"},
            {"recompui.mods.empty",             "No tienes mods. Consigue alguno!"},
            {"recompui.mods.install_mods",      "Instalar Mods"},
            {"recompui.mods.open_folder",       "Abrir Carpeta de Mods"},

            {"recompui.quit.title_online",      "Salir de la Sesion?"},
            {"recompui.quit.title_offline",     "Seguro que quieres salir?"},
            {"recompui.quit.body_online",       "Te desconectaras del juego."},
            {"recompui.quit.body_offline",      "Cualquier progreso desde el ultimo guardado se perdera."},
            {"recompui.quit.btn_disconnect",    "Desconectar"},
            {"recompui.quit.btn_quit",          "Salir"},

            {"recompui.controls.go_back",       "Volver"},
            {"recompui.controls.assign_players","Asignar jugadores"},
            {"recompui.controls.reset_defaults","Restablecer por defecto"},

            {"recompui.tab.unapplied_suffix",   "tiene cambios sin aplicar."},

            {"recompui.general.rumble_strength.title", "Intensidad de Vibracion"},
            {"recompui.general.rumble_strength.desc",  "Controla la intensidad de la vibracion al usar un mando que la soporte. <b>Ponerlo a cero desactivara la vibracion.</b>"},
            {"recompui.general.joystick_deadzone.title", "Zona Muerta del Joystick"},
            {"recompui.general.joystick_deadzone.desc",  "Aplica una zona muerta a la entrada del joystick."},
            {"recompui.general.bg_input.title",          "Entrada en Segundo Plano"},
            {"recompui.general.bg_input.desc",           "Permite que el juego lea el mando cuando esta en segundo plano.<br/><b>No afecta a la entrada de teclado.</b>"},

            {"recompui.graphics.resolution.title",  "Resolucion"},
            {"recompui.graphics.resolution.desc",   "Define la resolucion de salida del juego. <recomp-color primary>Original</recomp-color> usa la resolucion original 240p del juego. <recomp-color primary>Original 2x</recomp-color> renderiza a 480p. <recomp-color primary>Auto</recomp-color> escala segun la ventana del juego."},
            {"recompui.graphics.downsampling.title", "Calidad de Submuestreo"},
            {"recompui.graphics.downsampling.desc",  "Renderiza a mayor resolucion y la reduce para mayor calidad. Solo disponible en <recomp-color primary>Original</recomp-color> y <recomp-color primary>Original 2x</recomp-color>.<br /><br />Nota: <recomp-color primary>4x</recomp-color> en <recomp-color primary>Original 2x</recomp-color> puede causar problemas de rendimiento, ya que el juego renderizaria <recomp-color warning>cerca de 4k internamente</recomp-color>."},
            {"recompui.graphics.aspect_ratio.title",  "Relacion de Aspecto"},
            {"recompui.graphics.aspect_ratio.desc",   "Define la relacion horizontal. <recomp-color primary>Original</recomp-color> usa 4:3 (original). <recomp-color primary>Expandir</recomp-color> se ajusta a la ventana."},
            {"recompui.graphics.window_mode.title",   "Modo de Ventana"},
            {"recompui.graphics.window_mode.desc",    "Define si el juego se muestra en <recomp-color primary>Ventana</recomp-color> o <recomp-color primary>Pantalla Completa</recomp-color>. Tambien puedes usar <recomp-color primary>F11</recomp-color> o <recomp-color primary>Alt + Enter</recomp-color>."},
            {"recompui.graphics.framerate.title",     "Tasa de Cuadros"},
            {"recompui.graphics.msaa.title",          "Anti-Aliasing MSAA"},
            {"recompui.graphics.msaa.desc",           "Define el nivel de anti-aliasing MSAA. Reduce los bordes irregulares a costa del rendimiento.<br /><br /><recomp-color primary>Nota: No estara disponible si tu GPU no soporta MSAA programable.</recomp-color>"},
            {"recompui.graphics.hud_placement.title", "Ubicacion del HUD"},
            {"recompui.graphics.hud_placement.desc",  "Ajusta la posicion del HUD segun la relacion seleccionada. <recomp-color primary>Expandir</recomp-color> usara la relacion de la ventana."},

            {"recompui.sound.main_volume.title", "Volumen General"},
            {"recompui.sound.main_volume.desc",  "Controla el volumen general del juego."},

            {"recompui.opt.original",     "Original"},
            {"recompui.opt.original_2x",  "Original 2x"},
            {"recompui.opt.auto",         "Auto"},
            {"recompui.opt.x2",           "2x"},
            {"recompui.opt.x4",           "4x"},
            {"recompui.opt.windowed",     "Ventana"},
            {"recompui.opt.fullscreen",   "Pantalla Completa"},
            {"recompui.opt.display",      "Pantalla"},
            {"recompui.opt.manual",       "Manual"},
            {"recompui.opt.expand",       "Expandir"},

            {"recompui.input.up",         "Arriba"},
            {"recompui.input.down",       "Abajo"},
            {"recompui.input.left",       "Izquierda"},
            {"recompui.input.right",      "Derecha"},
            {"recompui.input.a",          "A"},
            {"recompui.input.b",          "B"},
            {"recompui.input.z",          "Z"},
            {"recompui.input.l",          "L"},
            {"recompui.input.r",          "R"},
            {"recompui.input.start",      "Inicio"},
            {"recompui.input.c_up",       "C Arriba"},
            {"recompui.input.c_down",     "C Abajo"},
            {"recompui.input.c_left",     "C Izquierda"},
            {"recompui.input.c_right",    "C Derecha"},
            {"recompui.input.dpad_up",    "Cruceta Arriba"},
            {"recompui.input.dpad_down",  "Cruceta Abajo"},
            {"recompui.input.dpad_left",  "Cruceta Izquierda"},
            {"recompui.input.dpad_right", "Cruceta Derecha"},

            {"banjo.input_desc.move",   "Se usa para moverse y dirigir al volar y nadar. La inversion de ejes para vuelo y natacion se configura en la pestana General."},
            {"banjo.input_desc.a",      "Se usa para saltar y seleccionar opciones en menus. Tambien para volar hacia arriba."},
            {"banjo.input_desc.b",      "Se usa para ataques, que cambian segun estes quieto, en movimiento, en el aire o agachado."},
            {"banjo.input_desc.z",      "Se usa para agacharse, lo que permite que A, B y los botones C realicen acciones distintas."},
            {"banjo.input_desc.r",      "Se usa para centrar la camara detras de Banjo en el suelo y para giros mas cerrados al volar o nadar."},
            {"banjo.input_desc.start",  "Se usa para pausar y saltar ciertas cinematicas."},
            {"banjo.input_desc.c_up",   "Se usa para entrar en modo primera persona y para disparar huevos manteniendo Z."},
            {"banjo.input_desc.c_down", "Se usa para alternar los niveles de zoom de la camara y para disparar huevos hacia atras manteniendo Z."},
            {"banjo.input_desc.c_left", "Se usa para rotar la camara lateralmente. La inversion de ejes se configura en la pestana General. Tambien para entrar en Talon Trot manteniendo Z."},
            {"banjo.input_desc.c_right","Se usa para rotar la camara lateralmente. La inversion de ejes se configura en la pestana General. Tambien para entrar en Wonderwing manteniendo Z."},
            {"banjo.input_desc.unused", "Sin uso. Los mods pueden usarlo para funciones adicionales."},

            {"recompui.rom.browse",             "Buscar ROM"},
            {"recompui.rom.wrong_version",      "Esta ROM es del juego correcto, pero la version equivocada.\nEste proyecto requiere la version NTSC-U N64 del juego."},
            {"recompui.rom.wrong_game",         "Esta ROM no es del juego correcto."},
            {"recompui.rom.invalid",            "Archivo ROM invalido."},
        };
        return d;
    }

    const Dict& french_dict() {
        static const Dict d = {
            {"menu.host",               "Heberger"},
            {"menu.join",               "Rejoindre"},
            {"menu.settings",           "Parametres"},
            {"menu.exit",               "Quitter"},
            {"menu.load_rom",           "Charger ROM"},

            {"common.back",             "Retour"},
            {"common.cancel",           "Annuler"},
            {"common.ok",               "OK"},
            {"common.start",            "Demarrer"},
            {"common.retry",            "Reessayer"},
            {"common.erase",            "Effacer"},
            {"common.copy",             "Copier"},
            {"common.refresh",          "Rafraichir"},
            {"common.search",           "Rechercher"},

            {"host.title",              "Heberger une Partie"},
            {"host.player_name",        "Nom du Joueur"},
            {"host.save_slot",          "Emplacement de Sauvegarde"},
            {"host.connection_mode",    "Mode de Connexion"},
            {"host.direct_connection",  "Connexion Directe"},
            {"host.coopnet",            "CoopNet"},
            {"host.password",           "Mot de passe"},
            {"host.password_optional",  "Mot de passe (optionnel)"},
            {"host.port",               "Port"},
            {"host.your_ip_address",    "Votre Adresse IP"},
            {"host.network_system",     "Systeme Reseau"},
            {"host.start_hosting",      "Lancer la Partie"},
            {"host.erase_slot_title",   "Effacer la Sauvegarde"},
            {"host.erase_slot_body",    "Toute la progression de cet emplacement sera definitivement supprimee."},

            {"join.title",              "Rejoindre une Partie"},
            {"join.private_lobbies",    "Salons Prives"},
            {"join.direct_connection",  "Connexion Directe"},
            {"join.password_prompt",    "Entrez le mot de passe du salon prive :"},
            {"join.direct_prompt",      "Entrez l'IP et le port de connexion directe :"},
            {"join.ip_address",         "Adresse IP"},
            {"join.port",               "Port"},
            {"join.connecting",         "Connexion..."},
            {"join.connected",          "Connecte"},
            {"join.failed",             "Echec de connexion"},
            {"join.no_lobbies",         "Aucun salon trouve."},
            {"join.join_btn",           "Rejoindre"},
            {"join.searching",          "Recherche..."},
            {"join.host_ip_address",    "Adresse IP de l'Hote"},
            {"join.search",             "Rechercher"},
            {"join.connecting_server",  "Connexion au serveur..."},
            {"join.connecting_to",      "Connexion a "},
            {"join.joining_lobby",      "Connexion au salon..."},
            {"join.host_not_found",     "Hote introuvable ou ne repond pas."},

            {"coopnet.online",          "CoopNet: En ligne"},
            {"coopnet.offline",         "CoopNet: Hors ligne"},
            {"coopnet.checking",        "CoopNet: Verification..."},

            {"quit.title_online",       "Quitter la Session ?"},
            {"quit.body_online",        "Vous serez deconnecte de la session en cours."},
            {"quit.leave",              "Se deconnecter"},
            {"quit.stay",               "Rester"},

            {"settings.language_restart_title", "Redemarrage Requis"},
            {"settings.language_restart_body",  "Le changement de langue necessite un redemarrage. L'application va redemarrer maintenant."},
            {"settings.language_restart_btn",   "Redemarrer Maintenant"},

            {"settings.language.title",         "Langue"},
            {"settings.language.desc",          "Selectionne la langue du launcher, des parametres et du texte in-game. <recomp-color primary>Le pack de traduction correspondant est active automatiquement.</recomp-color><br /><br />Le changement redemarrera le jeu."},
            {"settings.note_saving.title",      "Sauvegarde des Notes"},
            {"settings.note_saving.desc",       "Sauvegarde les notes collectees pour ne pas avoir a les reprendre en revisitant un niveau. <recomp-color primary>Active</recomp-color> est le defaut; <recomp-color primary>desactive</recomp-color> correspond au jeu original."},
            {"settings.analog_cam.title",       "Camera Analogique"},
            {"settings.analog_cam.desc",        "Active la camera analogique."},
            {"settings.analog_cam_sens.title",  "Sensibilite Camera Analogique"},
            {"settings.analog_cam_sens.desc",   "Regle la sensibilite du stick droit quand la camera analogique est active."},
            {"settings.invert_cam.title",       "Inverser la Camera"},
            {"settings.invert_cam.desc",        "Inverse les controles de la camera a la troisieme personne. <recomp-color primary>Inverser X</recomp-color> est le defaut et correspond au jeu original.<br /><br />Si la camera analogique est desactivee, seul le reglage <recomp-color primary>Inverser X</recomp-color> aura un effet."},
            {"settings.invert_fp.title",        "Inverser la Vue a la Premiere Personne"},
            {"settings.invert_fp.desc",         "Inverse les controles de la camera en vue a la premiere personne. <recomp-color primary>Inverser Y</recomp-color> est le defaut et correspond au jeu original."},
            {"settings.invert_fs.title",        "Inverser Vol et Natation"},
            {"settings.invert_fs.desc",         "Inverse les controles de natation et de vol. <recomp-color primary>Inverser Y</recomp-color> est le defaut et correspond au jeu original."},
            {"settings.bgm_volume.title",       "Volume de la Musique de Fond"},
            {"settings.bgm_volume.desc",        "Controle le volume global de la musique de fond."},
            {"settings.cutscene_aspect.title",  "Format des Cinematiques"},
            {"settings.cutscene_aspect.desc",   "Definit le format limite pour les cinematiques. Les cinematiques ont ete ajustees pour fonctionner en <recomp-color primary>16:9</recomp-color>, qui est le defaut. Des formats plus larges peuvent montrer des details non prevus a l'ecran."},

            {"opt.on",                          "Active"},
            {"opt.off",                         "Desactive"},
            {"opt.invert.none",                 "Aucun"},
            {"opt.invert.x",                    "Inverser X"},
            {"opt.invert.y",                    "Inverser Y"},
            {"opt.invert.both",                 "Inverser les Deux"},
            {"opt.aspect.original",             "Original"},
            {"opt.aspect.16x9",                 "16:9"},
            {"opt.aspect.expand",               "Etendre"},

            {"recompui.tab.general",            "General"},
            {"recompui.tab.graphics",           "Graphismes"},
            {"recompui.tab.controls",           "Controles"},
            {"recompui.tab.sound",              "Son"},
            {"recompui.tab.mods",               "Mods"},

            {"recompui.btn.apply",              "Appliquer"},
            {"recompui.btn.reset",              "Reinitialiser"},
            {"recompui.btn.discard",            "Abandonner"},
            {"recompui.btn.continue",           "Continuer"},
            {"recompui.btn.save",               "Enregistrer"},
            {"recompui.btn.cancel",             "Annuler"},
            {"recompui.btn.close",              "Fermer"},
            {"recompui.btn.yes",                "Oui"},
            {"recompui.btn.no",                 "Non"},
            {"recompui.btn.ok",                 "OK"},

            {"recompui.prompt.unsaved.title",   "Modifications Non Enregistrees"},
            {"recompui.prompt.unsaved.body",    "Vous avez des modifications non enregistrees. Voulez-vous les appliquer ou les abandonner ?"},
            {"recompui.prompt.reset.title",     "Reinitialiser aux defauts ?"},
            {"recompui.prompt.reset.body",      "Tous les parametres de cet onglet seront reinitialises a leurs valeurs par defaut."},

            {"recompui.mods.install",           "Installer un Mod"},
            {"recompui.mods.uninstall",         "Desinstaller"},
            {"recompui.mods.enable",            "Activer"},
            {"recompui.mods.disable",           "Desactiver"},
            {"recompui.mods.no_mods",           "Aucun mod installe."},
            {"recompui.mods.error_install",     "Erreur lors de l'Installation des Mods"},
            {"recompui.mods.unknown_error",     "Une erreur inconnue est survenue."},
            {"recompui.mods.installing_title",  "Installation des Mods"},
            {"recompui.mods.installing_body",   "Veuillez patienter"},
            {"recompui.mods.overwrite_title",   "Ecraser les Mods ?"},
            {"recompui.mods.overwrite_btn",     "Ecraser"},
            {"recompui.mods.empty",             "Vous n'avez aucun mod. Allez en chercher !"},
            {"recompui.mods.install_mods",      "Installer des Mods"},
            {"recompui.mods.open_folder",       "Ouvrir le Dossier des Mods"},

            {"recompui.quit.title_online",      "Quitter la Session ?"},
            {"recompui.quit.title_offline",     "Voulez-vous vraiment quitter ?"},
            {"recompui.quit.body_online",       "Vous serez deconnecte de la partie."},
            {"recompui.quit.body_offline",      "Toute progression depuis votre derniere sauvegarde sera perdue."},
            {"recompui.quit.btn_disconnect",    "Se deconnecter"},
            {"recompui.quit.btn_quit",          "Quitter"},

            {"recompui.controls.go_back",       "Retour"},
            {"recompui.controls.assign_players","Assigner les joueurs"},
            {"recompui.controls.reset_defaults","Reinitialiser aux defauts"},

            {"recompui.tab.unapplied_suffix",   "a des modifications non appliquees."},

            {"recompui.general.rumble_strength.title", "Intensite de Vibration"},
            {"recompui.general.rumble_strength.desc",  "Controle l'intensite de la vibration sur les manettes compatibles. <b>Mettre a zero desactivera la vibration.</b>"},
            {"recompui.general.joystick_deadzone.title", "Zone Morte du Joystick"},
            {"recompui.general.joystick_deadzone.desc",  "Applique une zone morte aux entrees du joystick."},
            {"recompui.general.bg_input.title",          "Mode d'Entree en Arriere-Plan"},
            {"recompui.general.bg_input.desc",           "Permet au jeu de lire la manette quand il n'a pas le focus.<br/><b>N'affecte pas l'entree clavier.</b>"},

            {"recompui.graphics.resolution.title",  "Resolution"},
            {"recompui.graphics.resolution.desc",   "Definit la resolution de sortie. <recomp-color primary>Original</recomp-color> correspond au 240p original. <recomp-color primary>Original 2x</recomp-color> rend en 480p. <recomp-color primary>Auto</recomp-color> s'adapte a la fenetre."},
            {"recompui.graphics.downsampling.title", "Qualite du Sous-echantillonnage"},
            {"recompui.graphics.downsampling.desc",  "Rend a une resolution superieure puis reduit pour ameliorer la qualite. Disponible uniquement en <recomp-color primary>Original</recomp-color> et <recomp-color primary>Original 2x</recomp-color>.<br /><br />Note: <recomp-color primary>4x</recomp-color> en <recomp-color primary>Original 2x</recomp-color> peut causer des problemes de performance car le jeu rendra <recomp-color warning>presque en 4k interne</recomp-color>."},
            {"recompui.graphics.aspect_ratio.title",  "Format d'Image"},
            {"recompui.graphics.aspect_ratio.desc",   "Definit le format horizontal. <recomp-color primary>Original</recomp-color> utilise le 4:3 d'origine. <recomp-color primary>Etendre</recomp-color> s'adapte a la fenetre."},
            {"recompui.graphics.window_mode.title",   "Mode de Fenetre"},
            {"recompui.graphics.window_mode.desc",    "Definit l'affichage en <recomp-color primary>Fenetre</recomp-color> ou <recomp-color primary>Plein Ecran</recomp-color>. Vous pouvez aussi utiliser <recomp-color primary>F11</recomp-color> ou <recomp-color primary>Alt + Enter</recomp-color>."},
            {"recompui.graphics.framerate.title",     "Taux d'Images"},
            {"recompui.graphics.msaa.title",          "Anti-Crenelage MSAA"},
            {"recompui.graphics.msaa.desc",           "Definit le niveau d'anti-crenelage MSAA. Reduit les bords crenele au prix de performance.<br /><br /><recomp-color primary>Note: Indisponible si votre GPU ne supporte pas le MSAA programmable.</recomp-color>"},
            {"recompui.graphics.hud_placement.title", "Placement du HUD"},
            {"recompui.graphics.hud_placement.desc",  "Ajuste la position du HUD selon le format choisi. <recomp-color primary>Etendre</recomp-color> utilise le format de la fenetre."},

            {"recompui.sound.main_volume.title", "Volume Principal"},
            {"recompui.sound.main_volume.desc",  "Controle le volume principal du jeu."},

            {"recompui.opt.original",     "Original"},
            {"recompui.opt.original_2x",  "Original 2x"},
            {"recompui.opt.auto",         "Auto"},
            {"recompui.opt.x2",           "2x"},
            {"recompui.opt.x4",           "4x"},
            {"recompui.opt.windowed",     "Fenetre"},
            {"recompui.opt.fullscreen",   "Plein Ecran"},
            {"recompui.opt.display",      "Ecran"},
            {"recompui.opt.manual",       "Manuel"},
            {"recompui.opt.expand",       "Etendre"},

            {"recompui.input.up",         "Haut"},
            {"recompui.input.down",       "Bas"},
            {"recompui.input.left",       "Gauche"},
            {"recompui.input.right",      "Droite"},
            {"recompui.input.a",          "A"},
            {"recompui.input.b",          "B"},
            {"recompui.input.z",          "Z"},
            {"recompui.input.l",          "L"},
            {"recompui.input.r",          "R"},
            {"recompui.input.start",      "Start"},
            {"recompui.input.c_up",       "C Haut"},
            {"recompui.input.c_down",     "C Bas"},
            {"recompui.input.c_left",     "C Gauche"},
            {"recompui.input.c_right",    "C Droite"},
            {"recompui.input.dpad_up",    "Croix Haut"},
            {"recompui.input.dpad_down",  "Croix Bas"},
            {"recompui.input.dpad_left",  "Croix Gauche"},
            {"recompui.input.dpad_right", "Croix Droite"},

            {"banjo.input_desc.move",   "Sert a se deplacer et a diriger en vol et en nage. L'inversion d'axes pour le vol et la nage se configure dans l'onglet General."},
            {"banjo.input_desc.a",      "Sert a sauter et a selectionner des options dans les menus. Aussi pour voler vers le haut."},
            {"banjo.input_desc.b",      "Sert aux attaques, qui changent selon que vous etes immobile, en mouvement, en l'air ou accroupi."},
            {"banjo.input_desc.z",      "Sert a s'accroupir, ce qui permet a A, B et aux boutons C d'effectuer differentes actions."},
            {"banjo.input_desc.r",      "Sert a recentrer la camera derriere Banjo au sol, et a effectuer des virages plus serres en vol ou en nage."},
            {"banjo.input_desc.start",  "Sert a mettre en pause et a passer certaines cinematiques."},
            {"banjo.input_desc.c_up",   "Sert a passer en vue a la premiere personne et a tirer des oeufs en maintenant Z."},
            {"banjo.input_desc.c_down", "Sert a alterner les niveaux de zoom de la camera et a tirer des oeufs vers l'arriere en maintenant Z."},
            {"banjo.input_desc.c_left", "Sert a faire pivoter la camera sur les cotes. L'inversion d'axes se configure dans l'onglet General. Aussi pour entrer en Talon Trot en maintenant Z."},
            {"banjo.input_desc.c_right","Sert a faire pivoter la camera sur les cotes. L'inversion d'axes se configure dans l'onglet General. Aussi pour entrer en Wonderwing en maintenant Z."},
            {"banjo.input_desc.unused", "Inutilise. Les mods peuvent l'utiliser pour des fonctionnalites supplementaires."},

            {"recompui.rom.browse",             "Parcourir pour une ROM"},
            {"recompui.rom.wrong_version",      "Cette ROM est le bon jeu, mais la mauvaise version.\nCe projet necessite la version NTSC-U N64 du jeu."},
            {"recompui.rom.wrong_game",         "Cette ROM n'est pas le bon jeu."},
            {"recompui.rom.invalid",            "Fichier ROM invalide."},
        };
        return d;
    }

    const Dict& german_dict() {
        static const Dict d = {
            {"menu.host",               "Host"},
            {"menu.join",               "Beitreten"},
            {"menu.settings",           "Einstellungen"},
            {"menu.exit",               "Beenden"},
            {"menu.load_rom",           "ROM laden"},

            {"common.back",             "Zuruck"},
            {"common.cancel",           "Abbrechen"},
            {"common.ok",               "OK"},
            {"common.start",            "Starten"},
            {"common.retry",            "Erneut"},
            {"common.erase",            "Loschen"},
            {"common.copy",             "Kopieren"},
            {"common.refresh",          "Aktualisieren"},
            {"common.search",           "Suchen"},

            {"host.title",              "Spiel hosten"},
            {"host.player_name",        "Spielername"},
            {"host.save_slot",          "Speicherplatz"},
            {"host.connection_mode",    "Verbindungsmodus"},
            {"host.direct_connection",  "Direktverbindung"},
            {"host.coopnet",            "CoopNet"},
            {"host.password",           "Passwort"},
            {"host.password_optional",  "Passwort (optional)"},
            {"host.port",               "Port"},
            {"host.your_ip_address",    "Deine IP-Adresse"},
            {"host.network_system",     "Netzwerksystem"},
            {"host.start_hosting",      "Hosting starten"},
            {"host.erase_slot_title",   "Spielstand loschen"},
            {"host.erase_slot_body",    "Der gesamte Fortschritt dieses Speicherplatzes wird endgultig geloscht."},

            {"join.title",              "Spiel beitreten"},
            {"join.private_lobbies",    "Private Lobbys"},
            {"join.direct_connection",  "Direktverbindung"},
            {"join.password_prompt",    "Gib das Passwort der privaten Lobby ein:"},
            {"join.direct_prompt",      "Gib die IP-Adresse und den Port ein:"},
            {"join.ip_address",         "IP-Adresse"},
            {"join.port",               "Port"},
            {"join.connecting",         "Verbinde..."},
            {"join.connected",          "Verbunden"},
            {"join.failed",             "Verbindung fehlgeschlagen"},
            {"join.no_lobbies",         "Keine Lobbys gefunden."},
            {"join.join_btn",           "Beitreten"},
            {"join.searching",          "Suche..."},
            {"join.host_ip_address",    "IP-Adresse des Hosts"},
            {"join.search",             "Suchen"},
            {"join.connecting_server",  "Verbinde mit Server..."},
            {"join.connecting_to",      "Verbinde mit "},
            {"join.joining_lobby",      "Trete Lobby bei..."},
            {"join.host_not_found",     "Host nicht gefunden oder antwortet nicht."},

            {"coopnet.online",          "CoopNet: Online"},
            {"coopnet.offline",         "CoopNet: Offline"},
            {"coopnet.checking",        "CoopNet: Prufe..."},

            {"quit.title_online",       "Sitzung verlassen?"},
            {"quit.body_online",        "Du wirst von der aktuellen Sitzung getrennt."},
            {"quit.leave",              "Trennen"},
            {"quit.stay",               "Bleiben"},

            {"settings.language_restart_title", "Neustart erforderlich"},
            {"settings.language_restart_body",  "Die Sprachanderung erfordert einen Neustart. Die App startet jetzt neu."},
            {"settings.language_restart_btn",   "Jetzt neu starten"},

            {"settings.language.title",         "Sprache"},
            {"settings.language.desc",          "Wahlt die Sprache fur den Launcher und die Einstellungen. <recomp-color primary>Ingame-Texte werden ebenfalls ubersetzt.</recomp-color><br /><br />Eine Anderung startet das Spiel neu."},
            {"settings.note_saving.title",      "Notenspeicherung"},
            {"settings.note_saving.desc",       "Speichert eingesammelte Noten, damit du sie nicht erneut sammeln musst. <recomp-color primary>An</recomp-color> ist Standard, <recomp-color primary>Aus</recomp-color> entspricht dem Original."},
            {"settings.analog_cam.title",       "Analoge Kamera"},
            {"settings.analog_cam.desc",        "Aktiviert die analoge Kamerasteuerung."},
            {"settings.analog_cam_sens.title",  "Empfindlichkeit der analogen Kamera"},
            {"settings.analog_cam_sens.desc",   "Stellt die Empfindlichkeit des rechten Sticks ein, falls die analoge Kamera aktiv ist."},
            {"settings.invert_cam.title",       "Kamera invertieren"},
            {"settings.invert_cam.desc",        "Invertiert die Steuerung der Drittpersonkamera. <recomp-color primary>X invertieren</recomp-color> ist Standard."},
            {"settings.invert_fp.title",        "Egoperspektive invertieren"},
            {"settings.invert_fp.desc",         "Invertiert die Steuerung in der Egoperspektive. <recomp-color primary>Y invertieren</recomp-color> ist Standard."},
            {"settings.invert_fs.title",        "Fliegen & Schwimmen invertieren"},
            {"settings.invert_fs.desc",         "Invertiert die Steuerung beim Schwimmen und Fliegen. <recomp-color primary>Y invertieren</recomp-color> ist Standard."},
            {"settings.bgm_volume.title",       "Hintergrundmusik-Lautstarke"},
            {"settings.bgm_volume.desc",        "Steuert die Lautstarke der Hintergrundmusik."},
            {"settings.cutscene_aspect.title",  "Format der Zwischensequenzen"},
            {"settings.cutscene_aspect.desc",   "Legt das maximale Format fur Zwischensequenzen fest. Sie wurden fur <recomp-color primary>16:9</recomp-color> angepasst (Standard)."},

            {"opt.on",                          "An"},
            {"opt.off",                         "Aus"},
            {"opt.invert.none",                 "Keine"},
            {"opt.invert.x",                    "X invertieren"},
            {"opt.invert.y",                    "Y invertieren"},
            {"opt.invert.both",                 "Beide invertieren"},
            {"opt.aspect.original",             "Original"},
            {"opt.aspect.16x9",                 "16:9"},
            {"opt.aspect.expand",               "Erweitern"},

            {"recompui.tab.general",            "Allgemein"},
            {"recompui.tab.graphics",           "Grafik"},
            {"recompui.tab.controls",           "Steuerung"},
            {"recompui.tab.sound",              "Ton"},
            {"recompui.tab.mods",               "Mods"},

            {"recompui.btn.apply",              "Ubernehmen"},
            {"recompui.btn.reset",              "Zurucksetzen"},
            {"recompui.btn.discard",            "Verwerfen"},
            {"recompui.btn.continue",           "Weiter"},
            {"recompui.btn.save",               "Speichern"},
            {"recompui.btn.cancel",             "Abbrechen"},
            {"recompui.btn.close",              "Schliessen"},
            {"recompui.btn.yes",                "Ja"},
            {"recompui.btn.no",                 "Nein"},
            {"recompui.btn.ok",                 "OK"},

            {"recompui.prompt.unsaved.title",   "Ungespeicherte Anderungen"},
            {"recompui.prompt.unsaved.body",    "Es gibt ungespeicherte Anderungen. Mochtest du sie ubernehmen oder verwerfen?"},
            {"recompui.prompt.reset.title",     "Auf Standard zurucksetzen?"},
            {"recompui.prompt.reset.body",      "Alle Einstellungen dieses Tabs werden auf ihre Standardwerte zuruckgesetzt."},

            {"recompui.mods.install",           "Mod installieren"},
            {"recompui.mods.uninstall",         "Deinstallieren"},
            {"recompui.mods.enable",            "Aktivieren"},
            {"recompui.mods.disable",           "Deaktivieren"},
            {"recompui.mods.no_mods",           "Keine Mods installiert."},
            {"recompui.mods.error_install",     "Fehler bei der Mod-Installation"},
            {"recompui.mods.unknown_error",     "Ein unbekannter Fehler ist aufgetreten."},
            {"recompui.mods.installing_title",  "Mods werden installiert"},
            {"recompui.mods.installing_body",   "Bitte warten"},
            {"recompui.mods.overwrite_title",   "Mods uberschreiben?"},
            {"recompui.mods.overwrite_btn",     "Uberschreiben"},
            {"recompui.mods.empty",             "Du hast noch keine Mods. Geh und hol dir welche!"},
            {"recompui.mods.install_mods",      "Mods installieren"},
            {"recompui.mods.open_folder",       "Mod-Ordner offnen"},

            {"recompui.quit.title_online",      "Sitzung verlassen?"},
            {"recompui.quit.title_offline",     "Wirklich beenden?"},
            {"recompui.quit.body_online",       "Du wirst vom Spiel getrennt."},
            {"recompui.quit.body_offline",      "Aller Fortschritt seit dem letzten Speichern geht verloren."},
            {"recompui.quit.btn_disconnect",    "Trennen"},
            {"recompui.quit.btn_quit",          "Beenden"},

            {"recompui.controls.go_back",       "Zuruck"},
            {"recompui.controls.assign_players","Spieler zuweisen"},
            {"recompui.controls.reset_defaults","Standard wiederherstellen"},

            {"recompui.tab.unapplied_suffix",   "hat nicht ubernommene Anderungen."},

            {"recompui.general.rumble_strength.title", "Vibrationsstarke"},
            {"recompui.general.rumble_strength.desc",  "Steuert die Vibrationsstarke kompatibler Controller. <b>Auf Null setzen deaktiviert die Vibration.</b>"},
            {"recompui.general.joystick_deadzone.title", "Joystick-Totzone"},
            {"recompui.general.joystick_deadzone.desc",  "Wendet eine Totzone auf Joystick-Eingaben an."},
            {"recompui.general.bg_input.title",          "Hintergrundeingabe"},
            {"recompui.general.bg_input.desc",           "Erlaubt dem Spiel, Controller-Eingaben zu lesen, wenn es nicht im Vordergrund ist.<br/><b>Tastatur ist davon nicht betroffen.</b>"},

            {"recompui.graphics.resolution.title",  "Auflosung"},
            {"recompui.graphics.resolution.desc",   "Legt die Ausgabeauflosung fest. <recomp-color primary>Original</recomp-color> entspricht dem 240p Original. <recomp-color primary>Original 2x</recomp-color> rendert in 480p. <recomp-color primary>Auto</recomp-color> passt sich der Fenstergrosse an."},
            {"recompui.graphics.downsampling.title", "Downsampling-Qualitat"},
            {"recompui.graphics.downsampling.desc",  "Rendert in hoherer Auflosung und skaliert herunter fur bessere Qualitat. Nur in <recomp-color primary>Original</recomp-color> und <recomp-color primary>Original 2x</recomp-color> verfugbar."},
            {"recompui.graphics.aspect_ratio.title",  "Seitenverhaltnis"},
            {"recompui.graphics.aspect_ratio.desc",   "Legt das horizontale Seitenverhaltnis fest. <recomp-color primary>Original</recomp-color> nutzt das 4:3 Original. <recomp-color primary>Erweitern</recomp-color> passt sich dem Fenster an."},
            {"recompui.graphics.window_mode.title",   "Fenstermodus"},
            {"recompui.graphics.window_mode.desc",    "Legt fest, ob das Spiel <recomp-color primary>im Fenster</recomp-color> oder <recomp-color primary>Vollbild</recomp-color> lauft."},
            {"recompui.graphics.framerate.title",     "Bildrate"},
            {"recompui.graphics.msaa.title",          "MS Anti-Aliasing"},
            {"recompui.graphics.msaa.desc",           "Stellt die Qualitat des Multisample-Anti-Aliasing (MSAA) ein."},
            {"recompui.graphics.hud_placement.title", "HUD-Position"},
            {"recompui.graphics.hud_placement.desc",  "Passt die HUD-Position an das gewahlte Seitenverhaltnis an."},

            {"recompui.sound.main_volume.title", "Hauptlautstarke"},
            {"recompui.sound.main_volume.desc",  "Steuert die Hauptlautstarke des Spiels."},

            {"recompui.opt.original",     "Original"},
            {"recompui.opt.original_2x",  "Original 2x"},
            {"recompui.opt.auto",         "Auto"},
            {"recompui.opt.x2",           "2x"},
            {"recompui.opt.x4",           "4x"},
            {"recompui.opt.windowed",     "Fenster"},
            {"recompui.opt.fullscreen",   "Vollbild"},
            {"recompui.opt.display",      "Bildschirm"},
            {"recompui.opt.manual",       "Manuell"},
            {"recompui.opt.expand",       "Erweitern"},

            {"recompui.input.up",         "Hoch"},
            {"recompui.input.down",       "Runter"},
            {"recompui.input.left",       "Links"},
            {"recompui.input.right",      "Rechts"},
            {"recompui.input.a",          "A"},
            {"recompui.input.b",          "B"},
            {"recompui.input.z",          "Z"},
            {"recompui.input.l",          "L"},
            {"recompui.input.r",          "R"},
            {"recompui.input.start",      "Start"},
            {"recompui.input.c_up",       "C Hoch"},
            {"recompui.input.c_down",     "C Runter"},
            {"recompui.input.c_left",     "C Links"},
            {"recompui.input.c_right",    "C Rechts"},
            {"recompui.input.dpad_up",    "Steuerkreuz Hoch"},
            {"recompui.input.dpad_down",  "Steuerkreuz Runter"},
            {"recompui.input.dpad_left",  "Steuerkreuz Links"},
            {"recompui.input.dpad_right", "Steuerkreuz Rechts"},

            {"banjo.input_desc.move",   "Zum Bewegen und Lenken beim Fliegen und Schwimmen. Achseninvertierung im Tab Allgemein einstellbar."},
            {"banjo.input_desc.a",      "Zum Springen und Auswahlen in Menus. Auch zum Aufwartsfliegen."},
            {"banjo.input_desc.b",      "Fur Angriffe, die je nach Position variieren."},
            {"banjo.input_desc.z",      "Zum Ducken; ermoglicht andere Aktionen mit A, B und C-Tasten."},
            {"banjo.input_desc.r",      "Zum Zentrieren der Kamera am Boden und engerem Wenden beim Fliegen/Schwimmen."},
            {"banjo.input_desc.start",  "Zum Pausieren und Uberspringen einiger Zwischensequenzen."},
            {"banjo.input_desc.c_up",   "Wechselt in die Egoperspektive; schiesst Eier mit gehaltenem Z."},
            {"banjo.input_desc.c_down", "Wechselt zwischen Zoomstufen; schiesst Eier ruckwarts mit gehaltenem Z."},
            {"banjo.input_desc.c_left", "Dreht die Kamera. Mit Z aktiviert es Talon Trot."},
            {"banjo.input_desc.c_right","Dreht die Kamera. Mit Z aktiviert es Wonderwing."},
            {"banjo.input_desc.unused", "Unbenutzt. Mods konnen es fur weitere Funktionen nutzen."},

            {"recompui.rom.browse",             "ROM auswahlen"},
            {"recompui.rom.wrong_version",      "Diese ROM ist das richtige Spiel, aber die falsche Version.\nDieses Projekt benotigt die NTSC-U N64 Version."},
            {"recompui.rom.wrong_game",         "Diese ROM ist nicht das richtige Spiel."},
            {"recompui.rom.invalid",            "Ungultige ROM-Datei."},
        };
        return d;
    }

    const Dict& portuguese_dict() {
        static const Dict d = {
            {"menu.host",               "Hospedar"},
            {"menu.join",               "Entrar"},
            {"menu.settings",           "Configuracoes"},
            {"menu.exit",               "Sair"},
            {"menu.load_rom",           "Carregar ROM"},

            {"common.back",             "Voltar"},
            {"common.cancel",           "Cancelar"},
            {"common.ok",               "OK"},
            {"common.start",            "Iniciar"},
            {"common.retry",            "Tentar de novo"},
            {"common.erase",            "Apagar"},
            {"common.copy",             "Copiar"},
            {"common.refresh",          "Atualizar"},
            {"common.search",           "Buscar"},

            {"host.title",              "Hospedar Partida"},
            {"host.player_name",        "Nome do Jogador"},
            {"host.save_slot",          "Slot de Save"},
            {"host.connection_mode",    "Modo de Conexao"},
            {"host.direct_connection",  "Conexao Direta"},
            {"host.coopnet",            "CoopNet"},
            {"host.password",           "Senha"},
            {"host.password_optional",  "Senha (opcional)"},
            {"host.port",               "Porta"},
            {"host.your_ip_address",    "Seu Endereco IP"},
            {"host.network_system",     "Sistema de Rede"},
            {"host.start_hosting",      "Iniciar Hospedagem"},
            {"host.erase_slot_title",   "Apagar Save"},
            {"host.erase_slot_body",    "Todo o progresso deste slot sera permanentemente apagado."},

            {"join.title",              "Entrar na Partida"},
            {"join.private_lobbies",    "Salas Privadas"},
            {"join.direct_connection",  "Conexao Direta"},
            {"join.password_prompt",    "Digite a senha da sala privada:"},
            {"join.direct_prompt",      "Digite o IP e a porta da conexao direta:"},
            {"join.ip_address",         "Endereco IP"},
            {"join.port",               "Porta"},
            {"join.connecting",         "Conectando..."},
            {"join.connected",          "Conectado"},
            {"join.failed",             "Falha na conexao"},
            {"join.no_lobbies",         "Nenhuma sala encontrada."},
            {"join.join_btn",           "Entrar"},
            {"join.searching",          "Procurando..."},
            {"join.host_ip_address",    "Endereco IP do Host"},
            {"join.search",             "Buscar"},
            {"join.connecting_server",  "Conectando ao servidor..."},
            {"join.connecting_to",      "Conectando a "},
            {"join.joining_lobby",      "Entrando na sala..."},
            {"join.host_not_found",     "Host nao encontrado ou nao responde."},

            {"coopnet.online",          "CoopNet: Online"},
            {"coopnet.offline",         "CoopNet: Offline"},
            {"coopnet.checking",        "CoopNet: Verificando..."},

            {"quit.title_online",       "Sair da Sessao?"},
            {"quit.body_online",        "Voce sera desconectado da sessao atual."},
            {"quit.leave",              "Desconectar"},
            {"quit.stay",               "Ficar"},

            {"settings.language_restart_title", "Reinicializacao Necessaria"},
            {"settings.language_restart_body",  "A mudanca de idioma requer reiniciar o jogo. O aplicativo sera reiniciado agora."},
            {"settings.language_restart_btn",   "Reiniciar Agora"},

            {"settings.language.title",         "Idioma"},
            {"settings.language.desc",          "Seleciona o idioma do launcher, configuracoes e texto in-game. <recomp-color primary>O pacote de traducao correspondente e ativado automaticamente.</recomp-color><br /><br />A mudanca reiniciara o jogo."},
            {"settings.note_saving.title",      "Salvar Notas"},
            {"settings.note_saving.desc",       "Salva as notas coletadas para que voce nao precise pega-las novamente ao revisitar uma fase. <recomp-color primary>Ligado</recomp-color> e o padrao; <recomp-color primary>desligado</recomp-color> corresponde ao jogo original."},
            {"settings.analog_cam.title",       "Camera Analogica"},
            {"settings.analog_cam.desc",        "Ativa a camera analogica."},
            {"settings.analog_cam_sens.title",  "Sensibilidade da Camera Analogica"},
            {"settings.analog_cam_sens.desc",   "Ajusta a sensibilidade do analogico direito quando a camera analogica esta ativa."},
            {"settings.invert_cam.title",       "Inverter Camera"},
            {"settings.invert_cam.desc",        "Inverte os controles da camera de terceira pessoa. <recomp-color primary>Inverter X</recomp-color> e o padrao."},
            {"settings.invert_fp.title",        "Inverter Visao em Primeira Pessoa"},
            {"settings.invert_fp.desc",         "Inverte os controles da camera em primeira pessoa. <recomp-color primary>Inverter Y</recomp-color> e o padrao."},
            {"settings.invert_fs.title",        "Inverter Voo e Natacao"},
            {"settings.invert_fs.desc",         "Inverte os controles ao voar e nadar. <recomp-color primary>Inverter Y</recomp-color> e o padrao."},
            {"settings.bgm_volume.title",       "Volume da Musica de Fundo"},
            {"settings.bgm_volume.desc",        "Controla o volume geral da musica de fundo."},
            {"settings.cutscene_aspect.title",  "Formato das Cutscenes"},
            {"settings.cutscene_aspect.desc",   "Define o formato maximo para as cutscenes. Foram ajustadas para <recomp-color primary>16:9</recomp-color>, que e o padrao."},

            {"opt.on",                          "Ligado"},
            {"opt.off",                         "Desligado"},
            {"opt.invert.none",                 "Nenhum"},
            {"opt.invert.x",                    "Inverter X"},
            {"opt.invert.y",                    "Inverter Y"},
            {"opt.invert.both",                 "Inverter Ambos"},
            {"opt.aspect.original",             "Original"},
            {"opt.aspect.16x9",                 "16:9"},
            {"opt.aspect.expand",               "Expandir"},

            {"recompui.tab.general",            "Geral"},
            {"recompui.tab.graphics",           "Graficos"},
            {"recompui.tab.controls",           "Controles"},
            {"recompui.tab.sound",              "Som"},
            {"recompui.tab.mods",               "Mods"},

            {"recompui.btn.apply",              "Aplicar"},
            {"recompui.btn.reset",              "Restaurar"},
            {"recompui.btn.discard",            "Descartar"},
            {"recompui.btn.continue",           "Continuar"},
            {"recompui.btn.save",               "Salvar"},
            {"recompui.btn.cancel",             "Cancelar"},
            {"recompui.btn.close",              "Fechar"},
            {"recompui.btn.yes",                "Sim"},
            {"recompui.btn.no",                 "Nao"},
            {"recompui.btn.ok",                 "OK"},

            {"recompui.prompt.unsaved.title",   "Alteracoes Nao Salvas"},
            {"recompui.prompt.unsaved.body",    "Existem alteracoes nao salvas. Deseja aplicar ou descartar?"},
            {"recompui.prompt.reset.title",     "Restaurar para os padroes?"},
            {"recompui.prompt.reset.body",      "Todas as configuracoes desta aba serao restauradas para os valores padrao."},

            {"recompui.mods.install",           "Instalar Mod"},
            {"recompui.mods.uninstall",         "Desinstalar"},
            {"recompui.mods.enable",            "Ativar"},
            {"recompui.mods.disable",           "Desativar"},
            {"recompui.mods.no_mods",           "Nenhum mod instalado."},
            {"recompui.mods.error_install",     "Erro ao Instalar Mods"},
            {"recompui.mods.unknown_error",     "Ocorreu um erro desconhecido."},
            {"recompui.mods.installing_title",  "Instalando Mods"},
            {"recompui.mods.installing_body",   "Por favor, aguarde"},
            {"recompui.mods.overwrite_title",   "Sobrescrever Mods?"},
            {"recompui.mods.overwrite_btn",     "Sobrescrever"},
            {"recompui.mods.empty",             "Voce nao tem nenhum mod. Va e pegue alguns!"},
            {"recompui.mods.install_mods",      "Instalar Mods"},
            {"recompui.mods.open_folder",       "Abrir Pasta de Mods"},

            {"recompui.quit.title_online",      "Sair da Sessao?"},
            {"recompui.quit.title_offline",     "Realmente sair?"},
            {"recompui.quit.body_online",       "Voce sera desconectado do jogo."},
            {"recompui.quit.body_offline",      "Todo progresso desde o ultimo save sera perdido."},
            {"recompui.quit.btn_disconnect",    "Desconectar"},
            {"recompui.quit.btn_quit",          "Sair"},

            {"recompui.controls.go_back",       "Voltar"},
            {"recompui.controls.assign_players","Atribuir Jogadores"},
            {"recompui.controls.reset_defaults","Restaurar Padroes"},

            {"recompui.tab.unapplied_suffix",   "tem alteracoes nao aplicadas."},

            {"recompui.general.rumble_strength.title", "Intensidade da Vibracao"},
            {"recompui.general.rumble_strength.desc",  "Controla a intensidade da vibracao em controles compativeis. <b>Defina como zero para desativar a vibracao.</b>"},
            {"recompui.general.joystick_deadzone.title", "Zona Morta do Analogico"},
            {"recompui.general.joystick_deadzone.desc",  "Aplica uma zona morta as entradas do analogico."},
            {"recompui.general.bg_input.title",          "Modo de Entrada em Segundo Plano"},
            {"recompui.general.bg_input.desc",           "Permite que o jogo leia entradas do controle quando esta fora de foco.<br/><b>Nao afeta o teclado.</b>"},

            {"recompui.graphics.resolution.title",  "Resolucao"},
            {"recompui.graphics.resolution.desc",   "Define a resolucao de saida. <recomp-color primary>Original</recomp-color> corresponde aos 240p originais. <recomp-color primary>Original 2x</recomp-color> renderiza em 480p. <recomp-color primary>Auto</recomp-color> ajusta com base na janela."},
            {"recompui.graphics.downsampling.title", "Qualidade de Downsampling"},
            {"recompui.graphics.downsampling.desc",  "Renderiza em uma resolucao maior e reduz para melhor qualidade. Disponivel apenas em <recomp-color primary>Original</recomp-color> e <recomp-color primary>Original 2x</recomp-color>."},
            {"recompui.graphics.aspect_ratio.title",  "Proporcao de Tela"},
            {"recompui.graphics.aspect_ratio.desc",   "Define a proporcao horizontal. <recomp-color primary>Original</recomp-color> usa o 4:3 original. <recomp-color primary>Expandir</recomp-color> ajusta a janela."},
            {"recompui.graphics.window_mode.title",   "Modo de Janela"},
            {"recompui.graphics.window_mode.desc",    "Define se o jogo roda em <recomp-color primary>Janela</recomp-color> ou <recomp-color primary>Tela cheia</recomp-color>."},
            {"recompui.graphics.framerate.title",     "Taxa de Quadros"},
            {"recompui.graphics.msaa.title",          "Anti-aliasing MSAA"},
            {"recompui.graphics.msaa.desc",           "Define o nivel de qualidade do MSAA."},
            {"recompui.graphics.hud_placement.title", "Posicao do HUD"},
            {"recompui.graphics.hud_placement.desc",  "Ajusta a posicao do HUD para a proporcao escolhida."},

            {"recompui.sound.main_volume.title", "Volume Principal"},
            {"recompui.sound.main_volume.desc",  "Controla o volume principal do jogo."},

            {"recompui.opt.original",     "Original"},
            {"recompui.opt.original_2x",  "Original 2x"},
            {"recompui.opt.auto",         "Auto"},
            {"recompui.opt.x2",           "2x"},
            {"recompui.opt.x4",           "4x"},
            {"recompui.opt.windowed",     "Janela"},
            {"recompui.opt.fullscreen",   "Tela cheia"},
            {"recompui.opt.display",      "Monitor"},
            {"recompui.opt.manual",       "Manual"},
            {"recompui.opt.expand",       "Expandir"},

            {"recompui.input.up",         "Cima"},
            {"recompui.input.down",       "Baixo"},
            {"recompui.input.left",       "Esquerda"},
            {"recompui.input.right",      "Direita"},
            {"recompui.input.a",          "A"},
            {"recompui.input.b",          "B"},
            {"recompui.input.z",          "Z"},
            {"recompui.input.l",          "L"},
            {"recompui.input.r",          "R"},
            {"recompui.input.start",      "Start"},
            {"recompui.input.c_up",       "C Cima"},
            {"recompui.input.c_down",     "C Baixo"},
            {"recompui.input.c_left",     "C Esquerda"},
            {"recompui.input.c_right",    "C Direita"},
            {"recompui.input.dpad_up",    "Direcional Cima"},
            {"recompui.input.dpad_down",  "Direcional Baixo"},
            {"recompui.input.dpad_left",  "Direcional Esquerda"},
            {"recompui.input.dpad_right", "Direcional Direita"},

            {"banjo.input_desc.move",   "Para se mover e direcionar voo e natacao. A inversao de eixos pode ser configurada na aba Geral."},
            {"banjo.input_desc.a",      "Para pular e selecionar opcoes nos menus. Tambem voa para cima."},
            {"banjo.input_desc.b",      "Para ataques, que mudam dependendo da posicao."},
            {"banjo.input_desc.z",      "Para se agachar; permite outras acoes com A, B e botoes C."},
            {"banjo.input_desc.r",      "Centraliza a camera atras do Banjo no chao e faz curvas mais fechadas voando ou nadando."},
            {"banjo.input_desc.start",  "Para pausar e pular algumas cutscenes."},
            {"banjo.input_desc.c_up",   "Entra em primeira pessoa e atira ovos com Z pressionado."},
            {"banjo.input_desc.c_down", "Alterna o zoom da camera e atira ovos para tras com Z."},
            {"banjo.input_desc.c_left", "Gira a camera. Com Z entra em Talon Trot."},
            {"banjo.input_desc.c_right","Gira a camera. Com Z ativa Wonderwing."},
            {"banjo.input_desc.unused", "Nao usado. Mods podem usar para funcionalidades extras."},

            {"recompui.rom.browse",             "Procurar ROM"},
            {"recompui.rom.wrong_version",      "Esta ROM e o jogo certo, mas a versao errada.\nEste projeto requer a versao NTSC-U N64."},
            {"recompui.rom.wrong_game",         "Esta ROM nao e o jogo correto."},
            {"recompui.rom.invalid",            "Arquivo ROM invalido."},
        };
        return d;
    }

    const Dict& dict_for(Language lang) {
        switch (lang) {
            case Language::Spanish:    return spanish_dict();
            case Language::French:     return french_dict();
            case Language::German:     return german_dict();
            case Language::Portuguese: return portuguese_dict();
            case Language::English:
            default:                   return english_dict();
        }
    }
}

namespace banjo::locale {
    void init() {
        // Read from the standalone file, NOT the recompui::config (which isn't set up yet).
        g_startup = read_language_file();
        g_current = g_startup;
    }

    bool refresh() {
        Language now = banjo::get_language();
        if (now != g_current) {
            g_current = now;
            // Mirror to the standalone file so the next launch reads the new choice
            // before the recompui::config system is initialized.
            write_language_file(now);
            return true;
        }
        return false;
    }

    Language get_startup_language() { return g_startup; }
    Language get_current() { return g_current; }

    void commit_language_to_disk(Language lang) {
        write_language_file(lang);
        // Don't update g_current here — that would mark the language as "applied"
        // and suppress the polling-based refresh() prompt fallback. The next launch
        // will pick up the new value via read_language_file() in init().
    }

    const std::string& tr(const char* key) {
        const std::string k{key};
        const Dict& active = dict_for(g_current);
        auto it = active.find(k);
        if (it != active.end()) return it->second;
        // Fallback to English.
        const Dict& en = english_dict();
        auto eit = en.find(k);
        if (eit != en.end()) return eit->second;
        // Fallback to the key itself (so missing translations surface visibly).
        static thread_local std::string missing;
        missing = k;
        return missing;
    }
}

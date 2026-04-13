#include <cstdio>
#include <cassert>
#include <unordered_map>
#include <vector>
#include <array>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <atomic>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <cinttypes>
#include <cstdlib>

#include "nfd.h"
#include "../net/net_manager.h"
#include "../net/net_config.h"
#include "../net/net_chat.h"
#include "../net/net_chat_ui.h"

// Network recomp API functions (defined in net_recomp_api.cpp)
#include "recomp.h"
extern "C" void recomp_net_push_full_state(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_push_local_state(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_is_connected(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_get_remote_state(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_get_remote_count(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_get_local_player_id(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_send_collectible(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_send_enemy_death(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_send_flag_change(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_pop_world_event(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_is_host(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_send_enemy_positions(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_get_enemy_positions(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_should_send_full_sync(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_send_world_state_full(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_pop_full_state(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_am_i_world_owner(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_push_level_id(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_is_online_mode(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_get_save_slot(uint8_t* rdram, recomp_context* ctx);
extern "C" void recomp_net_is_join_mode(uint8_t* rdram, recomp_context* ctx);

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#define SDL_MAIN_HANDLED
#ifdef _WIN32
#include "SDL.h"
#else
#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"
// Undefine x11 macros that get included by SDL_syswm.h.
#undef None
#undef Status
#undef LockMask
#undef ControlMask
#undef Success
#undef Always
#endif

#include "recompui/recompui.h"
#include "recompui/program_config.h"
#include "recompui/renderer.h"
#include "recompui/config.h"
#include "elements/ui_text_input.h"
#include "elements/ui_select.h"
#include "util/file.h"
#include "recompinput/input_events.h"
#include "recompinput/recompinput.h"
#include "recompinput/profiles.h"
#include "banjo_config.h"
#include "banjo_sound.h"
#include "banjo_support.h"
#include "banjo_game.h"
#include "banjo_launcher.h"
#include "recomp_data.h"
#include "ovl_patches.hpp"
#include "theme.h"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/helpers.hpp"

#include "../../patches/graphics.h"
#include "../../patches/input.h"
#include "../../patches/sound.h"
#include "../../patches/misc_funcs.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <timeapi.h>
#include "SDL_syswm.h"
#define APP_ICON_B 1
#define APP_ICON_K 2
#endif

#include "../../lib/rt64/src/contrib/stb/stb_image.h"

const std::string version_string = "1.0.1";

template<typename... Ts>
void exit_error(const char* str, Ts ...args) {
    // TODO pop up an error
    ((void)fprintf(stderr, str, args), ...);
    assert(false);
        
    ultramodern::error_handling::quick_exit(__FILE__, __LINE__, __FUNCTION__);
}

ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) > 0) {
        exit_error("Failed to initialize SDL2: %s\n", SDL_GetError());
    }

    fprintf(stdout, "SDL Video Driver: %s\n", SDL_GetCurrentVideoDriver());

    return {};
}

ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (recompinput::players::is_single_player_mode() || recompinput::players::get_player_is_assigned(controller_num)) {
        return ultramodern::input::connected_device_info_t{
            .connected_device = ultramodern::input::Device::Controller,
            .connected_pak = ultramodern::input::Pak::RumblePak,
        };
    }

    return ultramodern::input::connected_device_info_t{
        .connected_device = ultramodern::input::Device::None,
        .connected_pak = ultramodern::input::Pak::None,
    };
}

#include "icon_bytes.h"

#if defined(__gnu_linux__)
bool SetImageAsIcon(const char* filename, SDL_Window* window)
{
    // Read data
    int width, height, bytesPerPixel;
    void* data = stbi_load_from_memory(reinterpret_cast<const uint8_t*>(icon_bytes), sizeof(icon_bytes), &width, &height, &bytesPerPixel, 4);

    // Calculate pitch
    int pitch;
    pitch = width * 4;
    pitch = (pitch + 3) & ~3;

    // Setup relevance bitmask
    int Rmask, Gmask, Bmask, Amask;

#if SDL_BYTEORDER == SDL_LIL_ENDIAN
    Rmask = 0x000000FF;
    Gmask = 0x0000FF00;
    Bmask = 0x00FF0000;
    Amask = 0xFF000000;
#else
    Rmask = 0xFF000000;
    Gmask = 0x00FF0000;
    Bmask = 0x0000FF00;
    Amask = 0x000000FF;
#endif

    SDL_Surface* surface = nullptr;
    if (data != nullptr) {
        surface = SDL_CreateRGBSurfaceFrom(data, width, height, 32, pitch, Rmask, Gmask,
                            Bmask, Amask);
    }

    if (surface == nullptr) {   
        if (data != nullptr) {
            stbi_image_free(data);
        }
        return false;
	} else {
        SDL_SetWindowIcon(window,surface);
        SDL_FreeSurface(surface);
        stbi_image_free(data);
        return true;
    }
}
#endif

SDL_Window* window;

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    uint32_t flags = SDL_WINDOW_RESIZABLE;

#if defined(__APPLE__)
    flags |= SDL_WINDOW_METAL;
#elif defined(RT64_SDL_WINDOW_VULKAN)
    flags |= SDL_WINDOW_VULKAN;
#endif

    window = SDL_CreateWindow("Banjo: Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1600, 900,  flags);

    if (window == nullptr) {
        exit_error("Failed to create window: %s\n", SDL_GetError());
    }

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(window, &wmInfo);

#if defined(_WIN32)
    // There's a 50/50 chance to choose the icon where the smallest variant is either Banjo or Kazooie alone.
    bool choose_kazooie_icon = (rand() % 2 == 0);
    HICON new_icon = LoadIcon(GetModuleHandle(NULL), choose_kazooie_icon ? MAKEINTRESOURCE(APP_ICON_K) : MAKEINTRESOURCE(APP_ICON_B));
    SendMessage(wmInfo.info.win.window, WM_SETICON, ICON_SMALL2, (LPARAM)(new_icon));
#elif defined(__linux__)
    SetImageAsIcon("icons/app.png", window);
#endif

#if defined(_WIN32)
    return ultramodern::renderer::WindowHandle{ wmInfo.info.win.window, GetCurrentThreadId() };
#elif defined(__linux__) || defined(__ANDROID__)
    return ultramodern::renderer::WindowHandle{ window };
#elif defined(__APPLE__)
    SDL_MetalView view = SDL_Metal_CreateView(window);
    return ultramodern::renderer::WindowHandle{ wmInfo.info.cocoa.window,  SDL_Metal_GetLayer(view) };
#else
    static_assert(false && "Unimplemented");
#endif
}

void update_gfx(void*) {
    // Poll chat input BEFORE game input so we can steal keyboard events
    bknet::ChatInput::instance().poll_events();

    recompinput::handle_events();

    // Pump network events and send/receive state
    auto& net = bknet::NetworkManager::instance();
    if (net.is_connected()) {
        net.update();
    }

    // Update chat overlay UI
    bknet::chat_ui_update();
}

static SDL_AudioCVT audio_convert;
static SDL_AudioDeviceID audio_device = 0;

// Samples per channel per second.
static uint32_t sample_rate = 48000;
static uint32_t output_sample_rate = 48000;
// Channel count.
constexpr uint32_t input_channels = 2;
static uint32_t output_channels = 2;

// Terminology: a frame is a collection of samples for each channel. e.g. 2 input samples is one input frame. This is unrelated to graphical frames.

// Number of frames to duplicate for fixing interpolation at the start and end of a chunk.
constexpr uint32_t duplicated_input_frames = 4;
// The number of output frames to skip for playback (to avoid playing duplicate inputs twice).
static uint32_t discarded_output_frames;

constexpr uint32_t bytes_per_frame = input_channels * sizeof(float);

void queue_samples(int16_t* audio_data, size_t sample_count) {
    // Buffer for holding the output of swapping the audio channels. This is reused across
    // calls to reduce runtime allocations.
    static std::vector<float> swap_buffer;
    static std::array<float, duplicated_input_frames * input_channels> duplicated_sample_buffer;

    // Make sure the swap buffer is large enough to hold the audio data, including any extra space needed for resampling.
    size_t resampled_sample_count = sample_count + duplicated_input_frames * input_channels;
    size_t max_sample_count = std::max(resampled_sample_count, resampled_sample_count * audio_convert.len_mult);
    if (max_sample_count > swap_buffer.size()) {
        swap_buffer.resize(max_sample_count);
    }
    
    // Copy the duplicated frames from last chunk into this chunk
    for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
        swap_buffer[i] = duplicated_sample_buffer[i];
    }

    // Convert the audio from 16-bit values to floats and swap the audio channels into the
    // swap buffer to correct for the address xor caused by endianness handling.
    float cur_main_volume = static_cast<float>(recompui::config::sound::get_main_volume()) / 100.0f; // Get the current main volume, normalized to 0.0-1.0.
    for (size_t i = 0; i < sample_count; i += input_channels) {
        swap_buffer[i + 0 + duplicated_input_frames * input_channels] = audio_data[i + 1] * (0.5f / 32768.0f) * cur_main_volume;
        swap_buffer[i + 1 + duplicated_input_frames * input_channels] = audio_data[i + 0] * (0.5f / 32768.0f) * cur_main_volume;
    }
    
    // TODO handle cases where a chunk is smaller than the duplicated frame count.
    assert(sample_count > duplicated_input_frames * input_channels);

    // Copy the last converted samples into the duplicated sample buffer to reuse in resampling the next queued chunk.
    for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
        duplicated_sample_buffer[i] = swap_buffer[i + sample_count];
    }
    
    audio_convert.buf = reinterpret_cast<Uint8*>(swap_buffer.data());
    audio_convert.len = (sample_count + duplicated_input_frames * input_channels) * sizeof(swap_buffer[0]);

    int ret = SDL_ConvertAudio(&audio_convert);

    if (ret < 0) {
        printf("Error using SDL audio converter: %s\n", SDL_GetError());
        throw std::runtime_error("Error using SDL audio converter");
    }

    uint64_t cur_queued_microseconds = uint64_t(SDL_GetQueuedAudioSize(audio_device)) / bytes_per_frame * 1000000 / sample_rate;
    uint32_t num_bytes_to_queue = audio_convert.len_cvt - output_channels * discarded_output_frames * sizeof(swap_buffer[0]);
    float* samples_to_queue = swap_buffer.data() + output_channels * discarded_output_frames / 2;

    // Prevent audio latency from building up by skipping samples in incoming audio when too many samples are already queued.
    // Skip samples based on how many microseconds of samples are queued already.
    uint32_t skip_factor = cur_queued_microseconds / 100000;
    if (skip_factor != 0) {
        uint32_t skip_ratio = 1 << skip_factor;
        num_bytes_to_queue /= skip_ratio;
        for (size_t i = 0; i < num_bytes_to_queue / (output_channels * sizeof(swap_buffer[0])); i++) {
            samples_to_queue[2 * i + 0] = samples_to_queue[2 * skip_ratio * i + 0];
            samples_to_queue[2 * i + 1] = samples_to_queue[2 * skip_ratio * i + 1];
        }
    }

    // Queue the swapped audio data.
    // Offset the data start by only half the discarded frame count as the other half of the discarded frames are at the end of the buffer.
    SDL_QueueAudio(audio_device, samples_to_queue, num_bytes_to_queue);
}

size_t get_frames_remaining() {
    constexpr float buffer_offset_frames = 1.0f;
    // Get the number of remaining buffered audio bytes.
    uint64_t buffered_byte_count = SDL_GetQueuedAudioSize(audio_device);

    // Scale the byte count based on the ratio of sample rates and channel counts.
    buffered_byte_count = buffered_byte_count * 2 * sample_rate / output_sample_rate / output_channels;

    // Adjust the reported count to be some number of refreshes in the future, which helps ensure that
    // there are enough samples even if the audio thread experiences a small amount of lag. This prevents
    // audio popping on games that use the buffered audio byte count to determine how many samples
    // to generate.
    uint32_t frames_per_vi = (sample_rate / 60);
    if (buffered_byte_count > (buffer_offset_frames * bytes_per_frame * frames_per_vi)) {
        buffered_byte_count -= (buffer_offset_frames * bytes_per_frame * frames_per_vi);
    }
    else {
        buffered_byte_count = 0;
    }
    // Convert from byte count to sample count.
    return static_cast<uint32_t>(buffered_byte_count / bytes_per_frame);
}

void update_audio_converter() {
    int ret = SDL_BuildAudioCVT(&audio_convert, AUDIO_F32, input_channels, sample_rate, AUDIO_F32, output_channels, output_sample_rate);

    if (ret < 0) {
        printf("Error creating SDL audio converter: %s\n", SDL_GetError());
        throw std::runtime_error("Error creating SDL audio converter");
    }

    // Calculate the number of samples to discard based on the sample rate ratio and the duplicate frame count.
    discarded_output_frames = duplicated_input_frames * output_sample_rate / sample_rate;
}

void set_frequency(uint32_t freq) {
    sample_rate = freq;
    
    update_audio_converter();
}

bool reset_audio(uint32_t output_freq) {
    SDL_AudioSpec spec_desired{
        .freq = (int)output_freq,
        .format = AUDIO_F32,
        .channels = (Uint8)output_channels,
        .silence = 0, // calculated
        .samples = 0x100, // Fairly small sample count to reduce the latency of internal buffering
        .padding = 0, // unused
        .size = 0, // calculated
        .callback = nullptr,
        .userdata = nullptr
    };

    audio_device = SDL_OpenAudioDevice(nullptr, false, &spec_desired, nullptr, 0);
    if (audio_device == 0) {
        std::string audio_error = std::string("No audio device could be found. Please make sure an audio device is available.\nError opening audio device: ") + std::string(SDL_GetError());
        recompui::message_box(audio_error.c_str());
        return false;
    }

    SDL_PauseAudioDevice(audio_device, 0);

    output_sample_rate = output_freq;
    update_audio_converter();

    return true;
}

extern RspUcodeFunc n_aspMain;

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_AUDTASK:
        return n_aspMain;

    default:
        fprintf(stderr, "Unknown task: %" PRIu32 "\n", task->t.type);
        return nullptr;
    }
}

extern "C" void recomp_entrypoint(uint8_t * rdram, recomp_context * ctx);
gpr get_entrypoint_address();

// array of supported GameEntry objects
std::vector<recomp::GameEntry> supported_games = {
    {
        .rom_hash = 0x1B67585D56E07F8CULL,
        .internal_name = "Banjo-Kazooie",
        .display_name = "Banjo-Kazooie",
        .game_id = u8"bk.n64.us.1.0",
        .mod_game_id = "bk",
        // Eep16k instead of Eep4k to have room for extra save file data.
        .save_type = recomp::SaveType::Eep16k,
        .thumbnail_bytes = std::span<const char>(icon_bytes),
        .is_enabled = false,
        .decompression_routine = banjo::decompress_bk,
        .has_compressed_code = true,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
        .on_init_callback = banjo::bk_on_init,
    },
};

// TODO: move somewhere else
namespace banjo {
    std::string get_game_thread_name(const OSThread* t) {
        std::string name = "[Game] ";

        switch (t->id) {
            case 0:
                switch (t->priority) {
                    case 150:
                        name += "PIMGR";
                        break;

                    case 80:
                        name += "VIMGR";
                        break;

                    default:
                        name += std::to_string(t->id);
                        break;
                }
                break;
            case 1:
                name += "INIT";
                break;
            case 2:
                name += "DEFRAG";
                break;
            case 4:
                name += "AUDIO";
                break;
            case 5:
                name += "RESET";
                break;
            case 6:
                name += "MAIN";
                break;
            case 7:
                name += "CONT";
                break;
            case 8:
                name += "RUMBLE";
                break;
            default:
                name += std::to_string(t->id);
                break;
        }

        return name;
    }
}

#ifdef _WIN32

struct PreloadContext {
    HANDLE handle;
    HANDLE mapping_handle;
    SIZE_T size;
    PVOID view;
};

bool preload_executable(PreloadContext& context) {
    wchar_t module_name[MAX_PATH];
    GetModuleFileNameW(NULL, module_name, MAX_PATH);

    context.handle = CreateFileW(module_name, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (context.handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to load executable into memory!");
        context = {};
        return false;
    }

    LARGE_INTEGER module_size;
    if (!GetFileSizeEx(context.handle, &module_size)) {
        fprintf(stderr, "Failed to get size of executable!");
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    context.size = module_size.QuadPart;

    context.mapping_handle = CreateFileMappingW(context.handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (context.mapping_handle == nullptr) {
        fprintf(stderr, "Failed to create file mapping of executable!");
        CloseHandle(context.handle);
        context = {};
        return EXIT_FAILURE;
    }

    context.view = MapViewOfFile(context.mapping_handle, FILE_MAP_READ, 0, 0, 0);
    if (context.view == nullptr) {
        fprintf(stderr, "Failed to map view of of executable!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    DWORD pid = GetCurrentProcessId();
    HANDLE process_handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process_handle == nullptr) {
        fprintf(stderr, "Failed to open own process!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    SIZE_T minimum_set_size, maximum_set_size;
    if (!GetProcessWorkingSetSize(process_handle, &minimum_set_size, &maximum_set_size)) {
        fprintf(stderr, "Failed to get working set size!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    if (!SetProcessWorkingSetSize(process_handle, minimum_set_size + context.size, maximum_set_size + context.size)) {
        fprintf(stderr, "Failed to set working set size!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    if (VirtualLock(context.view, context.size) == 0) {
        fprintf(stderr, "Failed to lock view of executable! (Error: %08lx)\n", GetLastError());
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }
    
    return true;
}

void release_preload(PreloadContext& context) {
    VirtualUnlock(context.view, context.size);
    CloseHandle(context.mapping_handle);
    CloseHandle(context.handle);
    context = {};
}

#elif defined(__linux__) || defined(APPLE)

struct PreloadContext {

};

bool preload_executable(PreloadContext& context) {
    // Preloading isn't implemented on Linux and MacOS, but it's also unnecessary there, as the OS already preloads the executable.
    // Therefore, we can just consider the executable to be preloaded.
    return true;
}

void release_preload(PreloadContext& context) {
}

#else

struct PreloadContext {};

bool preload_executable(PreloadContext& context) {
    return false;
}

void release_preload(PreloadContext& context) {
}

#endif

void enable_texture_pack(recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod) {
    recompui::renderer::enable_texture_pack(context, mod);
}

void disable_texture_pack(recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
    recompui::renderer::disable_texture_pack(mod);
}

void reorder_texture_pack(recomp::mods::ModContext&) {
    recompui::renderer::trigger_texture_pack_update();
}

// --- Multiplayer submenu panels ---
static recompui::Element* host_panel = nullptr;
static recompui::Element* join_panel = nullptr;
static recompui::GameOptionsMenu* g_game_options_menu = nullptr;
static recompui::LauncherMenu* g_launcher_menu = nullptr;
static int selected_save_slot = 0;
static bool host_slots_dirty = false;

// --- Save data parsing (read from disk without running game) ---

struct SlotInfo {
    bool valid;
    int jiggies;
    int notes;
};

// BK EEPROM layout: SaveData = 120 bytes per slot (magic, slotIndex, data[0x70], padding[2], checksum)
// data[0x70] internal layout (from savedata_init / code_B5040.c):
//   jiggyOffset     = 2   (13 bytes, bitmap: 1 bit per jiggy, IDs 1-100)
//   honeycombOffset = 15  (3 bytes, ((25-1+7)&~7)/8 = 3)
//   mumbotokenOffset= 18  (16 bytes, ((126-1+7)&~7)/8 = 16)
//   notescoresOffset= 34  (8 bytes, 9 levels x 7-bit packed into u64 big-endian)
static constexpr int SAVE_SLOT_SIZE   = 120;  // sizeof(SaveData)
static constexpr int JIGGY_OFFSET     = 2;    // within save slot
static constexpr int JIGGY_SIZE       = 13;   // 0x0D
static constexpr int HONEYCOMB_SIZE   = 3;    // ((25-1+7)&~7)/8 = 3
static constexpr int MUMBO_SIZE       = 16;   // ((126-1+7)&~7)/8 = 16
static constexpr int NOTES_OFFSET     = JIGGY_OFFSET + JIGGY_SIZE + HONEYCOMB_SIZE + MUMBO_SIZE; // 2+13+3+16 = 34
static constexpr int NOTES_SIZE       = 8;

static int popcount_bytes(const uint8_t* data, int num_bytes) {
    int count = 0;
    for (int i = 0; i < num_bytes; i++) {
        uint8_t b = data[i];
        while (b) { count += (b & 1); b >>= 1; }
    }
    return count;
}

static int unpack_notes_total(const uint8_t* data8) {
    // Notes are packed as 9 levels x 7 bits into a big-endian u64
    // Levels 1-10, skipping level 6 → 9 values
    uint64_t packed = 0;
    for (int i = 0; i < 8; i++) {
        packed = (packed << 8) | data8[i];
    }
    int total = 0;
    for (int i = 0; i < 9; i++) {
        total += (int)(packed & 0x7F);
        packed >>= 7;
    }
    return total;
}

// BK stores 4 physical SaveData blocks in EEPROM. Each has a slotIndex field (1-based)
// that maps it to a game slot (0-2). The physical order can differ from the game slot order.
// We scan all 4 physical blocks to find the one matching the requested game slot.
static SlotInfo read_save_slot(int game_slot) {
    SlotInfo info = { false, 0, 0 };
    // Use game_id from supported_games directly — current_game_id() is not available before start_game()
    std::filesystem::path save_path = recomp::get_config_path() / u8"saves" / (supported_games[0].game_id + u8".bin");

    std::ifstream f(save_path, std::ios::binary);
    if (!f.good()) return info;

    f.seekg(0, std::ios::end);
    auto file_size = f.tellg();

    // Scan all 4 physical slots to find the one with matching slotIndex
    for (int phys = 0; phys < 4; phys++) {
        int slot_start = phys * SAVE_SLOT_SIZE;
        if (file_size < slot_start + SAVE_SLOT_SIZE) continue;

        uint8_t slot_data[SAVE_SLOT_SIZE];
        f.seekg(slot_start);
        f.read(reinterpret_cast<char*>(slot_data), SAVE_SLOT_SIZE);

        // slot_data[0] = magic, slot_data[1] = slotIndex (1-based)
        if (slot_data[0] == 0x00) continue;
        int stored_slot = slot_data[1] - 1; // convert to 0-based
        if (stored_slot != game_slot) continue;

        info.valid = true;
        info.jiggies = popcount_bytes(&slot_data[JIGGY_OFFSET], JIGGY_SIZE);
        info.notes = unpack_notes_total(&slot_data[NOTES_OFFSET]);
        printf("[Save] Game slot %d → phys block %d: magic=0x%02X slotIdx=%d jiggies=%d notes=%d\n",
               game_slot, phys, slot_data[0], slot_data[1], info.jiggies, info.notes);
        return info;
    }

    return info;
}

// --- Helper: create a full-screen backdrop + centered dialog card ---
static std::pair<recompui::Element*, recompui::Element*> create_dialog_pair(recompui::ContextId& context) {
    // Backdrop — parented to launcher root so it covers the entire viewport
    auto backdrop = context.create_element<recompui::Element>(static_cast<recompui::Element*>(g_launcher_menu));
    backdrop->set_display(recompui::Display::Flex);
    backdrop->set_flex_direction(recompui::FlexDirection::Column);
    backdrop->set_align_items(recompui::AlignItems::Center);
    backdrop->set_justify_content(recompui::JustifyContent::Center);
    backdrop->set_position(recompui::Position::Absolute);
    backdrop->set_left(0.0f);
    backdrop->set_top(0.0f);
    backdrop->set_width(100.0f, recompui::Unit::Percent);
    backdrop->set_height(100.0f, recompui::Unit::Percent);
    backdrop->set_padding_left(25.0f);
    backdrop->set_padding_right(25.0f);
    backdrop->set_background_color(recompui::Color{0, 0, 0, 180});
    backdrop->display_hide();

    // Card
    auto card = context.create_element<recompui::Element>(backdrop);
    card->set_display(recompui::Display::Flex);
    card->set_flex_direction(recompui::FlexDirection::Column);
    card->set_align_items(recompui::AlignItems::FlexStart);
    card->set_gap(12.0f);
    card->set_padding_top(44.0f);
    card->set_padding_bottom(44.0f);
    card->set_padding_left(64.0f);
    card->set_padding_right(64.0f);
    card->set_width(100.0f, recompui::Unit::Percent);
    card->set_max_width(700.0f);
    card->set_background_color(recompui::Color{14, 18, 30, 250});
    card->set_border_radius(16.0f);

    return {backdrop, card};
}

// --- Helper: create a labeled section inside a card (label + content grouped) ---
static recompui::Element* create_section(recompui::ContextId& context, recompui::Element* card, const char* title) {
    auto section = context.create_element<recompui::Element>(card);
    section->set_display(recompui::Display::Flex);
    section->set_flex_direction(recompui::FlexDirection::Column);
    section->set_align_items(recompui::AlignItems::FlexStart);
    section->set_gap(8.0f);
    section->set_width(100.0f, recompui::Unit::Percent);
    section->set_margin_top(12.0f);

    context.create_element<recompui::Label>(section, title, recompui::theme::Typography::LabelMD);
    return section;
}

// --- Refresh slot button labels ---
static std::string make_slot_label(int slot_index);
static recompui::Button* slot_buttons[3] = {};

// Called every frame by the launcher update callback
static void refresh_host_slots() {
    if (!host_slots_dirty) return;
    host_slots_dirty = false;
    for (int k = 0; k < 3; k++) {
        if (slot_buttons[k]) {
            slot_buttons[k]->set_text(make_slot_label(k));
        }
    }
}

// --- Show/hide a panel ---
static void show_panel(recompui::Element* panel) {
    if (panel) panel->display_show();
}
static void hide_panel(recompui::Element* panel) {
    if (panel) panel->display_hide();
}

// --- Erase a save slot on disk (find physical block by slotIndex) ---
static void erase_save_slot(int game_slot) {
    std::filesystem::path save_path = recomp::get_config_path() / u8"saves" / (supported_games[0].game_id + u8".bin");
    std::fstream f(save_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f.good()) return;

    f.seekg(0, std::ios::end);
    auto file_size = f.tellg();

    for (int phys = 0; phys < 4; phys++) {
        int slot_start = phys * SAVE_SLOT_SIZE;
        if (file_size < slot_start + SAVE_SLOT_SIZE) continue;

        uint8_t header[2];
        f.seekg(slot_start);
        f.read(reinterpret_cast<char*>(header), 2);

        if (header[0] == 0x00) continue;
        int stored_slot = header[1] - 1;
        if (stored_slot != game_slot) continue;

        // Found it — zero out the entire physical block
        f.seekp(slot_start);
        uint8_t zeros[SAVE_SLOT_SIZE] = {};
        f.write(reinterpret_cast<char*>(zeros), SAVE_SLOT_SIZE);
        f.flush();
        printf("[Save] Erased game slot %d (physical block %d)\n", game_slot + 1, phys);
        return;
    }
    printf("[Save] Game slot %d not found in save file\n", game_slot + 1);
}

// --- Build a slot label from save data ---
static std::string make_slot_label(int slot_index) {
    SlotInfo info = read_save_slot(slot_index);
    if (info.valid) {
        return "Save " + std::to_string(slot_index + 1) + "  -  " +
               std::to_string(info.jiggies) + " Jiggies / " +
               std::to_string(info.notes) + " Notes";
    }
    return "Save " + std::to_string(slot_index + 1) + "  -  Empty";
}

// ===================== HOST PANEL =====================

static void ensure_host_panel();

static void start_host_game() {
    bknet::set_mode(bknet::NetworkMode::Host);
    bknet::get_config().save_slot = selected_save_slot;
    bknet::NetworkManager::instance().host_game();
    recompui::update_game_mod_id(supported_games[0].mod_game_id);
    recomp::start_game(supported_games[0].game_id, {});
    recompui::hide_all_contexts();
}

static void ensure_host_panel() {
    if (host_panel != nullptr) return;

    auto context = recompui::get_launcher_context_id();
    auto [backdrop, card] = create_dialog_pair(context);
    host_panel = backdrop;

    // --- Title (centered) ---
    auto title_row = context.create_element<recompui::Element>(card);
    title_row->set_display(recompui::Display::Flex);
    title_row->set_justify_content(recompui::JustifyContent::Center);
    title_row->set_width(100.0f, recompui::Unit::Percent);
    title_row->set_margin_bottom(8.0f);
    context.create_element<recompui::Label>(title_row, "Host Game", recompui::theme::Typography::Header2);

    // --- Connection mode section ---
    auto conn_section = create_section(context, card, "Connection Mode");
    auto mode_select = context.create_element<recompui::Select>(
        conn_section,
        std::vector<recompui::SelectOption>{
            {"Direct (LAN)", "lan"},
            {"Online (WAN)", "wan"},
        },
        "lan"
    );
    mode_select->set_width(100.0f, recompui::Unit::Percent);
    mode_select->add_change_callback([](recompui::SelectOption& opt, int) {
        if (opt.value == "wan") {
            printf("[Network] WAN mode selected (not yet implemented, using LAN)\n");
        }
    });
    context.create_element<recompui::Label>(conn_section, "Port: 7777", recompui::theme::Typography::LabelXS);

    // --- Save slot section ---
    auto slots_section = create_section(context, card, "Select Save Slot");

    static recompui::Button* erase_buttons[3] = {};

    for (int i = 0; i < 3; i++) {
        // Row: [slot button] [erase button]
        auto slot_row = context.create_element<recompui::Element>(slots_section);
        slot_row->set_display(recompui::Display::Flex);
        slot_row->set_flex_direction(recompui::FlexDirection::Row);
        slot_row->set_gap(8.0f);
        slot_row->set_width(100.0f, recompui::Unit::Percent);
        slot_row->set_as_navigation_container(recompui::NavigationType::Horizontal);

        slot_buttons[i] = context.create_element<recompui::Button>(
            slot_row, make_slot_label(i),
            recompui::ButtonStyle::Secondary,
            recompui::ButtonSize::Large
        );
        slot_buttons[i]->set_width(100.0f, recompui::Unit::Percent);
        slot_buttons[i]->set_opacity(i == 0 ? 1.0f : 0.5f);

        slot_buttons[i]->add_pressed_callback([i]() {
            selected_save_slot = i;
            for (int j = 0; j < 3; j++) {
                slot_buttons[j]->set_opacity(j == i ? 1.0f : 0.5f);
            }
        });

        erase_buttons[i] = context.create_element<recompui::Button>(
            slot_row, "Erase",
            recompui::ButtonStyle::Danger,
            recompui::ButtonSize::Large
        );
        erase_buttons[i]->set_min_width(120.0f);
        erase_buttons[i]->set_overflow(recompui::Overflow::Visible);

        erase_buttons[i]->add_pressed_callback([i]() {
            recompui::open_choice_prompt(
                "Erase Save " + std::to_string(i + 1),
                "All progress in this slot will be permanently deleted.",
                "Erase", "Cancel",
                [i]() {
                    erase_save_slot(i);
                    host_slots_dirty = true;
                },
                []() {},
                recompui::ButtonStyle::Danger,
                recompui::ButtonStyle::Secondary,
                true
            );
        });
    }

    // --- Action buttons ---
    auto buttons_row = context.create_element<recompui::Element>(card);
    buttons_row->set_display(recompui::Display::Flex);
    buttons_row->set_flex_direction(recompui::FlexDirection::Row);
    buttons_row->set_gap(20.0f);
    buttons_row->set_justify_content(recompui::JustifyContent::Center);
    buttons_row->set_width(100.0f, recompui::Unit::Percent);
    buttons_row->set_margin_top(16.0f);
    buttons_row->set_as_navigation_container(recompui::NavigationType::Horizontal);

    auto back_btn = context.create_element<recompui::Button>(
        buttons_row, "Back", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Large
    );
    back_btn->set_min_width(160.0f);
    back_btn->set_overflow(recompui::Overflow::Visible);
    back_btn->add_pressed_callback([]() { hide_panel(host_panel); });

    auto start_btn = context.create_element<recompui::Button>(
        buttons_row, "Start", recompui::ButtonStyle::Primary, recompui::ButtonSize::Large
    );
    start_btn->set_min_width(160.0f);
    start_btn->set_overflow(recompui::Overflow::Visible);
    start_btn->add_pressed_callback([]() { start_host_game(); });
}

// ===================== JOIN PANEL =====================

// Async join state machine
enum class JoinState : int { Idle = 0, Connecting = 1, Connected = 2, Failed = 3 };
static std::atomic<int> join_state{0};
static std::string join_target_ip;
static std::chrono::steady_clock::time_point join_start_time;

// Join panel UI elements (need per-frame access)
static recompui::Element* join_form = nullptr;      // the form card content
static recompui::Element* join_status_view = nullptr; // connecting/result screen
static recompui::Label* join_status_label = nullptr;
static recompui::Label* join_timer_label = nullptr;
static recompui::Button* join_cancel_btn = nullptr;
static recompui::Button* join_retry_btn = nullptr;
static recompui::TextInput* ip_input = nullptr;

static void ensure_join_panel();

static void join_show_form() {
    if (join_form) join_form->display_show();
    if (join_status_view) join_status_view->display_hide();
}

static void join_show_status() {
    if (join_form) join_form->display_hide();
    if (join_status_view) join_status_view->display_show();
}

static void begin_join(const std::string& ip) {
    join_target_ip = ip.empty() ? "127.0.0.1" : ip;
    join_state.store(static_cast<int>(JoinState::Connecting));
    join_start_time = std::chrono::steady_clock::now();

    if (join_status_label) join_status_label->set_text("Connecting to " + join_target_ip + "...");
    if (join_timer_label) join_timer_label->set_text("0s");
    if (join_cancel_btn) join_cancel_btn->display_show();
    if (join_retry_btn) join_retry_btn->display_hide();
    join_show_status();

    // Spawn async connection thread
    std::thread([]() {
        bknet::set_mode(bknet::NetworkMode::Join);
        bknet::set_join_ip(join_target_ip);
        bool ok = bknet::NetworkManager::instance().join_game();
        int expected = static_cast<int>(JoinState::Connecting);
        if (ok) {
            join_state.compare_exchange_strong(expected, static_cast<int>(JoinState::Connected));
        } else {
            join_state.compare_exchange_strong(expected, static_cast<int>(JoinState::Failed));
        }
    }).detach();
}

// Called every frame from launcher update callback
static void update_join_state() {
    int state = join_state.load();

    if (state == static_cast<int>(JoinState::Connecting)) {
        // Update elapsed timer
        auto elapsed = std::chrono::steady_clock::now() - join_start_time;
        int secs = (int)std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        if (join_timer_label) {
            join_timer_label->set_text(std::to_string(secs) + "s");
        }
    }
    else if (state == static_cast<int>(JoinState::Connected)) {
        join_state.store(static_cast<int>(JoinState::Idle));
        // Start the game
        recompui::update_game_mod_id(supported_games[0].mod_game_id);
        recomp::start_game(supported_games[0].game_id, {});
        recompui::hide_all_contexts();
    }
    else if (state == static_cast<int>(JoinState::Failed)) {
        join_state.store(static_cast<int>(JoinState::Idle));
        // Show failure
        if (join_status_label) join_status_label->set_text("Connection failed");
        if (join_timer_label) join_timer_label->set_text("Host not found or not responding.");
        if (join_cancel_btn) join_cancel_btn->display_hide();
        if (join_retry_btn) join_retry_btn->display_show();
    }
}

static void ensure_join_panel() {
    if (join_panel != nullptr) return;

    auto context = recompui::get_launcher_context_id();
    auto [backdrop, card] = create_dialog_pair(context);
    join_panel = backdrop;

    // ========== FORM VIEW ==========
    join_form = context.create_element<recompui::Element>(card);
    join_form->set_display(recompui::Display::Flex);
    join_form->set_flex_direction(recompui::FlexDirection::Column);
    join_form->set_align_items(recompui::AlignItems::FlexStart);
    join_form->set_gap(12.0f);
    join_form->set_width(100.0f, recompui::Unit::Percent);

    // Title (centered)
    auto title_row = context.create_element<recompui::Element>(join_form);
    title_row->set_display(recompui::Display::Flex);
    title_row->set_justify_content(recompui::JustifyContent::Center);
    title_row->set_width(100.0f, recompui::Unit::Percent);
    title_row->set_margin_bottom(8.0f);
    context.create_element<recompui::Label>(title_row, "Join Game", recompui::theme::Typography::Header2);

    // Connection section
    auto conn_section = create_section(context, join_form, "Host IP Address");
    ip_input = context.create_element<recompui::TextInput>(conn_section);
    ip_input->set_text("127.0.0.1");
    ip_input->set_width(100.0f, recompui::Unit::Percent);
    context.create_element<recompui::Label>(conn_section, "Port: 7777", recompui::theme::Typography::LabelXS);

    // Action buttons
    auto buttons_row = context.create_element<recompui::Element>(join_form);
    buttons_row->set_display(recompui::Display::Flex);
    buttons_row->set_flex_direction(recompui::FlexDirection::Row);
    buttons_row->set_gap(20.0f);
    buttons_row->set_justify_content(recompui::JustifyContent::Center);
    buttons_row->set_width(100.0f, recompui::Unit::Percent);
    buttons_row->set_margin_top(16.0f);
    buttons_row->set_as_navigation_container(recompui::NavigationType::Horizontal);

    auto back_btn = context.create_element<recompui::Button>(
        buttons_row, "Back", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Large
    );
    back_btn->set_min_width(160.0f);
    back_btn->set_overflow(recompui::Overflow::Visible);
    back_btn->add_pressed_callback([]() { hide_panel(join_panel); });

    auto connect_btn = context.create_element<recompui::Button>(
        buttons_row, "Connect", recompui::ButtonStyle::Primary, recompui::ButtonSize::Large
    );
    connect_btn->set_min_width(160.0f);
    connect_btn->set_overflow(recompui::Overflow::Visible);
    connect_btn->add_pressed_callback([]() {
        begin_join(ip_input->get_text());
    });

    // ========== STATUS VIEW (hidden by default) ==========
    join_status_view = context.create_element<recompui::Element>(card);
    join_status_view->set_display(recompui::Display::Flex);
    join_status_view->set_flex_direction(recompui::FlexDirection::Column);
    join_status_view->set_align_items(recompui::AlignItems::Center);
    join_status_view->set_justify_content(recompui::JustifyContent::Center);
    join_status_view->set_gap(20.0f);
    join_status_view->set_width(100.0f, recompui::Unit::Percent);
    join_status_view->set_min_height(200.0f);
    join_status_view->display_hide();

    join_status_label = context.create_element<recompui::Label>(
        join_status_view, "Connecting...", recompui::theme::Typography::Header3
    );

    join_timer_label = context.create_element<recompui::Label>(
        join_status_view, "0s", recompui::theme::Typography::Body
    );

    // Cancel button (shown during connecting)
    join_cancel_btn = context.create_element<recompui::Button>(
        join_status_view, "Cancel", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Large
    );
    join_cancel_btn->set_min_width(160.0f);
    join_cancel_btn->set_overflow(recompui::Overflow::Visible);
    join_cancel_btn->add_pressed_callback([]() {
        // Set state to idle so thread result is ignored
        join_state.store(static_cast<int>(JoinState::Idle));
        join_show_form();
    });

    // Retry/Back button (shown on failure)
    join_retry_btn = context.create_element<recompui::Button>(
        join_status_view, "Back", recompui::ButtonStyle::Secondary, recompui::ButtonSize::Large
    );
    join_retry_btn->set_min_width(160.0f);
    join_retry_btn->set_overflow(recompui::Overflow::Visible);
    join_retry_btn->display_hide();
    join_retry_btn->add_pressed_callback([]() {
        join_show_form();
    });
}

void on_launcher_init(recompui::LauncherMenu *menu) {
    auto game_options_menu = menu->init_game_options_menu(
        supported_games[0].game_id,
        supported_games[0].mod_game_id,
        supported_games[0].display_name,
        supported_games[0].thumbnail_bytes,
        recompui::GameOptionsMenuLayout::Center
    );
    g_game_options_menu = game_options_menu;
    g_launcher_menu = menu;

    // Online menu: Host, Join, Settings, Exit
    game_options_menu->add_start_game_or_load_rom_option("Load ROM", "Host");
    if (auto* host_opt = game_options_menu->get_start_game_option()) {
        host_opt->set_callback([]() {
            ensure_host_panel();
            show_panel(host_panel);
        });
    }

    game_options_menu->add_option("Join", []() {
        ensure_join_panel();
        show_panel(join_panel);
    });
    game_options_menu->add_settings_option();
    game_options_menu->add_exit_option();
    game_options_menu->set_width(30, recompui::Unit::Percent);

    for (auto option : game_options_menu->get_options()) {
        option->set_justify_content(recompui::JustifyContent::FlexEnd);
        option->set_border_radius(0);

        std::vector<recompui::Style *> hover_focus = {&option->hover_style, &option->focus_style};
        for (auto style : hover_focus) {
            style->set_background_color(recompui::theme::color::Transparent);
        }
    }

    recompui::Element *menu_container = menu->get_menu_container();
    menu_container->set_width(1440);
    menu_container->unset_left();
    menu_container->set_top(banjo::launcher_options_top_offset);
    menu_container->set_bottom(-banjo::launcher_options_top_offset);
    menu_container->set_right(50, recompui::Unit::Percent);
    menu_container->set_translate_2D(50.0f, 0.0f, recompui::Unit::Percent);

    game_options_menu->unset_left();
    game_options_menu->set_bottom(50.0f, recompui::Unit::Percent);
    game_options_menu->set_translate_2D(0.0f, 50.0f, recompui::Unit::Percent);
    game_options_menu->set_right(banjo::launcher_options_right_position_start);

    menu->remove_default_title();

    banjo::launcher_animation_setup(menu);
}

#define REGISTER_FUNC(name) recomp::overlays::register_base_export(#name, name)

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    recomp::Version project_version{};
    if (!recomp::Version::from_string(version_string, project_version)) {
        ultramodern::error_handling::message_box(("Invalid version string: " + version_string).c_str());
        return EXIT_FAILURE;
    }

    // Map this executable into memory and lock it, which should keep it in physical memory. This ensures
    // that there are no stutters from the OS having to load new pages of the executable whenever a new code page is run.
    PreloadContext preload_context;
    bool preloaded = preload_executable(preload_context);

    if (!preloaded) {
        fprintf(stderr, "Failed to preload executable!\n");
    }

    // Initialize random seed for icon easter egg.
    std::srand(std::time(nullptr));

#ifdef _WIN32
    // Set up high resolution timing period.
    timeBeginPeriod(1);

    // Process arguments.
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--show-console") == 0)
        {
            if (GetConsoleWindow() == nullptr)
            {
                AllocConsole();
                freopen("CONIN$", "r", stdin);
                freopen("CONOUT$", "w", stderr);
                freopen("CONOUT$", "w", stdout);
            }

            break;
        }
    }

    // Set up console output to accept UTF-8 on windows
    SetConsoleOutputCP(CP_UTF8);

    // Change to a font that supports Japanese characters
    CONSOLE_FONT_INFOEX cfi;
    cfi.cbSize = sizeof cfi;
    cfi.nFont = 0;
    cfi.dwFontSize.X = 0;
    cfi.dwFontSize.Y = 16;
    cfi.FontFamily = FF_DONTCARE;
    cfi.FontWeight = FW_NORMAL;
    wcscpy_s(cfi.FaceName, L"NSimSun");
    SetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &cfi);
#endif

#ifdef _WIN32
    // Force wasapi on Windows, as there seems to be some issue with sample queueing with directsound currently.
    SDL_setenv("SDL_AUDIODRIVER", "wasapi", true);
#endif

#if defined(__linux__) && defined(RECOMP_FLATPAK)
    // When using Flatpak, applications tend to launch from the home directory by default.
    // Mods might use the current working directory to store the data, so we switch it to a directory
    // with persistent data storage and write permissions under Flatpak to ensure it works.
    std::error_code ec;
    std::filesystem::current_path("/var/data", ec);
#endif

    // Initialize native file dialogs.
    NFD_Init();

    // Initialize program settings.
    recompui::programconfig::set_program_name(banjo::program_name);
    recompui::programconfig::set_program_id(banjo::program_id);
    
    // Initialize SDL audio and set the output frequency.
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!reset_audio(48000)) {
        // It is not possible to initialize without an audio device.
        return EXIT_FAILURE;
    }

    // Source controller mappings file
    std::u8string controller_db_path = (recompui::file::get_program_path() / "recompcontrollerdb.txt").u8string();
    if (SDL_GameControllerAddMappingsFromFile(reinterpret_cast<const char *>(controller_db_path.c_str())) < 0) {
        fprintf(stderr, "Failed to load controller mappings: %s\n", SDL_GetError());
    }

    // Register fonts.
    recompui::register_primary_font("InterVariable.ttf", "Inter Variable");
    recompui::register_extra_font("Suplexmentary Comic NC.ttf");

    // Register configuration path.
    recomp::register_config_path(recompui::file::get_app_folder_path());

    // Register supported games and patches
    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }

    recomp::mods::register_deprecated_mod("bk_recomp_mod_fov_slider", recomp::mods::DeprecationStatus::BrokenVersion, recomp::Version(1, 1, 0));

    REGISTER_FUNC(recomp_get_window_resolution);
    REGISTER_FUNC(recomp_get_target_aspect_ratio);
    REGISTER_FUNC(recomp_get_target_framerate);
    REGISTER_FUNC(recomp_get_cutscene_aspect_ratio);
    REGISTER_FUNC(recomp_get_analog_cam_enabled);
    REGISTER_FUNC(recomp_get_right_analog_inputs);
    REGISTER_FUNC(recomp_get_bgm_volume);
    // REGISTER_FUNC(recomp_get_gyro_deltas);
    // REGISTER_FUNC(recomp_get_mouse_deltas);
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
    recompui::register_ui_exports();
    recomputil::register_data_api_exports();
    recomptheme::set_custom_theme();

    banjo::register_bk_overlays();
    banjo::register_bk_patches();

    // Register extensions for two types: Props and ActorMarkers.
    recomputil::init_extended_object_data(2);

    // Initialize networking (ENet)
    bknet::NetworkManager::instance().initialize();

    banjo::init_config();

    // Network mode is now set from the launcher UI (Host/Join buttons).
    // The mode is configured when the user clicks Host or Join, and the
    // game starts with host_game()/join_game() called at that point.
    recompinput::players::set_single_player_mode(true);

    // Initialize chat input system (SDL event watcher)
    bknet::ChatInput::instance().init();

    recompui::register_launcher_init_callback(on_launcher_init);
    recompui::register_launcher_update_callback([](recompui::LauncherMenu* menu) {
        refresh_host_slots();
        update_join_state();
        banjo::launcher_animation_update(menu);
    });

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = get_rsp_microcode,
    };

    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = [](uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
            auto presentation_mode = ultramodern::renderer::PresentationMode::PresentEarly;
            return recompui::renderer::create_render_context(rdram, window_handle, presentation_mode, developer_mode);
        },
    };

    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = create_gfx,
        .create_window = create_window,
        .update_gfx = update_gfx,
    };

    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = queue_samples,
        .get_frames_remaining = get_frames_remaining,
        .set_frequency = set_frequency,
    };

    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = recompinput::poll_inputs,
        .get_input = recompinput::profiles::get_n64_input,
        .set_rumble = recompinput::set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };

    ultramodern::events::callbacks_t thread_callbacks{
        .vi_callback = recompinput::update_rumble,
        .gfx_init_callback = nullptr,
    };

    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = recompui::message_box,
    };

    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = banjo::get_game_thread_name,
    };

    // Register the texture pack content type with rt64.json as its content file.
    recomp::mods::ModContentType texture_pack_content_type{
        .content_filename = "rt64.json",
        .allow_runtime_toggle = true,
        .on_enabled = enable_texture_pack,
        .on_disabled = disable_texture_pack,
        .on_reordered = reorder_texture_pack,
    };
    auto texture_pack_content_type_id = recomp::mods::register_mod_content_type(texture_pack_content_type);

    // Register the .rtz texture pack file format with the previous content type as its only allowed content type.
    recomp::mods::register_mod_container_type("rtz", std::vector{ texture_pack_content_type_id }, false);

    recomp::start(
        project_version,
        {},
        rsp_callbacks,
        renderer_callbacks,
        audio_callbacks,
        input_callbacks,
        gfx_callbacks,
        thread_callbacks,
        error_handling_callbacks,
        threads_callbacks
    );

    bknet::NetworkManager::instance().shutdown();

    NFD_Quit();

    if (preloaded) {
        release_preload(preload_context);
    }

#ifdef _WIN32
    // End high resolution timing period.
    timeEndPeriod(1);
#endif

    return EXIT_SUCCESS;
}

#include "net_nametag_ui.h"
#include "net_manager.h"
#include "net_interpolation.h"

#include "recompui/recompui.h"
#include "core/ui_context.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "elements/ui_types.h"

#include <cmath>
#include <string>
#include <cstdio>
#include <mutex>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace bknet {

using namespace recompui;

// ---- Camera state (thread-safe, written by game thread) ----

static std::mutex camera_mutex;
static CameraState s_camera_state;

void nametag_set_camera_state(const CameraState& state) {
    std::lock_guard<std::mutex> lock(camera_mutex);
    s_camera_state = state;
}

CameraState nametag_get_camera_state() {
    std::lock_guard<std::mutex> lock(camera_mutex);
    return s_camera_state;
}

// ---- 3D -> 2D projection (replicates viewport_func_8024E030) ----

static void vec3f_yaw_rotate(float out[3], const float in[3], float yaw_deg) {
    float rad = yaw_deg * (float)(M_PI / 180.0);
    float s = sinf(rad);
    float c = cosf(rad);
    out[0] = in[0] * c + in[2] * s;
    out[1] = in[1];
    out[2] = -in[0] * s + in[2] * c;
}

static void vec3f_pitch_rotate(float out[3], const float in[3], float pitch_deg) {
    float rad = pitch_deg * (float)(M_PI / 180.0);
    float s = sinf(rad);
    float c = cosf(rad);
    out[0] = in[0];
    out[1] = in[1] * c - in[2] * s;
    out[2] = in[1] * s + in[2] * c;
}

static bool project_world_to_screen(const float pos[3], const CameraState& cam,
                                     float* out_norm_x, float* out_norm_y) {
    float delta[3] = {
        pos[0] - cam.position[0],
        pos[1] - cam.position[1],
        pos[2] - cam.position[2]
    };

    float temp[3];
    vec3f_yaw_rotate(temp, delta, -cam.rotation[1]);
    vec3f_pitch_rotate(delta, temp, -cam.rotation[0]);

    if (-cam.near_plane <= delta[2]) return false;

    float fovy_radians = cam.fov_y * (float)M_PI / 360.0f;

    // Use the real viewport aspect ratio (accounts for widescreen)
    float aspect = cam.viewport_aspect;
    if (aspect <= 0.0f) aspect = 16.0f / 9.0f;

    float temp_f2 = sqrtf(delta[1] * delta[1] + delta[2] * delta[2]) * sinf(fovy_radians);
    if (temp_f2 == 0.0f) return false;

    float temp_f2_2 = aspect * temp_f2;
    if (temp_f2_2 == 0.0f) return false;

    // Project to normalized screen coords (0..1)
    *out_norm_x = (delta[0] / temp_f2_2 + 1.0f) * 0.5f;
    *out_norm_y = (1.0f - delta[1] / temp_f2) * 0.5f;

    if (*out_norm_x < -0.5f || *out_norm_x > 1.5f) return false;
    if (*out_norm_y < -0.5f || *out_norm_y > 1.5f) return false;

    return true;
}

// ---- Nametag UI ----

static constexpr float LABEL_HEIGHT_OFFSET = 150.0f; // world units above ghost feet
static constexpr float LABEL_HALF_WIDTH_DP = 80.0f;  // half the container width for centering

static ContextId nametag_context;
static bool nametag_initialized = false;
static bool nametag_enabled = true;

static const Color player_colors[] = {
    {255, 215, 0,   255},  // Gold (host/P1)
    {92,  184, 255, 255},  // Sky blue (P2)
    {255, 107, 107, 255},  // Coral red (P3)
    {107, 255, 135, 255},  // Mint green (P4)
};

struct NametagRow {
    Element* container = nullptr;
    Label* name_label = nullptr;
    // Smoothed screen position (lerped each frame)
    float smooth_x = 0.0f;  // percent 0..100
    float smooth_y = 0.0f;
    bool has_prev = false;
};
static NametagRow nametag_rows[MAX_PLAYERS];

static constexpr float SMOOTH_SPEED = 0.35f; // 0=frozen, 1=instant

void nametag_ui_init() {
    nametag_initialized = false;
    nametag_enabled = true;
}

static void ensure_init() {
    if (nametag_initialized) return;
    nametag_initialized = true;

    nametag_context = create_context();
    nametag_context.set_captures_input(false);
    nametag_context.set_captures_mouse(false);
    nametag_context.open();

    for (int i = 0; i < MAX_PLAYERS; i++) {
        auto& nr = nametag_rows[i];

        // Container: uses percent positioning + negative margin trick to center
        nr.container = nametag_context.create_element<Element>(nametag_context.get_root_element());
        nr.container->set_position(Position::Absolute);
        nr.container->set_display(Display::Flex);
        nr.container->set_flex_direction(FlexDirection::Column);
        nr.container->set_align_items(AlignItems::Center);
        nr.container->set_width(LABEL_HALF_WIDTH_DP * 2.0f, Unit::Dp);
        // Negative margin-left = half width → centers on the percent anchor point
        nr.container->set_margin_left(-LABEL_HALF_WIDTH_DP, Unit::Dp);
        nr.container->display_hide();

        // Inner pill with background
        Element* pill = nametag_context.create_element<Element>(nr.container);
        pill->set_display(Display::InlineBlock);
        pill->set_background_color(Color{10, 14, 28, 180});
        pill->set_border_radius(8, Unit::Dp);
        pill->set_padding_top(2, Unit::Dp);
        pill->set_padding_bottom(2, Unit::Dp);
        pill->set_padding_left(8, Unit::Dp);
        pill->set_padding_right(8, Unit::Dp);

        // Player name
        nr.name_label = nametag_context.create_element<Label>(pill, "", theme::Typography::LabelXS);
        nr.name_label->set_color(player_colors[i]);
        nr.name_label->set_font_weight(600);
    }

    nametag_context.close();
    std::printf("[Nametag] UI initialized\n");
}

void nametag_ui_set_enabled(bool enabled) {
    nametag_enabled = enabled;
}

bool nametag_ui_is_enabled() {
    return nametag_enabled;
}

void nametag_ui_update() {
    auto& net = NetworkManager::instance();

    if (!net.is_connected() || !nametag_enabled) {
        if (nametag_initialized && is_context_shown(nametag_context)) {
            hide_context(nametag_context);
        }
        return;
    }

    ensure_init();

    if (!is_context_shown(nametag_context)) {
        show_context(nametag_context, "");
    }

    CameraState cam = nametag_get_camera_state();
    uint8_t local_id = net.local_player_id();
    uint32_t local_map = cam.map_id;

    nametag_context.open();

    for (int i = 0; i < MAX_PLAYERS; i++) {
        auto& nr = nametag_rows[i];

        if (i == (int)local_id) {
            nr.container->display_hide();
            continue;
        }

        auto info = net.get_player_info((uint8_t)i);
        auto state = net.get_remote_player((uint8_t)i);

        if (!state.active || !cam.valid) {
            nr.container->display_hide();
            nr.has_prev = false;
            continue;
        }

        if (state.map_id != local_map) {
            nr.container->display_hide();
            nr.has_prev = false;
            continue;
        }

        // Project point above ghost head to screen
        float world_pos[3] = { state.x, state.y + LABEL_HEIGHT_OFFSET, state.z };
        float norm_x, norm_y;

        if (!project_world_to_screen(world_pos, cam, &norm_x, &norm_y)) {
            nr.container->display_hide();
            continue;
        }

        // Skip if off visible screen edges
        if (norm_x < 0.02f || norm_x > 0.98f || norm_y < 0.02f || norm_y > 0.98f) {
            nr.container->display_hide();
            continue;
        }

        // Target position in percent
        float target_x = norm_x * 100.0f;
        float target_y = norm_y * 100.0f;

        // Smooth interpolation to reduce jitter from RT64 frame mismatch
        if (nr.has_prev) {
            nr.smooth_x += (target_x - nr.smooth_x) * SMOOTH_SPEED;
            nr.smooth_y += (target_y - nr.smooth_y) * SMOOTH_SPEED;
        } else {
            nr.smooth_x = target_x;
            nr.smooth_y = target_y;
            nr.has_prev = true;
        }

        nr.container->set_left(nr.smooth_x, Unit::Percent);
        nr.container->set_top(nr.smooth_y, Unit::Percent);

        // Update name
        std::string name = (!info.name.empty()) ? info.name
            : ("Player " + std::to_string(i));
        nr.name_label->set_text(name);

        nr.container->display_show();
    }

    nametag_context.close();
}

} // namespace bknet

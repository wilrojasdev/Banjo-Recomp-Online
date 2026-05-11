// Android touch input — Phase 6 implementation.
//
// State machine: an array of TouchButton (radial hit zones, sticky one-pointer
// each) + a single VirtualStick. process_motion_event() updates the state from
// raw AInputEvent data; ultramodern::input callbacks read the state each frame
// when the recompiled MIPS asks for controller input.
//
// Render side is deferred to Phase 7/8 (will draw via imgui or RT64 overlay).

#include "android_touch.h"

#ifdef __ANDROID__

#include <android/input.h>
#include <array>
#include <cmath>
#include <cstring>
#include <jni.h>
#include <mutex>

#include "imgui/imgui.h"

namespace banjo_android::touch {

// N64 controller button bits (PR/os_cont.h).
constexpr uint16_t BTN_A       = 0x8000;
constexpr uint16_t BTN_B       = 0x4000;
constexpr uint16_t BTN_Z       = 0x2000;
constexpr uint16_t BTN_START   = 0x1000;
constexpr uint16_t BTN_UP      = 0x0800;
constexpr uint16_t BTN_DOWN    = 0x0400;
constexpr uint16_t BTN_LEFT    = 0x0200;
constexpr uint16_t BTN_RIGHT   = 0x0100;
constexpr uint16_t BTN_L       = 0x0020;
constexpr uint16_t BTN_R       = 0x0010;
constexpr uint16_t BTN_C_UP    = 0x0008;
constexpr uint16_t BTN_C_DOWN  = 0x0004;
constexpr uint16_t BTN_C_LEFT  = 0x0002;
constexpr uint16_t BTN_C_RIGHT = 0x0001;

struct TouchButton {
    float x_norm, y_norm;
    float radius_norm;
    uint16_t mask;
    int32_t active_pointer_id;  // -1 if not pressed
};

struct VirtualStick {
    float center_x_norm, center_y_norm;
    float radius_norm;
    int32_t active_pointer_id;  // -1 if not held
};

// All coordinates normalized 0..1 (origin top-left, +y down).
// Default layout targets a 16:9 phone in landscape.
static VirtualStick g_stick = { 0.18f, 0.78f, 0.12f, -1 };

static std::array<TouchButton, 12> g_buttons = {{
    { 0.88f, 0.78f, 0.06f, BTN_A,        -1 },
    { 0.78f, 0.86f, 0.05f, BTN_B,        -1 },
    { 0.93f, 0.50f, 0.04f, BTN_Z,        -1 },
    { 0.50f, 0.95f, 0.04f, BTN_START,    -1 },
    { 0.10f, 0.10f, 0.04f, BTN_L,        -1 },
    { 0.90f, 0.10f, 0.04f, BTN_R,        -1 },
    { 0.70f, 0.32f, 0.035f, BTN_C_UP,    -1 },
    { 0.70f, 0.46f, 0.035f, BTN_C_DOWN,  -1 },
    { 0.62f, 0.39f, 0.035f, BTN_C_LEFT,  -1 },
    { 0.78f, 0.39f, 0.035f, BTN_C_RIGHT, -1 },
    { 0.42f, 0.10f, 0.035f, BTN_UP,      -1 },  // D-pad simplified
    { 0.42f, 0.18f, 0.035f, BTN_DOWN,    -1 },
}};

static std::mutex g_mutex;
static int g_viewport_w = 1920;
static int g_viewport_h = 1080;
static uint16_t g_btn_state = 0;
static float g_stick_x = 0.0f;
static float g_stick_y = 0.0f;

void set_viewport(int width, int height) {
    std::lock_guard lock{g_mutex};
    g_viewport_w = width > 0 ? width : 1;
    g_viewport_h = height > 0 ? height : 1;
}

void get_viewport(int& width, int& height) {
    std::lock_guard lock{g_mutex};
    width = g_viewport_w;
    height = g_viewport_h;
}

static bool point_in_circle(float px, float py, float cx, float cy, float r) {
    float dx = px - cx;
    float dy = py - cy;
    return (dx*dx + dy*dy) <= (r * r);
}

// Internal helper: must be called with g_mutex held.
static void apply_pointer(int32_t pointer_id, float x_norm, float y_norm, bool active) {
    if (active) {
        // Stick takes priority if its area is touched and free.
        if (g_stick.active_pointer_id == -1 &&
            point_in_circle(x_norm, y_norm, g_stick.center_x_norm, g_stick.center_y_norm, g_stick.radius_norm)) {
            g_stick.active_pointer_id = pointer_id;
        }

        if (g_stick.active_pointer_id == pointer_id) {
            float dx = x_norm - g_stick.center_x_norm;
            float dy = y_norm - g_stick.center_y_norm;
            float r = g_stick.radius_norm;
            float dist = std::sqrt(dx*dx + dy*dy);
            if (dist > r && r > 0.0f) {
                dx *= r / dist;
                dy *= r / dist;
            }
            // N64 stick: y is "up positive"; Android screen-y grows down.
            g_stick_x = dx / r;
            g_stick_y = -dy / r;
        }

        // Buttons (multiple pointers can hold different buttons concurrently).
        for (auto& btn : g_buttons) {
            if (btn.active_pointer_id == -1 &&
                point_in_circle(x_norm, y_norm, btn.x_norm, btn.y_norm, btn.radius_norm)) {
                btn.active_pointer_id = pointer_id;
                g_btn_state = static_cast<uint16_t>(g_btn_state | btn.mask);
            }
        }
    } else {
        if (g_stick.active_pointer_id == pointer_id) {
            g_stick.active_pointer_id = -1;
            g_stick_x = 0.0f;
            g_stick_y = 0.0f;
        }
        for (auto& btn : g_buttons) {
            if (btn.active_pointer_id == pointer_id) {
                btn.active_pointer_id = -1;
                g_btn_state = static_cast<uint16_t>(g_btn_state & ~btn.mask);
            }
        }
    }
}

int32_t process_motion_event(AInputEvent* event) {
    if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) {
        return 0;
    }

    int32_t action = AMotionEvent_getAction(event);
    int32_t action_masked = action & AMOTION_EVENT_ACTION_MASK;
    int32_t pointer_index = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)
        >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;

    std::lock_guard lock{g_mutex};
    float w = static_cast<float>(g_viewport_w);
    float h = static_cast<float>(g_viewport_h);

    auto pointer_norm = [&](int32_t idx, float& x, float& y) {
        x = AMotionEvent_getX(event, idx) / w;
        y = AMotionEvent_getY(event, idx) / h;
    };

    switch (action_masked) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN: {
        float x, y;
        pointer_norm(pointer_index, x, y);
        apply_pointer(AMotionEvent_getPointerId(event, pointer_index), x, y, true);
        return 1;
    }
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP: {
        float x, y;
        pointer_norm(pointer_index, x, y);
        apply_pointer(AMotionEvent_getPointerId(event, pointer_index), x, y, false);
        return 1;
    }
    case AMOTION_EVENT_ACTION_MOVE: {
        // For move events, re-apply every active pointer at its new position
        // so the stick tracks continuously and buttons stay held.
        int32_t count = AMotionEvent_getPointerCount(event);
        for (int32_t i = 0; i < count; ++i) {
            float x, y;
            pointer_norm(i, x, y);
            apply_pointer(AMotionEvent_getPointerId(event, i), x, y, true);
        }
        return 1;
    }
    case AMOTION_EVENT_ACTION_CANCEL: {
        // Force-release every held button and the stick.
        for (auto& btn : g_buttons) {
            if (btn.active_pointer_id != -1) {
                g_btn_state = static_cast<uint16_t>(g_btn_state & ~btn.mask);
                btn.active_pointer_id = -1;
            }
        }
        g_stick.active_pointer_id = -1;
        g_stick_x = 0.0f;
        g_stick_y = 0.0f;
        return 1;
    }
    default:
        return 0;
    }
}

static void poll_input() {
    // No-op on Android: state is updated synchronously from process_motion_event.
}

static bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) return false;
    std::lock_guard lock{g_mutex};
    *buttons = g_btn_state;
    *x = g_stick_x;
    *y = g_stick_y;
    return true;
}

static void set_rumble(int /*controller_num*/, bool /*on*/) {
    // Phase 7: implement via AVibrator / VIBRATOR_MANAGER_SERVICE.
}

static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num == 0) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::RumblePak };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

ultramodern::input::callbacks_t make_input_callbacks() {
    return ultramodern::input::callbacks_t{
        .poll_input = poll_input,
        .get_input = get_input,
        .set_rumble = set_rumble,
        .get_connected_device_info = get_connected_device_info,
    };
}

void render_overlay() {
    // Snapshot state under lock, then render outside the lock to keep
    // contention with the input thread minimal.
    int viewport_w, viewport_h;
    uint16_t btn_state;
    float stick_x, stick_y;
    bool stick_held;
    {
        std::lock_guard lock{g_mutex};
        viewport_w = g_viewport_w;
        viewport_h = g_viewport_h;
        btn_state = g_btn_state;
        stick_x = g_stick_x;
        stick_y = g_stick_y;
        stick_held = g_stick.active_pointer_id != -1;
    }

    // Full-screen, click-through overlay window.
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(viewport_w), static_cast<float>(viewport_h)));
    if (!ImGui::Begin("BanjoTouchOverlay", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float fw = static_cast<float>(viewport_w);
    const float fh = static_cast<float>(viewport_h);

    // Colors: 50% alpha for default, brighter for pressed.
    const ImU32 col_button_idle    = IM_COL32(220, 220, 220, 90);
    const ImU32 col_button_pressed = IM_COL32(255, 220,  90, 200);
    const ImU32 col_button_outline = IM_COL32(  0,   0,   0, 180);
    const ImU32 col_stick_ring     = IM_COL32(220, 220, 220, 90);
    const ImU32 col_stick_thumb    = IM_COL32(255, 220,  90, 200);

    // Buttons.
    for (const auto& btn : g_buttons) {
        const ImVec2 center{ btn.x_norm * fw, btn.y_norm * fh };
        const float radius_px = btn.radius_norm * fh;
        const bool pressed = (btn_state & btn.mask) != 0;
        const ImU32 fill = pressed ? col_button_pressed : col_button_idle;
        dl->AddCircleFilled(center, radius_px, fill, 24);
        dl->AddCircle(center, radius_px, col_button_outline, 24, 2.0f);
    }

    // Virtual stick: outer ring (deadzone visualization) + draggable thumb.
    {
        const ImVec2 center{ g_stick.center_x_norm * fw, g_stick.center_y_norm * fh };
        const float ring_radius_px = g_stick.radius_norm * fh;
        dl->AddCircle(center, ring_radius_px, col_stick_ring, 32, 2.5f);

        // Thumb position offset by current stick state (flip y back to screen-space).
        const ImVec2 thumb{
            center.x + stick_x * ring_radius_px,
            center.y - stick_y * ring_radius_px
        };
        const float thumb_radius_px = ring_radius_px * 0.4f;
        dl->AddCircleFilled(thumb, thumb_radius_px,
                            stick_held ? col_stick_thumb : col_button_idle, 24);
        dl->AddCircle(thumb, thumb_radius_px, col_button_outline, 24, 2.0f);
    }

    ImGui::End();
}

}  // namespace banjo_android::touch

// ---------------------------------------------------------------------------
// recompinput stubs.
//
// The recompinput .cpp tree (lib/RecompFrontend/recompinput/src/) isn't
// cross-compiled for Android (skipped along with RecompFrontend in Phase 5).
// main.cpp still references three recompinput symbols at desktop-only call
// sites. Stub them so the .so links cleanly. The Android input flow goes
// through banjo_android::touch::make_input_callbacks() instead, registered
// as the ultramodern::input::callbacks_t.

namespace recompinput {
    void handle_events() {
        // SDL event polling happens only in the desktop build.
    }
    namespace players {
        bool is_single_player_mode() {
            // Android port is single-player-on-device by design (multiplayer
            // is the network ghost layer, not couch co-op).
            return true;
        }
        bool get_player_is_assigned(int player_index, bool /*temp_player*/) {
            return player_index == 0;
        }
    }
}

namespace banjo_android::touch {

// Phase 9 smoke test path. The Java overlay (MainActivity.java) calls this
// via JNI to OR/AND a button bit into g_btn_state without needing a touch
// hit-test. Used to validate the input → recomp pipeline independently of
// the Vulkan render (the white-frame bug means the imgui overlay isn't
// visible right now).
void debug_set_button(uint16_t mask, bool pressed) {
    std::lock_guard lock{g_mutex};
    if (pressed) {
        g_btn_state |= mask;
    } else {
        g_btn_state &= ~mask;
    }
}

}  // namespace banjo_android::touch

// JNI bridge for MainActivity.java's `private static native void
// nativeSetButton(int mask, boolean pressed)`. Symbol name follows the
// JNI mangling rules for class `com.banjorecomp.online.MainActivity`.
extern "C" JNIEXPORT void JNICALL
Java_com_banjorecomp_online_MainActivity_nativeSetButton(
    JNIEnv* /*env*/, jclass /*clazz*/, jint mask, jboolean pressed) {
    banjo_android::touch::debug_set_button(static_cast<uint16_t>(mask), pressed == JNI_TRUE);
}

#endif  // __ANDROID__

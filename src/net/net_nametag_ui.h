#ifndef NET_NAMETAG_UI_H
#define NET_NAMETAG_UI_H

#include <cstdint>
#include <mutex>

namespace bknet {

// Camera state pushed from game thread for 3D->2D projection
struct CameraState {
    float position[3] = {};
    float rotation[3] = {};   // pitch, yaw, roll
    float fov_y = 40.0f;
    float near_plane = 100.0f;
    int framebuffer_width = 292;
    int framebuffer_height = 216;
    float viewport_aspect = 1.777f; // actual rendered aspect ratio (16:9 = 1.777)
    uint32_t map_id = 0;
    bool valid = false;
};

// Called from recomp API (game thread) to update camera state
void nametag_set_camera_state(const CameraState& state);

// Called from recomp API (game thread) to get camera state
CameraState nametag_get_camera_state();

// Initialize the nametag overlay (call once after recompui is ready)
void nametag_ui_init();

// Update the nametag overlay each frame (call from update_gfx)
void nametag_ui_update();

// Toggle nametags on/off
void nametag_ui_set_enabled(bool enabled);
bool nametag_ui_is_enabled();

} // namespace bknet

#endif // NET_NAMETAG_UI_H

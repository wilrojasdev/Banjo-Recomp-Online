// Android render context — Phase 9.
//
// Standalone equivalent of recompui::renderer::RT64Context for the Android
// build, where RecompFrontend isn't cross-compiled. This is the minimum
// surface area needed to drive RT64 with an ANativeWindow*: no launcher UI,
// no texture pack hot-swap, no graphics-config polling.

#pragma once

#ifdef __ANDROID__

#include <memory>
#include "ultramodern/renderer_context.hpp"

namespace banjo_android::renderer {

std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
    uint8_t* rdram,
    ultramodern::renderer::WindowHandle window_handle,
    bool developer_mode);

}  // namespace banjo_android::renderer

// JNI / present hook used by android_run_game.cpp and recompui::RT64Context.
extern "C" {
void banjo_android_notify_rom_gate(bool rom_present);
void banjo_android_mark_expecting_first_game_frame();
void banjo_android_cancel_expecting_first_game_frame();
}

#endif  // __ANDROID__

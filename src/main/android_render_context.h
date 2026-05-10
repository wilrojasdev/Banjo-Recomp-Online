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

#endif  // __ANDROID__

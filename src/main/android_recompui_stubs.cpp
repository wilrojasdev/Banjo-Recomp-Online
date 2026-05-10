// Android no-op replacements for RecompFrontend recompui exports that the
// patches still call by symbol name.
//
// On desktop these come from lib/RecompFrontend/recompui, which depends on
// SDL2 + RmlUi + Freetype — none of which are linked into the Android .so
// (Phase 5 deliberately skipped them). The patches still emit calls into a
// few of these symbols on every-frame paths (e.g. recomp_run_ui_callbacks
// from baMotor_80250C08), so we provide the minimum-viable no-ops here and
// let the regen_stubs.sh filter preserve them.
//
// If a recompui symbol turns out to *do* something the gameplay path
// depends on, lift the desktop implementation in here verbatim instead of
// no-op-ing it.

#ifdef __ANDROID__

#include "android_touch.h"

// We intentionally do not include recomp.h / recomp_context.h here — the
// signature only matters to the recompiled patches that call us, and they
// pass arguments via x0/x1 regardless of what we declare. An empty body is
// a safe ABI-compatible no-op.

extern "C" void recomp_run_ui_callbacks(unsigned char* /*rdram*/, void* /*ctx*/) {
    // No UI mods on Android yet — the queued_callbacks queue is always empty.
}

// Definition for recompui::get_window_size. The desktop implementation reads
// from the SDL window (which we don't have on Android); we serve the cached
// ANativeWindow dimensions that android_main publishes via set_viewport.
namespace recompui {
    void get_window_size(int& width, int& height) {
        banjo_android::touch::get_viewport(width, height);
    }
}

#endif  // __ANDROID__

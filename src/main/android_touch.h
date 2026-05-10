// Android touch input layer for BK64-Online.
//
// Phase 6 deliverable: a virtual on-screen gamepad that maps touch events to
// the existing ultramodern::input callback pipeline. The wiring to the real
// ANativeActivity input loop happens in Phase 8 — for now this module is
// self-contained and links cleanly into libBanjoRecompiled.so.

#pragma once

#ifdef __ANDROID__

#include "ultramodern/input.hpp"

struct AInputEvent;

namespace banjo_android::touch {

// Tell the touch layer the current surface dimensions. Layout is defined in
// normalized 0..1 coordinates; this conversion happens at hit-test time.
void set_viewport(int width, int height);

// Read back the cached surface dimensions. Used by code paths that need
// the window size (e.g. recompui::get_window_size) but don't have a direct
// reference to the ANativeWindow.
void get_viewport(int& width, int& height);

// Dispatch an Android NativeActivity motion event into the touch state
// machine. Returns 1 if the event was consumed, 0 otherwise.
// Safe to call from the input thread; the implementation locks internally.
int32_t process_motion_event(AInputEvent* event);

// Build a ready-to-register ultramodern input callbacks struct that drives
// the recompiled MIPS code from on-screen touch state.
ultramodern::input::callbacks_t make_input_callbacks();

// Draw the on-screen virtual gamepad as a transparent imgui overlay.
// Call inside an active imgui frame (between NewFrame() and Render()).
// Phase 8 will invoke this from the render loop after the game's main HUD.
// Pressed buttons are drawn brighter; the analog stick draws a ring + thumb.
void render_overlay();

}  // namespace banjo_android::touch

#endif  // __ANDROID__

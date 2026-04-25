#ifndef __BANJO_LAUNCHER_H__
#define __BANJO_LAUNCHER_H__

#include "recompui/recompui.h"

namespace banjo {
    void launcher_animation_setup(recompui::LauncherMenu *menu);
    void launcher_animation_update(recompui::LauncherMenu *menu);

    // Request a relaunch of the app from any thread / UI callback. Sets an atomic
    // flag that the gfx tick consumes via process_return_to_launcher() — disconnect,
    // spawn a fresh process, quit.
    void request_return_to_launcher();

    constexpr float launcher_options_right_position_start = 96.0f;
    constexpr float launcher_options_right_position_end = 96.0f + 24.0f;
    constexpr float launcher_options_top_offset = 96.0f;
    constexpr float launcher_options_title_offset = 120.0f;
}

#endif

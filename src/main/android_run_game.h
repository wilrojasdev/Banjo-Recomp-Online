// Android game runner — Phase 9 entry point.
//
// android_main() spawns this on a dedicated thread once the NativeActivity
// hands us an ANativeWindow. It mirrors the bare-minimum subset of int main()
// needed to drive recomp::start() into rt64 — no launcher UI, no SDL, no
// language packs, no chat overlay. Networking, settings UI, and locale are
// deferred to follow-up passes.

#pragma once

#ifdef __ANDROID__

#include <android/native_window.h>

namespace banjo_android {

// Per-activity paths NativeActivity hands us in ANativeActivity. Owned by
// android_main; android_run_game just borrows them.
struct AppPaths {
    const char* internal_data_path;  // /data/data/<pkg>/files (writable)
    const char* external_data_path;  // /storage/emulated/0/Android/data/<pkg>/files (writable, user-visible)
};

void run_game(ANativeWindow* window, AppPaths paths);

}  // namespace banjo_android

#endif

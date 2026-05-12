# BK64-Online — Android Port

**Status**: Phases 1-13 — **DONE** ✅ (Mali Valhall renders BK64 title; RmlUi launcher navigable on emulator)
**Last updated**: 2026-05-11
**Working branch**: `android-phase12-mali-vulkan-wip` (Phase 13 work-in-progress on top of Phase 12 Mali fixes)
**Target**: arm64-v8a Android, NativeActivity + Vulkan path (no SDL2-Vulkan), Adreno 6xx+ / Mali Valhall+

---

## TL;DR

**The game boots into a navigable launcher on a Samsung A24 (Mali-G57 Valhall) and on the Pixel 8 emulator.** Phases 0-13 are **DONE**. APK installs cleanly, ubershaders compile during a Java loading splash (~27 s on A24, gated to dismiss on first frame), then the title screen and RmlUi launcher (`Start Game / Controls / Settings / Mods / Exit`) render via the RT64 compositor. Tapping `Start Game` enters the game; touch overlay maps to N64 buttons + analog stick; audio runs through Oboe at 32 kHz.

**Active blocker**: file-select menu shows a black screen after the title — the game emits `FillRect`-only display lists, no triangles. Renderer is healthy; this is a game-logic-level issue currently under investigation (rt64 has uncapped `ALOG` instrumentation in workload_queue / framebuffer_renderer / present_queue on the Phase 12 branch).

---

## Phase 0 — Feasibility Spike (DONE 2026-05-09)

Cross-compiled the rendering core for `aarch64-linux-android26` from a macOS host using NDK 27.0.12077973 + Ninja + CMake.

### Outputs

| Artifact | Size | Status |
|---|---|---|
| `build-android-plume/libplume.a` | 9.0 MB | ✅ clean compile |
| `build-android-rt64/rt64.a` | 100 MB (94 .o files) | ✅ links cleanly |

### Cumulative artifact size after Phases 0-3

| Artifact | Phase 0 | Phase 1 | Phase 2 | Phase 3 |
|---|---|---|---|---|
| `rt64.a` object count | 94 | 95 | 154 | **155** |
| `rt64.a` size | 100 MB | 100 MB | 104 MB | **105 MB** |
| `libplume.a` | 9.0 MB | 9.0 MB | 9.0 MB | 9.0 MB |
| `libzstd.a` | — | — | — | **6.5 MB** |
| Shader SPIR-V blobs | 0 | 0 | 56 | **56** |
| Compile-time blockers | 4 | 3 | 1 | **0** |

### Files that cross-compiled with zero source changes

- `src/common/*` — all (12 files)
- `src/gbi/*` — all (12 files, N64 microcode interpreters)
- `src/gui/*` — all (4 files, imgui-based)
- `src/hle/*` — all **except** `rt64_application_window.cpp`
- `src/render/*` — all **except** `rt64_raster_shader.cpp` and `rt64_shader_library.cpp`
- `src/preset/*`, `src/shared/*`, `src/rhi/*` — all
- `src/contrib/imgui/*`, `src/contrib/im3d/*`, `src/contrib/implot/*`, `src/contrib/miniz/*` — all
- `src/contrib/plume/*` — all (Vulkan RHI, VMA, volk, Vulkan-Headers)

### Blockers identified (4 files)

| File | Reason | Fix complexity |
|---|---|---|
| `src/hle/rt64_application_window.cpp` | `static_assert(false && "Android unimplemented")` × 2 (lines 107, 151). Bug: line 14 `#elif defined(__linux__)` captures Android because NDK defines `__linux__`. | **Phase 1** — write Android NativeActivity / ANativeWindow path |
| `src/render/rt64_raster_shader.cpp` | Includes `shaders/RenderParams.hlsli.rw.h` (preprocessor output) | **Phase 2** — host-side shader tooling in cross-compile |
| `src/render/rt64_shader_library.cpp` | Includes `shaders/*.spirv.h` (DXC + file_to_c output) | **Phase 2** — same |
| `src/common/rt64_filesystem_zip.cpp` | Includes `<zstd.h>` | **Phase 3** — re-enable zstd subdirectory for arm64-v8a |

### Spike CMake patches (preserved in repo)

- [`lib/rt64/CMakeLists.txt`](lib/rt64/CMakeLists.txt) — added `RT64_ANDROID_SPIKE` option that gates host tools, shaders, and contrib libs out of the build. Extended Linux Vulkan/SDL branches to include Android.
- [`lib/rt64/src/contrib/plume/CMakeLists.txt`](lib/rt64/src/contrib/plume/CMakeLists.txt) — extended `IS_LINUX` predicate to `IS_LINUX_OR_ANDROID` for `cmake_dependent_option` gating.

### How to reproduce the spike

```bash
# from BanjoRecomp/
NDK=/Volumes/Kingston/Library/Android/sdk/ndk/27.0.12077973

# Plume only (smoke test)
cmake -S lib/rt64/src/contrib/plume -B build-android-plume -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release -DPLUME_SDL_VULKAN_ENABLED=OFF
cmake --build build-android-plume -j

# rt64 with spike gates
cmake -S lib/rt64 -B build-android-rt64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release -DRT64_STATIC=ON -DRT64_ANDROID_SPIKE=ON \
  -DRT64_SDL_WINDOW_VULKAN=OFF
cmake --build build-android-rt64 -j
```

---

## Roadmap

### Phase 1 — Android Window Glue

Replace the two `static_assert(false && "Android unimplemented")` in `rt64_application_window.cpp` with a working NativeActivity + ANativeWindow path.

**Decisions**:
- **NativeActivity vs SDL2 Android backend** → Pick **NativeActivity**. Plume already defines `RenderWindow = ANativeWindow*` for Android (see `lib/rt64/src/contrib/plume/plume_render_interface_types.h:40-41`). Going SDL2 means parching Plume to make `RenderWindow = SDL_Window*` again (extra surface area). Path of least resistance is what's already wired.

#### Phase 1.0 — Fix `__linux__` capturing Android — **DONE 2026-05-09**

NDK defines `__linux__`, so the existing `#elif defined(__linux__)` branches were catching Android and trying to include X11. Added `__ANDROID__` branch **before** every `__linux__` branch in `rt64_application_window.cpp`.

#### Phase 1.1 — Compile-clean Android stubs — **DONE 2026-05-09**

Wrote minimum-viable Android paths that let `rt64.a` cross-compile arm64-v8a without `RT64_ANDROID_SPIKE` excluding the file. Stubs:

- `setup(title, listener)` overload: `assert(false ...)` + return. NativeActivity owns the surface, so this overload is never called on Android — calling code uses `setup(RenderWindow, listener, threadId)` with an `ANativeWindow*` directly.
- `detectRefreshRate()`: returns `60` (TODO Phase 1.3: query `AChoreographer` / `AConfiguration` for actual device rate).
- `detectWindowMoved()`: empty body, INT32_MAX sentinels stay equal so "not moved" is reported. NativeActivity surfaces are fullscreen and don't move.

Verified: `rt64.a` arm64-v8a, 95 object files, 100 MB, zero compile warnings.

#### Phase 1.2 — Wire to NativeActivity callbacks (deferred to after Phase 8)

The runtime entry path on Android: `ANativeActivity_onCreate` → `onNativeWindowCreated(ANativeWindow*)` → call `ApplicationWindow::setup(window, listener, pthread_self())`. This requires the APK skeleton (Phase 8) to exist first. Defer.

#### Phase 1.3 — Real DPI, orientation, refresh rate (deferred to after Phase 9)

Replace the `60` Hz stub with `AChoreographer_postFrameCallback` queries; query `AConfiguration_getDensity` / `AConfiguration_getOrientation` for device profile. Needs a real device for tuning. Defer.

**Phase 1 deliverable status**: ✅ Compile-clean. Functional wiring deferred to Phase 8/9.

---

### Phase 2 — Host-Side Shader Pipeline — **DONE 2026-05-09**

Made CMake build the shader tooling on the **host** (macOS arm64) and run it during the Android cross-compile to generate `.spirv.c` / `.rw.c` blobs.

**Changes shipped**:
- DXC selection refactored to use `CMAKE_HOST_*` (lines 38-65) — separates host-side build tooling from target-side runtime files (NOMINMAX, dxcompiler.dll).
- `file_to_c` host build for Android: direct `clang++` invocation (skips nested CMake which leaked the NDK toolchain via env vars). The single-file source compiles in ~1 second.
- Pinned `find_program` to `/usr/bin/clang++` with `NO_DEFAULT_PATH` — without this, CMake picked the NDK's `clang++` from PATH, which has its own libc++ that mismatches macOS SDK headers (breaks `<charconv>` / `<filesystem>`).
- Added `-isysroot $(xcrun --show-sdk-path)` because the cross-compile leaks empty `SDKROOT` env that prevents Apple clang from finding system headers.
- Dropped the shader gate from the build-shader call block.
- Re-included `rt64_raster_shader.cpp` and `rt64_shader_library.cpp` in the source list.

**Verified**: `rt64.a` arm64-v8a, **154 object files**, 104 MB. **56 SPIR-V shader blobs** + 1 `.rw.c` preprocessor output generated and linked. Zero compile errors.

---

### Phase 3 — Restore zstd — **DONE 2026-05-09**

Re-included zstd for Android.

**Changes shipped**:
- `add_subdirectory(src/contrib/zstd/build/cmake)` un-gated for Android.
- `target_link_libraries(rt64 libzstd_static)` un-gated for Android.
- `rt64_filesystem_zip.cpp` re-included in source list.
- Disabled `ZSTD_BUILD_DICTBUILDER`, `ZSTD_BUILD_PROGRAMS`, `ZSTD_BUILD_TESTS` for Android. **Reason**: dictBuilder uses `qsort_r()`, which Bionic doesn't provide. rt64 only uses compression/decompression, never dictionary training, so dropping it is safe.

**Verified**: `rt64.a` 155 .o files, 105 MB; `libzstd.a` 6.5 MB; `libplume.a` 9.0 MB. Build green.

**Deferred to later phases**:
- `re-spirv` (SPIR-V optimizer) — not yet audited for arm64-v8a. Will check when needed by main app.
- `nativefiledialog-extended` — replaced by Storage Access Framework (Phase 7).
- `texture_hasher` / `texture_packer` — host-only tools, not needed at runtime.

---

### Phase 4 — N64ModernRuntime Cross-Compile — **DONE 2026-05-09**

Cross-compiled the recompilation runtime + ultramodern + N64Recomp + LiveRecomp + thirdparty (rabbitizer, fmt, miniz, sse2neon, concurrentqueue, o1heap) for arm64-v8a Android.

**Cost**: 1 build, **zero source changes, zero CMake patches**. The runtime is already platform-agnostic C++. `task_win32.cpp` is auto-empty under `#ifdef _WIN32`. Threading uses `std::thread`, filesystem uses `std::filesystem`, both work on NDK r23+.

**Cumulative artifacts** (arm64-v8a):

| Library | Size | .o files |
|---|---|---|
| `liblibrecomp.a` | 34 MB | 31 |
| `libN64Recomp.a` | 6.8 MB | 7 |
| `libultramodern.a` | 3.7 MB | 17 |
| `librabbitizer.a` (vendored) | 3.5 MB | 39 |
| `libSymbolLists.a` | 3.0 MB | 3 |
| `libLiveRecomp.a` | 3.0 MB | 4 |
| `libfmt.a` (vendored) | 1.8 MB | — |
| `libminiz.a` (vendored) | 652 KB | 5 |

**Reproduce**:
```bash
NDK=/Volumes/Kingston/Library/Android/sdk/ndk/27.0.12077973
cmake -B build-android-modernruntime -S lib/N64ModernRuntime -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-android-modernruntime -j
```

**Notes for Phase 5**:
- ENet, libcoopnet, libjuice are configured in the **main BanjoRecomp `CMakeLists.txt`**, not here. They're Phase 5 territory.
- libjuice has **no Android prebuilt binary** in `lib/coopnet/lib/`. Phase 5 will need a `FetchContent_Declare(libjuice ...)` block for Android, mirroring the existing Windows path at [BanjoRecomp/CMakeLists.txt:91-99](CMakeLists.txt:91).

---

### Phase 5 — BanjoRecomp Main App Cross-Compile — **DONE 2026-05-09**

Built `libBanjoRecompiled.so` ELF 64-bit ARM aarch64, **87 MB**, 14,908 symbols. Bumped `ANDROID_PLATFORM` from 26 to 28 for `aligned_alloc` (required by `lib/SlotMap/slot_map.h`).

**Changes shipped** (all in `BanjoRecomp/CMakeLists.txt`):
- `RT64_SDL_WINDOW_VULKAN=FALSE` for Android (NativeActivity path), auto-set `RT64_ANDROID_SPIKE=ON` for Android.
- `add_executable(BanjoRecompiled)` → `add_library(BanjoRecompiled SHARED)` on Android.
- Skip `add_subdirectory(RecompFrontend)` on Android (RmlUi + Freetype not needed; Phase 6 will provide touch UI).
- Trim Android link line: drop `nfd`, `recompui`, `recompinput`. Add `log`, `android`, `vulkan` system libs.
- `coopnet` Android branch: `FetchContent_Declare(libjuice v1.6.2)` (mirrors the existing Windows path); links `LibJuice::LibJuiceStatic`.
- Skip `PatchesBin` make target + `N64Recomp patches.toml` custom command on Android — those generate `patches.elf` / `patches.bin` / `RecompiledPatches/patches.c`, which are reused as-is from a prior desktop build. Regenerating them is host-only territory.
- Refactored DXC selection (duplicated in main CMakeLists) to use `CMAKE_HOST_*` instead of target macros (same fix as Phase 2).
- Linux block (`X11`, `Freetype`, `find_package(SDL2)`) gated to `elseif (CMAKE_SYSTEM_NAME MATCHES "Linux")` so Android (where `__linux__` is also defined) doesn't fall through.
- Added include paths for header-only deps the main app references but whose .cpp impls live in skipped `RecompFrontend`: `lib/RecompFrontend/lib/GamepadMotionHelpers`, `recompinput/include`, `recompui/include`. Also added `${SDL2_INCLUDE_DIRS}/..` so `#include <SDL2/SDL.h>` resolves.
- `target_link_options(BanjoRecompiled PRIVATE "-Wl,--unresolved-symbols=ignore-all")` on Android to allow recompui/recompinput/SDL2 undefined symbols at link time. They land in the .so as dynamic UND entries; the dynamic linker resolves them at load time once Phase 6/7/8 ships their Android-native implementations.

**Source-level fix**: [src/main/main.cpp:280](src/main/main.cpp:280) — changed `#elif defined(__linux__)` to `#elif defined(__gnu_linux__)` for the `SetImageAsIcon` call. NDK clang defines `__linux__` on Android, but the function is gated to glibc Linux only.

**164 expected undefined dynamic symbols** (resolved by Phase 6/7/8):
- `recompui::Style::set_*` (set_position, set_width, set_height, set_translate_2D, etc.)
- `recompui::Svg::Svg(...)`, `recompui::ContextId::create_resource_impl`, `recompui::get_current_context()`
- `recompinput::handle_events()`, `recompinput::players::*`
- `SDL_Init`, `SDL_CreateWindow`, `SDL_GetError`, `SDL_GetWindowWMInfo`, `SDL_SetHint`, `SDL_GetCurrentVideoDriver`

**Reproduce**:
```bash
NDK=/Volumes/Kingston/Library/Android/sdk/ndk/27.0.12077973
cmake -B build-android-main -S . -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-android-main -j
```

---

### Phase 6 — Touch Input Layer — **DONE 2026-05-09**

Built the on-screen virtual gamepad input pipeline. Render side and wire-up to NativeActivity are deferred to later phases (need an APK first).

**Files created**:
- [`src/main/android_touch.h`](src/main/android_touch.h) — public API: `set_viewport`, `process_motion_event`, `make_input_callbacks`.
- [`src/main/android_touch.cpp`](src/main/android_touch.cpp) — state machine + AInputEvent dispatch + `ultramodern::input` callbacks + 3 `recompinput::*` stubs.

**Architecture**:
- Default layout: 12 `TouchButton` (A, B, Z, Start, L, R, 4× C-buttons, 2× D-pad simplified) + 1 `VirtualStick`. All coordinates normalized 0..1; viewport size injected via `set_viewport(w, h)`.
- Multi-touch state machine: each button remembers its active `pointer_id`, can be held simultaneously with others. Stick takes ownership of the first pointer that lands inside its radius and tracks continuously until that pointer goes up.
- `process_motion_event(AInputEvent*)` is the entry point Phase 8 will hook to `ANativeActivity_setInputEventCallbacks`. Handles `ACTION_DOWN`, `ACTION_POINTER_DOWN`, `ACTION_UP`, `ACTION_POINTER_UP`, `ACTION_MOVE`, `ACTION_CANCEL`. Thread-safe (internal mutex).
- N64 button bits sourced from [lib/bk-decomp/include/2.0L/PR/os_cont.h:122-135](lib/bk-decomp/include/2.0L/PR/os_cont.h:122).
- `ultramodern::input::callbacks_t` returned by `make_input_callbacks()`:
  - `poll_input` no-op (state is updated synchronously from motion events)
  - `get_input` reads `g_btn_state` + `g_stick_x/y` under lock
  - `set_rumble` no-op (Phase 7: AVibrator)
  - `get_connected_device_info` returns `{Controller, RumblePak}` for player 0

**main.cpp hook** ([src/main/main.cpp:2330](src/main/main.cpp:2330)): `#ifdef __ANDROID__` branch picks `banjo_android::touch::make_input_callbacks()` instead of the recompinput-based desktop callbacks.

**Verified**: `libBanjoRecompiled.so` arm64-v8a still 87 MB, links clean. `recompui` / `recompinput` / `SDL_*` UND symbols dropped 164 → **159** (3 `recompinput::*` stubs + dead-code elimination of 2 transitive references).

**Remaining for "Phase 6 functional"**:
- **Configurable layout** — read from user config (size, position, opacity per-button). Defer until we have an APK that runs.
- **AInputEvent wire-up** — `ANativeActivity::callbacks->onInputQueueCreated` → loop reading from queue → `process_motion_event`. Lives in Phase 8 (APK skeleton).

---

### Phase 6.5 — Touch Overlay Render — **DONE 2026-05-09**

Added [`banjo_android::touch::render_overlay()`](src/main/android_touch.cpp) using imgui (already linked into rt64). Self-contained — Phase 8 calls it once per frame after the game's HUD draws.

**Implementation**:
- Full-screen click-through `ImGui::Begin` window with `NoBackground | NoInputs | NoFocusOnAppearing`.
- 12 circles (`ImDrawList::AddCircleFilled` + outline) for buttons, brighter when pressed.
- Stick: outer ring + thumb circle that moves with `g_stick_x/y`; thumb brighter while held.
- State snapshotted under `g_mutex` then released before drawing — minimal contention with the input thread.

**Verified**: `libBanjoRecompiled.so` 87 MB → 89 MB (+2 MB imgui code). Zero errors. UND count stable at 159.

---

### Phase 7 — Audio (Oboe) — **DONE 2026-05-09**

Replaced the desktop SDL2 audio path with [Oboe](https://github.com/google/oboe) (Google's low-latency Android audio library, AAudio + OpenSL ES under the hood).

**Files created**:
- [`src/main/android_audio.h`](src/main/android_audio.h) — public API: `start()`, `stop()`, `make_audio_callbacks()`.
- [`src/main/android_audio.cpp`](src/main/android_audio.cpp) — ring buffer + Oboe stream + 3 callbacks.

**Architecture**:
```
recompiled MIPS  --queue_samples-->  RingBuffer  --Oboe pull-->  speakers
                                       ^
                                       | get_frames_remaining
                                       | set_frequency (rebuilds stream)
```
- Single `std::mutex` ring buffer (~6× game frames at 32 kHz, ≈100 ms). For Banjo's sample rates, contention is negligible. Lock-free SPSC ring is a Phase 9 optimization candidate if jitter shows up.
- Producer is the game thread (`queue_samples`); consumer is Oboe's callback thread (`onAudioReady`). Underrun fills with silence; overflow drops oldest frames.
- `setPerformanceMode(LowLatency)` + `setSharingMode(Shared)` + `setUsage(Game)`.
- `set_frequency(freq)` reopens the stream if the rate actually changes (Banjo runs at 32 kHz; Mario 64 might be different).

**CMake changes**:
- Added Oboe via `FetchContent_Declare(oboe v1.9.3)` in the Android branch of [CMakeLists.txt](CMakeLists.txt:108).
- Linked `oboe` into `BanjoRecompiled` Android target.
- Hooked into [main.cpp:2330](src/main/main.cpp:2330) via `#ifdef __ANDROID__` — `banjo_android::audio::start()` then `make_audio_callbacks()`.

**CMake compatibility note**: Oboe's old `cmake_minimum_required(VERSION 3.4)` trips CMake 4.x. **Build flag `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` is now required** for the Android cross-compile (and won't hurt the desktop build).

**Verified**: `libBanjoRecompiled.so` 89 MB → **92 MB** (+3 MB Oboe code + ring buffer). `liboboe.a` produced at `build-android-main/_deps/oboe-build/liboboe.a`. Zero errors. UND count stable at 159 (Phase 7 replaces the audio runtime, not the desktop SDL window/events).

**Deferred to Phase 8** (need NativeActivity / JNI Activity context):
- Save data routing to `Context.getFilesDir()` (internal app storage).
- ROM file picker via Storage Access Framework (`ACTION_OPEN_DOCUMENT`).
- Resource bundle access via `AAssetManager` (.nrm and .rtz files).
- Haptic feedback via `Vibrator` / `VibratorManager` (rumble pak).

---

### Phase 7 — Audio + System Paths

Audio is **DONE** above. Save paths, SAF, and AAssetManager are deferred to Phase 8 (need NativeActivity / JNI Activity context to call into Java for those APIs).

---

### Phase 8 — APK Skeleton — **DONE 2026-05-09**

`platform/android/app/build/outputs/apk/debug/app-debug.apk` — **36 MB, structurally valid, installable**.

**Files created**:
- [`platform/android/settings.gradle.kts`](platform/android/settings.gradle.kts) — root project, includes `:app`.
- [`platform/android/build.gradle.kts`](platform/android/build.gradle.kts) — top-level, declares AGP 8.7.3.
- [`platform/android/gradle.properties`](platform/android/gradle.properties) — JVM args, AndroidX, parallel/cache.
- [`platform/android/local.properties`](platform/android/local.properties) — `sdk.dir` pointer (gitignore!).
- [`platform/android/app/build.gradle.kts`](platform/android/app/build.gradle.kts) — Android app config + `externalNativeBuild` pointing at root `CMakeLists.txt`, passes `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` (Oboe), `-DANDROID_PLATFORM=android-28` (`aligned_alloc`), `targets += "BanjoRecompiled"`.
- [`platform/android/app/src/main/AndroidManifest.xml`](platform/android/app/src/main/AndroidManifest.xml) — `<application android:hasCode="false">` (pure NativeActivity, no Java needed), `<activity android:name="android.app.NativeActivity">` with `<meta-data android:name="android.app.lib_name" android:value="BanjoRecompiled" />`.
- [`platform/android/app/src/main/res/values/strings.xml`](platform/android/app/src/main/res/values/strings.xml) — `Banjo: Recompiled Online`.
- [`src/main/android_main.cpp`](src/main/android_main.cpp) — `extern "C" void android_main(struct android_app*)` entry; ALooper event loop; routes motion events to `banjo_android::touch::process_motion_event`; tracks `ANativeWindow*` on `APP_CMD_INIT_WINDOW`.

**CMake additions** ([CMakeLists.txt](CMakeLists.txt:225)):
- `${NDK}/sources/android/native_app_glue/android_native_app_glue.c` added to the Android `SOURCES` list (provides `ANativeActivity_onCreate`).
- `target_link_options(BanjoRecompiled PRIVATE "-u" "ANativeActivity_onCreate")` — without this, `--gc-sections` strips the entry point and the OS can't find it.
- `target_include_directories(BanjoRecompiled PRIVATE ${NATIVE_APP_GLUE_DIR})` for the header.

**APK contents**:
```
36 MB lib/arm64-v8a/libBanjoRecompiled.so  (92 MB raw, compressed)
1.3 MB lib/arm64-v8a/libc++_shared.so      (NDK C++ runtime)
3 KB AndroidManifest.xml
```

**Manifest summary** (`aapt2 dump badging`):
```
package: com.banjorecomp.online
versionCode: 1, versionName: 0.1-android-spike
minSdkVersion: 28, targetSdkVersion: 35
launchable-activity: android.app.NativeActivity
uses-permission: INTERNET, ACCESS_NETWORK_STATE
uses-feature: android.hardware.vulkan.version=4198400 (1.0.4)
```

**Reproduce**:
```bash
cd BanjoRecomp/platform/android
JAVA_HOME=/Applications/Android\ Studio.app/Contents/jbr/Contents/Home gradle assembleDebug
# Output: app/build/outputs/apk/debug/app-debug.apk
```

**Caveat — what Phase 8 doesn't achieve yet**: the Activity launches and `android_main()` runs its event loop, but **`recomp::start()` is never called** because the desktop main.cpp expects an `SDL_Window*` and that path is dead-coded for Android. Phase 9 will refactor the boot flow to feed the `ANativeWindow*` into `rt64::Application::setup()` — Plume already accepts `ANativeWindow*` natively.

### Phase 8 — Emulator Boot Confirmed (2026-05-09)

Installed and ran on the bundled **Pixel 8 / API 36 / arm64-v8a** AVD (Apple Silicon native, no translation). Boot trace:

```
17:50:40.894  Load libBanjoRecompiled.so ... ok
17:50:40.899  BK64-Main: BK64-Online native entry — android_main()
17:50:40.899  OboeAudio: openStreamInternal() OUTPUT —— OboeVersion1.9.3 ——
17:50:41.188  BK64-Audio: Oboe stream opened: 32000 Hz, 2 channels
17:50:41.267  BK64-Main: APP_CMD_RESUME
17:50:41.432  BK64-Main: APP_CMD_INIT_WINDOW: 2400x1080
17:50:41.477  Displayed for user 0: +2s309ms
17:50:41.498  BK64-Main: APP_CMD_GAINED_FOCUS
```

Screen is black — expected, since rt64 isn't yet hooked to the `ANativeWindow*` (Phase 9). Activity stays alive and responsive; no crashes, no stub aborts.

**Stub gotcha discovered**: an early stubs file accidentally aliased Android NDK symbols (`AConfiguration_*`, `ALooper_*`, `AInputQueue_*`, `AMotionEvent_*`, `ANativeWindow_*`) to our abort-stub, masking the real implementations from `libandroid.so`. The first launch crashed at `AConfiguration_delete` inside `native_app_glue`'s teardown. Fix: regex `^A[A-Z][a-zA-Z]*_` excludes Android NDK symbols from the stub list. Documented in [src/main/android_stubs.cpp](src/main/android_stubs.cpp) header comment.

**Reproduce on the emulator**:
```bash
SDK=/Volumes/Kingston/Library/Android/sdk
$SDK/emulator/emulator -avd Pixel_8_API_36 -no-snapshot-load &
adb wait-for-device
until adb shell getprop sys.boot_completed | grep -q "^1$"; do sleep 5; done

# Install + launch
adb install -r BanjoRecomp/platform/android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.banjorecomp.online/android.app.NativeActivity
adb logcat -s BK64-Main BK64-Audio BK64-Stub  # tail logs
```

---

### Phase 9-10 — Boot Wire-Up + First Display List — **DONE 2026-05-09** (commit `41b4da6`)

Squashed Phases 9 and 10 into one bring-up effort. The APK now boots through full BK init and emits its first display list.

**Boot path** ([src/main/android_run_game.cpp](src/main/android_run_game.cpp)):
- Anchors recomp config in the app's external/internal data dir.
- Registers all `recomp_*` / `recomp_net_*` / `bknet_debug_log` REGISTER_FUNC targets.
- Calls `recomputil::register_data_api_exports` + `register_bk_overlays` + `register_bk_patches` + `init_extended_object_data(2)`.
- Auto-starts BK on the first gfx update once VI states are seeded.

**Render context** ([src/main/android_render_context.cpp](src/main/android_render_context.cpp)):
- Minimal `RT64::Application` driver for the NativeActivity path. Forces Vulkan, mirrors the desktop `recompui RT64Context` for the bits we need (no texture-pack hot-swap, no recompui dependency at this stage).
- Heartbeat counters log every ~5 s so logcat shows "frames flowing".

**Per-domain stub partitioning** (replaces the monolithic `android_stubs.cpp`):
- `android_nfd_stubs.cpp` — `NFD_Init/Quit → OKAY`, dialogs return ERROR. Needed because `RT64::FileDialog::initialize()` runs unconditionally.
- `android_respv_stubs.cpp` — re-spirv shader/optimizer return false (replaced by real lib in Phase 11).
- `android_recompui_stubs.cpp` — `recomp_run_ui_callbacks` no-op + `recompui::get_window_size` routed to android_touch viewport.
- `android_recompinput_stubs.cpp` — right analog / gyro / mouse return zero; rumble + mapper setup no-op.
- `android_stubs.cpp` — aborting per-symbol stubs for the residue, named via `android_unimplemented_stub_named` so logcat shows exactly which symbol fires next.
- `platform/android/scripts/regen_stubs.sh` regenerates `android_stubs.cpp` from the current `.so` UND list, filtering NDK + per-domain providers.

**APK size after Phase 10**: ~77 MB.

**CMake**: `-Wl,--unresolved-symbols=ignore-all` retained so Bionic-incompatible desktop symbols don't block link.

---

### Phase 11 — Real re-spirv Shader Optimizer — **DONE 2026-05-09** (commit `6f93393`)

The Phase 10 stub of `respv::Optimizer::run` returned empty vectors, which were then handed to `vkCreateShaderModule` with `codeSize=0` and SIGSEGV'd inside the Mali driver (validation caught `VUID-VkShaderModuleCreateInfo-codeSize-01085`).

**Fix**: `rt64/CMakeLists.txt` now builds re-spirv when `ANDROID` is set even with `RT64_ANDROID_SPIKE` on. re-spirv is plain C++17 with header-only SPIRV-Headers — cross-compiles cleanly for arm64-v8a. Stub file dropped from SOURCES; real lib linked.

**Result on Samsung A24 / Mali G57**: RT64's first DL pipeline creation succeeds. Gfx thread produces steady 60 Hz: **1107 display lists, 4926 screen_updates over 85 s**, swapchain presenting.

`.gitignore` excludes `platform/android/app/src/main/jniLibs/` so the 24 MB Vulkan validation layer .so doesn't end up in the repo (download from KhronosGroup/Vulkan-ValidationLayers releases when debugging on device).

---

### Phase 12 — Mali Valhall White Frame → BK64 Title Renders — **DONE 2026-05-10**

Mali-G57 booted steady 60 Hz but rendered pure white. Root cause: when `dualSrcBlend` is OFF, Mali routes `SV_TARGET1` to the color attachment instead of the blend factor — the fallback path was reading coverage as color.

**Fix sequence** (commits `541b0dd` → `7f656b8`):
- `541b0dd` Phase 12 WIP scaffold: NativeActivity swapped for a thin `MainActivity` Java subclass with a START button overlay (validates input → recomp pipeline regardless of render). Two gradle props (`-Prt64DiagRasterPs=N` / `-Prt64DiagVi=N`) wired through to CMake cache for in-shader diagnostic modes (cyan/grid/classifier outputs).
- `d8d815d` — enable Mali `SV_TARGET1` strip via `RT64_NO_DUAL_SOURCE_DYNAMIC_PS`.
- `ef077fb` — 2× resolution scale, drop validation, document via callback.
- `3999231` — loading splash + JNI first-frame signal + skip-spec-constant bump.
- `ee0b5ef` — strip `SV_TARGET1` from spec-constant + restore alpha blend.
- `79852cc` — gate game thread on ubershader compile + Java loading splash. `setup()` now blocks until ubershader pipelines `0+3+rest` are ready; `recomp::start` waits on `setup()`, stalling game loop AND audio scheduler in lockstep so splash, music, and intro first-frame all start together. `nativeInit()` caches a global ref to `MainActivity` from the static initializer (FindClass from the workload thread otherwise fails because the system classloader has no access to `com.banjorecomp.online.*`).
- `7f656b8` — bump rt64 to `8b92211` (uncapped `ALOG` instrumentation in workload_queue / framebuffer_renderer / present_queue for the next investigation — file-select menu black-screen).

**Cost**: boot is ~27 s on A24 because Mali serializes the 8 ubershader pipeline compiles. Reduction targets: persistent `VkPipelineCache` on disk, prune unused variants (BK64 may only need 3-4 of 8), or compile critical-path synchronously + rest lazily.

**Result**: A24 renders the BK64 title screen.

---

### Phase 13 — RmlUi Launcher on Android (WIP) — Renders + Navigable on Emulator — commit `aed5b99`

End-to-end working on the Pixel 8 emulator: launcher menu (`Start Game / Controls / Settings / Mods / Exit`) renders in RT64's compositor over the BK framebuffer, touches map to clicks, tapping `Settings` opens the General/Graphics/Controls tabs built by `banjo::init_config()`.

**Build / link**:
- `add_subdirectory(RecompFrontend)` un-gated for Android; `BanjoRecompiled` now links `recompui` + `recompinput` on Android. The `android_recompui_stubs.cpp` / `android_recompinput_stubs.cpp` files are dropped (real impls win). `android_stubs.cpp` regenerated — **33 remaining off-path SDL_*/text-input symbols vs 162 before**.
- `lib/RecompFrontend` submodule bumped: Freetype FetchContent for Android, SDL shim under `recompinput/include/sdl_shim/`, `set_program_path_override`.
- `platform/android/app/build.gradle.kts`: `assets.srcDirs("../../../assets")` — APK picks up the same fonts/SVGs/RCSS the desktop build uses, no duplication.

**Boot path** ([src/main/android_run_game.cpp](src/main/android_run_game.cpp)):
- `extract_apk_assets` walks the APK's root + `icons/` + `promptfont/` subdirs into `internalDataPath/assets/` (idempotent on file size).
- `recompui::file::set_program_path_override(internalDataPath)` so `get_asset_path` resolves to the extracted tree.
- `setenv("HOME", internalDataPath, 1)` so RT64's `__linux__` branch builds `$HOME/.rt64` inside the sandbox (was `/data/.rt64` → `EACCES`).
- `register_primary_font("Suplexmentary Comic NC.ttf")` + Inter font, `banjo::locale::init()`, `banjo::init_config()` builds all launcher tabs.
- Render context switched from `banjo_android::renderer::create_render_context` to `recompui::renderer::create_render_context(rdram, win, PresentEarly, developer_mode)` — installs RT64 render hooks for the UI compositor.
- `android_on_launcher_init` overrides `start_game` callback to also call `banjo_android_notify_game_started()` (JNI), then `recomp::start_game(supported_games.front().game_id, {})`.

**Touch → click bridge** ([src/main/android_touch.cpp](src/main/android_touch.cpp)):
- `push_ui_mouse_event` synthesizes `SDL_MOUSEMOTION` + `SDL_MOUSEBUTTONDOWN/UP` and feeds them into `recompui::queue_event` (bypassing `SDL_PollEvent` → `handle_events`, which nothing drives on Android).
- Pointer 0 only — multi-touch still routes to the N64 button bitmask.

**Java side** ([MainActivity.java](platform/android/app/src/main/java/com/banjorecomp/online/MainActivity.java)):
- START / A overlay buttons no longer auto-installed at boot. New `nativeNotifyGameStarted()` / `nativeNotifyReturnToLauncher()` JNI methods show/hide them so the launcher gets the full screen.

**Config migration** ([src/game/config.cpp](src/game/config.cpp)): default controller bindings use `recompinput::GamepadAxis` / `GamepadButton` enums instead of SDL2's `SDL_CONTROLLER_AXIS_/BUTTON_` symbols. Numerically identical (static_assert on `platform_sdl.cpp`); JSON profiles round-trip byte-for-byte.

**Pending (next session)**: hide touch overlay while launcher is visible, networking init, JNI EditText bridge for lobby code.

---

### Active blocker — File-Select Menu Black-Screen

After Phase 12 ships the title, the file-select menu renders black. Game emits `FillRect`-only display lists, no triangles. Renderer is healthy — this is a game-logic-level bug. rt64 has uncapped `ALOG` instrumentation in `workload_queue` / `framebuffer_renderer` / `present_queue` on the WIP branch for the next investigation pass.

---

### Remaining work (post-Phase 13)

Real backlog, in recommended order:

- [ ] **Resolve file-select black-screen** — debug why the game emits FillRect-only DLs after title.
- [ ] **Hide touch overlay while launcher is visible** — currently overlaps the RmlUi menu.
- [ ] **Networking init on Android** — Phase 13 left this as "next session".
- [ ] **JNI EditText bridge for lobby code** — text-input is the main residue in the 33 remaining UND stubs.
- [ ] **Boot-time reduction** — persistent `VkPipelineCache` on disk + prune unused ubershader variants. Target 10-15 s (from current ~27 s on A24).
- [ ] **HUD pads** — D-pad / B / Z / C-buttons / stick layout polish (configurable size, position, opacity).
- [ ] **Bluetooth gamepad support** — currently touch-only.
- [ ] **Aspect ratio + multi-resolution** (foldables, tablets).
- [ ] **Real DPI / orientation / refresh rate** — Phase 1.3 deferred: replace 60 Hz stub with `AChoreographer_postFrameCallback`; query `AConfiguration_getDensity` / `getOrientation`.
- [ ] **Adreno reference device test** — Snapdragon 8 Gen 2+ (Adreno 740/750). All Phase 12 work was on Mali; Adreno path untested on real hardware.
- [ ] **Battery / thermal profiling**.
- [ ] **Multiplayer over cellular** (CoopNet ICE behavior on mobile networks).
- [ ] **Public-ready APK** with documented minimum specs and known-issue list.

---

## Key References (preserved from research phase)

- **RT64 Vulkan/Android scaffolding already present**:
  - `lib/rt64/src/contrib/plume/plume_vulkan.h:19-20` — `VK_USE_PLATFORM_ANDROID_KHR`
  - `lib/rt64/src/contrib/plume/plume_vulkan.cpp:53-54` — `VK_KHR_ANDROID_SURFACE_EXTENSION_NAME`
  - `lib/rt64/src/contrib/plume/plume_vulkan.cpp:2105-2114` — `vkCreateAndroidSurfaceKHR`
  - `lib/rt64/src/contrib/plume/plume_vulkan.cpp:2462-2464` — `ANativeWindow_getWidth/Height`
- **N64ModernRuntime Android awareness**:
  - `lib/N64ModernRuntime/ultramodern/include/ultramodern/renderer_context.hpp:12-13` — `<android/native_window.h>`
- **Optional Vulkan extensions used by Plume** (all gated, fallbacks exist):
  - `VK_EXT_descriptor_indexing`, `VK_KHR_buffer_device_address`, `VK_EXT_scalar_block_layout`,
  - `VK_EXT_robustness2`, `VK_EXT_sample_locations`, `VK_KHR_sampler_mirror_clamp_to_edge`
- **Ray tracing**: `RT_ENABLED` macro — disable for Android (no mobile GPU has it).
- **Upstream issues (open, no maintainer response)**:
  - [N64Recomp #22](https://github.com/N64Recomp/N64Recomp/issues/22) — Android port compatibility
  - [Zelda64Recomp #45](https://github.com/Zelda64Recomp/Zelda64Recomp/issues/45) — Android support

## Precedent: Closest projects to copy from

- **Ship of Harkinian Android** ([Waterdish/Shipwright-Android](https://github.com/Waterdish/Shipwright-Android)) — uses LibUltraship + GLES, **different stack**, but the Gradle/JNI scaffolding is reusable.
- **sm64coopdx Android** ([ManIsCat2/sm64coopdx](https://github.com/ManIsCat2/sm64coopdx) branch `android`) — decomp + GLES2, **different renderer**, but their touch overlay pattern is directly applicable.
- **Portmaster** Linux ARM64 builds of Recomp games — proves the recompiled C + RT64 + Vulkan stack runs on ARM64 with mainline Mesa drivers.

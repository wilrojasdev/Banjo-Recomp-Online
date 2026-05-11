// Android no-op stubs for NFD (Native File Dialog).
//
// RT64::FileDialog::initialize() is called from RT64::Application's
// constructor — even when developer mode is off and no dialog will ever be
// shown. We can't get rid of the call without forking RT64, but we can
// satisfy the symbol with a no-op that returns NFD_OKAY so construction
// proceeds. The dialog accessors all return NFD_ERROR so any caller that
// actually tries to open a picker just sees an empty result.

#ifdef __ANDROID__

#include <android/log.h>
#include <cstddef>

#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "BK64-NFD", __VA_ARGS__)

// nfdresult_t enum values from nfd.h. Inlining the constants avoids pulling
// the contrib header into the Android build (which would also drag in
// platform-gated function bodies).
constexpr int NFD_ERROR = 0;
constexpr int NFD_OKAY = 1;
constexpr int NFD_CANCEL = 2;

extern "C" int NFD_Init(void) {
    // Pretend init succeeded so RT64::FileDialog::initialize is happy.
    return NFD_OKAY;
}

extern "C" void NFD_Quit(void) {
    // No state to release.
}

extern "C" int NFD_OpenDialogN(void** /*outPath*/, const void* /*filterList*/,
                               unsigned int /*filterCount*/, const void* /*defaultPath*/) {
    LOGW("NFD_OpenDialogN: file picker not implemented on Android (returning ERROR)");
    return NFD_ERROR;
}

extern "C" int NFD_SaveDialogN(void** /*outPath*/, const void* /*filterList*/,
                               unsigned int /*filterCount*/, const void* /*defaultPath*/,
                               const void* /*defaultName*/) {
    LOGW("NFD_SaveDialogN: file picker not implemented on Android (returning ERROR)");
    return NFD_ERROR;
}

extern "C" int NFD_PickFolderN(void** /*outPath*/, const void* /*defaultPath*/) {
    LOGW("NFD_PickFolderN: folder picker not implemented on Android (returning ERROR)");
    return NFD_ERROR;
}

extern "C" void NFD_FreePathN(void* /*filePath*/) {
    // No allocation to free since the dialog stubs never produce a path.
}

extern "C" int NFD_OpenDialogMultipleN(const void** /*outPaths*/, const void* /*filterList*/,
                                       unsigned int /*filterCount*/, const void* /*defaultPath*/) {
    LOGW("NFD_OpenDialogMultipleN: multi-file picker not implemented on Android (returning ERROR)");
    return NFD_ERROR;
}

// nfdpathsetsize_t on Android (non-_WIN32 / non-__APPLE__) is `unsigned int`,
// see lib/rt64/src/contrib/nativefiledialog-extended/src/include/nfd.h:221.
// Pinning the type here avoids the silent 8-vs-4-byte write that would happen
// if the stub used size_t.
extern "C" int NFD_PathSet_GetCount(const void* /*pathSet*/, unsigned int* outCount) {
    if (outCount) *outCount = 0;
    return NFD_OKAY;
}

extern "C" int NFD_PathSet_GetPathN(const void* /*pathSet*/, unsigned int /*index*/, void** /*outPath*/) {
    return NFD_ERROR;
}

extern "C" void NFD_PathSet_Free(const void* /*pathSet*/) {
    // No allocation to free; OpenDialogMultipleN always returns ERROR.
}

#endif  // __ANDROID__

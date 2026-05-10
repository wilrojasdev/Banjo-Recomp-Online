#!/usr/bin/env bash
#
# Regenerate src/main/android_stubs.cpp from the current libBanjoRecompiled.so.
#
# Bionic's dlopen() rejects shared libraries that have GLOBAL UND symbols not
# provided by an already-loaded shared library. We link with
#   -Wl,--unresolved-symbols=ignore-all
# so the link succeeds even when desktop-only paths (SDL2, NFD, recompui,
# re-spirv, ImGui's SDL2 backend, etc.) leave UND symbols behind. To make the
# .so dlopen-able we then alias every "leftover" UND to a function that
# logs+aborts, which is fine because Phase 9's boot path is supposed to never
# touch those code paths.
#
# This script:
#   1. Reads UND symbols from the freshly built .so.
#   2. Drops everything that's actually provided by system .so files (libc,
#      libm, libdl, libandroid, liblog, libvulkan, libc++_shared, etc.)
#   3. Writes the remaining symbols as `attribute((alias))` aliases pointing
#      at android_unimplemented_stub().
#
# Run this whenever the link error list changes (new code path pulls in new
# UND symbols, or we delete a desktop-only symbol from the stubs list).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")"/../../.. && pwd)"
NDK="${ANDROID_NDK:-/Volumes/Kingston/Library/Android/sdk/ndk/27.0.12077973}"
OUT="$ROOT/src/main/android_stubs.cpp"

# Important: we must read UND from a build that has the existing stubs
# REMOVED, otherwise alias definitions hide the symbols we need to stub.
# The flow is: blank the stubs file -> rebuild -> regen sees full UND set
# -> rebuild again with the new stubs.
ANDROID_DIR="$ROOT/platform/android"
JAVA_HOME="${JAVA_HOME:-/Applications/Android Studio.app/Contents/jbr/Contents/Home}"
export JAVA_HOME

echo "stubs: blanking $OUT and running pre-build to expose all UND"
cat > "$OUT" <<'BLANK'
// Auto-managed by platform/android/scripts/regen_stubs.sh — blank by design
// during regen so the linker exposes the complete UND set.
#ifdef __ANDROID__
#include <android/log.h>
#include <stdlib.h>

extern "C" void android_unimplemented_stub();
extern "C" void android_unimplemented_stub() {
    __android_log_print(ANDROID_LOG_FATAL, "BK64-Stub",
        "unimplemented Android stub called - aborting");
    abort();
}
#endif
BLANK

(cd "$ANDROID_DIR" && gradle assembleDebug --quiet 2>&1) | tail -3 || {
    echo "warning: pre-build returned non-zero; continuing anyway (stubs will be regen'd)" >&2
}

SO="${SO_PATH:-$(find "$ROOT/platform/android/app/.cxx" -name libBanjoRecompiled.so | head -1)}"
if [ ! -f "$SO" ]; then
    echo "error: libBanjoRecompiled.so not found after pre-build" >&2
    exit 1
fi

READELF="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf"
if [ ! -x "$READELF" ]; then
    READELF="$(command -v llvm-readelf || command -v readelf)"
fi

echo "stubs: reading UND from $SO"

ALL=$(mktemp)
"$READELF" --dyn-syms "$SO" \
    | awk '$5 == "GLOBAL" && $7 == "UND" { print $NF }' \
    | sort -u > "$ALL"

# Strip versioning (e.g. __cxa_atexit@LIBC -> __cxa_atexit) and dedup.
# Without this we'd emit invalid C identifiers in the stubs file.
sed -i.bak 's/@.*//' "$ALL"
sort -u -o "$ALL" "$ALL"
rm -f "$ALL.bak"

# Sequential filters. Each regex excludes lines provided by a known
# system library so they don't end up aliased to the abort stub.
filter_re() {
    local re="$1"; local in="$2"
    local out
    out=$(mktemp)
    grep -Ev "$re" "$in" > "$out" || true
    rm -f "$in"
    echo "$out"
}

KEEP="$ALL"
KEEP=$(filter_re '^A[A-Z][a-zA-Z]*_'                                "$KEEP")  # libandroid
KEEP=$(filter_re '^NFD_'                                            "$KEEP")  # provided by android_nfd_stubs.cpp (no-op)
KEEP=$(filter_re '^_ZN5respv'                                       "$KEEP")  # provided by android_respv_stubs.cpp (no-op)
KEEP=$(filter_re '^_ZNK5respv'                                      "$KEEP")  # provided by android_respv_stubs.cpp (no-op)
KEEP=$(filter_re '^recomp_run_ui_callbacks$'                        "$KEEP")  # provided by android_recompui_stubs.cpp (no-op)
KEEP=$(filter_re '^_ZN8recompui15get_window_sizeERiS0_$'            "$KEEP")  # provided by android_recompui_stubs.cpp
KEEP=$(filter_re '^_ZN11recompinput(13update_rumble|15get_gyro_deltas|16get_mouse_deltas|16get_right_analog|19game_input_disabled|19set_game_input_name|26set_game_input_description|27set_right_analog_suppressed|34set_default_mapping_for_controller)' "$KEEP")  # provided by android_recompinput_stubs.cpp
KEEP=$(filter_re '^_ZN11recompinput7players22set_single_player_mode' "$KEEP")  # provided by android_recompinput_stubs.cpp
KEEP=$(filter_re '^__android_log'                                   "$KEEP")  # liblog
KEEP=$(filter_re '^vk[A-Z]'                                         "$KEEP")  # libvulkan
KEEP=$(filter_re '^volk'                                            "$KEEP")  # volk
KEEP=$(filter_re '^pthread_'                                        "$KEEP")  # libc
KEEP=$(filter_re '^sem_'                                            "$KEEP")  # libc
KEEP=$(filter_re '^dl(open|close|sym|error|addr|_iterate|vsym)$'    "$KEEP")  # libdl
KEEP=$(filter_re '^__cxa_'                                          "$KEEP")  # libc++abi (in c++_shared)
KEEP=$(filter_re '^__gxx_personality'                               "$KEEP")  # libc++abi
KEEP=$(filter_re '^_Unwind_'                                        "$KEEP")  # libunwind (in c++_shared)
KEEP=$(filter_re '^__'                                              "$KEEP")  # compiler builtins / libc internals
# C++ ABI mangled symbols provided by libc++_shared.so. Cover both the std::
# (St prefix) and std::__ndk1:: (N6__ndk1 / NSt6__ndk1) variants. typeinfo
# prefixes _ZTI / _ZTS / _ZTV may appear with or without an N nesting marker.
KEEP=$(filter_re '^_Z(NK?|TS|TI|TV)?N?St'                           "$KEEP")  # any std:: mangled
KEEP=$(filter_re '^_Z(NK?|TS|TI|TV)?N?[0-9]*__ndk1'                 "$KEEP")  # std::__ndk1
KEEP=$(filter_re '^_ZSt'                                            "$KEEP")  # std:: free functions
KEEP=$(filter_re '^_(Znwm|Znam|ZdlPv|ZdaPv|ZdlPvm|ZdaPvm)$'         "$KEEP")  # operator new/delete
KEEP=$(filter_re '^_ZT[VIS]N[0-9]+__cxxabiv1'                       "$KEEP")  # libc++abi typeinfo / vtable
KEEP=$(filter_re '^_ZN[0-9]+__cxxabiv1'                             "$KEEP")  # libc++abi nested
KEEP=$(filter_re '^_ZNK[0-9]+__cxxabiv1'                            "$KEEP")  # libc++abi const nested
KEEP=$(filter_re '^_ZTT'                                            "$KEEP")  # construction vtables (libc++)
KEEP=$(filter_re '^_ZThn'                                           "$KEEP")  # non-virtual thunks (libc++)
KEEP=$(filter_re '^_ZTv'                                            "$KEEP")  # virtual thunks (libc++)
KEEP=$(filter_re '^_Z(nwm|nam|dlPv|daPv)'                           "$KEEP")  # operator new / delete variants
KEEP=$(filter_re '^_ZSt(9nothrow_t|9terminatev|13set_terminate)'    "$KEEP")  # std:: globals
# libc / libm common funcs (one per line for readability).
LIBC_FUNCS='^(aligned_alloc|posix_memalign|calloc|malloc|free|realloc|reallocarray|memcpy|memset|memmove|memcmp|memchr|strlen|strcmp|strncmp|strcpy|strncpy|strcat|strncat|strchr|strrchr|strstr|strspn|strcspn|strdup|strndup|strerror|strerror_r|strcasecmp|strncasecmp|strtok|strtok_r|strsignal|sprintf|snprintf|fprintf|printf|sscanf|fscanf|vsnprintf|vsprintf|vfprintf|vprintf|abort|exit|_Exit|atexit|getenv|setenv|unsetenv|errno|environ|stderr|stdin|stdout|fflush|fopen|fclose|fread|fwrite|fputs|fputc|fgets|fgetc|fseek|fseeko|fseeko64|ftell|ftello|ftello64|feof|ferror|fileno|setvbuf|open|openat|close|read|write|lseek|stat|fstat|lstat|fstatat|access|faccessat|mkdir|mkdirat|rmdir|unlink|unlinkat|rename|renameat|readlink|symlink|chmod|fchmod|chown|opendir|readdir|closedir|getcwd|chdir|umask|getpid|getppid|getuid|geteuid|getgid|getegid|setpgid|getpgid|getsid|setsid|kill|raise|signal|sigaction|sigprocmask|sigsuspend|sigemptyset|sigaddset|sigfillset|sigaltstack|setjmp|longjmp|sigsetjmp|siglongjmp|select|pselect|poll|ppoll|eventfd|fcntl|ioctl|mmap|mmap64|munmap|mremap|mprotect|madvise|posix_madvise|sysconf|prctl|getauxval|gettimeofday|nanosleep|usleep|sleep|alarm|setitimer|getitimer|wait|waitpid|fork|vfork|execve|execv|execvp|setlocale|localeconv|nl_langinfo|atoi|atol|atoll|atof|strtol|strtoll|strtoul|strtoull|strtod|strtof|tolower|toupper|isalpha|isdigit|isspace|isxdigit|isalnum|isupper|islower|qsort|bsearch|backtrace|backtrace_symbols|fopen64|stat64|fstat64|lstat64|getline|getdelim|qsort_r|bsearch_r|secure_getenv|freeifaddrs|getifaddrs|getnameinfo|freopen|gethostbyname|getpagesize|gettid|isatty|pipe|posix_spawn|puts|quick_exit|random|remove|srand|srandom|syscall|utime|dl_iterate_phdr|tcgetattr|tcsetattr|ttyname)$'
KEEP=$(filter_re "$LIBC_FUNCS" "$KEEP")
LIBM_FUNCS='^(sin|cos|tan|asin|acos|atan|atan2|sinf|cosf|tanf|atanf|atan2f|asinf|acosf|sqrt|sqrtf|pow|powf|exp|expf|exp2|exp2f|log|logf|log2|log2f|log10|log10f|fabs|fabsf|floor|floorf|ceil|ceilf|fmod|fmodf|round|roundf|trunc|truncf|fmax|fmaxf|fmin|fminf|hypot|hypotf|copysign|copysignf|sinh|cosh|tanh|asinh|acosh|atanh|cbrt|cbrtf|expm1|expm1f|log1p|log1pf|frexp|frexpf|ldexp|ldexpf|modf|modff|nearbyint|rint|rintf|lrint|lrintf|lround|lroundf|signbit|fpclassify|isnan|isinf|finite|finitef|sincosf|sincos|fegetround|fesetround|fegetenv|fesetenv|fegetexceptflag|fesetexceptflag|feclearexcept|fetestexcept|feraiseexcept|feholdexcept|feupdateenv)$'
KEEP=$(filter_re "$LIBM_FUNCS" "$KEEP")
SOCKET_FUNCS='^(socket|socketpair|bind|listen|accept|connect|send|sendto|sendmsg|recv|recvfrom|recvmsg|shutdown|setsockopt|getsockopt|getsockname|getpeername|getaddrinfo|freeaddrinfo|gai_strerror|inet_ntoa|inet_aton|inet_addr|inet_pton|inet_ntop|htons|htonl|ntohs|ntohl|gethostname|getpwnam|getpwuid|getgrnam|getgrgid)$'
KEEP=$(filter_re "$SOCKET_FUNCS" "$KEEP")
RLIMIT_SCHED_FUNCS='^(getrlimit|setrlimit|prlimit|prlimit64|getrusage|sched_yield|sched_getaffinity|sched_setaffinity|sched_getparam|sched_setparam|sched_getscheduler|sched_setscheduler|sched_get_priority_max|sched_get_priority_min|sched_rr_get_interval|getpriority|setpriority|setresuid|setresgid|getresuid|getresgid|getgroups|setgroups)$'
KEEP=$(filter_re "$RLIMIT_SCHED_FUNCS" "$KEEP")
TIME_FUNCS='^(time|times|clock|clock_gettime|clock_settime|clock_getres|clock_nanosleep|tzset|localtime|gmtime|mktime|timegm|strftime|strptime|asctime|ctime|difftime|localtime_r|gmtime_r)$'
KEEP=$(filter_re "$TIME_FUNCS" "$KEEP")

NUM=$(wc -l < "$KEEP" | tr -d ' ')
echo "stubs: $NUM symbols need stubbing"

{
    cat <<'HEADER'
// Auto-generated by platform/android/scripts/regen_stubs.sh.
//
// Bionic's dlopen() rejects .so files with GLOBAL UND symbols not provided
// by an already-loaded shared library. We link with
// -Wl,--unresolved-symbols=ignore-all so the link succeeds despite leftover
// desktop-only references (SDL2, NFD, RecompFrontend recompui/recompinput,
// re-spirv, ImGui_SDL2). To make dlopen happy we provide a tiny per-symbol
// stub for every UND name that logs the offending symbol via logcat and
// aborts. The boot path in src/main/android_run_game.cpp deliberately
// avoids invoking these code paths; if one of them does fire, the logcat
// line tells us exactly which to implement next.
//
// EXCLUDES (don't stub these - system libraries provide them):
//   - Android NDK funcs matching ^A[A-Z][a-zA-Z]*_  (libandroid.so)
//   - __android_log_*  (liblog.so)
//   - vk*, volk*       (libvulkan.so + volk)
//   - pthread_*, sem_* (Bionic libc)
//   - libm / libdl     (Bionic)
//   - libc++_shared.so symbols (_ZNSt*, _ZNKSt*, _ZN6__ndk1*, etc.)
//   - libc++abi __cxa_*, _Unwind_*, __gxx_personality_*
//
// Regenerate after the link-error list changes:
//   platform/android/scripts/regen_stubs.sh

#ifdef __ANDROID__
#include <android/log.h>
#include <stdlib.h>

extern "C" __attribute__((noreturn)) void android_unimplemented_stub_named(const char* name) {
    __android_log_print(ANDROID_LOG_FATAL, "BK64-Stub",
        "unimplemented Android stub called: %s -- aborting", name);
    abort();
}

HEADER

    while IFS= read -r sym; do
        # Skip empty lines just in case.
        [ -z "$sym" ] && continue
        # Each stub is its own function so the abort message names the symbol
        # that fired. Without the per-symbol body, every abort would look
        # identical in logcat and we'd be guessing which API to implement.
        printf 'extern "C" void %s() { android_unimplemented_stub_named("%s"); }\n' "$sym" "$sym"
    done < "$KEEP"

    cat <<'FOOTER'

#endif  // __ANDROID__
FOOTER
} > "$OUT"

echo "stubs: wrote $OUT"
rm -f "$ALL" "$KEEP"

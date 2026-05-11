plugins {
    id("com.android.application")
}

android {
    namespace = "com.banjorecomp.online"
    compileSdk = 35
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.banjorecomp.online"
        minSdk = 28
        targetSdk = 35
        versionCode = 1
        versionName = "0.1-android-spike"

        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                // Pass through every option needed by the Android cross-compile
                // path of our root CMakeLists.txt (Phases 1-7 patches).
                arguments += buildList {
                    addAll(
                        listOf(
                            "-DCMAKE_BUILD_TYPE=Release",
                            "-DANDROID_PLATFORM=android-28",
                            "-DANDROID_STL=c++_shared",
                            // Oboe's old cmake_minimum_required(3.4) needs this on
                            // CMake 4.x. Harmless on older versions.
                            "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
                            // Mali Valhall G57 routes SV_TARGET1 to the color
                            // attachment when dualSrcBlend is off — strip the
                            // second output from the ubershader.
                            "-DRT64_NO_DUAL_SOURCE_DYNAMIC_PS=ON"
                        )
                    )
                    // RasterPS Vulkan/Mali diagnostics (matches lib/rt64 CMake cache).
                    // Example: ./gradlew :app:assembleDebug -Prt64DiagRasterPs=1
                    val diag = project.findProperty("rt64DiagRasterPs")?.toString()
                    if (!diag.isNullOrBlank()) {
                        add("-DRT64_DIAG_RASTER_PS_MODE=$diag")
                    }
                    val diagVi = project.findProperty("rt64DiagVi")?.toString()
                    if (!diagVi.isNullOrBlank()) {
                        add("-DRT64_DIAG_VI_MODE=$diagVi")
                    }
                }
                // Build only the .so we actually want shipped.
                targets += "BanjoRecompiled"
            }
        }
    }

    externalNativeBuild {
        cmake {
            // Reuse the existing root CMakeLists.txt that already compiles
            // rt64 + N64ModernRuntime + libBanjoRecompiled.so for arm64-v8a.
            path = file("../../../CMakeLists.txt")
            // Stick to the cmake bundled with the SDK (3.22.x); keeps
            // Gradle from trying to pick up Homebrew CMake 4.x.
            version = "3.22.1"
        }
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
        }
        getByName("debug") {
            isJniDebuggable = true
            isDebuggable = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        // The .so ships unstripped (helps native crash reporting on Phase 9).
        // jniLibs.useLegacyPackaging = false
    }
}

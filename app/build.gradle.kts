plugins {
    id("com.android.application")
}

android {
    namespace = "hu.balazs.mirrortv"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "hu.balazs.mirrortv"
        minSdk = 26          // AAudio; Google TV devices run Android 10+
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"

        ndk {
            // 32-bit ARM is common on projectors, 64-bit on newer devices
            abiFilters += listOf("armeabi-v7a", "arm64-v8a")
        }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DTHIRD_PARTY_DIR=${rootProject.projectDir}/third_party",
                    "-DANDROID_STL=c++_static"
                )
            }
        }
    }

    signingConfigs {
        // Fixed key kept in the repo so every build can be installed over the previous one.
        create("sideload") {
            storeFile = file("../keystore/mirrortv.jks")
            storePassword = "mirrortv"
            keyAlias = "mirrortv"
            keyPassword = "mirrortv"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("sideload")
        }
        debug {
            signingConfig = signingConfigs.getByName("sideload")
        }
    }

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    lint {
        checkReleaseBuilds = false
        abortOnError = false
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }
}

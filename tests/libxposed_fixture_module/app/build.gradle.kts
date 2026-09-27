plugins {
    id("com.android.application")
}

val family = providers.gradleProperty("dartplantFamily").orElse("3.12.1").get()
require(family in setOf("3.4.4", "3.5.0", "3.12.1")) { "Unsupported exact VM family: $family" }
val nativeStrictFallback =
    providers.gradleProperty("dartplantNativeStrictFallback").orElse("false").get().toBoolean()
val sdkSources = providers.environmentVariable("DARTPLANT_DART_SDK_ROOT")
    .orElse(rootProject.file("../../../sdk").absolutePath)
// Allow the external fixture to exercise a candidate Dobby transaction API
// without mutating the pinned third_party/dobby submodule or staging a SHA.
val dobbySources = providers.environmentVariable("DARTPLANT_DOBBY_ROOT")
val ndkVersion = "30.0.15729638"

android {
    namespace = "dev.dartplant.integration"
    compileSdk = 36
    ndkVersion = ndkVersion

    defaultConfig {
        applicationId = "dev.dartplant.integration"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "0.1-$family"
        buildConfigField("boolean", "NATIVE_STRICT_FALLBACK", nativeStrictFallback.toString())

        ndk { abiFilters += "arm64-v8a" }
        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DDARTPLANT_MODULE_VM_ADAPTER_FAMILY=$family",
                    "-DDARTPLANT_DART_SDK_ROOT=${sdkSources.get()}",
                    "-DDARTPLANT_BUILD_ANDROID_MODULE=ON",
                    "-DDARTPLANT_BUILD_TESTS=OFF",
                    "-DDARTPLANT_BUILD_ANDROID_TESTS=OFF",
                )
                dobbySources.orNull?.let {
                    arguments += "-DDARTPLANT_DOBBY_ROOT=$it"
                }
                targets += "dartplant"
            }
        }
    }
    buildFeatures { buildConfig = true }
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }
    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("debug")
        }
    }
    lint {
        // The test APK is verified separately by check_libxposed_module.py.
        // Do not require AGP's extra release-lint compiler download to package
        // source-proven fixed adapter binaries in an offline build.
        checkReleaseBuilds = false
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    packaging {
        // VectorModuleClassLoader resolves APK!/lib/<abi> only for STORED ELF
        // entries. Compressed native libraries are invisible to that loader.
        jniLibs { useLegacyPackaging = false }
    }
}

dependencies {
    compileOnly("io.github.libxposed:api:102.0.0")
}

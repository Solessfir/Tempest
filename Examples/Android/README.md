# Android example

A minimal Vulkan window built through Tempest's CMake Android packaging helper.
The application is C++ only and needs no assets. Tempest provides the Android activity.

Install JDK 17 and Android SDK packages `platforms;android-35`, `build-tools;35.0.0`,
`ndk;27.0.12077973` and `cmake;3.22.1`. Set `JAVA_HOME` and `ANDROID_HOME` to their locations.
Use CMake 3.22 or newer and Ninja for the packaging project.

From the Tempest repository root:

```sh
cmake -S Examples/Android -B build/android-example -G Ninja
cmake --build build/android-example --target TempestExample-apk
```

Output: `build/android-example/TempestExample/app/build/outputs/apk/release/app-release.apk`.
Release APKs use the local debug key unless distribution signing is configured.

```sh
adb install -r build/android-example/TempestExample/app/build/outputs/apk/release/app-release.apk
adb shell am start -n org.tempest.example/org.tempest.TempestNativeActivity
```

Generation itself requires only CMake and the selected build tool, not Java or an SDK.
The generated project can also be opened in Android Studio.
Do not edit or commit generated Gradle files, APKs or caches.

## Using the helper in another application

Create a separate `project(... LANGUAGES NONE)` for packaging and call
`tempest_android_application`, as in this example. Its `NATIVE_SOURCE_DIR` points
to the existing native CMake project, not the packaging project.
The native project builds a shared library and calls `tempest_android_native_target`
to retain Android entry points and enable 16 KiB page alignment.
Desktop projects do not invoke the packaging helper and need no Android tools.

Required arguments: `APPLICATION_ID`, `NATIVE_SOURCE_DIR`, `NATIVE_TARGET`, `LIBRARY_NAME`.
The library name must match the target's `OUTPUT_NAME`, without `lib` or `.so`.

Optional application configuration:

- `LABEL`, `VERSION_CODE`, `VERSION_NAME`: app metadata.
- `MANIFEST`: a complete custom manifest, including the Tempest activity and library metadata.
- `JAVA_DIRS`, `RESOURCE_DIRS`, `ASSET_DIRS`: application-owned source directories.
- `DEPENDENCIES`, `CMAKE_ARGUMENTS`, `CPP_FLAGS`, `PROGUARD_FILES`, `NO_COMPRESS`: lists.
- `SHRINK_RELEASE`, `ANDROIDX`, `REPACKAGE`: enable shrinking, AndroidX or full ZIP repackaging.
- `ASSET_PROPERTY`: optional Gradle property naming an additional asset directory.

Paths are relative to the packaging CMakeLists.txt. Tempest's Java sources and JNI keep rules
are included automatically. Common tool versions and ABIs are `TEMPEST_ANDROID_*` CMake cache
settings; `TEMPEST_ANDROID_BUILD_TYPE` selects `Release` (default) or `Debug`.
The Gradle wrapper pins Gradle 8.9 and verifies its distribution checksum.

For distribution signing, set all four environment variables `TEMPEST_KEYSTORE`,
`TEMPEST_KEY_ALIAS`, `TEMPEST_STORE_PASSWORD`, `TEMPEST_KEY_PASSWORD`.
`SIGNING_ENV_PREFIX` changes the `TEMPEST` prefix. Secrets are read only by Gradle at build time,
never written to generated files or the CMake cache.
Gradle properties `tempestVersionCode` and `tempestVersionName` override versions;
`PROPERTY_PREFIX` changes the `tempest` prefix.

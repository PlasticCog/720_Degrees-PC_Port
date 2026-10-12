# 720 Degrees for Android

The game for Android phones and tablets (Android 7.0 or newer, 64-bit ARM or x86-64). It is the
same program as on the desktop (`port/src/app/main.cpp`), built by CMake as `libmain.so` and
run by SDL's Java activity. The Java glue in `app/src/main/java/org/libsdl/app` and the Gradle
wrapper come unchanged from SDL 3.2.28's `android-project` (zlib licence).

## Building

You need the Android SDK with NDK 27.2.12479018 and CMake 3.22.1 (Android Studio's SDK Manager
installs them, or Gradle downloads them), and Java 17 or newer (the one inside Android Studio
works).

    cd port/android
    ./gradlew assembleDebug        # app/build/outputs/apk/debug/app-debug.apk

Tell Gradle where the SDK is with `ANDROID_HOME` or a `local.properties` file containing
`sdk.dir=<path>`. CMake downloads SDL 3.2.28 during the build. The ROMs are not needed to build.

For a signed release APK (`./gradlew assembleRelease`), put the keystore details into
`~/.gradle/gradle.properties`, never into the repository:

    D720_KEYSTORE=C:/path/to/release.jks
    D720_KEYSTORE_PASSWORD=...
    D720_KEY_ALIAS=...
    D720_KEY_PASSWORD=...

Updates must be signed with the same key, or Android refuses to install them over the old
version.

## Playing

On the first start the game asks for the ROM files. Tap **Choose ROM files...** and pick MAME's
`720.zip`, or select all 37 loose files at once. The game keeps a copy in its private storage.
Alternatively, copy the files over USB into `Android/data/com.plasticcog.degrees720/files/Game`.

Touch controls (`port/src/app/touch.cpp`):

- **Steer:** put your left thumb down anywhere on the left half of the screen and slide it in
  the direction the skater should point.
- **JUMP** and **KICK:** bottom right.
- **COIN**, **1P**, **2P:** top right. Insert a coin, then press 1P (or JUMP).
- **MENU:** the debug menu. **PAUSE:** pause.
- **Back button:** closes a window, then pauses, and quits when pressed again while paused.

Gamepads work as on the desktop. The touch controls fade out while one is used and come back
at the next touch. Level mods go into `Android/data/com.plasticcog.degrees720/files/mods`.

# re:xsystem4

[![Build APK](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml/badge.svg)](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](xsystem4/COPYING)
[![Platform](https://img.shields.io/badge/Platform-Android%20%28arm64--v8a%29-3DDC84.svg)](https://github.com/TomorrowX6/re-xsystem4/releases)

**English** | **[简体中文](README.zh-CN.md)** | **[日本語](README.ja.md)**

**re:xsystem4** is a heavily customized and extended native Android engine for AliceSoft **System 4.x** games, built upon [xsystem4](https://github.com/nunuhara/xsystem4) and [xsystem4-android](https://github.com/kichikuou/xsystem4-android).

Written in pure C/C++, SDL2, and OpenGL ES 3.0, it directly interprets AIN bytecode and renders original game assets on Android `arm64-v8a` devices without relying on Wine, Winlator, or any x86/x64 translation layer. In addition to supporting classic System 4.0 titles, it provides comprehensive implementations and mobile-focused optimizations for late-era large-scale AliceSoft titles (such as *Rance X / Rance 10 -Showdown-*) powered by **AIN v12**, **PartsEngine v12**, **FLAT v15 skeletal & particle animations**, **SealEngine 3D**, **APEG video streaming**, and **SaveData v26**.

---

## Key Features

### 1. AIN v12 Virtual Machine & HLL Extensions
- **AIN v12 Bytecode Support**: Full support for AIN v12 opcodes, delegates (`DG_*`), local reference management, deep struct copying, and reflection interfaces.
- **Full HLL Coverage**: Implements and extends `PartsEngine` (804/804 functions), `SealEngine`, `ADVEngine`, `SaveData`, `RSave`, `APEG`, `SoundEngine`, `FileOperation`, `FileDialog`, `MsgSkip`, `Math`, `PassRegister`, `BanMisc`, and other core HLL libraries.

### 2. PartsEngine Rendering & 3D Transform Pipeline
- **FLAT v15 Animation & Particle System**:
  - Supports zlib-compressed `ELNA` / `FLAT` v15 timelines, keyframe interpolation, script frame triggers, and composite graphic layers.
  - Implements nested ancestor keyframe birth-matrix inheritance (`parent_key_mode == 2`) and real-time particle position tracking.
  - Supports additive blending for glow/route layers and `.flat` origin offset correction.
- **3D Perspective Rotation & Back-Face Culling**:
  - Supports X/Y/Z Euler perspective rotation matrices for UI parts (e.g., card flip animations), hierarchical parent-child 3D transform cascading, and automatic front/back-face culling.
- **Construction Process Filters**:
  - Full implementation of text layout rendering, surface crop/copy, color fill, alpha blending, Gaussian blur, grayscale conversion, and HSL/HSV adjustment construction processes.
- **Editor Layer Filtering & Accurate Hit-Testing**:
  - Automatically suppresses internal editor placeholder parts (`CheckBox`, `RadioButton`, `■ボタン上敷き`, etc.) that should not appear at runtime.
  - Excludes hidden (`alpha == 0`) parts and transparent `.flat` containers from hit-testing, resolving localized touch unresponsiveness caused by invisible overlay bounds.

### 3. Multimedia & Performance Optimization
- **OpenGL ES 3.0 Hardware Acceleration**: GLES 3.0 shader pipeline with offscreen framebuffer (FBO) composition and 16 KB page alignment (Android 15+ ready).
- **APEG Video Decoder**: Built-in MPEG-1 video and Ogg Vorbis audio synchronized decoder, fixing macroblock bounds issues and green screen flickering during cutscene transitions.
- **Per-Frame Texture GC & Fast-Forward Throttling**: Reclaims deferred textures during the active main loop and throttles redundant surface rebuilds during continuous message skipping (`MsgSkip` / `Ctrl`), preventing frame drops and memory spikes.

### 4. Touch Controls & Built-in Save Manager
- **Two-Phase Touch Event Queue**: Redesigned Android touch sampling and C-level event dispatch pipeline to ensure strict press/release coordinate and frame-order consistency during rapid tapping.
- **Bidirectional PC Save Compatibility**: Full `SaveData` (v26 `.asd`) recursive serialization and `RSave` (`.rsd`) support, directly compatible with original PC saves.
- **Built-in Save Manager**: Import saves from external ZIP archives, folders, or individual files; export internal saves to ZIP archives or external directories; and manage, back up, rename, or delete individual save files directly inside the launcher.

---

## System Requirements

| Component | Minimum | Recommended |
| :--- | :--- | :--- |
| **CPU Architecture** | `arm64-v8a` (64-bit ARM) | Snapdragon 8 Series / Dimensity 8000+ or equivalent |
| **OS Version** | Android 8.0 (API 26) or higher | Android 11.0 or higher |
| **Graphics API** | OpenGL ES 3.0 | OpenGL ES 3.2 |
| **Memory (RAM)** | 4 GB RAM | 6 GB RAM or higher |
| **Storage** | Depends on game size (5 GB+ free space recommended) | UFS 3.0+ Flash Storage |

---

## Download & Installation

1. Visit the [**Releases**](https://github.com/TomorrowX6/re-xsystem4/releases) page to download the latest signed APK `re-xsystem4-arm64-v8a.apk`.
2. You can also download the latest automated build artifact from [**GitHub Actions**](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml).
3. Install the APK on your Android device and launch **re:xsystem4**.

---

## Importing Games & Managing Saves

To run a game, prepare the complete original PC game files (containing `AliceStart.ini` or `System40.ini`, `*.ain` bytecode file, `*.afa` / `*.ald` archives, `Data/`, `Movie/`, etc.).

### Method 1: Import from Game Directory (Recommended)
1. Copy the complete PC game folder to any folder on your device's internal storage (e.g., `/sdcard/Download/Rance10/`).
2. Open **re:xsystem4** and tap **Import Game Directory (`导入游戏目录`)**.
3. Select the game folder in the system directory picker to import.

### Method 2: Import from ZIP Archive
1. Package the game folder into a `.zip` archive (with `AliceStart.ini` / `System40.ini` either at the archive root or inside a single top-level folder).
2. Open **re:xsystem4** and tap **Import from ZIP (`从ZIP导入`)**.
3. Select the `.zip` archive; the app will automatically extract and register the game.

### Save Manager (External Import / Internal Export / Save Management)
The main launcher includes a **Save Manager (`存档管理器`)** button between **Import from ZIP (`从ZIP导入`)** and **GitHub**:
- **Import Saves from External Storage**: Import saves from an external `.zip` archive, an external folder, individual save files (`GlobalSave.asd`, `Collection.asd`, `SaveData*.asd`, etc.), or sync from `Android/data/io.github.rance10.nativeport/files/<GameDir>/SaveData`.
- **Export Internal Saves**: Package all internal saves into an external `.zip` archive, export directly to any external folder, or sync to the external game directory's `SaveData/` folder.
- **Manage Save Files**: Switch between game save directories, inspect file types, sizes, and modification timestamps, and export, back up (`.bak`), rename, or delete individual save files.

---

## Touch Controls

| In-Game Action | Touch Gesture |
| :--- | :--- |
| **Left Mouse Click / Confirm** | Single-finger tap on the target position |
| **Right Mouse Click / Cancel / Menu** | Single-finger tap on the left/right letterbox bars, or two-finger tap anywhere |
| **Skip Text (Hold Ctrl)** | Single-finger long press and hold |
| **Mouse Wheel Up / Down (Backlog / Scroll)** | Two-finger swipe down / up |
| **Mouse Cursor Hover / Drag** | Single-finger press and drag across the screen |

---

## Repository Structure

```text
.
├── .github/workflows/
│   └── build.yml           # GitHub Actions CI workflow for building and signing APK
├── CMakeLists.txt          # Top-level CMake ExternalProject script for cross-compiling dependencies & xsystem4
├── build-shared-libs.sh    # Entry script to build Android JNI native shared libraries (.so)
├── collect-licenses.sh     # Script to collect third-party open-source licenses
├── cmake/                  # CMake modules for fetching FreeType and SDL2
├── xsystem4/               # Extended xsystem4 engine and libsys4 core source code
│   ├── include/            # Engine headers
│   ├── src/                # VM, rendering pipeline, audio, video, HLL modules, and PartsEngine
│   ├── shaders/            # OpenGL ES 3.0 shaders
│   └── subprojects/
│       └── libsys4/        # Parsers for AIN, AFA, FLAT, EX, ACF, and save formats
└── project/                # Android Gradle application project
    └── app/
        ├── build.gradle    # Android build and signing configuration
        └── src/main/
            ├── AndroidManifest.xml
            ├── java/io/github/kichikuou/xsystem4/  # Launcher, Save Manager, directory/ZIP importer & SDLActivity
            ├── jniLibs/arm64-v8a/                  # Output directory for compiled native .so libraries
            └── res/                                # Layouts, icons, and string resources
```

---

## Building from Source

### 1. Prerequisites
Build on Linux (Ubuntu 22.04 / 24.04 recommended) or macOS with the following tools installed:
- **JDK 17**
- **Android SDK** (Platform API 34+, Build-Tools 34.0.0+)
- **Android NDK** (r26d or r27c recommended)
- **CMake** (3.22+) & **Ninja**
- Build utilities: `flex`, `bison`, `pkg-config`, `autoconf`, `automake`, `libtool`, `python3`, `git`

On Ubuntu / Debian:
```bash
sudo apt-get update
sudo apt-get install -y ninja-build bison flex autoconf automake libtool pkg-config cmake git python3 openjdk-17-jdk
```

### 2. Build Steps

1. **Clone the repository**:
   ```bash
   git clone https://github.com/TomorrowX6/re-xsystem4.git
   cd re-xsystem4
   ```

2. **Set Android SDK & NDK environment variables**:
   ```bash
   export ANDROID_SDK_ROOT=/path/to/android-sdk
   export ANDROID_NDK_HOME=/path/to/android-sdk/ndk/27.2.12479018
   ```

3. **Cross-compile native shared libraries (`arm64-v8a`)**:
   ```bash
   ./build-shared-libs.sh
   ```
   This script automatically downloads and cross-compiles `libwebp`, `libjpeg-turbo`, `libpng`, `libogg`, `libvorbis`, `libsndfile`, `freetype`, `SDL2`, `cglm`, `libffi`, `libdeflate`, and `xsystem4`, installing the resulting `.so` files into `project/app/src/main/jniLibs/arm64-v8a/`.

4. **Assemble the Android APK**:
   ```bash
   cd project
   ./gradlew assembleRelease
   ```
   The signed APK will be generated at `project/app/build/outputs/apk/release/app-release.apk`.

### 3. Automated Signing in CI
The repository includes a GitHub Actions workflow ([`.github/workflows/build.yml`](.github/workflows/build.yml)). To use your own keystore in a fork, configure the following repository secrets under `Settings -> Secrets and variables -> Actions` (if omitted, a temporary debug signing key is generated automatically):
- `KEYSTORE_BASE64`: Base64-encoded keystore file
- `KEYSTORE_PASSWORD`: Keystore password
- `KEY_ALIAS`: Key alias
- `KEY_PASSWORD`: Key password

---

## Credits & License

- [nunuhara/xsystem4](https://github.com/nunuhara/xsystem4) & [nunuhara/libsys4](https://github.com/nunuhara/libsys4): Core cross-platform System 4 engine and file format parsers.
- [kichikuou/xsystem4-android](https://github.com/kichikuou/xsystem4-android): Android port framework for xsystem4.

Licensed under the [GNU General Public License v2.0 (GPL-2.0)](xsystem4/COPYING).

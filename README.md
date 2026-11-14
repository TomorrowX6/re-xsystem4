# re:xsystem4

[![Build APK](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml/badge.svg)](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](xsystem4/COPYING)
[![Platform](https://img.shields.io/badge/Platform-Android%20%28arm64--v8a%29-3DDC84.svg)](https://github.com/TomorrowX6/re-xsystem4/releases)

**English** | **[简体中文](README.zh-CN.md)** | **[日本語](README.ja.md)**

**re:xsystem4** is a heavily customized and extended native Android engine for AliceSoft **System 4.x** games, built upon [xsystem4](https://github.com/nunuhara/xsystem4) and [xsystem4-android](https://github.com/kichikuou/xsystem4-android).

Written in pure C/C++, SDL2, and OpenGL ES 3.0, it directly interprets AIN bytecode and renders original game assets on Android `arm64-v8a` devices without relying on Wine, Winlator, or any x86/x64 translation layer. Beyond classic System 4.0 titles, it provides full implementations, MOD compatibility, and mobile-focused optimizations for late-era large-scale AliceSoft titles (such as *Rance X / Rance 10 -Showdown-* and community expansions like *Sunset MOD / 日不落 MOD*) powered by **AIN v12**, **PartsEngine v12**, **FLAT v15 skeletal & particle animations**, **SealEngine 3D**, **APEG video streaming**, and **SaveData v26**.

---

## Key Features

### 1. AIN v12 Virtual Machine, HLL & Deep MOD Compatibility
- **AIN v12 Bytecode Support**: Full support for AIN v12 opcodes, delegates (`DG_*`), local reference management, deep struct copying, `CALLFUNC` `struct_page` inheritance, and reflection interfaces.
- **Full HLL Coverage**: Implements and extends `PartsEngine` (804/804 functions), `SealEngine`, `ADVEngine`, `SaveData`, `RSave`, `APEG`, `SoundEngine`, `FileOperation`, `FileDialog`, `MsgSkip`, `Math`, `PassRegister`, `BanMisc`, and other core HLL libraries.
- **Large-Scale MOD Compatibility**:
  - Archive-indexed AFA asset IDs and per-entry `FILETIME` timestamp + MOD priority resolution across overlapping `.afa` archives, ensuring localized base/hotfix assets (`CG` / `CG6`) and MOD overrides coexist cleanly without collisions.
  - Canonical `.ex` data table prioritization, incremental MOD patch layering, and automatic Shift-JIS / GBK / UTF-8 text & font fallback.
  - Cross-version MOD save hardening (`resume` schema drift protection and automatic fallback card rendering when saves reference removed/missing EX entries).

### 2. PartsEngine Rendering & 3D Transform Pipeline
- **FLAT v15 Animation & Particle System**:
  - Supports zlib-compressed `ELNA` / `FLAT` v15 timelines, keyframe interpolation, script frame triggers, radial/fixed-end particle emitter trajectories, and `.flat` binary/timeline caching.
  - Implements nested ancestor keyframe birth-matrix inheritance (`parent_key_mode == 2`), ancestor alpha clipper inheritance, and additive blending for glow/route layers.
- **3D Perspective Rotation & Back-Face Culling**:
  - Supports X/Y/Z Euler perspective rotation matrices for UI parts (e.g., card flip animations), hierarchical parent-child 3D transform cascading, and automatic front/back-face culling.
- **Construction Process (`cproc`) Pipeline**:
  - Full implementation of text layout rendering, surface crop/copy, color fill, alpha blending, Gaussian blur, grayscale conversion, HSL/HSV adjustment, pixel snapshots (`m_procOrg` preservation), and gauge clipping.
- **Editor Layer Filtering & Accurate Hit-Testing**:
  - Automatically suppresses internal editor placeholder parts (`CheckBox`, `RadioButton`, `■ボタン上敷き`, etc.) and excludes hidden (`alpha == 0`) parts and transparent `.flat` containers from hit-testing.

### 3. Performance, Scheduling & Multimedia Optimization
- **OpenGL ES 3.0 Hardware Acceleration**: GLES 3.0 shader pipeline with offscreen framebuffer (FBO) composition, early `glFlush()` GPU pipelining, and 16 KB page alignment (Android 15+ ready).
- **Android Thread Scheduling & Frame Pacing**:
  - Elevates the render thread to `THREAD_PRIORITY_URGENT_DISPLAY` (`-8`, `50us` timer slack) and the audio mixing callback thread to `ANDROID_PRIORITY_URGENT_AUDIO` (`-16`).
  - Integrates Android 12+ **ADPF (`APerformanceHintManager`)** for dynamic CPU boost on heavy frames, paired with `eglPresentationTimeANDROID` and seamless `Surface.setFrameRate` 60fps VSync pacing on 60Hz/120Hz displays.
  - Automatically suspends audio mixing when backgrounded to eliminate idle CPU/battery drain.
- **APEG Video Decoder & Texture GC**: Built-in MPEG-1 video and Ogg Vorbis audio synchronized decoder, per-frame deferred texture garbage collection, `SceneParty` card loading acceleration, and fast-forward (`MsgSkip` / `Ctrl`) surface rebuild throttling.

### 4. Online & Local Installer, Multi-Format Archive Engine, Save Manager & Game Modifier
- **Online & Local Dual-Mode Installation (Base Game & MODs)**:
  - **Dual-Line Online Base Game Installer (EdgeOne / Cloudflare)**: One-click cloud download of the complete Chinese Rance 10 base game via **EdgeOne** or **Cloudflare** CDN lines, with seamless cross-line HTTP `Range` resume and per-file SHA-256 verification (no double storage overhead for archive extraction).
  - **Background Download & System Notifications**: Powered by an Android Foreground Service with CPU/Wi-Fi locks so downloads and extractions continue at full speed when backgrounded or with the screen off; includes a **Background** dialog minimize action, live notification progress bar & speed, notification **Pause** button, and completion alerts.
  - **Native Full-Screen MOD Manager & Portal (`rance10sns.cn`)**: Unified native UI matching the Save Manager and Game Modifier; automatically fetches the full MOD release catalog and changelogs, tracks installed versions, detects updates, and supports smart incremental patch updates, automatic base + patch combo installation, clean switching across historical versions, and an integrated portal login/download Activity.
- **Native `.zip` / `.rar` / `.7z` Streaming Extractor**: Powered by `libarchive` + `xz` (`liblzma`), supporting ZIP/Zip64, RAR4/RAR5 (including solid archives and large dictionaries), and 7z (LZMA/LZMA2/PPMd/BCJ) with automatic UTF-8 / UTF-16LE / GBK filename detection.
- **Non-Destructive MOD Manager**: Install full or incremental MODs online or from local `.zip` / `.rar` / `.7z` archives and folders with automatic vanilla file backup and one-tap **Restore Vanilla (`还原原版`)**.
- **Built-in Save Manager**: Bidirectional PC save compatibility (`SaveData` v26 `.asd` & `RSave` `.rsd`) with atomic `fsync` writes; import saves from `.zip` / `.rar` / `.7z` archives, folders, or individual files; export saves to ZIP or external folders; and back up, rename, or delete individual files.
- **Standalone & In-Game Floating Modifier (Default ON)**:
  - **In-Game Floating Modifier (`悬浮窗修改器`)**: Enabled by default via a draggable overlay icon; provides **live Font Size scaling (`75%–160%` with overflow-safe layout adaptation)**, **live Scrollbar width & touch-thumb enlargement (`100%–300%` with swipe-hijack protection)**, 11 real-time battle/campaign locks (Max HP, Infinite AP, 999 Combo, Round 1 Lock, One-Hit Kill, Enemy 0 ATK/DEF, Boost & Crit, Infinite Skill Uses, Infinite Zippo, Resources >= 999, 100% Treasure Drop), persistent **Disable Streamer Mode (`关闭主播模式`)** & cheat code submission for MODs, a draggable **`⏩ Ctrl`** latched skip button (enabled by default), and live resource/party/world/card editors.
  - **Standalone Game Modifier (`游戏修改器`)**: Offline `GlobalSave.asd` / `Collection.asd` and `SaveData*.asd` slot editor, one-click full-unlock save installation with automatic `.modbak` backup, **Font Size** & **Scrollbar** presets, and master toggles for the floating modifier and **`⏩ Ctrl`** skip button (disabling **`⏩ Ctrl`** also blocks summoning it in-game).

---

## System Requirements

| Component | Minimum | Recommended |
| :--- | :--- | :--- |
| **CPU Architecture** | `arm64-v8a` (64-bit ARM) | Snapdragon 8 Series / Dimensity 8000+ or equivalent |
| **OS Version** | Android 8.0 (API 26) or higher | Android 12.0 (API 31) or higher (for ADPF & seamless frame rate) |
| **Graphics API** | OpenGL ES 3.0 | OpenGL ES 3.2 |
| **Memory (RAM)** | 4 GB RAM | 6 GB RAM or higher |
| **Storage** | Depends on game size (5 GB+ free space recommended) | UFS 3.0+ Flash Storage |

---

## Download & Installation

1. Visit the [**Releases**](https://github.com/TomorrowX6/re-xsystem4/releases) page to download the latest signed APK `re-xsystem4-arm64-v8a.apk`.
2. You can also download the latest automated build artifact from [**GitHub Actions**](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml).
3. Install the APK on your Android device and launch **re:xsystem4**.

---

## Game, MOD, Save & Modifier Guide

To run a game, prepare the complete original PC game files (containing `AliceStart.ini` or `System40.ini`, `*.ain` bytecode file, `*.afa` / `*.ald` archives, `Data/`, `Movie/`, etc.).

### 1. Online Install (`在线安装`)
Tap **Online Install (`在线安装`)** on the launcher and choose:
- **Install Base Game**: Select **EdgeOne** or **Cloudflare** to download the complete Chinese Rance 10 base game (~3.75 GB) with cross-line pause/resume support and SHA-256 integrity verification.
- **Install MOD**: Opens the native **MOD Manager** screen to check for new releases against the currently installed version, perform one-tap smart updates, browse changelogs and switch to any historical version, open the integrated `https://rance10sns.cn` portal screen, or restore the vanilla game.

### 2. Local Install (`本地安装`)
Tap **Local Install (`本地安装`)** on the launcher and choose:
- **Install Base Game**: Import the base game from a local **`.zip` / `.rar` / `.7z` archive** or an **external folder** (e.g., `/sdcard/Download/Rance10/`).
- **Install MOD**: Install full or incremental MODs from a local **`.zip` / `.rar` / `.7z` archive** or **external folder** with automatic vanilla file backup and one-tap **Restore Vanilla (`还原原版`)**.

### 3. Save Manager (`存档管理器`)
- **Import Saves**: Import saves from an external `.zip` / `.rar` / `.7z` archive, external folder, or individual save files (`GlobalSave.asd`, `Collection.asd`, `SaveData*.asd`, etc.).
- **Export Saves**: Package all internal saves into a `.zip` archive or export directly to any external folder.
- **Manage Save Files**: Switch or create save directories per game/MOD, inspect file tags, sizes, and modification timestamps, and export, back up (`.bak`), rename, or delete individual save files.

### 4. Game Modifier (`游戏修改器` & In-Game Floating Overlay)
- **In-Game Floating Modifier**: Enabled by default when launching a game (draggable icon in the top-left corner). Tap it anytime to preview and adjust **Font Size** and **Scrollbar Size** live (without modifying save files), toggle real-time battle/resource locks, disable MOD streamer mode (persisted automatically across restarts), enter MOD cheat codes, toggle the draggable **`⏩ Ctrl`** skip button, or edit live campaign/battle stats.
- **Launcher Game Modifier**: Edit `GlobalSave.asd` / `Collection.asd` and individual `SaveData*.asd` slots offline, install/restore full-unlock saves, configure **Font Size** and **Scrollbar Size**, or toggle the floating modifier icon and **`⏩ Ctrl`** skip button on/off (enabled by default; turning **`⏩ Ctrl`** off also disables summoning it in-game).

---

## Touch Controls

| In-Game Action | Touch Gesture |
| :--- | :--- |
| **Left Mouse Click / Confirm** | Single-finger tap on the target position |
| **Right Mouse Click / Cancel / Menu** | Single-finger tap on the left/right letterbox bars, or two-finger tap anywhere |
| **Skip Text (`Ctrl`)** | Single-finger long press and hold, or tap the floating **`⏩ Ctrl`** pill to latch skip |
| **Mouse Wheel Up / Down (Backlog / Scroll)** | Two-finger swipe down / up |
| **Cursor Hover / Card Drag-Scroll** | Single-finger press and drag across the screen |
| **Exit Confirmation Dialog** | System Back gesture / button (triggers in-game `SetHookCloseApp` confirmation) |

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
├── archive_jni/            # Native libarchive + xz (liblzma) JNI streaming extractor (.zip / .rar / .7z)
├── xsystem4/               # Extended xsystem4 engine, modifier core, and libsys4 source code
│   ├── include/            # Engine headers
│   ├── src/                # VM, rendering pipeline, ADPF pacing, audio, video, HLL, PartsEngine & modifier
│   ├── shaders/            # OpenGL ES 3.0 shaders
│   └── subprojects/
│       └── libsys4/        # Parsers for AIN, AFA, FLAT, EX, ACF, and save formats
└── project/                # Android Gradle application project
    └── app/
        ├── build.gradle    # Android build and signing configuration
        └── src/main/
            ├── AndroidManifest.xml
            ├── java/io/github/kichikuou/xsystem4/  # Launcher, MOD/Save Manager, Modifier, ArchiveExtractor & Activity
            ├── jniLibs/arm64-v8a/                  # Output directory for compiled native .so libraries
            └── res/                                # Layouts, icons, full-unlock saves, and multilingual resources
```

---

## Building from Source

### 1. Prerequisites
Build on Linux (Ubuntu 22.04 / 24.04 recommended) or macOS with the following tools installed:
- **JDK 17**
- **Android SDK** (Platform API 34+, Build-Tools 34.0.0+)
- **Android NDK** (r26d – r29 recommended)
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
   This script automatically downloads and cross-compiles `libwebp`, `libjpeg-turbo`, `libpng`, `libogg`, `libvorbis`, `libsndfile`, `freetype`, `SDL2`, `cglm`, `libffi`, `libdeflate`, `xz` (`liblzma`), `libarchive`, `archive_jni`, and `xsystem4`, installing the resulting `.so` files into `project/app/src/main/jniLibs/arm64-v8a/`.

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
- [xiexiahai/xsystem4-rance10-android](https://github.com/xiexiahai/xsystem4-rance10-android): Project Features & Enhancements.

Licensed under the [GNU General Public License v2.0 (GPL-2.0)](xsystem4/COPYING).

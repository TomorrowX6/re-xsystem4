# re:xsystem4

[![Build APK](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml/badge.svg)](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](xsystem4/COPYING)
[![Platform](https://img.shields.io/badge/Platform-Android%20%28arm64--v8a%29-3DDC84.svg)](https://github.com/TomorrowX6/re-xsystem4/releases)

**re:xsystem4** 是基于 [xsystem4](https://github.com/nunuhara/xsystem4) 与 [xsystem4-android](https://github.com/kichikuou/xsystem4-android) 深度定制和扩展的 AliceSoft **System 4.x** Android 原生引擎。

本项目通过纯 C/C++、SDL2 与 OpenGL ES 3.0 在 Android `arm64-v8a` 设备上直接解释执行 AIN 字节码并渲染原版资源，无需借助 Wine、Winlator 或任何 x86/x64 指令转译层。除兼容传统 System 4.0 游戏外，重点针对后期大型作品（如《兰斯10 -决战- / Rance X》）所使用的 **AIN v12**、**PartsEngine v12**、**FLAT v15 骨骼与粒子动画**、**SealEngine 3D**、**APEG 视频流** 以及 **SaveData v26** 进行了完整实现与移动端深度优化。

---

## 核心特性

### 1. AIN v12 虚拟机与HLL扩展
- **AIN v12 字节码支持**：完整解析并执行 AIN v12 新增操作码、委托（Delegate / `DG_*`）、局部引用管理、结构体深拷贝与反射接口。
- **HLL 模块覆盖**：实现并完善 `PartsEngine`、`SealEngine`、`ADVEngine`、`SaveData`、`RSave`、`APEG`、`SoundEngine`、`FileOperation`、`FileDialog`、`MsgSkip`、`Math`、`PassRegister`、`BanMisc` 等核心 HLL 库。

### 2. PartsEngine 渲染与 3D 变换管线
- **FLAT v15 动画与粒子系统**：
  - 支持 zlib 压缩的 `ELNA` / `FLAT` v15 动画时间轴、骨骼关键帧插值、脚本帧触发及图文混合图层。
  - 实现嵌套祖先关键帧出生矩阵继承（`parent_key_mode == 2`）与实时粒子位置跟随。
  - 支持发光路线图层（Additive Blending 加法混合）与 `.flat` 原点偏移校正。
- **3D 透视旋转与背面剔除**：
  - 支持 UI 部件（如卡牌翻转动画）的 X/Y/Z 三轴欧拉角透视旋转矩阵、父子节点 3D 变换级联以及正反面自动翻转剔除。
- **构建工序（Construction Process）滤镜**：
  - 完整支持文本排版绘制、表面裁剪复制、色彩填充、Alpha 混合、高斯模糊、灰阶化、色相/饱和度/明度（HSL/HSV）调整等构建工序指令。
- **编辑器图层过滤与精准命中测试**：
  - 自动过滤不应在运行期显示的编辑器占位组件（如 `CheckBox`、`RadioButton`、`■ボタン上敷き` 等辅助层）。
  - 剔除 `alpha == 0` 隐藏部件与透明 `.flat` 容器的无效碰撞，解决透明遮挡层导致的局部触控失灵问题。

### 3. 多媒体与性能优化
- **OpenGL ES 3.0 硬件加速**：基于 GLES 3.0 着色器管线与离屏帧缓冲（FBO）合成，支持 16 KB 页面对齐（Android 15+ 兼容）。
- **APEG 视频解码**：内置 MPEG-1 视频流与 Ogg Vorbis 音频同步解码器，修复宏块解码越界与过场动画切换时的绿屏闪烁问题。
- **帧内内存回收与快进节流**：在游戏主循环中及时清理延迟释放纹理（Deferred Texture Garbage Collection），并对持续快进对话（MsgSkip / Ctrl）时的贴图重绘进行节流优化，避免长时间快进引发的卡顿与发热。

### 4. 移动端触控与存档互通
- **双阶段触控事件队列**：重构 Android 触控采样与 C 层事件分发管线，保证高频连点、快速切页时的按下/抬起坐标与帧序严格一致。
- **PC 原版存档双向兼容**：完整实现 `SaveData`（v26 `.asd`）递归序列化与 `RSave`（`.rsd`）读写，手机端存档可与 PC 原版直接互通。

---

## 系统与硬件要求

| 项目 | 最低配置要求 | 推荐配置 |
| :--- | :--- | :--- |
| **处理器架构** | `arm64-v8a`（64 位 ARM） | 骁龙 8 系列 / 天玑 8000+ 或同级芯片 |
| **操作系统** | Android 8.0（API 26）及以上 | Android 11.0 及以上 |
| **图形接口** | OpenGL ES 3.0 | OpenGL ES 3.2 |
| **运行内存** | 4 GB RAM | 6 GB RAM 及以上 |
| **存储空间** | 取决于游戏体积（大型作品建议预留 5 GB 以上） | UFS 3.0+ 闪存 |

---

## 下载与安装

1. 前往本仓库的 [**Releases 页面**](https://github.com/TomorrowX6/re-xsystem4/releases) 下载最新版本的已签名安装包 `re-xsystem4-arm64-v8a.apk`。
2. 也可以在 [**GitHub Actions**](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml) 中下载最新提交自动构建生成的 Artifact 产物。
3. 在 Android 设备上安装 APK 并启动 **re:xsystem4**。

---

## 游戏导入指南

要运行游戏，需准备好 PC 原版完整游戏文件（需包含 `AliceStart.ini` 或 `System40.ini` 配置文件、`*.ain` 脚本文件，以及对应的 `*.afa` / `*.ald` 资源包、`Data/`、`Movie/` 等子目录）。

### 方式一：导入游戏目录（推荐）
1. 将完整的 PC 游戏文件夹复制到手机内部存储的任意目录（例如 `/sdcard/Download/Rance10/`）。
2. 打开 **re:xsystem4**，点击主界面的 **「导入游戏目录」** 按钮。
3. 根据系统提示授予存储访问权限，并在弹出的目录选择器中选中该游戏文件夹即可完成导入。

### 方式二：从 ZIP 压缩包导入
1. 将游戏文件夹打包为 `.zip` 格式压缩包（压缩包根目录可直接放置 `AliceStart.ini` / `System40.ini`，或包含单层游戏主目录）。
2. 打开 **re:xsystem4**，点击主界面的 **「从ZIP导入」** 按钮。
3. 选择手机中的 `.zip` 文件，应用会自动将其解压至应用数据目录并识别游戏。

### 方式三：手动放置至应用数据目录
你也可以通过文件管理器或 ADB 将游戏文件夹直接放置到以下路径：
```text
Android/data/io.github.kichikuou.xsystem4/files/<游戏目录名>/
├── AliceStart.ini (或 System40.ini)
├── *.ain
├── *.afa / *.ald
├── Data/
├── Movie/
└── SaveData/
```

### 存档管理器（外部导入 / 内部导出 / 存档管理）
主界面在 **「从ZIP导入」** 与 **「GitHub」** 之间内置了 **「存档管理器」**：
- **外部导入存档**：支持从外部 `.zip` 压缩包、外部文件夹、单个或多个存档文件（如 `GlobalSave.asd`、`Collection.asd`、`SaveData*.asd` 等）一键导入至应用内部存档目录。
- **内部导出存档**：支持将内部存档一键打包导出为 `.zip` 压缩包、导出至手机任意外部文件夹，或同步至 `Android/data/io.github.rance10.nativeport/files/<游戏目录>/SaveData/`。
- **存档管理**：支持按游戏切换或新建存档目录，查看存档文件类型、大小与修改时间，以及对单个存档执行导出、备份副本、重命名、删除或清理崩溃日志。

---

## 触控操作说明

游戏运行期间支持以下触控手势模拟鼠标与键盘操作：

| 游戏内操作 | 触控手势 |
| :--- | :--- |
| **鼠标左键点击 / 确认** | 单指轻触画面目标位置 |
| **鼠标右键点击 / 返回 / 呼出菜单** | 单指轻触游戏画面左右两侧黑边区域，或双指同时轻触屏幕 |
| **对话快进（对应键盘 Ctrl 键）** | 单指长按屏幕不放 |
| **鼠标滚轮上下滚动（历史文本 / 列表翻页）** | 双指在屏幕上向下 / 向上滑动 |
| **鼠标光标悬停移动** | 单指在屏幕上按住并滑动 |

---

## 仓库目录结构

```text
.
├── .github/workflows/
│   └── build.yml           # GitHub Actions CI 自动构建与 APK 签名工作流
├── CMakeLists.txt          # 顶层 CMake ExternalProject 构建脚本（自动拉取并交叉编译所有依赖库与 xsystem4）
├── build-shared-libs.sh    # 编译 Android JNI 原生动态库 (.so) 的入口脚本
├── collect-licenses.sh     # 收集第三方依赖开源许可证脚本
├── patches/                # 针对 SDL2 等第三方依赖的 Android 补丁
├── xsystem4/               # 深度扩展后的 xsystem4 引擎与 libsys4 核心源码
│   ├── include/            # 引擎头文件
│   ├── src/                # 虚拟机、渲染管线、音频、视频、HLL 模块与 PartsEngine 实现
│   ├── shaders/            # OpenGL ES 着色器源码
│   └── subprojects/
│       └── libsys4/        # AIN、AFA、FLAT、EX、ACF、存档等格式解析库
└── project/                # Android Gradle 应用工程
    └── app/
        ├── build.gradle    # Android 应用构建与签名配置
        └── src/main/
            ├── AndroidManifest.xml
            ├── java/io/github/kichikuou/xsystem4/  # 启动器、目录/ZIP 导入与 SDLActivity 封装
            ├── jniLibs/arm64-v8a/                  # 编译生成的原生动态库输出目录
            └── res/                                # 界面布局、图标与多语言字符串资源
```

---

## 从源码构建

### 1. 环境依赖
在 Linux（推荐 Ubuntu 22.04 / 24.04）或 macOS 环境下构建，需安装以下工具：
- **JDK 17**
- **Android SDK**（Platform API 35、Build-Tools 35.0.0）
- **Android NDK**（推荐 r26d 或 r27c）
- **CMake**（3.22 及以上）与 **Ninja**
- 构建工具链：`flex`、`bison`、`pkg-config`、`meson`、`python3`、`git`

Ubuntu / Debian 安装命令示例：
```bash
sudo apt-get update
sudo apt-get install -y flex bison pkg-config ninja-build meson cmake git python3 openjdk-17-jdk
```

### 2. 编译步骤

1. **克隆本仓库**：
   ```bash
   git clone https://github.com/TomorrowX6/re-xsystem4.git
   cd re-xsystem4
   ```

2. **设置 Android SDK 与 NDK 环境变量**：
   ```bash
   export ANDROID_SDK_ROOT=/path/to/android-sdk
   export ANDROID_NDK_HOME=/path/to/android-sdk/ndk/27.2.12479018
   ```

3. **交叉编译原生动态库（`arm64-v8a`）**：
   ```bash
   ./build-shared-libs.sh
   ```
   该脚本会通过 CMake 自动下载并交叉编译 `zlib`、`libpng`、`libjpeg-turbo`、`libwebp`、`freetype2`、`ogg`、`vorbis`、`SDL2`、`SDL2_mixer`、`cJSON` 以及 `xsystem4` 核心库，并将生成的 `.so` 文件输出至 `project/app/src/main/jniLibs/arm64-v8a/`。

4. **打包 Android APK**：
   ```bash
   cd project
   ./gradlew assembleRelease
   ```
   生成的 APK 位于 `project/app/build/outputs/apk/release/`。

### 3. CI 自动签名配置
本仓库内置了 GitHub Actions 工作流（[`.github/workflows/build.yml`](.github/workflows/build.yml)）。若需在 Fork 仓库中启用 Release 自动签名，请在仓库的 `Settings -> Secrets and variables -> Actions` 中配置以下 Secrets（未配置时会自动生成临时签名密钥完成构建）：
- `KEYSTORE_BASE64`：Keystore 文件的 Base64 编码字符串
- `KEYSTORE_PASSWORD`：Keystore 密码
- `KEY_ALIAS`：密钥别名
- `KEY_PASSWORD`：密钥密码

---

## 致谢与开源许可

- [nunuhara/xsystem4](https://github.com/nunuhara/xsystem4) & [nunuhara/libsys4](https://github.com/nunuhara/libsys4)：跨平台 System 4 引擎及文件格式解析基础实现。
- [kichikuou/xsystem4-android](https://github.com/kichikuou/xsystem4-android)：xsystem4 的 Android 移植框架基础。

本项目遵循 [GNU General Public License v2.0 (GPL-2.0)](xsystem4/COPYING) 协议开源。

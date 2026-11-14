# re:xsystem4

[![Build APK](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml/badge.svg)](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](xsystem4/COPYING)
[![Platform](https://img.shields.io/badge/Platform-Android%20%28arm64--v8a%29-3DDC84.svg)](https://github.com/TomorrowX6/re-xsystem4/releases)

**[English](README.md)** | **简体中文** | **[日本語](README.ja.md)**

**re:xsystem4** 是基于 [xsystem4](https://github.com/nunuhara/xsystem4) 与 [xsystem4-android](https://github.com/kichikuou/xsystem4-android) 深度定制和扩展的 AliceSoft **System 4.x** Android 原生引擎。

本项目通过纯 C/C++、SDL2 与 OpenGL ES 3.0 在 Android `arm64-v8a` 设备上直接解释执行 AIN 字节码并渲染原版资源，无需借助 Wine、Winlator 或任何 x86/x64 指令转译层。除兼容传统 System 4.0 游戏外，重点针对后期大型作品（如《兰斯10 -决战- / Rance X》及「日不落 MOD」等大型扩展 MOD）所使用的 **AIN v12**、**PartsEngine v12**、**FLAT v15 骨骼与粒子动画**、**SealEngine 3D**、**APEG 视频流** 以及 **SaveData v26** 进行了完整实现与移动端深度优化。

---

## 核心特性

### 1. AIN v12 虚拟机、HLL 扩展与大型 MOD 深度兼容
- **AIN v12 字节码支持**：完整解析并执行 AIN v12 操作码、委托（Delegate / `DG_*`）、局部引用管理、结构体深拷贝、`CALLFUNC` `struct_page` 继承与反射接口。
- **HLL 模块覆盖**：完整实现并扩展 `PartsEngine`（804/804 函数全覆盖）、`SealEngine`、`ADVEngine`、`SaveData`、`RSave`、`APEG`、`SoundEngine`、`FileOperation`、`FileDialog`、`MsgSkip`、`Math`、`PassRegister`、`BanMisc` 等核心 HLL 库。
- **大型 MOD 深度适配**：
  - 将归档索引编码进 AFA 资源 ID，并按条目 `FILETIME` 时间戳与 MOD 优先级精确解析同名 AFA 资源，确保汉化包与热更补丁（如 `CG6`）完整生效且与大型 MOD 无缝共存。
  - 优先解析规范 `.ex` 数据表，支持增量 MOD 补丁分层覆盖以及 Shift-JIS / GBK / UTF-8 多编码文本与字体自动回退渲染。
  - 跨版本 MOD 存档兼容增强：加固 `resume` 结构体偏移变化容错，并对跨版本存档中缺失 `.ex` 条目的卡牌自动渲染兜底卡面。

### 2. PartsEngine 渲染与 3D 变换管线
- **FLAT v15 动画与粒子系统**：
  - 支持 zlib 压缩的 `ELNA` / `FLAT` v15 动画时间轴、骨骼关键帧插值、脚本帧触发、径向/定终点粒子发射器轨迹及 `.flat` 二进制缓存加速。
  - 实现嵌套祖先关键帧出生矩阵继承（`parent_key_mode == 2`）、祖先 Alpha 裁剪蒙版继承以及发光路线图层（Additive Blending 加法混合）。
- **3D 透视旋转与背面剔除**：
  - 支持 UI 部件（如卡牌翻转动画）的 X/Y/Z 三轴欧拉角透视旋转矩阵、父子节点 3D 变换级联以及正反面自动翻转剔除。
- **构建工序（Construction Process / `cproc`）管线**：
  - 完整支持文本排版绘制、表面裁剪复制、色彩填充、Alpha 混合、高斯模糊、灰阶化、HSL/HSV 调整、像素快照（`m_procOrg` 保留）与血条/计量槽裁剪。
- **编辑器图层过滤与精准命中测试**：
  - 自动过滤不应在运行期显示的编辑器占位组件（`CheckBox`、`RadioButton`、`■ボタン上敷き` 等），并剔除 `alpha == 0` 隐藏部件与透明 `.flat` 容器的无效碰撞。

### 3. 性能调度与多媒体优化
- **OpenGL ES 3.0 硬件加速**：基于 GLES 3.0 着色器管线与离屏帧缓冲（FBO）合成，提交渲染任务后立即执行 `glFlush()` 提前驱动 GPU 并行渲染，兼容 Android 15+ 16 KB 页面对齐。
- **Android 线程调度与高刷帧率节奏（Frame Pacing）**：
  - 渲染主线程提升至 `THREAD_PRIORITY_URGENT_DISPLAY`（`-8`，`50us` 定时器精度），音频混音回调线程提升至 `ANDROID_PRIORITY_URGENT_AUDIO`（`-16`）。
  - 接入 Android 12+ **ADPF（`APerformanceHintManager`）** 动态性能提示，在重负载帧即时引导 CPU 提频；配合 `eglPresentationTimeANDROID` 与无缝 `Surface.setFrameRate` 在 60Hz/120Hz 高刷屏上稳定对齐 60fps VSync。
  - 应用切入后台时自动休眠音频混音线程，彻底消除后台解码耗电。
- **APEG 视频解码与内存回收**：内置 MPEG-1 视频流与 Ogg Vorbis 音频同步解码器，主循环实时回收延迟纹理（Deferred Texture GC），加速 `SceneParty` 编成界面卡牌加载并对快进（`MsgSkip` / `Ctrl`）贴图重建节流。

### 4. 在线与本地安装、多格式解压引擎、存档管理与游戏修改器
- **游戏本体与 MOD 在线 / 本地双模式安装**：
  - **游戏本体在线双线路下载（EdgeOne / Cloudflare）**：支持一键选择 **EdgeOne 线路** 或 **Cloudflare 线路** 流式下载《兰斯10》中文完整本体，支持跨线路无缝 HTTP `Range` 断点续传与逐文件 SHA-256 完整性校验，无需双倍解压暂存空间。
  - **后台保活下载与系统通知栏进度**：基于 Android 前台服务与 CPU/Wi-Fi 唤醒锁保活，切回桌面或锁屏息屏仍可满速下载与解压；支持点击进度框 **「后台」** 收起弹窗、在系统通知栏实时查看进度与网速、通知栏一键 **「暂停」**，以及安装完成/异常系统提醒。
  - **原生「MOD 管理」与「官网下载」页面**：采用与存档管理、游戏修改器一致的原生全屏界面；自动同步 `rance10sns.cn` 官网全部版本与更新日志，记录本地已装 MOD 版本并检测新版本更新；支持一键增量补丁升级、基础包+补丁自动组合安装、任意历史版本无损切换，以及内置官网登录与下载页面。
- **原生 `.zip` / `.rar` / `.7z` 流式解压引擎**：集成 `libarchive` + `xz`（`liblzma`），原生支持 ZIP / Zip64、RAR4 / RAR5（含大型 MOD 常用的固实压缩与大字典）及 7z（LZMA / LZMA2 / PPMd / BCJ），自动识别 UTF-8 / UTF-16LE / GBK 中文与日文文件名。
- **无损 MOD 管理器**：支持在线下载或从本地 `.zip` / `.rar` / `.7z` 压缩包、外部文件夹安装完整/增量 MOD，自动备份原版被覆盖文件，随时一键 **「还原原版」**。
- **内置存档管理器**：PC 原版存档双向互通（`SaveData` v26 `.asd` 与 `RSave` `.rsd`，关键全局存档 `fsync` 原子写入）；支持从 `.zip` / `.rar` / `.7z` 压缩包、外部文件夹或单文件导入存档，支持打包导出为 ZIP 或外部文件夹，以及单文件备份、重命名与删除。
- **独立修改器与局内悬浮窗修改器（默认开启）**：
  - **局内悬浮窗修改器**：进入游戏默认显示可自由拖动的悬浮球，支持 **字体大小实时缩放（`75%~160%`，自动防溢出重排）**、**滚动条宽度与触控滑块实时放大（`100%~300%`，防滑动抢占）**、11 项实时战斗/全局锁定（满生命、无限 AP、999 连击、锁定回合为 1、敌方 1 血秒杀、敌方 0 攻防、全员蓄力必暴、技能无限次数、无限打火机、资源不低于 999、宝箱 100% 掉落）、持久化 **「关闭主播模式」**（适配日不落 MOD 0.49+ 并双端即时原子保存）、免外接键盘 **「输入作弊码」**、可拖动 **`⏩ Ctrl`** 快进锁定悬浮键（默认开启），以及资源/部队/四国兵力/卡牌与角色星级实时编辑。
  - **独立「游戏修改器」页面**：支持离线修改全局存档（`GlobalSave.asd` / `Collection.asd`）与各进度存档槽位（`SaveData*.asd`）、一键安装/还原全解锁存档（自动 `.modbak` 备份），以及预先设置 **字体大小**、**滚动条比例**、局内悬浮窗与 **`⏩ Ctrl` 快进悬浮键**总开关（关闭后游戏内同时禁止唤起）。

---

## 系统与硬件要求

| 项目 | 最低配置要求 | 推荐配置 |
| :--- | :--- | :--- |
| **处理器架构** | `arm64-v8a`（64 位 ARM） | 骁龙 8 系列 / 天玑 8000+ 或同级芯片 |
| **操作系统** | Android 8.0（API 26）及以上 | Android 12.0（API 31）及以上（支持 ADPF 动态调频与无缝帧率） |
| **图形接口** | OpenGL ES 3.0 | OpenGL ES 3.2 |
| **运行内存** | 4 GB RAM | 6 GB RAM 及以上 |
| **存储空间** | 取决于游戏体积（大型作品建议预留 5 GB 以上） | UFS 3.0+ 闪存 |

---

## 下载与安装

1. 前往本仓库的 [**Releases 页面**](https://github.com/TomorrowX6/re-xsystem4/releases) 下载最新版本的已签名安装包 `re-xsystem4-arm64-v8a.apk`。
2. 也可以在 [**GitHub Actions**](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml) 中下载最新提交自动构建生成的 Artifact 产物。
3. 在 Android 设备上安装 APK 并启动 **re:xsystem4**。

---

## 游戏、MOD、存档与修改器使用指南

要运行游戏，需准备好 PC 原版完整游戏文件（包含 `AliceStart.ini` 或 `System40.ini` 配置文件、`*.ain` 脚本文件，以及对应的 `*.afa` / `*.ald` 资源包、`Data/`、`Movie/` 等目录）。

### 1. 在线安装（`在线安装`）
点击主界面的 **「在线安装」** 按钮，可选择：
- **安装游戏本体**：自由选择 **EdgeOne 线路** 或 **Cloudflare 线路** 下载中文完整本体（约 3.75 GB），支持暂停、跨线路断点续传与 SHA-256 校验，下载完成后自动注册入库。
- **安装 MOD**：进入原生全屏 **「MOD 管理」** 页面，自动检测官网最新版本并对比当前已安装版本，支持一键智能更新、自由选择任意历史版本查看更新日志并切换版本、内置 **「官网下载」** 登录与直连下载页，以及一键 **「还原原版」**。

### 2. 本地安装（`本地安装`）
点击主界面的 **「本地安装」** 按钮，可选择：
- **安装游戏本体**：支持从本地 **`.zip` / `.rar` / `.7z` 压缩包** 或 **外部文件夹**（如 `/sdcard/Download/Rance10/`）导入游戏本体。
- **安装 MOD**：支持从本地 **`.zip` / `.rar` / `.7z` 压缩包** 或 **外部文件夹** 安装完整包或增量补丁，自动备份原版文件，随时支持一键 **「还原原版」**。

### 3. 存档管理器（`存档管理器`）
- **外部导入存档**：支持从外部 `.zip` / `.rar` / `.7z` 压缩包、外部文件夹、单个或多个存档文件（`GlobalSave.asd`、`Collection.asd`、`SaveData*.asd` 等）一键导入。
- **内部导出存档**：支持将内部存档打包导出为 `.zip` 压缩包，或直接导出至任意外部文件夹。
- **存档管理**：支持按游戏/MOD 切换或新建存档目录，查看存档分类标签、体积与修改时间，并对单个存档执行导出、备份（`.bak`）、重命名与删除。

### 4. 游戏修改器（`游戏修改器` 与局内悬浮窗）
- **局内悬浮窗修改器**：默认开启（游戏画面左上角可拖动图标），支持实时调节预览 **字体大小** 与 **滚动条比例**（不污染存档文件）、随时开关实时锁定项、一键关闭 MOD 主播模式（重启自动保持）、输入 MOD 作弊码、呼出 **`⏩ Ctrl`** 快进悬浮键，以及实时修改资源、部队、四国兵力与卡牌星级。
- **主界面「游戏修改器」**：支持在未启动游戏时离线修改全局存档与进度存档、一键导入全解锁存档、预设 **字体大小** 与 **滚动条比例**，或开启/关闭局内悬浮窗与 **`⏩ Ctrl`** 快进悬浮键（默认开启，关闭后游戏内亦禁止唤起）。

---

## 触控操作说明

| 游戏内操作 | 触控手势 |
| :--- | :--- |
| **鼠标左键点击 / 确认** | 单指轻触画面目标位置 |
| **鼠标右键点击 / 返回 / 呼出菜单** | 单指轻触游戏画面左右两侧黑边区域，或双指同时轻触屏幕 |
| **对话快进（对应键盘 `Ctrl` 键）** | 单指长按屏幕不放，或点击 **`⏩ Ctrl`** 悬浮键一键锁定快进 |
| **鼠标滚轮上下滚动（历史文本 / 列表翻页）** | 双指在屏幕上向下 / 向上滑动 |
| **光标悬停移动 / 编成卡牌滑动拖拽** | 单指在屏幕上按住并滑动 |
| **退出确认弹窗** | 系统返回手势 / 返回键（触发游戏原生 `SetHookCloseApp` 退出确认） |

---

## 仓库目录结构

```text
.
├── .github/workflows/
│   └── build.yml           # GitHub Actions CI 自动构建与 APK 签名工作流
├── CMakeLists.txt          # 顶层 CMake ExternalProject 构建脚本（自动拉取并交叉编译所有依赖库与 xsystem4）
├── build-shared-libs.sh    # 编译 Android JNI 原生动态库 (.so) 的入口脚本
├── collect-licenses.sh     # 收集第三方依赖开源许可证脚本
├── cmake/                  # 拉取 FreeType 与 SDL2 的 CMake 模块
├── archive_jni/            # 基于 libarchive + xz (liblzma) 的原生 .zip / .rar / .7z 流式解压库
├── xsystem4/               # 深度扩展后的 xsystem4 引擎、修改器核心与 libsys4 源码
│   ├── include/            # 引擎头文件
│   ├── src/                # 虚拟机、渲染管线、ADPF 帧率调度、音视频、HLL、PartsEngine 与修改器实现
│   ├── shaders/            # OpenGL ES 3.0 着色器源码
│   └── subprojects/
│       └── libsys4/        # AIN、AFA、FLAT、EX、ACF、存档等格式解析库
└── project/                # Android Gradle 应用工程
    └── app/
        ├── build.gradle    # Android 应用构建与签名配置
        └── src/main/
            ├── AndroidManifest.xml
            ├── java/io/github/kichikuou/xsystem4/  # 启动器、MOD/存档管理、修改器、解压器与 Activity 封装
            ├── jniLibs/arm64-v8a/                  # 编译生成的原生动态库输出目录
            └── res/                                # 界面布局、图标、全解锁存档包与多语言资源
```

---

## 从源码构建

### 1. 环境依赖
在 Linux（推荐 Ubuntu 22.04 / 24.04）或 macOS 环境下构建，需安装以下工具：
- **JDK 17**
- **Android SDK**（Platform API 34+、Build-Tools 34.0.0+）
- **Android NDK**（推荐 r26d – r29）
- **CMake**（3.22 及以上）与 **Ninja**
- 构建工具链：`flex`、`bison`、`pkg-config`、`autoconf`、`automake`、`libtool`、`python3`、`git`

Ubuntu / Debian 安装命令示例：
```bash
sudo apt-get update
sudo apt-get install -y ninja-build bison flex autoconf automake libtool pkg-config cmake git python3 openjdk-17-jdk
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
   该脚本会通过 CMake 自动下载并交叉编译 `libwebp`、`libjpeg-turbo`、`libpng`、`libogg`、`libvorbis`、`libsndfile`、`freetype`、`SDL2`、`cglm`、`libffi`、`libdeflate`、`xz`（`liblzma`）、`libarchive`、`archive_jni` 以及 `xsystem4` 核心库，并将生成的 `.so` 文件输出至 `project/app/src/main/jniLibs/arm64-v8a/`。

4. **打包 Android APK**：
   ```bash
   cd project
   ./gradlew assembleRelease
   ```
   生成的签名 APK 位于 `project/app/build/outputs/apk/release/app-release.apk`。

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
- [xiexiahai/xsystem4-rance10-android](https://github.com/xiexiahai/xsystem4-rance10-android)：项目特性参考与增强致谢。

本项目遵循 [GNU General Public License v2.0 (GPL-2.0)](xsystem4/COPYING) 协议开源。

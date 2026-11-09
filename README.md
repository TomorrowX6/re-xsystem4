# re:xsystem4

基于 [xsystem4](https://github.com/nunuhara/xsystem4) 与 [xsystem4-android](https://github.com/kichikuou/xsystem4-android) 深度扩展的 System 4.x（含 AIN v12 / PartsEngine / SealEngine 3D）Android 原生引擎（`arm64-v8a`）。

## 系统要求

- **架构**：`arm64-v8a`（64 位 ARM）
- **系统**：Android 8.0（API 26）及以上
- **图形**：OpenGL ES 3.0+

## 游戏导入与触控操作

支持在应用内通过 **导入游戏目录** 或 **从ZIP导入** 导入包含 `AliceStart.ini` / `System40.ini` 与 `.ain` 的游戏目录或压缩包。

| 操作 | 触控手势 |
| :--- | :--- |
| 鼠标左键点击 | 单指轻触 |
| 鼠标右键 / 返回取消 | 点击画面两侧黑边或双指轻触 |
| 快进对话（Ctrl） | 单指长按屏幕 |
| 滚轮上下滚动 | 双指上下滑动 |

## 构建说明

```bash
export ANDROID_SDK_ROOT=/path/to/android-sdk
export ANDROID_NDK_HOME=/path/to/android-ndk

./build-shared-libs.sh
cd project && ./gradlew assembleRelease
```

## 开源许可

本项目遵循 [GPL-2.0](xsystem4/COPYING) 协议开源。

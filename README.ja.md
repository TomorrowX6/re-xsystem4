# re:xsystem4

[![Build APK](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml/badge.svg)](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](xsystem4/COPYING)
[![Platform](https://img.shields.io/badge/Platform-Android%20%28arm64--v8a%29-3DDC84.svg)](https://github.com/TomorrowX6/re-xsystem4/releases)

**[English](README.md)** | **[简体中文](README.zh-CN.md)** | **日本語**

**re:xsystem4** は、[xsystem4](https://github.com/nunuhara/xsystem4) および [xsystem4-android](https://github.com/kichikuou/xsystem4-android) をベースに独自拡張・最適化された AliceSoft **System 4.x** 向けのネイティブ Android エンジンです。

C/C++、SDL2、OpenGL ES 3.0 で実装されており、Wine や Winlator などの x86/x64 エミュレーション層を介さずに、Android `arm64-v8a` デバイス上で AIN バイトコードを直接解釈実行し、オリジナルのアセットを描画します。従来の System 4.0 作品に加え、後期の大規模タイトル（『ランス10 -決戦- / Rance X』など）で採用されている **AIN v12**、**PartsEngine v12**、**FLAT v15 スケルタル＆パーティクルアニメーション**、**SealEngine 3D**、**APEG 動画再生**、および **SaveData v26** を完全に実装し、モバイル環境向けに最適化しています。

---

## 主な機能

### 1. AIN v12 仮想マシン＆HLL 拡張
- **AIN v12 バイトコード対応**：AIN v12 の新規オペコード、デリゲート（`DG_*`）、ローカル参照管理、構造体のディープコピー、リフレクションインターフェースを完全実装。
- **包括的な HLL モジュール実装**：`PartsEngine`（全 804 関数実装済み）、`SealEngine`、`ADVEngine`、`SaveData`、`RSave`、`APEG`、`SoundEngine`、`FileOperation`、`FileDialog`、`MsgSkip`、`Math`、`PassRegister`、`BanMisc` などの主要 HLL ライブラリを網羅。

### 2. PartsEngine 描画＆3D 変換パイプライン
- **FLAT v15 アニメーション＆パーティクルシステム**：
  - zlib 圧縮された `ELNA` / `FLAT` v15 タイムライン、キーフレーム補間、スクリプトフレームトリガー、複合レイヤーをサポート。
  - ネストされた親キーフレームの生成マトリクス継承（`parent_key_mode == 2`）およびリアルタイムパーティクル追従を実装。
  - 発光ルートレイヤー（加算合成 / Additive Blending）および `.flat` 原点オフセット補正に対応。
- **3D 透視回転＆背面カリング**：
  - UI パーツ（カードの反転アニメーションなど）の X/Y/Z オイラー角透視回転行列、親子ノード間の 3D 変換カスケード、および表裏自動判定・背面カリングをサポート。
- **構築プロセス（Construction Process）フィルタ**：
  - テキスト組版描画、サーフェス切り抜き・コピー、塗りつぶし、アルファブレンド、ガウスぼかし、グレースケール化、色相・彩度・明度（HSL/HSV）調整などの構築プロセス命令を完全実装。
- **エディタ用レイヤーの除外＆高精度な当たり判定**：
  - 実行時に表示されるべきでない内部エディタ用プレースホルダ（`CheckBox`、`RadioButton`、`■ボタン上敷き` など）を自動フィルタリング。
  - 非表示パーツ（`alpha == 0`）や透明な `.flat` コンテナを当たり判定から除外し、透明なオーバーレイによるタッチ無反応問題を解消。

### 3. マルチメディア＆パフォーマンス最適化
- **OpenGL ES 3.0 ハードウェアアクセラレーション**：GLES 3.0 シェーダーパイプラインとオフスクリーンフレームバッファ（FBO）合成を採用し、16 KB ページアライメント（Android 15+）に対応。
- **APEG ビデオデコーダ**：MPEG-1 ビデオストリームと Ogg Vorbis オーディオの同期デコーダを内蔵し、マクロブロック境界の問題やムービー切り替え時の緑画面ちらつきを修正。
- **フレーム内テクスチャ GC ＆スキップ時のスロットリング**：メインループ内で遅延解放テクスチャを随時回収し、メッセージ連続スキップ（MsgSkip / Ctrl）時の不要なサーフェス再構築を抑制することで、カクつきや発熱を防止。

### 4. モバイルタッチ操作＆内蔵セーブマネージャー
- **2フェーズ・タッチイベントキュー**：Android のタッチサンプリングと C 層のイベントディスパッチパイプラインを再設計し、高速連打や画面切り替え時の押下・解放座標とフレーム順序の整合性を保証。
- **PC 版セーブデータとの双方向互換**：`SaveData`（v26 `.asd`）の再帰的シリアライズおよび `RSave`（`.rsd`）の読み書きに対応し、PC 版のセーブデータをそのまま利用可能。
- **内蔵セーブマネージャー**：外部の ZIP アーカイブ・フォルダ・個別ファイルからのセーブインポート、内部セーブデータの ZIP パッケージ化・外部フォルダへのエクスポート、および個別セーブデータのバックアップ・名前変更・削除をランチャー内で完結。

---

## 動作環境

| 項目 | 最小要件 | 推奨環境 |
| :--- | :--- | :--- |
| **CPU アーキテクチャ** | `arm64-v8a`（64ビット ARM） | Snapdragon 8 シリーズ / Dimensity 8000+ 同等以上 |
| **OS バージョン** | Android 8.0（API 26）以上 | Android 11.0 以上 |
| **グラフィックス API** | OpenGL ES 3.0 | OpenGL ES 3.2 |
| **メモリ (RAM)** | 4 GB RAM | 6 GB RAM 以上 |
| **ストレージ空き容量** | ゲームサイズに依存（大規模作品は 5 GB 以上の空き容量を推奨） | UFS 3.0 以上のフラッシュストレージ |

---

## ダウンロードとインストール

1. 本リポジトリの [**Releases ページ**](https://github.com/TomorrowX6/re-xsystem4/releases) から署名済み APK `re-xsystem4-arm64-v8a.apk` の最新版をダウンロードします。
2. または [**GitHub Actions**](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml) から最新コミットの自動ビルド Artifact を取得することも可能です。
3. Android デバイスに APK をインストールし、**re:xsystem4** を起動します。

---

## ゲームのインポートとセーブ管理

ゲームを実行するには、PC 版の完全なゲームファイル一式（`AliceStart.ini` または `System40.ini`、`*.ain` スクリプトファイル、`*.afa` / `*.ald` アーカイブ、`Data/`、`Movie/` フォルダなど）を用意してください。

### 方法 1：ゲームフォルダからインポート（推奨）
1. PC 版のゲームフォルダ一式を端末の内部ストレージの任意の場所（例：`/sdcard/Download/Rance10/`）にコピーします。
2. **re:xsystem4** を開き、メイン画面の **「导入游戏目录」（ディレクトリからインポート）** をタップします。
3. フォルダ選択画面で該当のゲームフォルダを選択するとインポートが開始されます。

### 方法 2：ZIP アーカイブからインポート
1. ゲームフォルダを `.zip` 形式に圧縮します（`AliceStart.ini` / `System40.ini` が ZIP の直下、または単一のルートフォルダ内にある状態）。
2. **re:xsystem4** を開き、メイン画面の **「从ZIP导入」（ZIP からインポート）** をタップして `.zip` ファイルを選択します。

### セーブマネージャー（外部インポート / 内部エクスポート / セーブ管理）
メイン画面の **「从ZIP导入」** と **「GitHub」** の間に **「存档管理器」（セーブマネージャー）** を搭載しています：
- **外部からインポート**：外部の `.zip` アーカイブ、外部フォルダ、個別のセーブファイル（`GlobalSave.asd`、`Collection.asd`、`SaveData*.asd` など）、または `Android/data/io.github.rance10.nativeport/files/<ゲーム名>/SaveData` から内部セーブディレクトリへ一括インポート可能。
- **外部へエクスポート**：内部セーブデータを `.zip` アーカイブとして出力、任意の外部フォルダへ書き出し、または外部ゲームフォルダ内の `SaveData/` へ同期可能。
- **セーブデータ管理**：ゲームごとのセーブディレクトリの切り替え・新規作成、ファイル種別・サイズ・更新日時の確認、個別ファイルの外部出力・バックアップ作成（`.bak`）・名前変更・削除、およびクラッシュログの削除に対応。

---

## タッチ操作一覧

| ゲーム内操作 | タッチジェスチャー |
| :--- | :--- |
| **左クリック / 決定** | 画面の対象位置を 1 本指でタップ |
| **右クリック / キャンセル / メニュー呼出** | 画面左右の黒帯エリアを 1 本指でタップ、または画面を 2 本指で同時タップ |
| **メッセージスキップ（Ctrl キー相当）** | 画面を 1 本指で長押し |
| **マウスホイール上下（バックログ / リストスクロール）** | 画面を 2 本指で下 / 上へスワイプ |
| **マウスカーソル移動（ホバー）** | 画面を 1 本指で押さえたままスライド |

---

## リポジトリ構成

```text
.
├── .github/workflows/
│   └── build.yml           # GitHub Actions CI 自動ビルド＆APK 署名ワークフロー
├── CMakeLists.txt          # 全依存ライブラリおよび xsystem4 をクロスコンパイルするトップレベル CMake スクリプト
├── build-shared-libs.sh    # Android JNI ネイティブ共有ライブラリ (.so) ビルドスクリプト
├── collect-licenses.sh     # サードパーティ製オープンソースライセンス収集スクリプト
├── cmake/                  # FreeType・SDL2 取得用 CMake モジュール
├── xsystem4/               # 拡張された xsystem4 エンジンおよび libsys4 コアソースコード
│   ├── include/            # エンジンヘッダファイル
│   ├── src/                # 仮想マシン、描画パイプライン、音声、動画、HLL モジュール、PartsEngine 実装
│   ├── shaders/            # OpenGL ES 3.0 シェーダー
│   └── subprojects/
│       └── libsys4/        # AIN、AFA、FLAT、EX、ACF、セーブ形式などのパーサライブラリ
└── project/                # Android Gradle アプリケーションプロジェクト
    └── app/
        ├── build.gradle    # Android ビルドおよび署名設定
        └── src/main/
            ├── AndroidManifest.xml
            ├── java/io/github/kichikuou/xsystem4/  # ランチャー、セーブマネージャー、インポータ、SDLActivity
            ├── jniLibs/arm64-v8a/                  # ビルドされたネイティブ .so ライブラリ出力先
            └── res/                                # レイアウト、アイコン、文字列リソース
```

---

## ソースコードからのビルド

### 1. 必要環境
Linux（Ubuntu 22.04 / 24.04 推奨）または macOS 環境で以下のツールが必要です：
- **JDK 17**
- **Android SDK**（Platform API 34+、Build-Tools 34.0.0+）
- **Android NDK**（r26d または r27c 推奨）
- **CMake**（3.22 以上）および **Ninja**
- ビルドツール：`flex`、`bison`、`pkg-config`、`autoconf`、`automake`、`libtool`、`python3`、`git`

Ubuntu / Debian でのインストール例：
```bash
sudo apt-get update
sudo apt-get install -y ninja-build bison flex autoconf automake libtool pkg-config cmake git python3 openjdk-17-jdk
```

### 2. ビルド手順

1. **リポジトリのクローン**：
   ```bash
   git clone https://github.com/TomorrowX6/re-xsystem4.git
   cd re-xsystem4
   ```

2. **Android SDK・NDK 環境変数の設定**：
   ```bash
   export ANDROID_SDK_ROOT=/path/to/android-sdk
   export ANDROID_NDK_HOME=/path/to/android-sdk/ndk/27.2.12479018
   ```

3. **ネイティブ共有ライブラリのクロスコンパイル（`arm64-v8a`）**：
   ```bash
   ./build-shared-libs.sh
   ```
   CMake によって `libwebp`、`libjpeg-turbo`、`libpng`、`libogg`、`libvorbis`、`libsndfile`、`freetype`、`SDL2`、`cglm`、`libffi`、`libdeflate`、および `xsystem4` が自動的にダウンロード・クロスコンパイルされ、`project/app/src/main/jniLibs/arm64-v8a/` に出力されます。

4. **Android APK のパッケージング**：
   ```bash
   cd project
   ./gradlew assembleRelease
   ```
   署名済み APK は `project/app/build/outputs/apk/release/app-release.apk` に生成されます。

---

## クレジット＆ライセンス

- [nunuhara/xsystem4](https://github.com/nunuhara/xsystem4) & [nunuhara/libsys4](https://github.com/nunuhara/libsys4)：クロスプラットフォーム System 4 エンジンおよびファイルフォーマット解析の基盤実装。
- [kichikuou/xsystem4-android](https://github.com/kichikuou/xsystem4-android)：xsystem4 の Android 移植フレームワーク。

本プロジェクトは [GNU General Public License v2.0 (GPL-2.0)](xsystem4/COPYING) の下で公開されています。

# re:xsystem4

[![Build APK](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml/badge.svg)](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](xsystem4/COPYING)
[![Platform](https://img.shields.io/badge/Platform-Android%20%28arm64--v8a%29-3DDC84.svg)](https://github.com/TomorrowX6/re-xsystem4/releases)

**[English](README.md)** | **[简体中文](README.zh-CN.md)** | **日本語**

**re:xsystem4** は、[xsystem4](https://github.com/nunuhara/xsystem4) および [xsystem4-android](https://github.com/kichikuou/xsystem4-android) をベースに独自拡張・最適化された AliceSoft **System 4.x** 向けのネイティブ Android エンジンです。

C/C++、SDL2、OpenGL ES 3.0 で実装されており、Wine や Winlator などの x86/x64 エミュレーション層を介さずに、Android `arm64-v8a` デバイス上で AIN バイトコードを直接解釈実行し、オリジナルのアセットを描画します。従来の System 4.0 作品に加え、後期の大規模タイトル（『ランス10 -決戦- / Rance X』および大型拡張 MOD など）で採用されている **AIN v12**、**PartsEngine v12**、**FLAT v15 スケルタル＆パーティクルアニメーション**、**SealEngine 3D**、**APEG 動画再生**、および **SaveData v26** を完全に実装し、モバイル環境向けに最適化しています。

---

## 主な機能

### 1. AIN v12 仮想マシン、HLL 拡張＆大型 MOD 対応
- **AIN v12 バイトコード対応**：AIN v12 の新規オペコード、デリゲート（`DG_*`）、ローカル参照管理、構造体のディープコピー、`CALLFUNC` における `struct_page` 継承、リフレクションインターフェースを完全実装。
- **包括的な HLL モジュール実装**：`PartsEngine`（全 804 関数実装済み）、`SealEngine`、`ADVEngine`、`SaveData`、`RSave`、`APEG`、`SoundEngine`、`FileOperation`、`FileDialog`、`MsgSkip`、`Math`、`PassRegister`、`BanMisc` などの主要 HLL ライブラリを網羅。
- **大型 MOD 互換性**：
  - アーカイブインデックスを AFA アセット ID にエンコードし、エントリ単位の `FILETIME` タイムスタンプ＆MOD 優先度解決により、ローカライズ版ベース／ホットフィックス資産と大型 MOD の競合を防止。
  - 正規 `.ex` テーブルの優先解決、差分 MOD パッチの上書き適用、Shift-JIS / GBK / UTF-8 テキスト・フォント自動フォールバックに対応。
  - 異バージョン間 MOD セーブ互換性強化（`resume` スキーマ変化への耐性向上および未定義 EX カードへのフォールバック描画）。

### 2. PartsEngine 描画＆3D 変換パイプライン
- **FLAT v15 アニメーション＆パーティクルシステム**：
  - zlib 圧縮された `ELNA` / `FLAT` v15 タイムライン、キーフレーム補間、スクリプトフレームトリガー、放射状・終点固定パーティクル軌道、および `.flat` キャッシュ高速化をサポート。
  - ネストされた親キーフレームの生成マトリクス継承（`parent_key_mode == 2`）、親アルファクリッパー継承、および発光ルートレイヤー（加算合成 / Additive Blending）に対応。
- **3D 透視回転＆背面カリング**：
  - UI パーツ（カードの反転アニメーションなど）の X/Y/Z オイラー角透視回転行列、親子ノード間の 3D 変換カスケード、および表裏自動判定・背面カリングをサポート。
- **構築プロセス（Construction Process / `cproc`）パイプライン**：
  - テキスト組版描画、サーフェス切り抜き・コピー、塗りつぶし、アルファブレンド、ガウスぼかし、グレースケール化、HSL/HSV 調整、ピクセルスナップショット（`m_procOrg` 保持）、およびゲージクリッピングを実装。
- **エディタ用レイヤーの除外＆高精度な当たり判定**：
  - 内部エディタ用プレースホルダ（`CheckBox`、`RadioButton`、`■ボタン上敷き` など）を自動除外し、非表示パーツ（`alpha == 0`）や透明な `.flat` コンテナを当たり判定から除外。

### 3. パフォーマンス・スケジューリング＆マルチメディア最適化
- **OpenGL ES 3.0 ハードウェアアクセラレーション**：GLES 3.0 シェーダーパイプラインとオフスクリーン FBO 合成、描画直後の `glFlush()` による GPU パイプライン並列化、および 16 KB ページアライメント（Android 15+）に対応。
- **Android スレッドスケジューリング＆フレームペーシング**：
  - 描画スレッドを `THREAD_PRIORITY_URGENT_DISPLAY`（`-8`、タイマースラック `50us`）、音声ミキシングスレッドを `ANDROID_PRIORITY_URGENT_AUDIO`（`-16`）へ引き上げ。
  - Android 12+ の **ADPF（`APerformanceHintManager`）** に対応し、高負荷フレームで即座に CPU クロック引き上げを要求。`eglPresentationTimeANDROID` とシームレスな `Surface.setFrameRate` により、60Hz/120Hz ディスプレイ上で安定した 60fps VSync ペーシングを実現。
  - バックグラウンド移行時にオーディオミキサーを自動休止し、待機時のバッテリー消費を削減。
- **APEG ビデオデコーダ＆テクスチャ GC**：MPEG-1 映像と Ogg Vorbis 音声の同期デコーダを内蔵。フレーム内の遅延解放テクスチャ回収、`SceneParty` カード読み込み高速化、およびスキップ（`MsgSkip` / `Ctrl`）時の再構築スロットリングを実装。

### 4. マルチフォーマット展開エンジン、MOD 管理、セーブ管理＆ゲーム改造
- **ネイティブ `.zip` / `.rar` / `.7z` ストリーミング展開エンジン**：`libarchive` + `xz`（`liblzma`）により、ZIP/Zip64、RAR4/RAR5（ソリッド書庫・大辞書対応）、7z（LZMA/LZMA2/PPMd/BCJ）に対応し、UTF-8 / UTF-16LE / GBK ファイル名を自動判別。
- **非破壊 MOD マネージャー**：`.zip` / `.rar` / `.7z` アーカイブまたはフォルダから MOD を導入でき、元ファイルを自動バックアップしていつでもワンタップでバニラ状態へ復元可能。
- **内蔵セーブマネージャー**：PC 版セーブデータとの双方向互換（`SaveData` v26 `.asd` / `RSave` `.rsd`、`fsync` アトミック保存）、`.zip` / `.rar` / `.7z`・フォルダ・個別ファイルからのインポート、ZIP / 外部フォルダへのエクスポート、バックアップ・名前変更・削除に対応。
- **スタンドアロン改造＆ゲーム内フローティング改造（デフォルト ON）**：
  - **ゲーム内フローティング改造**：起動時にドラッグ可能なオーバーレイアイコンをデフォルト表示。11 項目のリアルタイムロック（味方 HP 最大、AP 無限、999 コンボ、ラウンド 1 固定、敵 HP1 一撃撃破、敵攻防 0、全員ブースト＆必殺、スキル回数無限、ジッポ無限、リソース 999 維持、宝箱 100% ドロップ）、再起動後も保持される **MOD 配信者モード解除**、外付けキーボード不要の **チートコード入力**、ドラッグ可能な **`⏩ Ctrl` 早送りロックボタン**（デフォルト ON）、および各種ステータスのリアルタイム編集を搭載。
  - **ランチャー内「ゲーム改造」**：`GlobalSave.asd` / `Collection.asd` や個別セーブスロット（`SaveData*.asd`）のオフライン編集、全解放セーブの一括導入（自動 `.modbak` バックアップ付き）、フローティング改造アイコンおよび **`⏩ Ctrl` 早送りボタン**の ON/OFF 切替（OFF 時はゲーム内での呼び出しも無効化）に対応。

---

## 動作環境

| 項目 | 最小要件 | 推奨環境 |
| :--- | :--- | :--- |
| **CPU アーキテクチャ** | `arm64-v8a`（64ビット ARM） | Snapdragon 8 シリーズ / Dimensity 8000+ 同等以上 |
| **OS バージョン** | Android 8.0（API 26）以上 | Android 12.0（API 31）以上（ADPF・シームレスフレームレート対応） |
| **グラフィックス API** | OpenGL ES 3.0 | OpenGL ES 3.2 |
| **メモリ (RAM)** | 4 GB RAM | 6 GB RAM 以上 |
| **ストレージ空き容量** | ゲームサイズに依存（大規模作品は 5 GB 以上の空き容量を推奨） | UFS 3.0 以上のフラッシュストレージ |

---

## ダウンロードとインストール

1. 本リポジトリの [**Releases ページ**](https://github.com/TomorrowX6/re-xsystem4/releases) から署名済み APK `re-xsystem4-arm64-v8a.apk` の最新版をダウンロードします。
2. または [**GitHub Actions**](https://github.com/TomorrowX6/re-xsystem4/actions/workflows/build.yml) から最新コミットの自動ビルド Artifact を取得することも可能です。
3. Android デバイスに APK をインストールし、**re:xsystem4** を起動します。

---

## ゲーム・MOD・セーブ・改造機能ガイド

ゲームを実行するには、PC 版の完全なゲームファイル一式（`AliceStart.ini` または `System40.ini`、`*.ain` スクリプトファイル、`*.afa` / `*.ald` アーカイブ、`Data/`、`Movie/` フォルダなど）を用意してください。

### 1. ゲーム本体のインストール
メイン画面の **「ゲーム本体をインストール (`安装游戏本体`)」** をタップし、以下のいずれかを選択します：
- **アーカイブからインストール（`.zip` / `.rar` / `.7z`）**：`.zip`、`.rar`、または `.7z` 形式のゲーム書庫を選択して直接展開・登録します。
- **フォルダからインストール**：端末ストレージ上の展開済みゲームフォルダ（例：`/sdcard/Download/Rance10/`）を選択してインポートします。

### 2. MOD の導入とバニラ復元
1. ゲーム本体をインポート後、メイン画面の **「MOD をインストール (`安装 MOD`)」** をタップして対象ゲームを選択します。
2. **`.zip` / `.rar` / `.7z` アーカイブ** または **フォルダ** からフル MOD や差分パッチを導入できます（上書きされる元ファイルは自動バックアップされます）。
3. **「バニラに戻す (`还原原版游戏`)」** を選択すると、いつでも元の状態へ復元できます。

### 3. セーブマネージャー
- **外部からインポート**：外部の `.zip` / `.rar` / `.7z` アーカイブ、フォルダ、または個別ファイルからセーブデータを一括インポート。
- **外部へエクスポート**：内部セーブデータを `.zip` アーカイブ、または任意の外部フォルダへ書き出し。
- **セーブデータ管理**：セーブディレクトリの切り替え・新規作成、個別ファイルの出力・バックアップ（`.bak`）・名前変更・削除に対応。

### 4. ゲーム改造（フローティング改造＆オフライン編集）
- **ゲーム内フローティング改造**：デフォルトで画面左上に表示されるアイコンから、リアルタイムロック、MOD 配信者モード解除（自動保存）、チートコード入力、**`⏩ Ctrl`** 早送りボタンの表示切替、各種数値のライブ編集を行えます。
- **ランチャー内「ゲーム改造」**：起動前のセーブデータ編集、全解放セーブの導入・復元、フローティング改造および **`⏩ Ctrl`** 早送りボタンの ON/OFF 切り替え（デフォルト ON、OFF 時はゲーム内での呼び出しも禁止）を行えます。

---

## タッチ操作一覧

| ゲーム内操作 | タッチジェスチャー |
| :--- | :--- |
| **左クリック / 決定** | 画面の対象位置を 1 本指でタップ |
| **右クリック / キャンセル / メニュー呼出** | 画面左右の黒帯エリアを 1 本指でタップ、または画面を 2 本指で同時タップ |
| **メッセージスキップ（`Ctrl` キー相当）** | 画面を 1 本指で長押し、または **`⏩ Ctrl`** ボタンをタップしてスキップ固定 |
| **マウスホイール上下（バックログ / リストスクロール）** | 画面を 2 本指で下 / 上へスワイプ |
| **カーソル移動 / カード一覧ドラッグスクロール** | 画面を 1 本指で押さえたままスライド |
| **終了確認ダイアログ** | システムの戻る操作（ゲーム内の `SetHookCloseApp` 終了確認を呼び出し） |

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
├── archive_jni/            # libarchive + xz (liblzma) ベースの .zip / .rar / .7z 展開 JNI ライブラリ
├── xsystem4/               # 拡張された xsystem4 エンジン、改造コア、および libsys4 ソースコード
│   ├── include/            # エンジンヘッダファイル
│   ├── src/                # 仮想マシン、描画、ADPF ペーシング、音声・動画、HLL、PartsEngine、改造機能
│   ├── shaders/            # OpenGL ES 3.0 シェーダー
│   └── subprojects/
│       └── libsys4/        # AIN、AFA、FLAT、EX、ACF、セーブ形式などのパーサライブラリ
└── project/                # Android Gradle アプリケーションプロジェクト
    └── app/
        ├── build.gradle    # Android ビルドおよび署名設定
        └── src/main/
            ├── AndroidManifest.xml
            ├── java/io/github/kichikuou/xsystem4/  # ランチャー、MOD/セーブ管理、改造UI、書庫展開、Activity
            ├── jniLibs/arm64-v8a/                  # ビルドされたネイティブ .so ライブラリ出力先
            └── res/                                # レイアウト、アイコン、全解放セーブ、多言語リソース
```

---

## ソースコードからのビルド

### 1. 必要環境
Linux（Ubuntu 22.04 / 24.04 推奨）または macOS 環境で以下のツールが必要です：
- **JDK 17**
- **Android SDK**（Platform API 34+、Build-Tools 34.0.0+）
- **Android NDK**（r26d – r29 推奨）
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
   CMake によって `libwebp`、`libjpeg-turbo`、`libpng`、`libogg`、`libvorbis`、`libsndfile`、`freetype`、`SDL2`、`cglm`、`libffi`、`libdeflate`、`xz`（`liblzma`）、`libarchive`、`archive_jni`、および `xsystem4` が自動的にダウンロード・クロスコンパイルされ、`project/app/src/main/jniLibs/arm64-v8a/` に出力されます。

4. **Android APK のパッケージング**：
   ```bash
   cd project
   ./gradlew assembleRelease
   ```
   署名済み APK は `project/app/build/outputs/apk/release/app-release.apk` に生成されます。

### 3. CI 自動署名設定
本リポジトリには GitHub Actions ワークフロー（[`.github/workflows/build.yml`](.github/workflows/build.yml)）が含まれています。フォーク先で独自の署名キーを使用する場合は、`Settings -> Secrets and variables -> Actions` に以下のシークレットを設定してください（未設定の場合は一時的なデバッグ署名キーが自動生成されます）：
- `KEYSTORE_BASE64`：Base64 エンコードされた Keystore ファイル
- `KEYSTORE_PASSWORD`：Keystore パスワード
- `KEY_ALIAS`：キーエイリアス
- `KEY_PASSWORD`：キーパスワード

---

## クレジット＆ライセンス

- [nunuhara/xsystem4](https://github.com/nunuhara/xsystem4) & [nunuhara/libsys4](https://github.com/nunuhara/libsys4)：クロスプラットフォーム System 4 エンジンおよびファイルフォーマット解析の基盤実装。
- [kichikuou/xsystem4-android](https://github.com/kichikuou/xsystem4-android)：xsystem4 の Android 移植フレームワーク。
- [xiexiahai/xsystem4-rance10-android](https://github.com/xiexiahai/xsystem4-rance10-android)：プロジェクト機能および拡張への貢献。

本プロジェクトは [GNU General Public License v2.0 (GPL-2.0)](xsystem4/COPYING) の下で公開されています。

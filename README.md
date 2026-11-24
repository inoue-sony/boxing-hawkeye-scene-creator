# boxing-hawkeye-scene-creator

Hawk-Eye のボクシング計測データ（選手の関節位置フレーム）から HUMA 体型の選手をリアルタイムに再生し、
技（パンチ・ディフェンス・フットワーク・コンビネーション）の発動に合わせて画面上にアノテーションを表示する
Unreal Engine 5.8 プロジェクトです。

- UE プロジェクト: `BoxingHawkEyeSceneCreator.uproject`
- 起動マップ: `/Game/HUMARuntime/L_HUMARuntimePlayback`

## 必要なもの

| 項目 | 内容 |
| --- | --- |
| Unreal Engine | 5.8（Epic Games Launcher 版。既定 `C:\Program Files\Epic Games\UE_5.8`） |
| Git LFS | `.uasset` / `.umap` / テクスチャ / ONNX / DLL は LFS 管理です。クローン前に `git lfs install` |
| Hawk-Eye フレームデータ | リポジトリには含みません。下記「データの置き場所」を参照 |
| Visual Studio 2022 | C++（プロジェクト本体モジュールと HUMA 系プラグイン）のビルドに使います。ワークロード「C++ によるゲーム開発」と Unreal Engine 用コンポーネントを入れてください |

## セットアップ

```powershell
git lfs install
git clone https://github.com/inoue-sony/boxing-hawkeye-scene-creator.git
cd boxing-hawkeye-scene-creator
```

1. Hawk-Eye フレームデータを `Data/HawkEye/` に置く（下記）。
2. `BoxingHawkEyeSceneCreator.uproject` をダブルクリックして開く。
   - 初回は「モジュールが古い/見つからない」と聞かれるので「はい」でビルドします（Visual Studio が必要）。
   - Visual Studio から開発する場合は下記「Visual Studio でビルドする」を参照。
3. ツールバーの Play で再生します。

### Visual Studio でビルドする

1. `GenerateProjectFiles.bat` を実行する（または `.uproject` を右クリック →「Generate Visual Studio project files」）。
   `BoxingHawkEyeSceneCreator.sln` ができます。
2. `BoxingHawkEyeSceneCreator.sln` を Visual Studio 2022 で開く。
3. 構成 `Development Editor` / `Win64`、スタートアッププロジェクト `BoxingHawkEyeSceneCreator` を選ぶ。
4. ビルド（Ctrl+Shift+B）するとプロジェクト本体とプラグインがまとめてビルドされ、F5 でエディタが起動します。

エディタ起動中は Live Coding が有効なため、Visual Studio からのビルドは失敗します。
エディタを閉じてからビルドするか、エディタ上で Ctrl+Alt+F11（Live Coding）を使ってください。

### データの置き場所

| データ | 場所 | 備考 |
| --- | --- | --- |
| Hawk-Eye フレーム（BSON, 1 フレーム 1 ファイル）と `rounds.json` | `Data/HawkEye/` | Git 管理外。別途共有してください |
| IK モデル `ik_huma_causal_w9.onnx` | `Plugins/HUMALiveHawkEye/Resources/` | 同梱 |
| 選手の体型・トラック情報 | `Content/retarget_runtime_boxing_50fps_f9/` | 同梱 |

フレームデータの場所はレベル内 `HUMALiveHawkEye` アクターの **Frames Folder** で変更できます。
相対パスはプロジェクトフォルダ基準です。指定先が存在しない場合は `Data/HawkEye` を使います。

## 操作

画面下の操作パネル（すりガラス UI）またはキーボードで操作します。

| キー / ボタン | 機能 |
| --- | --- |
| Pause / シークバー | 再生・一時停止、時刻移動（R1〜R10 はラウンド区間） |
| `Y` | 再生速度の切り替え |
| `T` | スロー（0.1x）の切り替え（残像表現つき） |
| `F` | フリーカメラ（WASD 移動 / Q,E 上下 / Shift 高速 / 右ドラッグ・矢印キーで視点） |
| `↑` / `↓` | カメラのズームイン・アウト（最大ズームで選手の腰の高さ） |
| `H` | 頭の軌跡トレース ON/OFF |
| `G` | グローブの軌跡トレース ON/OFF |
| `K` | 技表示（左右のカード）ON/OFF |
| Annotation Settings | 技ごとのアノテーション表示 ON/OFF パネル |
| Hide UI | 操作パネルを隠す |

その他:

- カメラがロープに近づくと、近い側のロープが自動で透けます（`RopeFader` アクターで距離を調整）。
- 技ごとのアノテーション初期値は、レベル内 `TechAnnotator` の Details → `AnnotationSwitches` で設定できます。

## フォルダ構成

```
BoxingHawkEyeSceneCreator.uproject
GenerateProjectFiles.bat        Visual Studio ソリューション生成
Source/                         プロジェクト本体の C++ モジュール（Target.cs / Build.cs）
Config/                         プロジェクト設定
Content/
  HUMARuntime/                  起動マップ、ゲームモード、操作 UI（WBP_HUMARuntimeControls）
  BoxingArena/                  会場・リングのメッシュとマテリアル
  BoxingRing/                   カメラ、残像、軌跡 FX、ロープフェード、技タイムライン/アノテーション
    Techniques/                 BP_TechniqueTimeline（技の時刻表）、WBP_TechniqueFeed（技カード）
    Techniques/Anno/            BP_TechAnnotator（技に応じたアノテーション表示）
  HUMA_Asset/                   HUMA 体型キャラクター（BP_CharaBase など）
  GarmentAssets/                ウェア・シューズ（HUMAClothBind）
  retarget_runtime_boxing_50fps_f9/  選手の体型・トラックのランタイムデータ
Plugins/
  HUMALiveHawkEye/              Hawk-Eye フレーム → 因果 IK（ONNX/NNE）→ HUMA 姿勢
  HUMADynamicIdentityChanger/   HUMA ランタイム再生・体型
  WOCDeformer/                  WOC 変形（Deformer Graph）
  HUMAClothBind/                ウェアのバインド
  BaseballHawkEyeSceneCreator/  ウェア用マテリアル（コンテンツのみ）
Tools/
  HUMA_TechniqueAnalysis/       モーションから技の時刻表を作る解析スクリプト（Python）
Data/HawkEye/                   Hawk-Eye フレームデータ置き場（Git 管理外）
```

## 技の時刻表を作り直す

`Tools/HUMA_TechniqueAnalysis/` の Python スクリプトで、モーションデータから技の発動区間を解析して
`technique_table_*.csv` を作ります。結果は `BP_TechniqueTimeline` の配列に取り込まれています。

## エディタを MCP 付きで起動する

`Launch_MCP8000.bat` は Model Context Protocol サーバー（ポート 8000）付きでエディタを起動します。
UE のインストール先が異なる場合は、バッチ内の `UE_ROOT` を書き換えてください。

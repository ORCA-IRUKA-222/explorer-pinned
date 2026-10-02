# Explorer Pinned 📌

Windows のエクスプローラーで、よく使うファイルやフォルダーを **フォルダーの一番上にピン止め** できる無料ツールです。
ダウンロードフォルダーなどで、名前順・更新日時順のどちらで並べても、ピン止めした項目は常に一番上の
「**∨ ピン止め**」グループに表示されます。

![Windows 11 のエクスプローラーで、ピン止めした bravo と delta が「ピン止め」グループとして一番上に表示されている](docs/screenshot-windows11.png)

*更新日時の新しい順に並べても、ピン止めした項目は一番上の「ピン止め」グループに表示されます。
（自動テスト環境の画面です。Windows が英語のため、もう一方のグループ名が「Unspecified」になっています。日本語の Windows では「指定なし」と表示されます。）*

English summary is [below](#english).

## 特長

- 右クリック →「**このファイルをピン止めする**」/「**このフォルダーをピン止めする**」でピン止め
- ピン止めした項目は「ピン止め」グループとして一番上に表示（名前順・更新日時順・サイズ順など、どの並べ替えでも一番上）
- 右クリック →「**ピン止めを外す**」で解除
- ダウンロードに限らず、どのフォルダーでも使えます
- エクスプローラー本体を書き換えたり、エクスプローラーに DLL を読み込ませたりしません
  （Windows が公開している COM の API だけを使っています）
- ファイル自体には何も書き込みません（名前も更新日時も変わりません）
- 無料・オープンソース（MIT ライセンス）

## 動作環境

- Windows 10 / Windows 11（x64 / ARM64）
- 自動テスト: GitHub Actions 上の Windows Server 2022（Windows 10 と同じエクスプローラー）と
  Windows Server 2025（Windows 11 と同じエクスプローラー）で、本物のエクスプローラーを使って毎回テストしています。
  Windows 10 スタイルの画面は [docs/screenshot-windows10.png](docs/screenshot-windows10.png) を参照してください。

## インストール

1. [Releases](https://github.com/ORCA-IRUKA-222/explorer-pinned/releases) から
   `ExplorerPinned-Setup-<バージョン>.exe` をダウンロードして実行します。
2. 管理者の確認が 1 回表示されます。「ピン止め」グループの名前を Windows に登録するためです。
3. インストールが終わると常駐を開始します（通知領域にピンのアイコンが出ます）。次回からはサインイン時に自動で起動します。

> [!NOTE]
> コード署名をしていないため、「Windows によって PC が保護されました」と表示されることがあります。
> 「詳細情報」→「実行」で続行できます。気になる場合は、このリポジトリのソースから自分でビルドすることもできます。

### ポータブル版（zip）

インストーラーを使わない場合は `ExplorerPinned-<バージョン>-x64.zip`（ARM 版 PC では `-arm64.zip`）を
好きな場所に展開し、`ExplorerPinned.exe` をダブルクリックします。
初回はセットアップの確認が出るので「OK」を押してください（管理者の確認が 1 回出ます）。
セットアップ後に exe を別の場所へ移動した場合は、もう一度ダブルクリックしてセットアップし直してください。

## 使い方

### ピン止めする

ファイルまたはフォルダーを右クリックして、「このファイルをピン止めする」または「このフォルダーをピン止めする」を選びます。
複数選択してまとめてピン止めすることもできます（Windows の仕様で一度に 15 個まで）。

> [!TIP]
> **Windows 11** では、右クリックで最初に出る短いメニューには表示されません。
> 「**その他のオプションを確認**」を選ぶか、**Shift キーを押しながら右クリック** すると表示されます。

### ピン止めを外す

ピン止め済みの項目を右クリックして「ピン止めを外す」を選びます。
通知領域のアイコン →「ピン止め中の項目」からも外せます。

### 表示のしくみ

- ピン止めした項目があるフォルダーを開くと、エクスプローラーの「グループ化」が自動で「ピン止め」になり、
  ピン止めした項目が一番上の「ピン止め」グループにまとまります。
- それ以外の項目は「指定なし」グループに入ります（このグループ名は Windows が決めているため変更できません）。
- 並べ替え（名前・更新日時・種類・サイズなど）は普段どおり自由に変えられます。どの並べ替えでもピン止めグループが一番上です。
- 一時的に別のグループ化（例: 種類）を選ぶと、そのウィンドウではそのまま維持されます。フォルダーを開き直すと「ピン止め」グループに戻ります。
- フォルダー内のピン止めをすべて外すと、元のグループ化（例: ダウンロードの「更新日時」グループ）に戻ります。

### 通知領域（タスクトレイ）のメニュー

| メニュー | 内容 |
| --- | --- |
| ピン止め中の項目 | ピン止めの一覧。各項目から「エクスプローラーで表示」「ピン止めを外す」ができます |
| 開いているウィンドウに再適用 | 表示がずれたときに、開いているすべてのウィンドウへ再適用します |
| 存在しない項目のピン止めを整理 | 削除済みの項目のピン止めを一覧から消します |
| Windows の起動時に開始 | 自動起動のオン/オフ |
| 終了 | 常駐を終了し、開いているウィンドウのグループ化を元に戻します |

## 制限事項

- **常駐プロセスが必要です。** 「ピン止め」グループは常駐中のプロセスがエクスプローラーに指示して表示しています。
  終了中はピン止めグループが表示されません。
- **グループ化は 1 種類だけです。** ピン止めグループを表示している間は、ダウンロードフォルダー標準の
  「今日」「昨日」といった日付のグループは表示されません（並べ替えは日付順にできます）。
- **ピン止めはパスで記録します。** ピン止めした項目の名前を変えたり別のフォルダーへ移動したりすると、ピン止めは外れます。
  削除した項目と同じ名前の項目が後から作られると、再びピン止めされた状態になります。
- ファイルを開く/保存するダイアログ、「PC」「ホーム」「ライブラリ」、検索結果などの仮想フォルダーでは使えません。
- ピン止めしていない項目が入る「指定なし」グループの名前は変更できません（Windows の仕様）。
- 同じフォルダーを表示していても、隠しファイルとして非表示になっている項目はピン止めグループに出ません。

## アンインストール

「設定」→「アプリ」→「インストールされているアプリ」から **Explorer Pinned** をアンインストールします。
右クリックメニュー、自動起動、ピン止めの一覧、Windows に登録した「ピン止め」の名前がすべて削除され、
開いているウィンドウのグループ化も元に戻ります。

ポータブル版の場合は、コマンドプロンプトで次を実行してから exe を削除してください。

```bat
ExplorerPinned.exe uninstall
```

## コマンドライン

```text
ExplorerPinned.exe [コマンド] [パス...]

  (コマンドなし)      必要ならセットアップして、常駐を開始
  pin <パス>...       ファイル/フォルダーをピン止め
  unpin <パス>...     ピン止めを外す
  toggle <パス>...    ピン止め/解除を切り替え
  list                ピン止め中の項目を表示
  status              登録・実行の状態を表示
  reapply             開いているウィンドウに再適用
  setup               右クリックメニュー・「ピン止め」の名前・自動起動を登録
                      (--no-startup: 自動起動を登録しない / --no-agent: 常駐を開始しない / --quiet)
  uninstall           このプログラムが登録したものをすべて削除 (--quiet)
  exit                常駐を終了
```

## しくみ（開発者向け）

- 常駐プロセスが `IShellWindows` で開いているエクスプローラーのウィンドウ（タブ）を監視します。
- ピン止めした項目があるフォルダーが表示されると、そのビューの `IFolderView2::SetViewProperty` で
  ピン止めした項目に独自のプロパティ値を設定し、`IFolderView2::SetGroupBy` でそのプロパティでグループ化します。
  値はエクスプローラーのビューのキャッシュに入るだけで、ファイルには一切書き込みません。
- エクスプローラーはグループ化のキーが変わったときにだけ項目を振り分け直すため、同じ表示名を持つ 2 つのプロパティを
  交互に使って、ピン止めの変更・F5 更新・ファイルの更新に追従しています。
- グループ名「ピン止め」は、プロパティの説明（`.propdesc`）を `PSRegisterPropertySchema` で登録して表示しています（要管理者）。
- 右クリックメニューは `HKCU\Software\Classes\*\shell` と `Directory\shell` の静的な項目です。
  `AppliesTo` 条件にピン止め中のパスを入れて、「ピン止めする」と「ピン止めを外す」を出し分けています。
- ピン止めの一覧は `HKCU\Software\ExplorerPinned\Pins` に保存されます。

### ビルド

Visual Studio 2022（C++ によるデスクトップ開発）と CMake が必要です。

```bat
cmake -B build -A x64
cmake --build build --config Release
```

`build\Release\ExplorerPinned.exe` ができます。ARM64 版は `-A ARM64` で作成できます。

### テスト

`ExplorerPinnedE2E.exe` は、本物のエクスプローラーのウィンドウを開いてピン止めの表示を UI オートメーションで確認する
エンドツーエンドテストです。管理者として実行してください（テスト用のフォルダーを `%TEMP%\ep_e2e` に作り、
最後にこのツールの登録をすべて削除します）。

```bat
build\Release\ExplorerPinnedE2E.exe build\Release\ExplorerPinned.exe
```

GitHub Actions（`.github/workflows/build.yml`）では、push のたびにビルドとこのテストを Windows Server 2022 / 2025 で実行し、
インストーラーと zip を作成します。`v1.2.3` のようなタグを push すると Releases に公開されます。

## ライセンス

[MIT License](LICENSE)

---

## English

**Explorer Pinned** pins your favorite files and folders to the top of any folder in Windows File Explorer.
Pinned items are shown in a **"Pinned"** group at the top, whatever the sort order (name, date modified, size, ...).

- Right-click a file or folder → **"Pin this file to the top"** / **"Pin this folder to the top"**; **"Unpin from the top"** to remove.
  On Windows 11 these items are under **"Show more options"** (or Shift + right-click).
- Works in every regular folder, e.g. Downloads. Files are never modified; Explorer is not patched or injected into.
- A small background process (notification area icon) applies the "Pinned" grouping through public Shell COM APIs
  (`IFolderView2::SetViewProperty` + `SetGroupBy`). Unpinned items appear in Explorer's built-in "Unspecified" group.
- Install with `ExplorerPinned-Setup-<version>.exe` from [Releases](https://github.com/ORCA-IRUKA-222/explorer-pinned/releases)
  (administrator permission is needed once to register the "Pinned" label), or unzip the portable build and run `ExplorerPinned.exe`.
- The UI is in Japanese on Japanese Windows and in English otherwise.
- Limitations: the background process must be running; only one grouping can be active, so the default date groups
  of Downloads are replaced while pins exist in that folder; pins are stored by path (renaming/moving an item unpins it).

Licensed under the [MIT License](LICENSE).

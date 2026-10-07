# コード署名（SignPath Foundation）の手順

リリースするプログラムに、[SignPath Foundation](https://signpath.org/) の証明書で署名するための手順です。
オープンソースのプロジェクトは無料で使えます。証明書の発行者（署名に表示される名前）は「SignPath Foundation」になります。

リポジトリ側の準備は済んでいます。

- README の「コード署名ポリシー」（申請の条件）
- `.github/workflows/build.yml` の「Sign binaries」「Sign installer」ジョブ（SignPath の設定が済むまでは実行されず、リリースは今までどおり未署名）
- `.signpath/artifact-configurations/` の 2 つの設定（SignPath に貼り付けるもの）

残りは SignPath の申請と設定です。アカウントの持ち主が行う必要があります。

## 1. 申請の前に

- GitHub アカウントの **2 要素認証** を有効にします（SignPath Foundation の行動規範で、プロジェクトのメンバーに求められています）。
- 申請フォームは英語です。下の「申請内容の下書き」を使ってください。

## 2. 申請する

<https://signpath.org/apply> のフォームから申請します。審査には時間がかかることがあり、必ず承認されるとは限りません。

### 申請内容の下書き（英語）

| 項目 | 内容 |
| --- | --- |
| Project name | Explorer Pinned |
| Repository | https://github.com/ORCA-IRUKA-222/explorer-pinned |
| Homepage | https://github.com/ORCA-IRUKA-222/explorer-pinned#readme |
| Download page | https://github.com/ORCA-IRUKA-222/explorer-pinned/releases |
| License | MIT |
| Build system | GitHub Actions (`.github/workflows/build.yml` in the public repository) |
| Code signing policy | https://github.com/ORCA-IRUKA-222/explorer-pinned#code-signing-policy |

Description（説明）:

> Explorer Pinned is a free Windows tool that pins files and folders to the top of a folder: pinned items are shown
> in a "Pinned" group at the top of Windows File Explorer, whatever the sort order, and also in the standard
> Open/Save dialogs (for example when choosing a file to upload in a web browser). It does not modify files or
> Explorer, and it never accesses the network.
>
> It consists of a small background program (`ExplorerPinned.exe`, which drives Explorer windows through public
> Shell COM APIs) and a shell extension DLL (`ExplorerPinnedShell.dll` / `ExplorerPinnedShell32.dll`). The DLL is
> registered by the installer as an icon overlay handler that shows no overlay, so that Windows loads it into
> programs that show a file dialog; there it only changes the grouping of the dialog's file list and puts the
> original grouping back before the dialog closes. The DLL is registered only when the program is installed in
> Program Files, and the uninstaller removes everything.
>
> Files to sign: `ExplorerPinned-Setup-<version>.exe` (Inno Setup), `ExplorerPinned.exe` (x64, ARM64),
> `ExplorerPinnedShell.dll` (x64, ARM64), `ExplorerPinnedShell32.dll` (x86). All are built by GitHub Actions from
> the source code in the repository; every build runs end-to-end tests against real Explorer windows and file
> dialogs on Windows Server 2022 and 2025.

DLL がほかのアプリに読み込まれる仕組みは、説明がないと「望ましくないプログラム」と誤解されるおそれがあります。
そのため、説明文の中で目的と範囲をはっきり書いています。

## 3. 承認されたら：SignPath の設定

承認されると SignPath.io の組織（Organization）が用意され、案内のメールが届きます。SignPath.io にログインして、次を設定します。

1. **プロジェクト**: slug（URL などに使う短い名前）を `explorer-pinned` にします。
   別の slug になった場合は、手順 4 で変数 `SIGNPATH_PROJECT_SLUG` を設定します。
2. **Trusted Build System**: GitHub.com をプロジェクトに追加します（GitHub Actions でビルドしたファイルだけを署名する設定）。
3. **Artifact configurations**: 次の 2 つを作り、リポジトリのファイルの内容をそのまま貼り付けます。
   slug は名前のとおりにします（ワークフローがこの名前で指定します）。
   - `binaries` ← `.signpath/artifact-configurations/binaries.xml`
   - `installer` ← `.signpath/artifact-configurations/installer.xml`
4. **Signing policies**: `test-signing`（テスト用の証明書）と `release-signing`（本番の証明書）を使います。
   CI 用のユーザーを作り、両方の Submitter にします。自分を Approver にします。
5. **API トークン**: CI 用のユーザーの API トークンを作成します。

## 4. GitHub の設定

リポジトリの Settings → Secrets and variables → Actions で、次を登録します。

| 種類 | 名前 | 値 |
| --- | --- | --- |
| Secret | `SIGNPATH_API_TOKEN` | 手順 3-5 の API トークン |
| Variable | `SIGNPATH_ORGANIZATION_ID` | SignPath の組織の ID |
| Variable（任意） | `SIGNPATH_PROJECT_SLUG` | プロジェクトの slug が `explorer-pinned` 以外のとき |

`SIGNPATH_ORGANIZATION_ID` を登録した時点から、リリースは署名されてから公開されます。

## 5. 試す

Actions → Build → Run workflow で、`sign` にチェックを入れ、`signing_policy` に `test-signing` を選んで実行します
（`release` はチェックしません）。SignPath の画面で署名を承認すると、実行結果の成果物 `packages-signed` に
署名済みのインストーラーと zip が入ります。

## 6. リリース

今までどおり、`src/version.h` のバージョンを上げて、Run workflow で `release` にチェックを入れて実行します。

- 「Sign binaries」（本体と DLL）と「Sign installer」（インストーラー）で、**SignPath の承認が 2 回**必要です。
  承認を待つ間、ジョブは最大 3 時間ほど待機します。
- 署名に失敗した場合や承認しなかった場合は、リリースは公開されません（未署名のまま公開されることはありません）。

## 7. 署名が始まったら

- README の「コード署名ポリシー」の「申請中」の記述を外します。
- README の「コード署名をしていないため…」の注意書きと `.github/release-notes.md` の同じ注意書きを、
  「発行元が SignPath Foundation と表示されます」という案内に変えます。

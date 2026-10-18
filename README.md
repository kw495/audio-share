# Audio Share(modified)

Windows / Linux の PC で鳴っている音を、同じ LAN 内の Android 端末に送って再生するアプリです。
[Audio Share](https://github.com/mkckr0/audio-share)（作者 mkckr0、Apache License 2.0）を改造したもので、公式版ではありません。

## 本家からの主な変更点

- **通信量の削減**：無音の間は送らない／Opus 圧縮（鳴っている間 約 4KB/s）
- **送信形式の設定**：チャンネル（モノラル・ステレオ）と音質（Opus のビットレート、PCM のサンプルレート）
- **テストトーン**：モールス信号を送れるので、サウンドデバイスがなくても動作確認できる
- **Android**：出力先（メディア・アラーム・通知・着信音）の選択、状態・受信形式・ログの表示
- **日本語表示**：Windows GUI と Android アプリ

このフォークのサーバーとアプリは組み合わせて使ってください（公式版のアプリは Opus を再生できません）。Android アプリは公式版と別の ID なので、並べてインストールできます。

## 必要なもの

- サーバー：Windows 10 以降（x64、[Visual C++ 再頒布可能パッケージ](https://aka.ms/vs/17/release/vc_redist.x64.exe)）、または PipeWire が動く Linux
- スマホ：Android 6.0 以降
- PC とスマホが通信できるネットワーク

PC にサウンドデバイスがない場合は、仮想オーディオデバイス（VB-CABLE など）を別途入れてください。このリポジトリには含まれていません。

## 使い方

サーバー（コマンドライン版）：

```
as-cmd --bind=0.0.0.0 --encoding=opus --channels=1
as-cmd --bind=0.0.0.0 --test-tone --encoding=opus
```

主なオプション：`--encoding`（`opus`、`s16` など）、`--channels`（1／2）、`--opus-bitrate`、`--sample-rate`、`--test-tone[=文字列]`、`--no-silence-gate`。一覧は `as-cmd --help`。
GUI 版（AudioShareServer.exe）でも同じ設定ができます。

アプリ：ホーム画面で PC の IP アドレスとポート（既定 65530）を入れて再生。出力先は「音声」画面で選びます。

## ビルド

- Windows：vcpkg（トリプレット `x64-windows-static-md`）で `asio protobuf cxxopts "spdlog[wchar]" wil nlohmann-json opus` を入れる
  - コマンドライン版：`server-core` で `cmake --preset windows-Release` → `cmake --build --preset windows-Release`
  - GUI 版：`server-mfc` で `msbuild audio-share-server.sln /t:Rebuild /p:Configuration=Release,Platform=x64`（言語別リソースの生成に Python が必要）
- Android：`android-app` で `gradlew assembleDebug`（JDK 17。release 署名用の `keystore.properties` がないと失敗するので、debug ビルドでもダミーを置く）

## ライセンス

Apache License 2.0（[LICENSE](LICENSE)）。本家の著作権表示と、このフォークでの変更点は [NOTICE](NOTICE) を参照してください。

使用ライブラリ：
[Asio](https://github.com/chriskohlhoff/asio)（BSL-1.0）、
[nlohmann/json](https://github.com/nlohmann/json)（MIT）、
[WIL](https://github.com/microsoft/wil)（MIT）、
[Protocol Buffers](https://github.com/protocolbuffers/protobuf)（BSD-3-Clause）、
[spdlog](https://github.com/gabime/spdlog)（MIT）、
[{fmt}](https://github.com/fmtlib/fmt)（MIT）、
[cxxopts](https://github.com/jarro2783/cxxopts)（MIT）、
[Opus](https://opus-codec.org/)（BSD-3-Clause）、
[PipeWire](https://gitlab.freedesktop.org/pipewire/pipewire)（MIT）、
[Ktor](https://github.com/ktorio/ktor)（Apache-2.0）、
[MaterialKolor](https://github.com/jordond/MaterialKolor)（MIT）

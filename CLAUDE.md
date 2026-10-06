# Scramble — Claude Code 向けの仕様書

スペクトルを帯域の塊に切って並べ替えるグリッチ系エフェクト (VST3、JUCE 8.0.15)。名前は仮名 (ユーザーが後で決める)。
作者は MIDy (音楽プロデューサー)。Claude Code が作り、MIDy が Ableton Live で確認している (2026-09-26 実機で「いい感じ」)。
SpecGlitch (別プラグイン、中止済み) の scramble 機能を単体にしたもの。
このファイルは、別のマシン (Windows を含む) の Claude Code が作業を引き継げるように、仕様・決定事項・ビルド方法をまとめたもの。

## 前提と作業の約束

- 音を聴いて良し悪しを判断するのはユーザー。Claude は数値で壊れていないこと (切り替え時刻・レベル・レイテンシ・CPU) を確かめる
- パラメータ ID・PLUGIN_CODE (Scrm)・PLUGIN_MANUFACTURER_CODE (MIDy)・BUNDLE_ID (com.midy.scramble) は変えない。
  変えると保存済みの Live セットで設定が読めなくなる。Mac 版と Windows 版も同じ ID なので、セットを持ち回せる
- 表示名を変えても ID は据え置く (例: Stay の ID は hold のまま)
- コミットの身元は `git config user.name` = midy4234-hub、`user.email` = 331112920+midy4234-hub@users.noreply.github.com。
  本名やホスト名由来のアドレス、個人の Gmail をコミットに入れない。コミット前に `git config --show-origin --get-regexp "^user\."` で確認する

## パラメータ (ID / 表示名 / 範囲 / 既定)

| ID | 表示名 | 範囲 | 既定 | 中身 |
|---|---|---|---|---|
| mode | Mode | 0 Random / 1 Octave | Random | 下の「並べ替え」参照 |
| size | Size | 0.04〜1 オクターブ | 0.25 | 塊の幅。Octave では 1/N (N = 1/Size を丸めた整数) |
| reach | Reach | 0〜8 オクターブ | 3 | 塊が動く最大の距離。0 で元のまま。単調 (上げるほど遠くへ) |
| keeplow | Keep Low | 0〜1000 Hz | 200 Hz | これより下は動かさない (つまみの値どおり) |
| shuffle | Shuffle | ボタン | | 押した瞬間に新しい並べ替えへ |
| reset | Reset | ボタン | | 押した瞬間に元の並びへ |
| trans | Transient Trig | on/off | on | アタックで切り替え |
| sens | Sensitivity | 0〜100 % | 50 % | 閾値 18 dB (0) 〜 3 dB (100) |
| hold | Stay | 60〜1000 ms | 100 ms | 一度切り替えたら次のアタックを受け付けるまでの時間。表示名はユーザーの命名 (MIDI の Hold と区別するため) |
| seqon | Seq Trig | on/off | on | 16 ステップのシーケンサー |
| midion | MIDI Trig | on/off | on | MIDI ノートで切り替え |
| midimode | MIDI Mode | 0 Hold / 1 Latch | Hold | Hold: 離したら戻る (他に押さえているノートがあればそのノートの並べ替えへ)。Latch: 離しても保持 |
| out | Output | -24〜12 dB | 0 | |
| bypass | Bypass | on/off | off | |

- Shuffle / Reset はパラメータなので MIDI マップや Push に割り当てられる
- Shuffle / Reset ボタンで並べ替えが変わったとき (100〜200 ms 後) と、ステップを編集したときに、
  `updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true))` でホストに知らせる。
  知らせないと Live は古い state を持ったままで、フリーズ・バウンスが並べ替え前の音になった (2026-10-06 ユーザー報告)。
  トランジェント・シーケンサー・MIDI による切り替えでは知らせない (再生中ずっと「変更あり」になるため)
- シーケンサーの 16 ステップ (空 / S = Shuffle / R = Reset) と、現在の並べ替え (shuffled・seed) はパラメータではなく state の "pattern" 子要素に保存する

## DSP (Source/ScrambleEngine.h、STFT は lab/LabStft.h)

- STFT: Hann + Hann、重なり 75 %。フレーム長は 48 kHz で 2048 (サンプルレートに比例、2^8〜2^14)、hop = n/4
- レイテンシ = フレーム長 + D (D = 2 ms。トランジェントの先読みぶん)。setLatencySamples で申告している
- 並べ替え (Random): Keep Low より上、20 kHz までを Size オクターブずつの塊に切る (低域は 1 ビン未満にならない)。
  塊ごとに乱数 u ∈ [0,1) を持ち、「塊の位置 (オクターブ) + Reach × u」の順に、塊をそれぞれの幅のまま並べ直す (テープを切って並べ替えるのと同じ)。
  塊は整数ビンずれる = 周波数シフト。フレームごとに位相を回して、どんなずれ幅でも濁らないようにする
  (Δk が 4 の倍数でないとフレームごとに位相が食い違って濁る)
- Octave モード (ユーザーの発想「入れ替え先との周波数比を 1:2 に固定」): 1/N オクターブの塊を Keep Low (最低 20 Hz) から切り、
  音名の帯 (列) ごとに、段 (オクターブ) を「段 + Reach × u」の順に並べ替える。塊は 2^k 倍にピッチシフトして移す → 音名は保たれ、帯域ごとのオクターブだけ変わる。
  ピッチシフトはピーク単位 (Laroche & Dolson): 谷で区切った領域を丸ごと整数ビン d 移し、θ を積み上げて位相をそろえる
- 切り替え: 並べ替えを 2 つの「スロット」で持ち、切り替えの前後だけ両方を STFT で作って、時間領域で 1 ms クロスフェード。
  切り替わりの時刻がフレームの都合に縛られず、指定時刻 (アタックの頭、16 分の境目、ノート) ぴったりに来る
  (原則「タイミングは時間領域、中身は STFT」。STFT 内で切り替えると N = 2048 で 12.9 ms 鈍り、最大 5 ms ずれた)
- スロットが 2 つなので、切り替えどうしは minGap (フレーム長 + hop + 2 ms、48 kHz で約 55 ms) 以上あける。
  詰まった指示は、あけられる最も早い時刻まで遅らせる (最後の指示が必ず効く。捨てない)
- トランジェント: 入力を D だけ遅らせて STFT に入れ、検知時刻の 1 ms 前に切り替えを置く
- シーケンサー: ホストの ppq に同期した 16 分音符 × 16 ステップ
- MIDI: ノート番号ごとに固定の並べ替え (seed = ノート番号から決まる)。同じ鍵を押せば毎回同じ並びになる

## UI (Source/PluginEditor.cpp、共通の見た目は lab/LabLook.h)

- ChordRes と同じ Ableton 純正寄りの見た目
- パネル: SCRAMBLE (Mode・Size・Reach・Keep Low・Shuffle/Reset) / SPECTRUM / TRIGGER (Transient・MIDI・Seq の各ブロック、ステップ列) / MASTER (Output・Bypass・メーター)
- 色分けスペクトル (ユーザーが選んだ表示): 上が入力、下が出力。色 = その音が元いた周波数 (低 = 赤 → 高 = 紫)。
  出力側で色が入れ替わって並ぶ = そこへ飛んできた音。Keep Low より下は色を落とす。下端の目盛は塊の切れ目
- ステップ列: クリックで 空 → S → R → 空、右クリックで空

## 状態と経緯

- 耐久テスト (robust) FAIL 0 (既定と Octave の両方)
- Windows (GitHub Actions、MSVC) でも robust は 0 FAIL / 0 WARN (2026-10-01)
- Reach は当初「20 Hz 起点の区画」方式で、Reach を上げると低域の飛ぶ範囲が逆に狭まる所があった → ユーザー指摘で「最大でどこまで飛ぶか」(単調) に作り直し、塊の幅は Size で別に指定
- SpecGlitch は中止・削除済み。同種の可視化ツールや SpecGlitch の再提案はしない

## ビルド

JUCE は `../_deps/JUCE` があればそれを使い、無ければ CMake が GitHub から 8.0.15 を取ってくる (初回は数分かかる)。
`lab/` は PluginLab (MIDy の非公開の作業場) の共通ヘッダ (LabLook.h / LabStft.h / LabTest.h / LabRobust.h) の写し。
隣に `../PluginLab` があればそちらが優先される。プラグイン本体も LabLook.h と LabStft.h を使うので、lab/ を消すとビルドできない。

### Windows

必要なもの: Visual Studio 2022 以降 (「C++ によるデスクトップ開発」ワークロード。CMake も同梱)、Git。

```
cmake -B build -A x64
cmake --build build --config Release --target Scramble_VST3
```

- できるもの: `build\Scramble_artefacts\Release\VST3\Scramble.vst3` (フォルダ)
- フォルダごと `C:\Program Files\Common Files\VST3\` にコピーする (管理者権限が要る)。
  管理者のシェルなら `-DLAB_INSTALL=ON` を付けて configure すればビルド後に自動でコピーされる (既定は OFF。権限が無いとビルドが失敗するため)
- Live: 環境設定 → Plug-ins → 「VST3 プラグイン システムフォルダ」をオンにして再スキャン。MIDI で切り替えるときは別の MIDI トラックの MIDI To を「このトラック → Scramble」に
- ソースは UTF-8 の日本語コメント入り。CMakeLists で MSVC に `/utf-8` を渡している (外すと C4819 や誤コンパイル)

### Mac

```
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```
ユニバーサル (x86_64 + arm64)。`~/Library/Audio/Plug-Ins/VST3/` に自動でコピーされる (`-DLAB_INSTALL=OFF` で止められる)。

### GitHub Actions

`.github/workflows/windows.yml`: main への push (と手動実行) で Windows x64 の VST3 をビルドし、
Actions の実行ページの Artifacts に `Scramble-windows-x64` として置く。同時に検証ドライバの耐久テストも走らせる。

## 検証ドライバ (DAW 無し)

```
cmake -B build-test -DLAB_BUILD_TESTS=ON -DLAB_INSTALL=OFF
cmake --build build-test --config Release --target ScrambleTest
build-test/ScrambleTest_artefacts/Release/ScrambleTest robust                      # 標準の耐久テスト 12 項目
build-test/ScrambleTest_artefacts/Release/ScrambleTest render in.wav out.wav mode=1 # 任意設定の試聴用 A/B (原音2周 → 無音 → 処理後2周)
build-test/ScrambleTest_artefacts/Release/ScrambleTest snapshot out.png            # UI の画像
build-test/ScrambleTest_artefacts/Release/ScrambleTest params                      # パラメータ一覧
build-test/ScrambleTest_artefacts/Release/ScrambleTest                             # 固有テスト
```
変更したら最低でも固有テストと robust を通す。

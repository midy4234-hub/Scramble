# Scramble — スペクトルを帯域の塊ごとに並べ替えるグリッチ・エフェクト (VST3)

デモ

https://github.com/user-attachments/assets/a6b92257-0d08-478e-8651-3010080000e8


ChordRes と組み合わせたデモ

https://github.com/user-attachments/assets/38bca587-f39a-4684-97b2-493c34ba0431

コードは Claude Code (Anthropic の AI) が書き、MIDy が仕様を決めて Ableton Live で確認しました。
無保証です。不具合の報告は Issue で受け付けますが、返事や修正は約束しません。Pull Request は受け付けません。
ライセンスは AGPLv3 (LICENSE)。JUCE (AGPLv3) と VST3 SDK (MIT) を使っています。Copyright (C) 2026 MIDy

Made with Claude Code. Provided as-is. Bug reports via Issues are welcome, but replies and fixes are not promised. Pull requests are not accepted. Licensed under AGPLv3.

**ダウンロード**: Releases に Mac 版 (Intel / Apple Silicon 両対応) と Windows 版 (x64) の zip があります。

- Mac: `Scramble.vst3` を `~/Library/Audio/Plug-Ins/VST3/` に入れる。署名していないので、入れたあとターミナルで

  ```
  xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/Scramble.vst3
  ```

- Windows: `Scramble.vst3` フォルダごと `C:\Program Files\Common Files\VST3\` に入れる

ビルド方法は CLAUDE.md の「ビルド」。

音を周波数の塊に切って並べ替える。切り替えはアタック・16 分のステップ・MIDI ノートのタイミングぴったりに起きる。
レイテンシは 48 kHz で約 45 ms (DAW が自動で補正する)。

パラメータ
- SCRAMBLE
  - Mode: Random (塊を周波数シフトで並べ直す) / Octave (音名を保ったまま、帯域ごとにオクターブだけ入れ替える)
    / Mirror・Rotate・Swap (乱数を使わず規則で並べる。つまみが同じなら毎回同じ並び)
    - Mirror: Reach の幅ごとに、塊の順番を逆さにする
    - Rotate: 下の塊を上端へ回し、残りはそのぶん下へずらす
    - Swap: 塊の組の前半と後半を入れ替える (Reach が小さいと隣どうし)
    - 既定の Reach 3 では Rotate と Swap がほぼ同じ並びになる。違いが聞こえやすいのは Reach 1〜2 あたり
  - Size: 塊の幅 (オクターブ)
  - Reach: 塊が動く最大の距離 (オクターブ)。0 で元のまま
  - Keep Low: これより下は動かさない
  - Spray: 塊ごとに左右交互へ振ってステレオを広げる。左右を足すと元に戻る (モノラルにしても変わらない)。並べ替えていないときも効く
  - Shuffle / Reset: 押した瞬間に新しい並べ替え / 元に戻す
- TRIGGER
  - Transient: アタックで切り替え。Sensitivity、Stay (切り替えた後、次のアタックを受け付けるまでの時間)
  - MIDI: ノートごとに決まった並べ替えになる。Hold (離すと戻る) / Latch (離しても保持)。
    別の MIDI トラックの MIDI To を「このトラック → Scramble」にする
  - Seq: 16 ステップ。クリックで 空 → S (Shuffle) → R (Reset)、右クリックで空
- MASTER: Bypass / Dry/Wet (原音と処理後を混ぜる) / Output (混ぜたあとの全体)
- 画面: 上が入力、下が出力のスペクトル。色はその音が元いた周波数 (低 = 赤 → 高 = 紫)

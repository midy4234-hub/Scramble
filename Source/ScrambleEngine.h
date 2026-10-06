#pragma once

// Scramble の DSP (knowledge/spectral_glitch.md の研究と SpecGlitch の続き)
//
// 並べ替え: Keep Low より上、20 kHz までのスペクトルを size オクターブずつの塊に切る (低域は 1 ビン未満にならない)。
//           塊ごとに乱数 u ∈ [0,1) を持ち、「塊の位置 (オクターブ) + reach × u」の順に、塊をそれぞれの幅のまま並べ直す
//           (テープを切って並べ替えるのと同じ)。→ 各塊が動くのはおよそ reach オクターブ以内。reach 0 で元のまま、上げるほど遠くへ。
//           決まった区画の境目は無い (最初の版は 20 Hz 起点の区画で、reach を上げると低域の飛ぶ範囲が逆に狭まる所があった)。
//           塊は整数ビンずれる = その幅ぶんの周波数シフト。フレームごとに位相を回して、どんなずれ幅でも濁らないようにする。
// Octave モード (ユーザーの発想「入れ替え先との周波数比を 1:2 に固定」): 1/N オクターブの塊を Keep Low (最低 20 Hz) から切り、
//           音名の帯 (列) ごとに、段 (オクターブ) を「段 + reach × u」の順に並べ替える。塊は 2^k 倍にピッチシフトして移す
//           → 音名は保たれ、各帯域が何オクターブ上下するかだけ変わる。ピッチシフトはピーク単位 (Laroche & Dolson):
//           谷で区切った領域を丸ごと整数ビン d 移し、θ を積み上げて位相をそろえる (research/stft/glitch5_octave.py が基準実装)。
// 切り替え: 並べ替えを 2 つの「スロット」で持ち、切り替えの前後だけ両方を STFT で作って、時間領域で 1 ms クロスフェードする。
//           → 切り替わりの時刻がフレームの都合に縛られず、指定した時刻 (アタックの頭、16 分の境目、ノート) ぴったりに来る。
//           スロットは 2 つなので、切り替えどうしは minGap (フレーム長 + hop + 2 ms、48 kHz で約 55 ms) 以上あける。
//           それより詰まった指示は、あけられる最も早い時刻まで遅らせる (最後の指示が必ず効く。捨てない)。
// トリガー: 手動ボタン / トランジェント検知 / 16 ステップのシーケンサー / MIDI ノート。
//           トランジェントは入力を D (2 ms) だけ遅らせて STFT に入れ、検知した時刻の 1 ms 前に切り替えを置く (先読み)。
// レイテンシ: フレーム長 + D。

#include <juce_audio_processors/juce_audio_processors.h>
#include "LabStft.h"
#include <array>
#include <atomic>
#include <vector>

namespace scr
{
    constexpr int numSteps = 16;
    constexpr int maxBins = (1 << 14) / 2 + 1;
    enum StepAction { stepNone = 0, stepShuffle = 1, stepReset = 2 };
    enum Source { srcButton = 0, srcTransient = 1, srcSeq = 2, srcMidi = 3, srcMode = 4 };
    enum Mode { modeRandom = 0, modeOctave = 1 };
    constexpr int maxCuts = 512;

    struct Pattern
    {
        std::array<std::atomic<int>, numSteps> steps {};
    };

    struct Transport
    {
        bool playing = false;
        double bpm = 120.0;
        double ppqAtBlockStart = 0.0;
    };

    struct Settings
    {
        float reach = 3.0f, keepLow = 200.0f, size = 0.25f;
        int mode = modeRandom;
        bool transOn = true, seqOn = true, midiOn = true, midiLatch = false;
        float sensitivity = 50.0f, holdMs = 100.0f;
        bool shufflePressed = false, resetPressed = false;
    };

    // UI に見せるもの (オーディオスレッドが書き、UI が読む)
    struct Display
    {
        std::array<std::atomic<float>, maxBins> inMag {};            // 入力の振幅スペクトル (0 dBFS のサイン波 ≒ 1)
        std::array<std::array<std::atomic<int>, maxBins>, 2> srcBin {};   // スロットごとに、出力のビン k に来る入力のビン
        std::atomic<int> bins { 0 }, audibleSlot { 0 };
        std::atomic<float> sampleRate { 48000.0f }, fftSize { 2048.0f }, keepLow { 200.0f }, reach { 3.0f };
        std::atomic<bool> audibleShuffled { false };
        std::atomic<int> switchCount { 0 }, lastSource { srcButton }, transientCount { 0 };
        std::atomic<int> seqStep { -1 };
        // 塊の切れ目 (ビン)。スロットごとに、入力側 (元の切れ目) と出力側 (並べ直した後の切れ目)
        std::array<std::array<std::atomic<int>, maxCuts>, 2> cutIn {}, cutOut {};
        std::array<std::atomic<int>, 2> numCutIn {}, numCutOut {};
        std::atomic<int> audibleMode { modeRandom }, octaveDivisions { 4 };
    };

    struct State
    {
        bool shuffled = false;
        uint32_t seed = 1;
        int mode = modeRandom;   // 並べ替え中のモード (元の音のときは意味を持たない)
        bool operator== (const State& o) const { return shuffled == o.shuffled && (! shuffled || (seed == o.seed && mode == o.mode)); }
    };

    inline uint32_t mix32 (uint32_t x)
    {
        x += 0x9e3779b9u;
        x = (x ^ (x >> 16)) * 0x85ebca6bu;
        x = (x ^ (x >> 13)) * 0xc2b2ae35u;
        return x ^ (x >> 16);
    }
    inline uint32_t noteSeed (int note) { return mix32 (0xC0FFEEu + (uint32_t) note * 7919u); }
    // Shuffle (ボタン・トランジェント・シーケンサー) の新しい種: 今の種から決まる (保存した状態から先の並びが再現できる)
    inline uint32_t nextSeed (uint32_t s)
    {
        const uint32_t v = mix32 (s ^ 0x5bd1e995u);
        return v == s ? mix32 (v) : v;
    }

    class Engine
    {
    public:
        Display display;

        void prepare (double sampleRate, int channels)
        {
            sr = sampleRate;
            nch = channels;
            const int fftOrder = juce::jlimit (8, 14, (int) std::round (std::log2 (2048.0 * sr / 48000.0)));
            n = 1 << fftOrder;
            hop = n / 4;
            bins = n / 2 + 1;
            stft.prepare (n, hop, channels, 2);
            D = (int) std::round (0.002 * sr);
            pre = (int) std::round (0.001 * sr);
            halfRamp = std::max (1, (int) std::round (0.0005 * sr));
            rampStep = 1.0f / (float) (2 * halfRamp);
            minGap = n + hop + (int) std::round (0.002 * sr);

            inDelay.assign ((size_t) channels, std::vector<float> ((size_t) D, 0.0f));
            delayPos = 0;
            delayed.setSize (channels, 0);
            power.assign ((size_t) bins, 0.0f);
            for (auto& s : slots)
            {
                s.state = scheduled;   // 並べ替えの状態は prepare をまたいで保つ (ホストの prepare で解除されない)
                s.srcBin.assign ((size_t) bins, 0);
                s.shift.assign ((size_t) bins, 0);
                s.keys.reserve ((size_t) bins);
                s.ratio.assign ((size_t) bins, 1.0f);
                s.prev.assign ((size_t) channels, Track {});
                s.cur.assign ((size_t) channels, Track {});
                for (auto* tv : { &s.prev, &s.cur })
                    for (auto& t : *tv) { t.pk.reserve ((size_t) bins); t.d.reserve ((size_t) bins); t.theta.reserve ((size_t) bins); t.r.reserve ((size_t) bins); }
                s.builtFor = -1;
                s.trackFor = -2;
            }
            // Octave モードの解析 (チャンネルごと)
            mag.assign ((size_t) bins, 0.0f);
            phase.assign ((size_t) bins, 0.0f);
            prevPhase.assign ((size_t) channels, std::vector<float> ((size_t) bins, 0.0f));
            havePrev.assign ((size_t) channels, false);
            peaks.reserve ((size_t) bins); bounds.reserve ((size_t) bins + 1); omega.reserve ((size_t) bins);
            colIdx.reserve ((size_t) bins);
            kMax = std::min (bins - 1, (int) std::floor (20000.0 * n / sr));
            lastMode = -1;
            scheduledSlot = 0;
            numSwitches = 0; fc = 0; sc = 0;
            frameSlot = 0; targetSlot = 0; c = 0.0f;
            lastSwitchT = std::numeric_limits<int64_t>::min() / 2;
            hasDeferred = false;
            count = 0;
            // 検知
            fast = slow = 0.0f; holdCount = 0; armed = true;
            lastOnset = std::numeric_limits<int64_t>::min() / 2;
            fastRel = (float) std::exp (-1.0 / (0.008 * sr));   // 保持 (20 ms) のあとは速く下げる。遅いと大きい音の減衰に次の小さいアタックが埋もれる (30 ms ではハットを取りこぼした)
            slowAtt = (float) (1.0 - std::exp (-1.0 / (0.025 * sr)));
            peakHold = (int) (0.020 * sr);
            // MIDI
            held.clear();
            held.reserve (128);
            edges.reserve ((size_t) bins + 2);
            order.reserve ((size_t) bins + 2);
            sortKey.reserve ((size_t) bins + 2);
            cis.resize ((size_t) n);
            for (int r = 0; r < n; ++r)
                cis[(size_t) r] = std::polar (1.0f, (float) (2.0 * juce::MathConstants<double>::pi * r / n));
            events.clear();
            events.reserve (1024);
            lastSeqStep = std::numeric_limits<int64_t>::min() / 2;

            display.bins = bins;
            display.sampleRate = (float) sr;
            display.fftSize = (float) n;
            publishAudible (0);
        }

        int latency() const { return n + D; }
        State getState() const { return scheduled; }

        // 保存した状態に戻す (prepare の直後、またはオーディオスレッドから)
        void restore (State s)
        {
            scheduled = s;
            for (auto& sl : slots) { sl.state = s; sl.builtFor = -1; }
            scheduledSlot = 0; numSwitches = 0; fc = sc = 0; frameSlot = targetSlot = 0; c = 0.0f;
            hasDeferred = false;
            publishAudible (0);
        }

        void process (juce::AudioBuffer<float>& buf, const juce::MidiBuffer& midi, const Transport& tp,
                      const Pattern& pat, const Settings& st)
        {
            const int len = buf.getNumSamples();
            const int chs = std::min (nch, buf.getNumChannels());
            cfg = st;
            blockStart = count;
            events.clear();

            // モードを変えたら、今の種のまま新しいモードへ (ほかの切り替えと同じくクロスフェード)
            if (lastMode >= 0 && st.mode != lastMode)
                addEvent (blockStart + D, srcMode, scheduled.shuffled, scheduled.seed, false);
            lastMode = st.mode;

            // ---- 指示を集める (時刻は STFT 側の通し番号 = 入力の通し番号 + D) -----------------------
            if (st.shufflePressed) addEvent (blockStart + D, srcButton, true, 0, true);
            if (st.resetPressed)   addEvent (blockStart + D, srcButton, false, 0, false);

            if (st.midiOn)
                for (const auto meta : midi)
                {
                    const auto m = meta.getMessage();
                    const int64_t t = blockStart + juce::jlimit (0, len - 1, meta.samplePosition) + D;
                    if (m.isNoteOn())
                    {
                        held.erase (std::remove (held.begin(), held.end(), m.getNoteNumber()), held.end());
                        if (held.size() < held.capacity()) held.push_back (m.getNoteNumber());
                        addEvent (t, srcMidi, true, noteSeed (m.getNoteNumber()), false);
                    }
                    // 離して戻すのは、押さえていたノートを離したときだけ。押していないノートのノートオフや全ノートオフ
                    // (Live は再生開始や書き出しのときに送ってくる) で、ボタンやトランジェントの並べ替えを解除しない
                    else if (m.isNoteOff())
                    {
                        const auto it = std::find (held.begin(), held.end(), m.getNoteNumber());
                        if (it == held.end())
                            continue;
                        held.erase (it);
                        if (! st.midiLatch)
                        {
                            if (held.empty()) addEvent (t, srcMidi, false, 0, false);
                            else              addEvent (t, srcMidi, true, noteSeed (held.back()), false);
                        }
                    }
                    else if (m.isAllNotesOff() || m.isAllSoundOff())
                    {
                        const bool wasHeld = ! held.empty();
                        held.clear();
                        if (wasHeld && ! st.midiLatch) addEvent (t, srcMidi, false, 0, false);
                    }
                }
            else
                held.clear();

            if (st.seqOn && tp.playing && tp.bpm > 0.0)
            {
                const double spp = sr * 60.0 / tp.bpm;   // 1 拍のサンプル数
                const double q0 = tp.ppqAtBlockStart * 4.0;   // 16 分単位の位置
                const int64_t inStep = (int64_t) std::floor (q0 + 1.0e-6);
                const int64_t firstEdge = (int64_t) std::ceil (q0 - 1.0e-6);   // このブロックで最初に来る境目 (頭ちょうどを含む)
                auto fire = [&] (int64_t step, int64_t t)
                {
                    const int a = pat.steps[(size_t) patternIndex (step)].load();
                    if (a == stepShuffle) addEvent (t, srcSeq, true, 0, true);
                    else if (a == stepReset) addEvent (t, srcSeq, false, 0, false);
                    lastSeqStep = step;
                };
                // 再生開始・位置の飛び: いま居るステップの頭の動作を、ブロックの頭で
                const bool continuous = lastSeqStep == inStep || (firstEdge == inStep && lastSeqStep == inStep - 1);
                if (! continuous)
                {
                    if (firstEdge != inStep) fire (inStep, blockStart + D);
                    else lastSeqStep = inStep - 1;
                }
                for (int64_t s = firstEdge;; ++s)
                {
                    const double at = (s - q0) / 4.0 * spp;
                    if (at >= len) break;
                    fire (s, blockStart + std::max ((int64_t) 0, (int64_t) std::ceil (at - 1.0e-6)) + D);
                }
                display.seqStep = patternIndex ((int64_t) std::floor (q0 - 4.0 * (n + D) / spp + 1.0e-6));
            }
            else
            {
                lastSeqStep = std::numeric_limits<int64_t>::min() / 2;
                display.seqStep = -1;
            }

            // トランジェント (入力そのものを見る。STFT は D 遅れて同じ音を処理するので、先読みになる)
            detect (buf, chs, len);

            std::stable_sort (events.begin(), events.end(), [] (const Event& a, const Event& b) { return a.t < b.t; });
            for (auto& e : events) schedule (e);
            if (hasDeferred && deferred.t - halfRamp < blockStart + len + 1)
            {
                hasDeferred = false;
                doSwitch (deferred);
            }

            // ---- 入力を D 遅らせて STFT へ ----------------------------------------------------------
            delayed.setSize (chs, len, false, false, true);
            for (int ch = 0; ch < chs; ++ch)
            {
                auto& dl = inDelay[(size_t) ch];
                const float* src = buf.getReadPointer (ch);
                float* dst = delayed.getWritePointer (ch);
                int p = delayPos;
                for (int i = 0; i < len; ++i)
                {
                    dst[i] = dl[(size_t) p];
                    dl[(size_t) p] = src[i];
                    if (++p == D) p = 0;
                }
            }
            delayPos = (int) ((delayPos + len) % D);

            stft.process (delayed, [this] (int ch, const std::complex<float>* in, int nb, lab::Stft::Outputs& out)
            {
                frame (ch, in, nb, out);
            });

            // ---- 時間領域で 2 スロットをつなぐ (出力の時刻 τ = 通し番号 − フレーム長) ---------------------
            const auto& s1buf = stft.getOutput (1);
            for (int i = 0; i < len; ++i)
            {
                const int64_t tau = blockStart + i - n;
                while (sc < numSwitches && switches[(size_t) sc].t - halfRamp <= tau)
                {
                    targetSlot = switches[(size_t) sc].slot;
                    publishAudible (sc);
                    ++sc;
                }
                const float target = (float) targetSlot;
                if (c < target) c = std::min (target, c + rampStep);
                else if (c > target) c = std::max (target, c - rampStep);
                if (c > 0.0f)
                    for (int ch = 0; ch < chs; ++ch)
                    {
                        float* o = delayed.getWritePointer (ch);
                        o[i] += c * (s1buf.getSample (ch, i) - o[i]);
                    }
            }
            for (int ch = 0; ch < chs; ++ch)
                buf.copyFrom (ch, 0, delayed, ch, 0, len);

            compactSwitches();
            count += len;
        }

    private:
        struct Event { int64_t t; int source; bool shuffled; uint32_t seed; bool newSeed; };
        struct Switch { int64_t t; int slot; int source; State state; };
        // Octave モードで、前のフレームのピークと θ (チャンネルごと)
        struct Track
        {
            std::vector<int> pk, d;
            std::vector<double> theta;
            std::vector<float> r;
            int64_t lastF = std::numeric_limits<int64_t>::min();
            void clear() { pk.clear(); d.clear(); theta.clear(); r.clear(); }
        };
        struct Slot
        {
            State state;
            std::vector<int> srcBin, shift;   // Random: 出力のビン k に来る入力のビンと、そのずれ (ビン数)
            std::vector<float> ratio;         // Octave: 入力のビンごとの比 (1, 2, 1/2 ...)
            std::vector<float> keys;
            std::vector<Track> prev, cur;
            int64_t builtFor = -1;   // 作ったときの (状態, reach, keepLow, size, モード) の指紋
            int64_t trackFor = -2;   // 追跡がどの指紋のものか (作り直したら追跡をやり直す)
        };

        static int patternIndex (int64_t step) { return (int) (((step % numSteps) + numSteps) % numSteps); }

        void addEvent (int64_t t, int source, bool shuffled, uint32_t seed, bool newSeed)
        {
            if (events.size() < events.capacity())   // prepare で確保済み (オーディオスレッドで確保しない)
                events.push_back ({ t, source, shuffled, seed, newSeed });
        }

        void schedule (const Event& e)
        {
            if (hasDeferred && deferred.t <= e.t)
            {
                hasDeferred = false;
                doSwitch (deferred);
            }
            if (e.t < lastSwitchT + minGap)
            {
                // 詰まりすぎ: あけられる最も早い時刻へ。後から来た指示で上書き (最後の指示が効く)
                deferred = e;
                deferred.t = lastSwitchT + minGap;
                hasDeferred = true;
                return;
            }
            doSwitch (e);
        }

        void doSwitch (const Event& e)
        {
            State s;
            s.shuffled = e.shuffled;
            s.seed = e.newSeed ? nextSeed (scheduled.seed) : (e.shuffled ? e.seed : scheduled.seed);
            s.mode = cfg.mode;
            if (s == scheduled)
                return;   // 変わらない (元の音のときに Reset など)
            if (numSwitches >= (int) switches.size())
                return;
            const int slot = 1 - scheduledSlot;
            slots[(size_t) slot].state = s;
            switches[(size_t) numSwitches++] = { e.t, slot, e.source, s };
            scheduledSlot = slot;
            scheduled = s;
            lastSwitchT = e.t;
        }

        void compactSwitches()
        {
            const int done = std::min (fc, sc);
            if (done <= 0) return;
            for (int i = done; i < numSwitches; ++i) switches[(size_t) (i - done)] = switches[(size_t) i];
            numSwitches -= done; fc -= done; sc -= done;
        }

        void publishAudible (int switchIndex)
        {
            if (numSwitches > 0 && switchIndex < numSwitches)
            {
                const auto& s = switches[(size_t) switchIndex];
                display.audibleSlot = s.slot;
                display.audibleShuffled = s.state.shuffled;
                display.audibleMode = s.state.mode;
                display.lastSource = s.source;
                display.switchCount.fetch_add (1);
            }
            else
            {
                display.audibleSlot = targetSlot;
                display.audibleShuffled = slots[(size_t) targetSlot].state.shuffled;
                display.audibleMode = slots[(size_t) targetSlot].state.mode;
            }
        }

        // ---- トランジェント検知: 20 ms のピーク保持 (持続音のうねりで誤検知しない) と、それを 25 ms で追う包絡の比 ----
        void detect (const juce::AudioBuffer<float>& b, int chs, int len)
        {
            if (! cfg.transOn) { armed = true; return; }
            const float thrDb = 18.0f - 0.15f * juce::jlimit (0.0f, 100.0f, cfg.sensitivity);   // 感度 0 → 18 dB、100 → 3 dB
            const float thr = juce::Decibels::decibelsToGain (thrDb);
            const float rearm = juce::Decibels::decibelsToGain (thrDb * 0.5f);
            const float floorLvl = juce::Decibels::decibelsToGain (-60.0f);
            const int64_t holdS = (int64_t) (cfg.holdMs * 0.001 * sr);
            for (int i = 0; i < len; ++i)
            {
                float x = 0.0f;
                for (int ch = 0; ch < chs; ++ch) x = std::max (x, std::abs (b.getSample (ch, i)));
                if (x >= fast) { fast = x; holdCount = peakHold; }
                else if (holdCount > 0) --holdCount;
                else fast *= fastRel;
                if (fast > slow) slow += (fast - slow) * slowAtt;
                else slow = fast;
                const int64_t r = blockStart + i;
                const bool above = fast > floorLvl && fast > thr * std::max (slow, 1.0e-9f);
                if (armed && above && r - lastOnset >= holdS)
                {
                    armed = false;
                    lastOnset = r;
                    display.transientCount.fetch_add (1);
                    addEvent (r + D - pre, srcTransient, true, 0, true);
                }
                if (! armed && fast < rearm * std::max (slow, 1.0e-9f))
                    armed = true;
            }
        }

        int64_t fingerprint (const Slot& s) const
        {
            const int64_t a = s.state.shuffled ? ((int64_t) s.state.seed + 1) * 2 + s.state.mode : 0;
            return a ^ ((int64_t) std::lround (cfg.reach * 100.0f) << 34) ^ ((int64_t) std::lround (cfg.keepLow) << 46)
                     ^ ((int64_t) std::lround (cfg.size * 1000.0f) << 56);
        }

        void build (int slotIndex)
        {
            auto& s = slots[(size_t) slotIndex];
            const int64_t fp = fingerprint (s);
            if (s.builtFor == fp) return;
            s.builtFor = fp;
            for (int k = 0; k < bins; ++k) s.srcBin[(size_t) k] = k;
            std::fill (s.shift.begin(), s.shift.end(), 0);
            std::fill (s.ratio.begin(), s.ratio.end(), 1.0f);
            int nIn = 0, nOut = 0;
            auto& cIn = display.cutIn[(size_t) slotIndex];
            auto& cOut = display.cutOut[(size_t) slotIndex];
            auto& d = display.srcBin[(size_t) slotIndex];
            // Size と Reach は指紋と同じ精度に丸めてから使う。保存 → 復元でパラメータが 0.25 → 0.2499… のように
            // ごくわずかずれると、境目の丸めが変わって別の並べ替えになっていた (状態の復元テストで −25 dB)
            const double sz = juce::jlimit (0.01, 2.0, std::round (cfg.size * 1000.0) / 1000.0);
            const double reachQ = std::round (cfg.reach * 100.0) / 100.0;
            if (s.state.shuffled && s.state.mode == modeOctave)
            {
                buildOctave (s, sz, reachQ);
                // 表示: 出力のビン → 入力のビン (比のぶん引き伸ばす / 縮める)。切れ目は入出力とも同じ格子
                for (int k = 0; k < bins; ++k) d[(size_t) k].store (k, std::memory_order_relaxed);
                for (int k = 1; k <= kMax; ++k)
                {
                    const float r = s.ratio[(size_t) k];
                    if (r == 1.0f) continue;
                    const int a = (int) std::lround (k * r), b = std::max (a + 1, (int) std::lround ((k + 1) * r));
                    for (int j = a; j < b && j <= kMax; ++j) if (j >= 1) d[(size_t) j].store (k, std::memory_order_relaxed);
                }
                for (int e : edges)
                    if (nIn < maxCuts) { cIn[(size_t) nIn++].store (e); cOut[(size_t) nOut++].store (e); }
            }
            else
            {
                if (s.state.shuffled)
                    buildRandom (s, sz, reachQ, nIn, nOut);
                for (int k = 0; k < bins; ++k) d[(size_t) k].store (s.srcBin[(size_t) k], std::memory_order_relaxed);
            }
            display.numCutIn[(size_t) slotIndex] = nIn;
            display.numCutOut[(size_t) slotIndex] = nOut;
            display.keepLow = cfg.keepLow;
            display.reach = cfg.reach;
        }

        void buildRandom (Slot& s, double sz, double reachQ, int& nIn, int& nOut)
        {
            const int k0 = std::max (1, (int) std::ceil (cfg.keepLow * n / sr));
            const int kTop = kMax;
            // 塊の境目: k0 から size オクターブごと (最低 1 ビン)
            edges.clear();
            edges.push_back (k0);
            for (int i = 1; edges.back() <= kTop; ++i)
            {
                int e = (int) std::round (k0 * std::pow (2.0, i * sz));
                e = juce::jlimit (edges.back() + 1, kTop + 1, e);
                edges.push_back (e);
            }
            const int chunks = (int) edges.size() - 1;
            juce::Random r ((juce::int64) s.state.seed);
            s.keys.resize ((size_t) chunks);
            for (auto& k : s.keys) k = r.nextFloat();
            sortKey.resize ((size_t) chunks);
            order.resize ((size_t) chunks);
            for (int c = 0; c < chunks; ++c)
            {
                const double centre = 0.5 * (edges[(size_t) c] + edges[(size_t) c + 1]);
                sortKey[(size_t) c] = (float) (std::log2 (centre) + reachQ * s.keys[(size_t) c]);
                order[(size_t) c] = c;
            }
            std::sort (order.begin(), order.end(), [this] (int a, int b) { return sortKey[(size_t) a] < sortKey[(size_t) b]; });
            auto& cIn = display.cutIn[(size_t) (&s - slots.data())];
            auto& cOut = display.cutOut[(size_t) (&s - slots.data())];
            for (int e : edges) if (nIn < maxCuts) cIn[(size_t) nIn++].store (e);
            int pos = k0;
            for (int c : order)
            {
                const int from = edges[(size_t) c], w = edges[(size_t) c + 1] - from;
                if (nOut < maxCuts) cOut[(size_t) nOut++].store (pos);
                for (int b = 0; b < w; ++b)
                {
                    s.srcBin[(size_t) (pos + b)] = from + b;
                    s.shift[(size_t) (pos + b)] = pos - from;
                }
                pos += w;
            }
        }

        // Octave: 塊 i = [fref·2^(i/N), fref·2^((i+1)/N))。列 c = i mod N ごとに、段 o = i div N を「o + reach·u」順に並べ替える
        void buildOctave (Slot& s, double sz, double reachQ)
        {
            const double fref = std::max (20.0, (double) cfg.keepLow);
            const int N = juce::jlimit (1, 24, (int) std::lround (1.0 / std::max (0.04, sz)));
            display.octaveDivisions = N;
            const double hz = sr / n;
            int chunks = 0;
            while (fref * std::pow (2.0, (chunks + 1) / (double) N) <= 20000.0) ++chunks;
            edges.clear();
            for (int i = 0; i <= chunks; ++i)
                edges.push_back ((int) std::ceil (fref * std::pow (2.0, i / (double) N) / hz));
            juce::Random r ((juce::int64) s.state.seed);
            s.keys.resize ((size_t) chunks);
            for (auto& k : s.keys) k = r.nextFloat();
            for (int c = 0; c < N; ++c)
            {
                colIdx.clear();
                for (int i = c; i < chunks; i += N) colIdx.push_back (i);
                const int m = (int) colIdx.size();
                order.resize ((size_t) m);
                sortKey.resize ((size_t) m);
                for (int j = 0; j < m; ++j)
                {
                    order[(size_t) j] = j;
                    sortKey[(size_t) j] = (float) (colIdx[(size_t) j] / N + reachQ * s.keys[(size_t) colIdx[(size_t) j]]);
                }
                std::sort (order.begin(), order.end(), [this] (int a, int b) { return sortKey[(size_t) a] < sortKey[(size_t) b]; });
                // 段の小さい順の枠 j に、並べ替えた順の元 order[j] が入る
                for (int j = 0; j < m; ++j)
                {
                    const int src = colIdx[(size_t) order[(size_t) j]];
                    const float ratio = (float) std::pow (2.0, colIdx[(size_t) j] / N - src / N);
                    for (int k = edges[(size_t) src]; k < edges[(size_t) src + 1] && k < bins; ++k)
                        s.ratio[(size_t) k] = ratio;
                }
            }
        }

        // 入力のフレームの解析 (Octave 用): 振幅・位相・ピーク・谷の境目・真の周波数
        void analyse (int ch, const std::complex<float>* in, int nb)
        {
            float mx = 0.0f;
            for (int k = 0; k < nb; ++k)
            {
                mag[(size_t) k] = std::abs (in[k]);
                phase[(size_t) k] = std::arg (in[k]);
                mx = std::max (mx, mag[(size_t) k]);
            }
            peaks.clear(); bounds.clear(); omega.clear();
            const float thr = mx * 1.0e-5f + 1.0e-20f;
            for (int k = 1; k < nb - 1; ++k)
                if (mag[(size_t) k] > mag[(size_t) k - 1] && mag[(size_t) k] >= mag[(size_t) k + 1] && mag[(size_t) k] > thr)
                    peaks.push_back (k);
            bounds.push_back (0);
            for (size_t j = 0; j + 1 < peaks.size(); ++j)
            {
                int a = peaks[j], best = a;
                for (int k = a; k <= peaks[j + 1]; ++k) if (mag[(size_t) k] < mag[(size_t) best]) best = k;
                bounds.push_back (best);
            }
            bounds.push_back (nb);
            const double tp = 2.0 * juce::MathConstants<double>::pi;
            auto& pp = prevPhase[(size_t) ch];
            for (int p : peaks)
            {
                double w = tp * p / n;
                if (havePrev[(size_t) ch])
                {
                    double dp = phase[(size_t) p] - pp[(size_t) p] - tp * p * hop / n;
                    dp -= tp * std::floor ((dp + juce::MathConstants<double>::pi) / tp);
                    w += dp / hop;
                }
                omega.push_back (w);
            }
            std::copy (phase.begin(), phase.begin() + nb, pp.begin());
            havePrev[(size_t) ch] = true;
        }

        void synthOctave (Slot& s, int ch, int64_t F, const std::complex<float>* in, std::complex<float>* y)
        {
            const double pi = juce::MathConstants<double>::pi, tp = 2.0 * pi;
            auto& tr = s.prev[(size_t) ch];
            auto& cu = s.cur[(size_t) ch];
            if (tr.lastF != F - hop || s.trackFor != s.builtFor) tr.clear();   // 続きでなければ追跡をやり直す
            cu.clear();
            for (size_t j = 0; j < peaks.size(); ++j)
            {
                const int a = bounds[j], b = bounds[j + 1], p = peaks[j];
                const float r = s.ratio[(size_t) p];
                if (r == 1.0f)
                {
                    for (int k = a; k < b; ++k) y[k] += in[k];
                    continue;
                }
                const double dw = omega[j] * (r - 1.0);
                const int d = (int) std::lround (dw * n / tp);
                double theta;
                // 前のフレームの、±2 ビン以内で比が同じピークの続きなら θ を積み上げる
                auto it = std::lower_bound (tr.pk.begin(), tr.pk.end(), p - 2);
                int q = -1;
                for (; it != tr.pk.end() && *it <= p + 2; ++it)
                {
                    const int idx = (int) (it - tr.pk.begin());
                    if (tr.r[(size_t) idx] == r && (q < 0 || std::abs (*it - p) < std::abs (tr.pk[(size_t) q] - p))) q = idx;
                }
                if (q >= 0) theta = tr.theta[(size_t) q] + dw * hop - pi * (d - tr.d[(size_t) q]);
                else        theta = dw * ((double) F + n * 0.5) - pi * d;
                theta -= tp * std::floor (theta / tp);
                cu.pk.push_back (p); cu.d.push_back (d); cu.theta.push_back (theta); cu.r.push_back (r);
                const std::complex<float> rot = std::polar (1.0f, (float) theta);
                const int lo = std::max (a, 1 - d), hi = std::min (b, kMax + 1 - d);
                for (int k = lo; k < hi; ++k) y[k + d] += in[k] * rot;
            }
            std::swap (tr.pk, cu.pk); std::swap (tr.d, cu.d); std::swap (tr.theta, cu.theta); std::swap (tr.r, cu.r);
            tr.lastF = F;
            if (ch == nch - 1) s.trackFor = s.builtFor;
        }

        void frame (int ch, const std::complex<float>* in, int nb, lab::Stft::Outputs& out)
        {
            const int64_t F = stft.frameStart();
            if (ch == 0)
            {
                // このフレームの窓 [F, F+n) で鳴りうるスロット
                while (fc < numSwitches && switches[(size_t) fc].t + halfRamp < F)
                    frameSlot = switches[(size_t) fc++].slot;
                need = 1 << frameSlot;
                for (int j = fc; j < numSwitches && switches[(size_t) j].t - halfRamp < F + n; ++j)
                    need |= 1 << switches[(size_t) j].slot;
                for (int s = 0; s < 2; ++s)
                    if ((need >> s) & 1) build (s);
                std::fill (power.begin(), power.end(), 0.0f);
            }
            analyse (ch, in, nb);   // 位相の続きを保つため、Octave を使っていないフレームでも毎回

            for (int s = 0; s < 2; ++s)
                if ((need >> s) & 1)
                {
                    auto* y = out.spectrum (s);
                    auto& sl = slots[(size_t) s];
                    if (sl.state.shuffled && sl.state.mode == modeOctave)
                    {
                        synthOctave (sl, ch, F, in, y);
                        continue;
                    }
                    const auto& src = slots[(size_t) s].srcBin;
                    const auto& sh = slots[(size_t) s].shift;
                    // Δk ビンずらすときは e^{j 2π Δk F / n} を掛ける (F = 窓の先頭の通し番号)。これが無いと、
                    // Δk が 4 の倍数でないずれでフレームごとに位相が食い違い、音が濁る (hop = n/4 なので 4 の倍数なら偶然そろう)
                    const int64_t Fm = ((F % n) + n) % n;   // 最初の数フレームは F < 0 (窓が入力の頭より前から始まる)。負の添字で表を読んでいた
                    for (int k = 0; k < nb; ++k)
                    {
                        const int d = sh[(size_t) k];
                        if (d == 0) { y[k] = in[src[(size_t) k]]; continue; }
                        const int64_t rIdx = (((int64_t) d % n + n) % n * Fm) % n;
                        y[k] = in[src[(size_t) k]] * cis[(size_t) rIdx];
                    }
                }

            for (int k = 0; k < nb; ++k) power[(size_t) k] += std::norm (in[k]);
            if (ch == nch - 1)
            {
                const float scale = 4.0f / (float) n;   // Hann 窓: 振幅 A のサイン波 → |X| = A·n/4
                const float inv = 1.0f / (float) nch;
                for (int k = 0; k < nb; ++k)
                    display.inMag[(size_t) k].store (std::sqrt (power[(size_t) k] * inv) * scale, std::memory_order_relaxed);
            }
        }

        double sr = 48000.0;
        int n = 2048, hop = 512, bins = 1025, nch = 2, D = 96, pre = 48, halfRamp = 24, minGap = 2656;
        float rampStep = 0.02f;
        lab::Stft stft;
        Settings cfg;

        std::vector<std::vector<float>> inDelay;
        int delayPos = 0;
        juce::AudioBuffer<float> delayed;
        std::vector<float> power;

        std::array<Slot, 2> slots;
        std::vector<int> edges, order;
        std::vector<float> sortKey;
        std::vector<std::complex<float>> cis;
        State scheduled;
        int scheduledSlot = 0;
        std::array<Switch, 64> switches {};
        int numSwitches = 0, fc = 0, sc = 0, frameSlot = 0, targetSlot = 0, need = 1;
        float c = 0.0f;
        int64_t lastSwitchT = 0;
        Event deferred {};
        bool hasDeferred = false;
        std::vector<Event> events;

        int64_t count = 0, blockStart = 0;
        float fast = 0.0f, slow = 0.0f, fastRel = 0.999f, slowAtt = 0.001f;
        int holdCount = 0, peakHold = 960;
        bool armed = true;
        int64_t lastOnset = 0;

        std::vector<int> held;
        int64_t lastSeqStep = 0;

        // Octave
        std::vector<float> mag, phase;
        std::vector<std::vector<float>> prevPhase;
        std::vector<bool> havePrev;
        std::vector<int> peaks, bounds, colIdx;
        std::vector<double> omega;
        int kMax = 853, lastMode = -1;
    };
}

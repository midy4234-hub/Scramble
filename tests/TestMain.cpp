//  ScrambleTest — DAW を開かずに挙動を確かめるオフラインドライバ
//
//   ScrambleTest                                   … 固有テスト (下) と UI の画像
//   ScrambleTest demo in.wav out.wav [bpm] [k=v]    … 試聴用 A/B。k=v はパラメータ、pattern=S...R... でシーケンサー
//   ScrambleTest robust / render / snapshot / params (LabTest.h)

#include "LabTest.h"
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"

using Proc = ScrambleAudioProcessor;

namespace
{
    constexpr double sr = 48000.0;

    void setSteps (Proc& p, const juce::String& s)
    {
        for (int i = 0; i < scr::numSteps; ++i)
        {
            const auto c = i < s.length() ? s[i] : '.';
            p.pattern.steps[(size_t) i].store (c == 'S' ? scr::stepShuffle : c == 'R' ? scr::stepReset : scr::stepNone);
        }
    }

    void triggersOff (Proc& p)
    {
        lab::setParam (p, "trans", 0.0f);
        lab::setParam (p, "seqon", 0.0f);
        lab::setParam (p, "midion", 0.0f);
    }

    // 並べ替えを固定したときの出力 (参照)
    juce::AudioBuffer<float> runFixed (const juce::AudioBuffer<float>& in, bool shuffled, uint32_t seed, int mode = 0)
    {
        Proc p;
        triggersOff (p);
        lab::setParam (p, "mode", (float) mode);
        p.setScramble (shuffled, seed);
        lab::prepare (p, sr);
        return lab::run (p, in);
    }

    // out ≒ (1 − c)·A + c·B とみて、c が 0.5 を越える最初の時刻 (サンプル)。from から探す
    double crossing (const juce::AudioBuffer<float>& out, const juce::AudioBuffer<float>& A, const juce::AudioBuffer<float>& B, int from, int to)
    {
        const int w = 16;
        for (int i = from; i + w < to; i += 4)
        {
            double num = 0.0, den = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int k = i; k < i + w; ++k)
                {
                    const double d = B.getSample (ch, k) - A.getSample (ch, k);
                    num += (out.getSample (ch, k) - A.getSample (ch, k)) * d;
                    den += d * d;
                }
            if (den > 0.0 && num / den >= 0.5) return i + w * 0.5;
        }
        return -1.0;
    }

    double segResidual (const juce::AudioBuffer<float>& out, const juce::AudioBuffer<float>& ref, double a, double b)
    {
        return lab::residualDb (out, ref, (int) (a * sr), (int) ((b - a) * sr));
    }

    // ノイズの床 (−30 dB) に、頭の鋭いバースト (20 ms) を置く
    juce::AudioBuffer<float> bursts (double seconds, std::initializer_list<double> at)
    {
        auto x = lab::noise ((int) (seconds * sr), 0.03f, 5);
        auto b = lab::noise ((int) (0.02 * sr), 0.5f, 9);
        for (double t : at)
            for (int ch = 0; ch < 2; ++ch)
                x.addFrom (ch, (int) (t * sr), b, ch, 0, b.getNumSamples());
        return x;
    }

    // 1. トリガーが無ければ、レイテンシぶん遅れるだけで元の音。並べ替え中も音量はほぼ同じ
    void testIdentity()
    {
        Proc p;
        triggersOff (p);
        lab::prepare (p, sr);
        const int L = p.getLatencySamples();
        auto in = lab::noise ((int) sr * 2, 0.25f);
        auto out = lab::run (p, in);
        auto sh = runFixed (in, true, 12345u);
        std::printf ("[1] identity: latency %d samples (%.1f ms), residual %.1f dB; shuffled level %+.2f dB\n", L, L / sr * 1000.0,
                     lab::residualDb (out, in, 0, in.getNumSamples() - L, L),
                     lab::db (lab::rms (sh, L + 4800, 48000) / lab::rms (in, 4800, 48000)));
    }

    // 2. Keep Low はつまみの値どおり: 200 Hz なら 150 Hz は動かず、300 Hz は動く
    void testKeepLow()
    {
        for (double f : { 150.0, 300.0 })
        {
            auto in = lab::sine (sr, f, 2.0, 0.3f);
            auto a = runFixed (in, false, 1u), b = runFixed (in, true, 12345u);
            std::printf ("[2] keep low 200 Hz, sine %.0f Hz: shuffled vs original %.1f dB (%s)\n", f,
                         segResidual (b, a, 0.5, 1.5), f < 200.0 ? "should stay" : "should move");
        }
    }

    // 3. トランジェント: バーストの頭で切り替わるか。期待は「頭の 1 ms 前に切り替えの中央」
    void testTransient()
    {
        Proc p;
        triggersOff (p);
        lab::setParam (p, "trans", 1.0f);
        lab::prepare (p, sr);
        const int L = p.getLatencySamples();
        const double t1 = 0.4003;   // ブロックの区切りと関係ない位置
        auto in = bursts (2.0, { t1, 0.9, 1.4 });
        auto out = lab::run (p, in);
        // 信号の頭 (無音 → ノイズの床) も立ち上がりとして検知されるので、バースト 1 は 2 回目の Shuffle
        const uint32_t s1 = scr::nextSeed (1u), s2 = scr::nextSeed (s1);
        auto A = runFixed (in, true, s1), B = runFixed (in, true, s2);
        const int onset = (int) (t1 * sr) + L;
        const double x = crossing (out, A, B, onset - (int) (0.03 * sr), onset + (int) (0.03 * sr));
        std::printf ("[3] transient: %d hits detected (start of the bed + 3 bursts), switch centre %+.2f ms vs the attack; after: %.1f dB vs fixed shuffle\n",
                     p.display().transientCount.load(), (x - onset) / sr * 1000.0,
                     lab::residualDb (out, B, onset + (int) (0.01 * sr), (int) (0.3 * sr)));
    }

    // 4. シーケンサー: 2 拍目の頭で S、4 拍目の頭で R (120 BPM)
    void testSeq()
    {
        Proc p;
        triggersOff (p);
        lab::setParam (p, "seqon", 1.0f);
        setSteps (p, "....S.......R...");
        lab::prepare (p, sr);
        const int L = p.getLatencySamples();
        lab::SimPlayHead ph; ph.bpm = 120.0; ph.sampleRate = sr;
        lab::RunOptions o; o.playhead = &ph; o.randomBlocks = true;
        auto in = lab::noise ((int) (2.2 * sr), 0.25f, 3);
        auto out = lab::runWith (p, in, o);
        auto A = runFixed (in, false, 1u), B = runFixed (in, true, scr::nextSeed (1u));
        const int gS = (int) (0.5 * sr) + L, gR = (int) (1.5 * sr) + L;
        const double xs = crossing (out, A, B, gS - 1440, gS + 1440);
        const double xr = crossing (out, B, A, gR - 1440, gR + 1440);
        std::printf ("[4] sequencer (random block sizes): S centre %+.2f ms, R centre %+.2f ms vs the grid; "
                     "between: %.1f dB vs fixed shuffle, after R: %.1f dB vs original\n",
                     (xs - gS) / sr * 1000.0, (xr - gR) / sr * 1000.0,
                     lab::residualDb (out, B, gS + 480, (int) (0.9 * sr)), lab::residualDb (out, A, gR + 480, (int) (0.5 * sr)));
    }

    // 5. MIDI: Hold は押している間だけ (ノートごとに決まった並べ替え)、Latch は離しても残る
    void testMidi()
    {
        for (int latch = 0; latch < 2; ++latch)
        {
            Proc p;
            triggersOff (p);
            lab::setParam (p, "midion", 1.0f);
            lab::setParam (p, "midimode", (float) latch);
            lab::prepare (p, sr);
            const int L = p.getLatencySamples();
            auto in = lab::noise ((int) (1.6 * sr), 0.25f, 4);
            const int on = 14444, off = 38888;
            lab::RunOptions o;
            o.midi = { { on, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) }, { off, juce::MidiMessage::noteOff (1, 60) } };
            auto out = lab::runWith (p, in, o);
            auto A = runFixed (in, false, 1u), B = runFixed (in, true, scr::noteSeed (60));
            const double xOn = crossing (out, A, B, on + L - 1440, on + L + 1440);
            const double xOff = latch ? -1.0 : crossing (out, B, A, off + L - 1440, off + L + 1440);
            std::printf ("[5] MIDI %s: on centre %+.2f ms, %s; end state %.1f dB vs %s\n", latch ? "Latch" : "Hold",
                         (xOn - on - L) / sr * 1000.0,
                         latch ? "off ignored" : juce::String::formatted ("off centre %+.2f ms", (xOff - off - L) / sr * 1000.0).toRawUTF8(),
                         lab::residualDb (out, latch ? B : A, off + L + 2400, (int) (0.4 * sr)), latch ? "note 60 shuffle" : "original");
        }
    }

    // 6. 詰まった指示: 10 ms 違いの 2 ノート → 2 つ目は minGap まで遅れて、最後は 2 つ目の並べ替え
    void testMinGap()
    {
        Proc p;
        triggersOff (p);
        lab::setParam (p, "midion", 1.0f);
        lab::setParam (p, "midimode", 1.0f);
        lab::prepare (p, sr);
        const int L = p.getLatencySamples();
        auto in = lab::noise ((int) (1.2 * sr), 0.25f, 6);
        const int a = 12000, b = a + 480;
        lab::RunOptions o;
        o.midi = { { a, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) }, { b, juce::MidiMessage::noteOn (1, 62, (juce::uint8) 100) } };
        auto out = lab::runWith (p, in, o);
        auto B = runFixed (in, true, scr::noteSeed (60)), C = runFixed (in, true, scr::noteSeed (62));
        const double x = crossing (out, B, C, a + L, a + L + (int) (0.2 * sr));
        std::printf ("[6] two notes 10 ms apart: second applied %.1f ms after the first (min gap %.1f ms); end %.1f dB vs note 62\n",
                     (x - a - L) / sr * 1000.0, (2048 + 512 + 96) / sr * 1000.0,
                     lab::residualDb (out, C, (int) (0.6 * sr), (int) (0.5 * sr)));
    }

    // 7. 保存 → 復元で同じ並べ替え
    void testState()
    {
        Proc a;
        triggersOff (a);
        a.setScramble (true, 4242u);
        lab::prepare (a, sr);
        juce::MemoryBlock mb;
        a.getStateInformation (mb);
        Proc b;
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        lab::prepare (b, sr);
        auto in = lab::noise ((int) sr, 0.25f, 7);
        auto oa = lab::run (a, in), ob = lab::run (b, in);
        std::printf ("[7] state restore keeps the scramble: residual %.1f dB\n", lab::residualDb (ob, oa, 0, in.getNumSamples()));
        if (std::getenv ("SCR_DEBUG"))
        {
            for (auto* prm : a.getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (prm))
                {
                    auto* q = lab::findParam (b, rp->getParameterID());
                    std::printf ("   %s a=%.9f b=%.9f\n", rp->getParameterID().toRawUTF8(), rp->convertFrom0to1 (rp->getValue()), q->convertFrom0to1 (q->getValue()));
                }
            const auto& da = a.display(); const auto& dbb = b.display();
            int diff = 0;
            for (int k = 0; k < da.bins.load(); ++k) diff += da.srcBin[(size_t) da.audibleSlot.load()][(size_t) k].load() != dbb.srcBin[(size_t) dbb.audibleSlot.load()][(size_t) k].load();
            std::printf ("   mapping differs at %d bins; slots %d %d, shuffled %d %d; seed %u %u\n", diff, da.audibleSlot.load(), dbb.audibleSlot.load(),
                         (int) da.audibleShuffled.load(), (int) dbb.audibleShuffled.load(), a.savedSeed.load(), b.savedSeed.load());
        }
    }

    // 7b. ボタンで並べ替えたらホストに知らせる (知らせないと Live のフリーズ・バウンスが並べ替え前の状態で鳴る)。
    //     読み込んだだけでは知らせない。ステップを編集したら知らせる
    void testHostNotify()
    {
        struct Listener : juce::AudioProcessorListener
        {
            int dirty = 0;
            void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
            void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d) override { dirty += d.nonParameterStateChanged; }
        } li;
        auto pump = [] { juce::MessageManager::getInstance()->runDispatchLoopUntil (400); };
        auto in = lab::noise ((int) (0.5 * sr), 0.25f, 7);

        Proc a;
        a.addListener (&li);
        triggersOff (a);
        lab::prepare (a, sr);
        lab::run (a, in);
        pump();
        const int idle = li.dirty;
        lab::setParam (a, "shuffle", 1.0f);
        lab::run (a, in);
        lab::setParam (a, "shuffle", 0.0f);
        lab::run (a, in);
        pump();
        const int afterPress = li.dirty;

        // ホストが知らせを受けて取り直した状態で、別インスタンス (フリーズ・バウンス) を作る
        juce::MemoryBlock mb;
        a.getStateInformation (mb);
        Proc b;
        Listener lb;
        b.addListener (&lb);
        b.setStateInformation (mb.getData(), (int) mb.getSize());
        lab::prepare (b, sr);
        pump();
        auto ob = lab::run (b, in);
        auto orig = runFixed (in, false, 1), ref = runFixed (in, true, a.savedSeed.load());
        std::printf ("[7b] host told: idle %d, after Shuffle %d, after load %d (want 0/1/0); shuffled a=%d b=%d; "
                     "copy vs original %.1f dB, copy vs same seed %.1f dB\n",
                     idle, afterPress, lb.dirty, (int) a.savedShuffled.load(), (int) b.savedShuffled.load(),
                     lab::residualDb (ob, orig, 0, in.getNumSamples()), lab::residualDb (ob, ref, 0, in.getNumSamples()));

        setSteps (b, "S...............");
        pump();
        std::printf ("[7b] host told after a step edit: %d (want 1)\n", lb.dirty);
        a.removeListener (&li);
        b.removeListener (&lb);
    }

    void testCpu()
    {
        Proc p;
        lab::setParam (p, "hold", 60.0f);
        lab::prepare (p, sr);
        std::printf ("[8] CPU (transient trigger on noise): %.2f %% of realtime\n", lab::cpuPercent (p, sr, 5.0));
    }

    // 並べ替えを固定したインスタンスを少し回して、いま鳴っているスロットの対応表 (出力のビン → 入力のビン) を取る
    std::vector<int> mapping (float reach, float size, uint32_t seed, int& k0, int& kTop)
    {
        Proc p;
        triggersOff (p);
        lab::setParam (p, "reach", reach);
        lab::setParam (p, "size", size);
        p.setScramble (true, seed);
        lab::prepare (p, sr);
        lab::run (p, lab::noise (8192, 0.1f));
        const auto& d = p.display();
        const int bins = d.bins.load(), slot = d.audibleSlot.load();
        std::vector<int> m ((size_t) bins);
        for (int k = 0; k < bins; ++k) m[(size_t) k] = d.srcBin[(size_t) slot][(size_t) k].load();
        k0 = (int) std::ceil (200.0 * 2048 / sr);
        kTop = (int) std::floor (20000.0 * 2048 / sr);
        return m;
    }

    // 10. Reach = 塊が動く最大の距離。上げるほど遠くへ (単調)、最大は reach + size 程度に収まる
    void testReach()
    {
        std::printf ("[10] reach -> displacement (octaves, per-octave weighted mean / max over 6 seeds), size 1/4 oct:\n     ");
        for (float reach : { 0.0f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f })
        {
            double sum = 0.0, wsum = 0.0, mx = 0.0;
            for (uint32_t seed = 1; seed <= 6; ++seed)
            {
                int k0, kTop;
                const auto m = mapping (reach, 0.25f, seed * 977u, k0, kTop);
                for (int k = k0; k <= kTop; ++k)
                {
                    const double dsp = std::abs (std::log2 ((double) m[(size_t) k] / k)), w = 1.0 / k;
                    sum += dsp * w; wsum += w; mx = std::max (mx, dsp);
                }
            }
            std::printf ("%.1f: %.2f / %.2f   ", reach, sum / wsum, mx);
        }
        std::printf ("\n");
    }

    // 11. 半端なビン数ずらしても濁らない (位相の補正): 1 kHz のサイン波が、ずれた先でもきれいなサイン波か
    void testShiftPurity()
    {
        std::vector<double> res, res4;   // res4 = ずれが 4 ビンの倍数でないもの (補正が無いと濁るはずの場合)
        int nonMult4 = 0;
        for (uint32_t seed = 1; seed <= 80 && res4.size() < 8; ++seed)
        {
            int k0, kTop;
            const float size = 1.0f;   // 塊を広くして、サイン波の主成分 (±2 ビン) が 1 つの塊に収まりやすくする
            const auto m = mapping (4.0f, size, seed * 131u, k0, kTop);
            const int src = (int) std::round (1000.0 * 2048 / sr);
            int dst = -1;
            for (int k = 0; k < (int) m.size(); ++k) if (m[(size_t) k] == src) dst = k;
            // 主成分が塊の切れ目にかかっていたら外す
            bool clean = dst > 3 && dst + 3 < (int) m.size();
            for (int j = -3; clean && j <= 3; ++j) clean = m[(size_t) (dst + j)] == src + j;
            if (! clean || dst == src) continue;
            if ((dst - src) % 4 != 0) ++nonMult4;
            auto in = lab::sine (sr, 1000.0, 2.0, 0.3f);
            Proc p;
            triggersOff (p);
            lab::setParam (p, "reach", 4.0f);
            lab::setParam (p, "size", size);
            p.setScramble (true, seed * 131u);
            lab::prepare (p, sr);
            auto out = lab::run (p, in);
            const double f = 1000.0 + (dst - src) * sr / 2048.0, w = 2.0 * juce::MathConstants<double>::pi * f / sr;
            const int a = (int) (0.5 * sr), len = (int) sr;
            double cs = 0.0, sn = 0.0;
            for (int i = a; i < a + len; ++i) { cs += out.getSample (0, i) * std::cos (w * i); sn += out.getSample (0, i) * std::sin (w * i); }
            cs *= 2.0 / len; sn *= 2.0 / len;
            double e = 0.0, t = 0.0;
            for (int i = a; i < a + len; ++i)
            {
                const double y = out.getSample (0, i), fit = cs * std::cos (w * i) + sn * std::sin (w * i);
                e += (y - fit) * (y - fit); t += y * y;
            }
            const double v = lab::db (std::sqrt (e / std::max (t, 1.0e-20)));
            res.push_back (v);
            if ((dst - src) % 4 != 0) res4.push_back (v);
        }
        std::sort (res.begin(), res.end());
        std::sort (res4.begin(), res4.end());
        std::printf ("[11] shifted 1 kHz sine, residual after fitting one sine at the shifted frequency: %d cases "
                     "(%d with shifts not a multiple of 4 bins), median %.1f dB, worst %.1f dB\n",
                     (int) res.size(), nonMult4, res.empty() ? 0.0 : res[res.size() / 2], res.empty() ? 0.0 : res.back());
        std::printf ("     not a multiple of 4 only: median %.1f dB, worst %.1f dB\n",
                     res4.empty() ? 0.0 : res4[res4.size() / 2], res4.empty() ? 0.0 : res4.back());
    }

    // 12. Octave: 1 kHz のサイン波は、どれかのオクターブ (×1/4〜×4) のきれいなサイン波になる
    void testOctaveSine()
    {
        std::vector<double> res;
        int moved = 0;
        for (uint32_t seed = 1; seed <= 10; ++seed)
        {
            auto in = lab::sine (sr, 1000.0, 2.0, 0.3f);
            auto out = runFixed (in, true, seed * 7919u, 1);
            const int a = (int) (0.6 * sr), len = (int) sr;
            double best = 1.0e9; int bestK = 0;
            for (int k = -2; k <= 2; ++k)
            {
                const double w = 2.0 * juce::MathConstants<double>::pi * 1000.0 * std::pow (2.0, k) / sr;
                double cs = 0.0, sn = 0.0, t = 0.0, e = 0.0;
                for (int i = a; i < a + len; ++i) { cs += out.getSample (0, i) * std::cos (w * i); sn += out.getSample (0, i) * std::sin (w * i); }
                cs *= 2.0 / len; sn *= 2.0 / len;
                for (int i = a; i < a + len; ++i)
                {
                    const double y = out.getSample (0, i), f = cs * std::cos (w * i) + sn * std::sin (w * i);
                    e += (y - f) * (y - f); t += y * y;
                }
                const double r = lab::db (std::sqrt (e / std::max (t, 1.0e-20)));
                if (r < best) { best = r; bestK = k; }
            }
            res.push_back (best);
            if (bestK != 0) ++moved;
        }
        std::sort (res.begin(), res.end());
        std::printf ("[12] octave mode, 1 kHz sine over 10 seeds: %d moved by octaves; residual after fitting one sine at 1k x 2^k: median %.1f dB, worst %.1f dB\n",
                     moved, res[res.size() / 2], res.back());
    }

    // 13. Octave: 倍音のある音 (A3 = 220 Hz、倍音 1..12) の出力のピークが、元の倍音の音名から何セントずれているか
    void testOctavePitchClasses()
    {
        const int T = (int) (2.5 * sr);
        juce::AudioBuffer<float> in (2, T);
        in.clear();
        for (int i = 0; i < T; ++i)
        {
            double v = 0.0;
            for (int h = 1; h <= 12; ++h) v += 0.25 / h * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * h * i / sr);
            in.setSample (0, i, (float) v); in.setSample (1, i, (float) v);
        }
        std::vector<double> srcPc;
        for (int h = 1; h <= 12; ++h) srcPc.push_back (std::fmod (1200.0 * std::log2 (220.0 * h / 440.0) + 12000.0, 1200.0));
        double worst = 0.0; int peaks = 0, newFreq = 0;
        for (uint32_t seed = 1; seed <= 5; ++seed)
        {
            auto out = runFixed (in, true, seed * 104729u, 1);
            const int order = 16, N = 1 << order, a = (int) (0.6 * sr);
            std::vector<float> buf ((size_t) (2 * N), 0.0f);
            for (int i = 0; i < N; ++i) buf[(size_t) i] = out.getSample (0, a + i) * (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / N));
            juce::dsp::FFT fft (order);
            fft.performFrequencyOnlyForwardTransform (buf.data());
            float mx = 0.0f;
            for (int k = 1; k < N / 2; ++k) mx = std::max (mx, buf[(size_t) k]);
            for (int k = 3; k < N / 2 - 3; ++k)
            {
                const float v = buf[(size_t) k];
                if (v < mx * 0.01f) continue;   // −40 dB まで
                bool isPk = true;
                for (int j = -2; j <= 2; ++j) if (j != 0 && buf[(size_t) (k + j)] > v) isPk = false;
                if (! isPk) continue;
                const double l = std::log (buf[(size_t) k - 1] + 1e-20), c = std::log (v + 1e-20), r = std::log (buf[(size_t) k + 1] + 1e-20);
                const double f = (k + 0.5 * (l - r) / (l - 2 * c + r)) * sr / N;
                const double pc = std::fmod (1200.0 * std::log2 (f / 440.0) + 12000.0, 1200.0);
                double dmin = 1200.0;
                for (double sp : srcPc) dmin = std::min (dmin, std::min (std::abs (pc - sp), 1200.0 - std::abs (pc - sp)));
                worst = std::max (worst, dmin);
                ++peaks;
                bool orig = false;
                for (int h = 1; h <= 12; ++h) if (std::abs (f - 220.0 * h) < 3.0) orig = true;
                if (! orig) ++newFreq;
            }
        }
        std::printf ("[13] octave mode, harmonic A3 over 5 seeds: %d peaks (%d at frequencies not in the input), worst %.1f cents from the input's pitch classes\n",
                     peaks, newFreq, worst);
    }

    // 14. モードの切り替えもクロスフェード: 並べ替えたまま Random → Octave (0.7 s のブロックの頭で)
    void testModeSwitch()
    {
        Proc p;
        triggersOff (p);
        p.setScramble (true, 999u);
        lab::prepare (p, sr);
        const int L = p.getLatencySamples();
        auto in = lab::noise ((int) (1.5 * sr), 0.25f, 8);
        int at = -1;
        lab::RunOptions o;
        o.beforeBlock = [&at] (juce::AudioProcessor& q, int start, int)
        {
            if (start >= (int) (0.7 * sr) && at < 0) { at = start; lab::setParam (q, "mode", 1.0f); }
        };
        auto out = lab::runWith (p, in, o);
        auto A = runFixed (in, true, 999u, 0), B = runFixed (in, true, 999u, 1);
        const int g = at + L;
        const double x = crossing (out, A, B, g - 2400, g + 2400);
        // Octave は位相を積み上げるので、追い始めた時刻が違うと位相だけ変わる。波形の差ではなく、平均スペクトルの形で比べる
        auto spec = [] (const juce::AudioBuffer<float>& b, int a, int len)
        {
            const int order = 12, N = 1 << order;
            juce::dsp::FFT fft (order);
            std::vector<double> acc ((size_t) N / 2, 0.0);
            std::vector<float> buf ((size_t) (2 * N));
            for (int st = a; st + N <= a + len; st += N / 2)
            {
                std::fill (buf.begin(), buf.end(), 0.0f);
                for (int i = 0; i < N; ++i) buf[(size_t) i] = b.getSample (0, st + i) * (float) (0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / N));
                fft.performFrequencyOnlyForwardTransform (buf.data());
                for (int k = 0; k < N / 2; ++k) acc[(size_t) k] += (double) buf[(size_t) k] * buf[(size_t) k];
            }
            return acc;
        };
        auto dist = [&] (const std::vector<double>& u, const std::vector<double>& v)
        {
            // 100 Hz〜16 kHz、1/3 オクターブごとの音量の差 (dB) の平均
            double sum = 0.0; int cnt = 0;
            for (double f = 100.0; f < 16000.0; f *= std::pow (2.0, 1.0 / 3.0))
            {
                const int k0 = (int) (f * 4096 / sr), k1 = (int) (f * std::pow (2.0, 1.0 / 3.0) * 4096 / sr);
                double eu = 1e-30, ev = 1e-30;
                for (int k = k0; k < k1; ++k) { eu += u[(size_t) k]; ev += v[(size_t) k]; }
                sum += std::abs (10.0 * std::log10 (eu / ev)); ++cnt;
            }
            return sum / cnt;
        };
        const int a2 = g + 2400, len2 = (int) (0.45 * sr);
        const auto so = spec (out, a2, len2), sa = spec (A, a2, len2), sb = spec (B, a2, len2);
        std::printf ("[14] mode switch while shuffled: centre %+.2f ms vs the block where Mode changed; "
                     "after, mean 1/3-oct level difference: %.2f dB vs fixed Octave, %.2f dB vs fixed Random\n",
                     (x - g) / sr * 1000.0, dist (so, sb), dist (so, sa));
    }

    void testOctaveCpu()
    {
        Proc p;
        lab::setParam (p, "mode", 1.0f);
        lab::setParam (p, "hold", 60.0f);
        lab::prepare (p, sr);
        std::printf ("[15] CPU (Octave, transient trigger on noise): %.2f %% of realtime\n", lab::cpuPercent (p, sr, 5.0));
    }

    int demo (int argc, char* argv[])
    {
        double fsr = sr;
        auto src = lab::readAudio (juce::File (argv[2]), fsr);
        if (src.getNumSamples() == 0) { std::printf ("cannot read %s\n", argv[2]); return 1; }
        const double bpm = argc > 4 ? juce::String (argv[4]).getDoubleValue() : 174.0;
        Proc p;
        for (int i = 5; i < argc; ++i)
        {
            const juce::String kv (argv[i]);
            const auto k = kv.upToFirstOccurrenceOf ("=", false, false), v = kv.fromFirstOccurrenceOf ("=", false, false);
            if (k == "pattern") setSteps (p, v);
            else if (kv.contains ("=")) lab::setParam (p, k, v.getFloatValue());
        }
        lab::prepare (p, fsr);
        const int L = p.getLatencySamples();
        const int n = src.getNumSamples();
        auto in3 = lab::tile (src, 3);
        lab::SimPlayHead ph; ph.bpm = bpm; ph.sampleRate = fsr;
        lab::RunOptions o; o.playhead = &ph;
        auto out = lab::runWith (p, in3, o);
        // A/B: 原音 2 周 → 0.4 s → 処理後 2 周目・3 周目 (レイテンシを除いて頭をそろえる)
        const int gap = (int) (0.4 * fsr);
        juce::AudioBuffer<float> ab (2, n * 4 + gap);
        ab.clear();
        for (int ch = 0; ch < 2; ++ch)
        {
            ab.copyFrom (ch, 0, in3, ch, 0, 2 * n);
            ab.copyFrom (ch, 2 * n + gap, out, ch, n + L, std::min (2 * n, out.getNumSamples() - n - L));
        }
        lab::writeWav (juce::File (argv[3]), ab, fsr);
        std::printf ("demo written (%.0f BPM, latency %d, %d hits, %d switches)\n", bpm, L,
                     p.display().transientCount.load(), p.display().switchCount.load());
        return 0;
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc >= 4 && juce::String (argv[1]) == "demo")
        return demo (argc, argv);

    auto robustCfg = []
    {
        lab::RobustConfig cfg;
        cfg.skipAutomation = { "shuffle", "reset" };   // ボタン (押した瞬間に切り替わるのが仕様)
        cfg.stateSurvivesPrepare = true;                // 並べ替えの状態はホストの prepare で解除しない
        if (std::getenv ("SCR_ROBUST_OCTAVE"))          // Octave モードで並べ替えた状態から (トランジェントでも入れ替わる)
            cfg.setup = [] (juce::AudioProcessor& q)
            {
                lab::setParam (q, "mode", 1.0f);
                if (auto* p = dynamic_cast<Proc*> (&q)) p->setScramble (true, 4321u);
            };
        return cfg;
    };
    if (int r = lab::cliMain<Proc> (argc, argv, sr, robustCfg); r >= 0)
        return r;

    testIdentity();
    testKeepLow();
    testTransient();
    testSeq();
    testMidi();
    testMinGap();
    testState();
    testHostNotify();
    testCpu();
    testReach();
    testShiftPurity();
    testOctaveSine();
    testOctavePitchClasses();
    testModeSwitch();
    testOctaveCpu();

    Proc p;
    setSteps (p, "S...R...S.S.R...");
    lab::setParam (p, "reach", 4.0f);
    lab::prepare (p, sr);
    lab::SimPlayHead ph; ph.bpm = 174.0; ph.sampleRate = sr;
    lab::RunOptions o; o.playhead = &ph;
    auto sig = bursts (1.0, { 0.1, 0.35, 0.6, 0.85 });
    auto pink = lab::sine (sr, 110.0, 1.0, 0.2f);
    for (int ch = 0; ch < 2; ++ch) sig.addFrom (ch, 0, pink, ch, 0, sig.getNumSamples());
    lab::runWith (p, sig, o);
    auto png = juce::File::getCurrentWorkingDirectory().getChildFile ("renders/ui.png");
    lab::snapshot (p, png);
    std::printf ("[9] UI -> %s\n", png.getFullPathName().toRawUTF8());

    // Octave モードの画面 (倍音のある音をアタックで並べ替え)
    Proc q;
    lab::setParam (q, "mode", 1.0f);
    lab::setParam (q, "reach", 2.0f);
    lab::setParam (q, "seqon", 0.0f);
    lab::prepare (q, sr);
    const int T = (int) sr;
    juce::AudioBuffer<float> tone (2, T);
    for (int i = 0; i < T; ++i)
    {
        double v = 0.0;
        for (int h = 1; h <= 16; ++h) v += 0.2 / h * std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * h * i / sr);
        const float env = (i % (T / 4)) < 200 ? (float) (i % (T / 4)) / 200.0f : 1.0f;
        tone.setSample (0, i, (float) v * env); tone.setSample (1, i, (float) v * env);
    }
    lab::run (q, tone);
    auto png2 = juce::File::getCurrentWorkingDirectory().getChildFile ("renders/ui_octave.png");
    lab::snapshot (q, png2);
    std::printf ("[9] UI (Octave) -> %s\n", png2.getFullPathName().toRawUTF8());
    return 0;
}

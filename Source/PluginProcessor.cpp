#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    juce::String dbText (float v, int)  { return juce::String::formatted ("%+.1f dB", (double) v); }
    juce::String octText (float v, int) { return juce::String (v, 1) + " oct"; }
    // 1/n オクターブに近ければ分数で (1/4 oct など)
    juce::String sizeText (float v, int)
    {
        const float inv = 1.0f / std::max (v, 0.001f);
        if (v < 0.99f && std::abs (inv - std::round (inv)) < 0.04f * inv) return "1/" + juce::String ((int) std::round (inv)) + " oct";
        return juce::String (v, 2) + " oct";
    }
    juce::String hzText (float v, int)  { return juce::String (juce::roundToInt (v)) + " Hz"; }
    juce::String msText (float v, int)  { return juce::String (juce::roundToInt (v)) + " ms"; }
    juce::String pctText (float v, int) { return juce::String (juce::roundToInt (v)) + " %"; }
}

ScrambleAudioProcessor::ScrambleAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "Scramble", createLayout())
{
    pReach    = apvts.getRawParameterValue ("reach");
    pSize     = apvts.getRawParameterValue ("size");
    pMode     = apvts.getRawParameterValue ("mode");
    pKeepLow  = apvts.getRawParameterValue ("keeplow");
    pShuffle  = apvts.getRawParameterValue ("shuffle");
    pReset    = apvts.getRawParameterValue ("reset");
    pTrans    = apvts.getRawParameterValue ("trans");
    pSens     = apvts.getRawParameterValue ("sens");
    pHold     = apvts.getRawParameterValue ("hold");
    pSeqOn    = apvts.getRawParameterValue ("seqon");
    pMidiOn   = apvts.getRawParameterValue ("midion");
    pMidiMode = apvts.getRawParameterValue ("midimode");
    pOut      = apvts.getRawParameterValue ("out");
    pBypass   = apvts.getRawParameterValue ("bypass");
    lastSteps = stepsString();
    startTimer (100);
}

juce::String ScrambleAudioProcessor::stepsString() const
{
    juce::String steps;
    for (auto& s : pattern.steps) steps << s.load();
    return steps;
}

void ScrambleAudioProcessor::timerCallback()
{
    bool changed = false;
    if (buttonPressed.exchange (false))
        notifyIn = 2;   // 100〜200 ms 後 (切り替えは最大でフレーム 1 つ分ほど遅れることがある)
    else if (notifyIn > 0 && --notifyIn == 0)
        changed = true;
    const auto steps = stepsString();
    if (steps != lastSteps) { lastSteps = steps; changed = true; }
    if (changed)
    {
        ++hostNotifications;
        updateHostDisplay (ChangeDetails().withNonParameterStateChanged (true));
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout ScrambleAudioProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    // Mode: Random = 塊を周波数シフトで並べ直す / Octave = 塊を 2^k 倍して、音名を保ったままオクターブだけ入れ替える
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode", StringArray { "Random", "Octave" }, 0));
    // Size: 塊の幅 (オクターブ)。Octave モードでは 1/N (N = 1/Size を丸めた整数) として使う。Reach: 塊が動く最大の距離 (オクターブ)。0 で元のまま
    NormalisableRange<float> sizeRange (0.04f, 1.0f, 0.001f);
    sizeRange.setSkewForCentre (0.2f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "size", 1 }, "Size", sizeRange, 0.25f,
                                                       AudioParameterFloatAttributes().withStringFromValueFunction (sizeText)));
    NormalisableRange<float> reachRange (0.0f, 8.0f, 0.01f);
    reachRange.setSkewForCentre (2.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "reach", 1 }, "Reach", reachRange, 3.0f,
                                                       AudioParameterFloatAttributes().withStringFromValueFunction (octText)));
    NormalisableRange<float> lowRange (0.0f, 1000.0f, 1.0f);
    lowRange.setSkewForCentre (200.0f);
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "keeplow", 1 }, "Keep Low", lowRange, 200.0f,
                                                       AudioParameterFloatAttributes().withStringFromValueFunction (hzText)));
    // Shuffle / Reset はボタン (押した瞬間に効く)。パラメータなので MIDI マップや Push にも割り当てられる
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "shuffle", 1 }, "Shuffle", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "reset", 1 }, "Reset", false));
    // トリガー
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "trans", 1 }, "Transient Trig", true));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "sens", 1 }, "Sensitivity",
                                                       NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f,
                                                       AudioParameterFloatAttributes().withStringFromValueFunction (pctText)));
    NormalisableRange<float> holdRange (60.0f, 1000.0f, 1.0f);
    holdRange.setSkewForCentre (200.0f);
    // Stay: 一度シャッフルしたら、次のアタックを受け付けるまでその並べ替えに留まる時間 (ユーザーの命名。MIDI の Hold と区別。ID は hold のまま)
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "hold", 1 }, "Stay", holdRange, 100.0f,
                                                       AudioParameterFloatAttributes().withStringFromValueFunction (msText)));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "seqon", 1 }, "Seq Trig", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "midion", 1 }, "MIDI Trig", true));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "midimode", 1 }, "MIDI Mode",
                                                        StringArray { "Hold", "Latch" }, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "out", 1 }, "Output",
                                                       NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f,
                                                       AudioParameterFloatAttributes().withStringFromValueFunction (dbText)));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "bypass", 1 }, "Bypass", false));
    return layout;
}

bool ScrambleAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void ScrambleAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const int nch = std::max (1, getTotalNumInputChannels());
    engine.prepare (sampleRate, nch);
    engine.restore ({ savedShuffled.load(), savedSeed.load(), pMode->load() > 0.5f ? scr::modeOctave : scr::modeRandom });
    restorePending = false;
    setLatencySamples (engine.latency());

    dryDelay.prepare ({ sampleRate, (juce::uint32) std::max (1, samplesPerBlock), (juce::uint32) nch });
    dryDelay.setMaximumDelayInSamples (engine.latency() + 1);
    dryDelay.setDelay ((float) engine.latency());
    dryDelay.reset();
    dry.setSize (nch, std::max (1, samplesPerBlock));
    bypassMix.reset (sampleRate, 0.01);
    bypassMix.setCurrentAndTargetValue (pBypass->load() > 0.5f ? 1.0f : 0.0f);
    outGain.reset (sampleRate, 0.02);
    outGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (pOut->load()));
    // 読み込んだ直後に Shuffle が押された状態で保存されていた場合も、起動時には発火させない
    lastShuffle = pShuffle->load() > 0.5f;
    lastReset = pReset->load() > 0.5f;
}

void ScrambleAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    const int nch = getTotalNumInputChannels();
    for (int ch = nch; ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, n);
    if (n == 0 || nch == 0)
        return;

    if (restorePending.exchange (false))
        engine.restore ({ savedShuffled.load(), savedSeed.load(), pMode->load() > 0.5f ? scr::modeOctave : scr::modeRandom });

    scr::Settings st;
    st.reach = pReach->load();
    st.size = pSize->load();
    st.mode = pMode->load() > 0.5f ? scr::modeOctave : scr::modeRandom;
    st.keepLow = pKeepLow->load();
    st.transOn = pTrans->load() > 0.5f;
    st.sensitivity = pSens->load();
    st.holdMs = pHold->load();
    st.seqOn = pSeqOn->load() > 0.5f;
    st.midiOn = pMidiOn->load() > 0.5f;
    st.midiLatch = pMidiMode->load() > 0.5f;
    // ボタン: 押された瞬間 (0 → 1) に 1 回だけ
    const bool sh = pShuffle->load() > 0.5f, rs = pReset->load() > 0.5f;
    st.shufflePressed = sh && ! lastShuffle;
    st.resetPressed = rs && ! lastReset;
    lastShuffle = sh;
    lastReset = rs;
    if (st.shufflePressed || st.resetPressed)
        buttonPressed = true;

    scr::Transport tp;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            tp.playing = pos->getIsPlaying();
            if (auto b = pos->getBpm()) tp.bpm = *b;
            if (auto q = pos->getPpqPosition()) tp.ppqAtBlockStart = *q;
            else tp.playing = false;
        }

    // バイパス用に、入力をレイテンシぶん遅らせて控える
    dry.setSize (nch, n, false, false, true);
    float inPk = 0.0f;
    for (int ch = 0; ch < nch; ++ch)
    {
        const auto* src = buffer.getReadPointer (ch);
        auto* d = dry.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
        {
            inPk = std::max (inPk, std::abs (src[i]));
            dryDelay.pushSample (ch, src[i]);
            d[i] = dryDelay.popSample (ch);
        }
    }

    engine.process (buffer, midi, tp, pattern, st);
    midi.clear();

    bypassMix.setTargetValue (pBypass->load() > 0.5f ? 1.0f : 0.0f);
    outGain.setTargetValue (juce::Decibels::decibelsToGain (pOut->load()));
    float outPk = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float b = bypassMix.getNextValue(), g = outGain.getNextValue();
        for (int ch = 0; ch < nch; ++ch)
        {
            auto* p = buffer.getWritePointer (ch);
            const float wet = p[i] * g;
            p[i] = wet + (dry.getSample (ch, i) - wet) * b;
            outPk = std::max (outPk, std::abs (p[i]));
        }
    }

    if (! restorePending.load())
    {
        const auto s = engine.getState();
        savedShuffled.store (s.shuffled);
        savedSeed.store (s.seed);
    }
    auto raise = [] (std::atomic<float>& a, float v) { float c = a.load(); while (v > c && ! a.compare_exchange_weak (c, v)) {} };
    raise (meterIn, inPk);
    raise (meterOut, outPk);
}

juce::AudioProcessorEditor* ScrambleAudioProcessor::createEditor()
{
    return new ScrambleAudioProcessorEditor (*this);
}

void ScrambleAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    auto p = state.getOrCreateChildWithName ("pattern", nullptr);
    p.setProperty ("steps", stepsString(), nullptr);
    p.setProperty ("shuffled", savedShuffled.load(), nullptr);
    p.setProperty ("seed", (juce::int64) savedSeed.load(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void ScrambleAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            apvts.replaceState (state);
            auto p = state.getChildWithName ("pattern");
            if (p.isValid())
            {
                const auto steps = p.getProperty ("steps").toString();
                for (int s = 0; s < scr::numSteps && s < steps.length(); ++s)
                    pattern.steps[(size_t) s].store (juce::jlimit (0, 2, (int) steps[s] - (int) '0'));   // juce_wchar は Windows では符号なし
                if (juce::MessageManager::getInstanceWithoutCreating() != nullptr && juce::MessageManager::getInstance()->isThisTheMessageThread())
                    lastSteps = stepsString();   // 読み込んだだけでは「変更あり」にしない
                setScramble ((bool) p.getProperty ("shuffled", false), (uint32_t) (juce::int64) p.getProperty ("seed", 1));
            }
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ScrambleAudioProcessor();
}

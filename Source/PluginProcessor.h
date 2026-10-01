#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include "ScrambleEngine.h"

class ScrambleAudioProcessor  : public juce::AudioProcessor
{
public:
    ScrambleAudioProcessor();
    ~ScrambleAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return true; }    // MIDI ノートでシャッフル (Live では MIDI トラックの MIDI To からこのプラグインへ)
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return apvts.getParameter ("bypass"); }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    juce::AudioProcessorValueTreeState apvts;

    scr::Pattern pattern;   // シーケンサーの中身 (パラメータにせず状態として保存)
    const scr::Display& display() const { return engine.display; }
    std::atomic<float> meterIn { 0.0f }, meterOut { 0.0f };

    // 並べ替えの状態 (セットに保存)。restorePending はオーディオスレッドでエンジンに反映する
    std::atomic<bool> savedShuffled { false }, restorePending { false };
    std::atomic<uint32_t> savedSeed { 1 };
    void setScramble (bool shuffled, uint32_t seed) { savedShuffled = shuffled; savedSeed = seed; restorePending = true; }

private:
    scr::Engine engine;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dryDelay { 1 << 16 };
    juce::AudioBuffer<float> dry;
    juce::SmoothedValue<float> bypassMix;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outGain;
    bool lastShuffle = false, lastReset = false;

    std::atomic<float>* pReach = nullptr;
    std::atomic<float>* pSize = nullptr;
    std::atomic<float>* pMode = nullptr;
    std::atomic<float>* pKeepLow = nullptr;
    std::atomic<float>* pShuffle = nullptr;
    std::atomic<float>* pReset = nullptr;
    std::atomic<float>* pTrans = nullptr;
    std::atomic<float>* pSens = nullptr;
    std::atomic<float>* pHold = nullptr;
    std::atomic<float>* pSeqOn = nullptr;
    std::atomic<float>* pMidiOn = nullptr;
    std::atomic<float>* pMidiMode = nullptr;
    std::atomic<float>* pOut = nullptr;
    std::atomic<float>* pBypass = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScrambleAudioProcessor)
};

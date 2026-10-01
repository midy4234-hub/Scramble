#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "LabLook.h"

// シーケンサーの 16 ステップ: クリックで 空 → S (Shuffle) → R (Reset) → 空、右クリックで空
class StepLane  : public juce::Component
{
public:
    explicit StepLane (scr::Pattern& p) : pattern (p) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    int playStep = -1;
private:
    int cellAt (float x) const { return juce::jlimit (0, scr::numSteps - 1, (int) (x / getWidth() * scr::numSteps)); }
    scr::Pattern& pattern;
};

// 色分けスペクトル: 上が入力、下が出力。色 = その音が元いた周波数 (低い = 赤 → 高い = 紫)。
// 出力で色が入れ替わって並ぶ = そこへ飛んできた音。Keep Low より下は動かないので色を落とす。下端の目盛は塊の切れ目
class SpectrumView  : public juce::Component
{
public:
    explicit SpectrumView (const scr::Display& d) : disp (d) {}
    void update();   // 30 Hz で呼ぶ
    void paint (juce::Graphics&) override;
private:
    void drawSpectrum (juce::Graphics&, juce::Rectangle<float>, bool output) const;
    float xToFreq (float x, float w) const { return 20.0f * std::pow (1000.0f, x / w); }
    float freqToX (float f, float w) const { return w * std::log (f / 20.0f) / std::log (1000.0f); }
    const scr::Display& disp;
    std::vector<float> db;
    std::vector<int> map, cutIn, cutOut;
    int bins = 0, flash = 0, lastSwitch = -1, source = 0;
    float sr = 48000.0f, n = 2048.0f, keepLow = 200.0f;
    bool shuffled = false;
    int modeNow = 0, divisions = 4;
};

class ScrambleAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                      private juce::Timer
{
public:
    explicit ScrambleAudioProcessorEditor (ScrambleAudioProcessor&);
    ~ScrambleAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void setUpMomentary (juce::TextButton& b, const juce::String& paramId);

    ScrambleAudioProcessor& proc;
    lab::Look look;

    lab::Knob size, reach, keepLow, sens, hold, output;
    lab::Toggle transOn, midiOn, seqOn, bypass;
    lab::Switch midiMode, mode;
    juce::TextButton shuffleBtn { "Shuffle" }, resetBtn { "Reset" };
    StepLane lane;
    SpectrumView view;

    juce::Rectangle<int> scramblePanel, displayPanel, triggerPanel, masterPanel, meterArea;
    juce::Rectangle<int> transBlock, midiBlock, seqBlock, ledArea;
    float meterIn = -100.0f, meterOut = -100.0f;
    bool shuffled = false;
    int lastTransient = 0, led = 0, statusMode = 0, statusDiv = 4;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ScrambleAudioProcessorEditor)
};

#include "PluginEditor.h"

namespace
{
    constexpr int W = 960, H = 560;
    constexpr int margin = 16, gap = 10;
    constexpr int masterW = 120, scrambleW = 270, topH = 330;
    constexpr float floorDb = -84.0f;

    const char* sourceName (int s)
    {
        switch (s)
        {
            case scr::srcTransient: return "TRANSIENT";
            case scr::srcSeq:       return "SEQ";
            case scr::srcMidi:      return "MIDI";
            case scr::srcMode:      return "MODE";
            default:                return "BUTTON";
        }
    }

    juce::Colour hueFor (float f, bool kept)
    {
        const float h = 0.78f * juce::jlimit (0.0f, 1.0f, std::log (std::max (f, 20.0f) / 20.0f) / std::log (1000.0f));
        return kept ? juce::Colour::fromHSV (h, 0.12f, 0.55f, 1.0f) : juce::Colour::fromHSV (h, 0.72f, 0.95f, 1.0f);
    }

    void drawBeatLines (juce::Graphics& g, juce::Rectangle<float> r)
    {
        g.setColour (lab::col::panelStroke.brighter (0.25f));
        for (int s = 4; s < scr::numSteps; s += 4)
            g.drawVerticalLine ((int) (r.getX() + r.getWidth() * s / scr::numSteps), r.getY(), r.getBottom());
    }
}

//==============================================================================
void StepLane::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    const float cw = r.getWidth() / scr::numSteps;
    for (int s = 0; s < scr::numSteps; ++s)
    {
        auto cell = juce::Rectangle<float> (r.getX() + s * cw, r.getY(), cw, r.getHeight()).reduced (1.5f);
        const int v = pattern.steps[(size_t) s].load();
        g.setColour (v == scr::stepShuffle ? lab::col::accent : v == scr::stepReset ? lab::col::blue : lab::col::switchOff);
        g.fillRoundedRectangle (cell, 2.0f);
        if (s == playStep)
        {
            g.setColour (lab::col::text);
            g.drawRoundedRectangle (cell, 2.0f, 1.5f);
        }
        if (v != scr::stepNone)
        {
            g.setColour (lab::col::onText);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (v == scr::stepShuffle ? "S" : "R", cell, juce::Justification::centred);
        }
    }
    drawBeatLines (g, r);
}

void StepLane::mouseDown (const juce::MouseEvent& e)
{
    auto& v = pattern.steps[(size_t) cellAt ((float) e.x)];
    v.store (e.mods.isPopupMenu() ? scr::stepNone : (v.load() + 1) % 3);
    repaint();
}

//==============================================================================
void SpectrumView::update()
{
    const int b = disp.bins.load();
    if (b != bins)
    {
        bins = b;
        db.assign ((size_t) std::max (1, bins), -200.0f);
        map.assign ((size_t) std::max (1, bins), 0);
    }
    sr = disp.sampleRate.load();
    n = disp.fftSize.load();
    keepLow = disp.keepLow.load();
    shuffled = disp.audibleShuffled.load();
    const int slot = juce::jlimit (0, 1, disp.audibleSlot.load());
    for (int k = 0; k < bins; ++k)
    {
        const float v = juce::Decibels::gainToDecibels (disp.inMag[(size_t) k].load (std::memory_order_relaxed), -200.0f);
        db[(size_t) k] = std::max (v, db[(size_t) k] - 1.5f);   // 上がるときは即、下がるときは 1 フレーム 1.5 dB
        map[(size_t) k] = shuffled ? juce::jlimit (0, bins - 1, disp.srcBin[(size_t) slot][(size_t) k].load (std::memory_order_relaxed)) : k;
    }
    // 塊の切れ目 (エンジンが並べ替えを作るときに書く)
    cutOut.clear();
    cutIn.clear();
    if (shuffled)
    {
        const int ni = juce::jlimit (0, scr::maxCuts, disp.numCutIn[(size_t) slot].load());
        const int no = juce::jlimit (0, scr::maxCuts, disp.numCutOut[(size_t) slot].load());
        for (int i = 0; i < ni; ++i) cutIn.push_back (disp.cutIn[(size_t) slot][(size_t) i].load());
        for (int i = 0; i < no; ++i) cutOut.push_back (disp.cutOut[(size_t) slot][(size_t) i].load());
    }
    modeNow = disp.audibleMode.load();
    divisions = disp.octaveDivisions.load();
    const int sw = disp.switchCount.load();
    if (sw != lastSwitch)
    {
        if (lastSwitch >= 0) flash = 8;
        lastSwitch = sw;
        source = disp.lastSource.load();
    }
    else if (flash > 0)
        --flash;
    repaint();
}

void SpectrumView::drawSpectrum (juce::Graphics& g, juce::Rectangle<float> r, bool output) const
{
    g.setColour (lab::col::lcd);
    g.fillRoundedRectangle (r, 3.0f);
    if (bins <= 1) return;
    const float w = r.getWidth();
    // 縦の目盛 (100 / 1k / 10k)
    g.setColour (lab::col::lcdGrid);
    for (float f : { 100.0f, 1000.0f, 10000.0f })
        g.drawVerticalLine ((int) (r.getX() + freqToX (f, w)), r.getY(), r.getBottom());
    for (int d = -24; d > (int) floorDb; d -= 24)
        g.drawHorizontalLine ((int) (r.getBottom() - (d - floorDb) / -floorDb * r.getHeight()), r.getX(), r.getRight());

    const float binHz = sr / n;
    for (int x = 0; x < (int) w; ++x)
    {
        const float k0 = xToFreq ((float) x, w) / binHz, k1 = xToFreq ((float) x + 1.0f, w) / binHz;
        int lo = (int) std::ceil (k0), hi = (int) std::floor (k1);
        if (hi < lo) lo = hi = (int) std::round (0.5f * (k0 + k1));
        lo = juce::jlimit (1, bins - 1, lo);
        hi = juce::jlimit (lo, bins - 1, hi);
        int best = -1, bestSrc = lo;
        float v = -1000.0f;
        for (int k = lo; k <= hi; ++k)
        {
            const int src = output ? map[(size_t) k] : k;
            if (db[(size_t) src] > v) { v = db[(size_t) src]; best = k; bestSrc = src; }
        }
        if (best < 0) continue;
        const float h = juce::jlimit (0.0f, 1.0f, (v - floorDb) / -floorDb) * r.getHeight();
        if (h < 0.5f) continue;
        const bool kept = best * binHz < keepLow;
        g.setColour (hueFor (bestSrc * binHz, kept));
        g.fillRect (r.getX() + (float) x, r.getBottom() - h, 1.0f, h);
    }

    // Keep Low の線
    const float xk = r.getX() + freqToX (std::max (20.0f, keepLow), w);
    g.setColour (lab::col::text.withAlpha (0.55f));
    g.drawVerticalLine ((int) xk, r.getY(), r.getBottom());
    // 塊の切れ目 (下端の目盛)。出力は「並べ直した後の切れ目」、入力は「元の切れ目」
    if (shuffled)
    {
        g.setColour (lab::col::text.withAlpha (0.45f));
        const auto& cut = output ? cutOut : cutIn;
        for (int k : cut)
        {
            const float x = r.getX() + freqToX (std::max (20.0f, k * binHz), w);
            g.drawVerticalLine ((int) x, r.getBottom() - 6.0f, r.getBottom());
        }
    }
    g.setColour (lab::col::dim);
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    g.drawText (output ? "OUT" : "IN", r.reduced (8.0f, 5.0f), juce::Justification::topLeft);
}

void SpectrumView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    auto axis = r.removeFromBottom (14.0f);
    const float half = (r.getHeight() - 6.0f) * 0.5f;
    auto top = r.removeFromTop (half);
    r.removeFromTop (6.0f);
    drawSpectrum (g, top, false);
    drawSpectrum (g, r, true);

    g.setColour (lab::col::dim);
    g.setFont (juce::FontOptions (9.5f));
    const float w = axis.getWidth();
    for (auto [f, t] : { std::pair<float, const char*> { 100.0f, "100" }, { 1000.0f, "1k" }, { 10000.0f, "10k" } })
        g.drawText (t, (int) (axis.getX() + freqToX (f, w)) - 20, (int) axis.getY(), 40, 14, juce::Justification::centred);
    g.drawText ("KEEP", (int) (axis.getX() + freqToX (std::max (20.0f, keepLow), w)) - 20, (int) axis.getY(), 40, 14, juce::Justification::centred);

    // 切り替わった瞬間に、何がきっかけだったかを出す
    if (flash > 0)
    {
        g.setColour (lab::col::accent.withAlpha (juce::jlimit (0.0f, 1.0f, flash / 5.0f)));
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText (juce::String (shuffled ? "SHUFFLE" : "RESET") + " <- " + sourceName (source),
                    r.withHeight (18.0f).translated (0.0f, 4.0f).reduced (8.0f, 0.0f), juce::Justification::topRight);
    }
}

//==============================================================================
ScrambleAudioProcessorEditor::ScrambleAudioProcessorEditor (ScrambleAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p), lane (p.pattern), view (p.display())
{
    setLookAndFeel (&look);
    size.setUp (*this, proc.apvts, "size", "Size");
    reach.setUp (*this, proc.apvts, "reach", "Reach");
    keepLow.setUp (*this, proc.apvts, "keeplow", "Keep Low");
    sens.setUp (*this, proc.apvts, "sens", "Sensitivity");
    hold.setUp (*this, proc.apvts, "hold", "Stay");
    output.setUp (*this, proc.apvts, "out", "Output", true);
    transOn.setUp (*this, proc.apvts, "trans", "On");
    midiOn.setUp (*this, proc.apvts, "midion", "On");
    seqOn.setUp (*this, proc.apvts, "seqon", "On");
    bypass.setUp (*this, proc.apvts, "bypass", "Bypass");
    midiMode.setUp (*this, proc.apvts, "midimode", { { "Hold", 0 }, { "Latch", 1 } });
    mode.setUp (*this, proc.apvts, "mode", { { "Random", 0 }, { "Octave", 1 } });
    setUpMomentary (shuffleBtn, "shuffle");
    setUpMomentary (resetBtn, "reset");
    addAndMakeVisible (lane);
    addAndMakeVisible (view);
    setSize (W, H);
    startTimerHz (30);
}

ScrambleAudioProcessorEditor::~ScrambleAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

// 押している間だけ 1 (押した瞬間にプラグイン側で 1 回だけ発火)
void ScrambleAudioProcessorEditor::setUpMomentary (juce::TextButton& b, const juce::String& paramId)
{
    addAndMakeVisible (b);
    auto* param = proc.apvts.getParameter (paramId);
    b.onStateChange = [&b, param, down = std::make_shared<bool> (false)]
    {
        const bool isDown = b.isDown();
        if (isDown == *down) return;
        *down = isDown;
        if (isDown) { param->beginChangeGesture(); param->setValueNotifyingHost (1.0f); }
        else        { param->setValueNotifyingHost (0.0f); param->endChangeGesture(); }
    };
}

void ScrambleAudioProcessorEditor::timerCallback()
{
    view.update();
    const auto& d = proc.display();
    const int st = d.seqStep.load();
    if (st != lane.playStep) { lane.playStep = st; lane.repaint(); }
    const bool sh = d.audibleShuffled.load();
    const int md = d.audibleMode.load(), dv = d.octaveDivisions.load();
    if (sh != shuffled || md != statusMode || dv != statusDiv) { shuffled = sh; statusMode = md; statusDiv = dv; repaint (scramblePanel); }
    const int tc = d.transientCount.load();
    if (tc != lastTransient) { lastTransient = tc; led = 4; repaint (ledArea); }
    else if (led > 0) { --led; repaint (ledArea); }
    meterIn  = lab::fallDb (meterIn,  proc.meterIn.exchange (0.0f));
    meterOut = lab::fallDb (meterOut, proc.meterOut.exchange (0.0f));
    repaint (meterArea);
}

void ScrambleAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (lab::col::bg);
    lab::drawTitle (g, "Scramble");
    lab::drawPanel (g, scramblePanel, "SCRAMBLE");
    lab::drawPanel (g, displayPanel, "SPECTRUM");
    lab::drawPanel (g, triggerPanel, "TRIGGER");
    lab::drawPanel (g, masterPanel, "MASTER");

    g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
    g.setColour (shuffled ? lab::col::accent : lab::col::dim);
    // Octave で並べ替え中は、実際に使っている塊の幅 (1/N oct) も出す
    const juce::String status = ! shuffled ? "ORIGINAL"
                              : statusMode == scr::modeOctave ? "SHUFFLED  (OCTAVE, 1/" + juce::String (statusDiv) + " oct)" : "SHUFFLED  (RANDOM)";
    g.drawText (status, scramblePanel.getX() + 12, shuffleBtn.getY() - 26, scramblePanel.getWidth() - 24, 18,
                juce::Justification::centred);

    // トリガーの 3 区画
    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    for (auto [blk, name] : { std::pair<juce::Rectangle<int>, const char*> { transBlock, "TRANSIENT" }, { midiBlock, "MIDI NOTE" }, { seqBlock, "STEP SEQ" } })
    {
        g.setColour (lab::col::dim);
        g.drawText (name, blk.getX() + 8, blk.getY(), 120, 20, juce::Justification::centredLeft);
        if (blk != seqBlock)
        {
            g.setColour (lab::col::panelStroke.brighter (0.2f));
            g.drawVerticalLine (blk.getRight(), (float) blk.getY() + 4.0f, (float) blk.getBottom() - 4.0f);
        }
    }
    // 検知したら光る
    g.setColour (led > 0 ? lab::col::accent : lab::col::switchOff);
    g.fillEllipse (ledArea.toFloat());
    g.setColour (lab::col::dim);
    g.setFont (juce::FontOptions (9.5f));
    g.drawText ("HIT", ledArea.getX() - 6, ledArea.getBottom() + 2, ledArea.getWidth() + 12, 12, juce::Justification::centred);
    // ステップ番号
    const auto lr = lane.getBounds();
    for (int s = 0; s < scr::numSteps; s += 4)
        g.drawText (juce::String (s / 4 + 1), lr.getX() + lr.getWidth() * s / scr::numSteps + 3, lr.getY() - 15, 30, 14, juce::Justification::centredLeft);
    g.drawText ("click: S -> R -> off", lr.getX(), lr.getBottom() + 4, lr.getWidth(), 14, juce::Justification::centredRight);

    lab::drawLcd (g, meterArea);
    const auto m = meterArea.toFloat().reduced (10.0f, 10.0f).withTrimmedBottom (14.0f);
    lab::drawMeterV (g, m.withWidth (10.0f).withX (m.getCentreX() - 14.0f), meterIn);
    lab::drawMeterV (g, m.withWidth (10.0f).withX (m.getCentreX() + 4.0f), meterOut);
    g.setColour (lab::col::dim);
    g.setFont (juce::FontOptions (9.5f));
    g.drawText ("IN  OUT", meterArea.withTop (meterArea.getBottom() - 20), juce::Justification::centred);
}

void ScrambleAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (margin, 0);
    area.removeFromTop (34);
    area.removeFromBottom (14);
    masterPanel = area.removeFromRight (masterW);
    area.removeFromRight (gap);
    auto top = area.removeFromTop (topH);
    area.removeFromTop (gap);
    triggerPanel = area;
    scramblePanel = top.removeFromLeft (scrambleW);
    top.removeFromLeft (gap);
    displayPanel = top;

    // SCRAMBLE: つまみ 2 つ、その下に状態とボタン
    auto s = scramblePanel.reduced (6, 6).withTrimmedTop (24);
    auto knobs = s.removeFromTop (100);
    const int kw = knobs.getWidth() / 3;
    size.setBounds (knobs.removeFromLeft (kw));
    reach.setBounds (knobs.removeFromLeft (kw));
    keepLow.setBounds (knobs);
    s.removeFromTop (34);
    shuffleBtn.setBounds (s.removeFromTop (30).reduced (8, 0));
    s.removeFromTop (8);
    resetBtn.setBounds (s.removeFromTop (30).reduced (8, 0));

    view.setBounds (displayPanel.reduced (10, 8).withTrimmedTop (22));
    mode.setBounds ({ scramblePanel.getRight() - 150, scramblePanel.getY() + 4, 142, 20 });

    // TRIGGER: TRANSIENT | MIDI NOTE | STEP SEQ
    auto t = triggerPanel.reduced (6, 6).withTrimmedTop (22);
    transBlock = t.removeFromLeft (250);
    midiBlock = t.removeFromLeft (140);
    seqBlock = t;
    transOn.button.setBounds (transBlock.getRight() - 56, transBlock.getY() + 1, 46, 18);
    midiOn.button.setBounds (midiBlock.getRight() - 56, midiBlock.getY() + 1, 46, 18);
    seqOn.button.setBounds (seqBlock.getRight() - 52, seqBlock.getY() + 1, 46, 18);

    auto tb = transBlock.withTrimmedTop (24);
    sens.setBounds (tb.removeFromLeft (90));
    hold.setBounds (tb.removeFromLeft (90));
    ledArea = juce::Rectangle<int> (12, 12).withCentre ({ tb.getCentreX(), tb.getCentreY() - 6 });

    auto mb = midiBlock.withTrimmedTop (30).reduced (14, 0);
    for (auto& b : midiMode.buttons)
    {
        b->setBounds (mb.removeFromTop (26));
        mb.removeFromTop (6);
    }

    auto sb = seqBlock.withTrimmedTop (40).reduced (12, 0);
    lane.setBounds (sb.removeFromTop (44));

    // MASTER
    auto mr = masterPanel.reduced (4, 6).withTrimmedTop (22);
    bypass.button.setBounds (masterPanel.getX() + 10, masterPanel.getY() + 30, masterW - 20, 20);
    mr.removeFromTop (34);
    output.setBounds (mr.removeFromTop (92));
    meterArea = mr.reduced (8, 6);
}

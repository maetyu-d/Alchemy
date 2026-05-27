#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "WeldChucKEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

namespace
{
constexpr int maxStateCount = 16;
constexpr int maxTrackCountPerState = 16;

using Language = EmbeddedLanguageEngine::Language;

struct TrackModel
{
    juce::String name;
    Language language = Language::chuck;
    bool tightlySynced = true;
    double tempoBpm = 120.0;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    double phaseBeats = 0.0;
    float level = 0.75f;
    bool muted = false;
    bool soloed = false;
    juce::String code;
    std::vector<EmbeddedPerformanceEngine::TrackGainEvent> gainEvents;
};

struct StateTransitionModel
{
    int targetStateIndex = -1;
    double weight = 1.0;
};

struct StateModel
{
    juce::String name;
    double durationBeats = 16.0;
    double tailBeats = 4.0;
    int canvasX = 0;
    int canvasY = 0;
    std::vector<StateTransitionModel> transitions;
    std::vector<TrackModel> tracks;
};

struct ProjectModel
{
    double globalTempoBpm = 120.0;
    int globalTimeSigNumerator = 4;
    int globalTimeSigDenominator = 4;
    double globalPhaseBeats = 0.0;
    juce::String chuckScoreScript;
    std::vector<EmbeddedPerformanceEngine::TempoEvent> tempoMap;
    std::vector<EmbeddedPerformanceEngine::TimeSignatureEvent> timeSignatureMap;
    std::vector<EmbeddedPerformanceEngine::PhaseRotationEvent> phaseRotationMap;
    double scheduledStopBeat = -1.0;
    std::vector<int> arrangementOrder;
    std::vector<StateModel> states;
};

juce::String languageName (Language language)
{
    return EmbeddedLanguageEngine::getLanguageName (language);
}

Language languageFromIndex (int index)
{
    switch (index)
    {
        case 1: return Language::chuck;
        case 2: return Language::supercollider;
        case 3: return Language::rtcmix;
        case 4: return Language::csound;
        case 5: return Language::faust;
        default: return Language::chuck;
    }
}

int indexForLanguage (Language language)
{
    switch (language)
    {
        case Language::chuck: return 1;
        case Language::supercollider: return 2;
        case Language::rtcmix: return 3;
        case Language::csound: return 4;
        case Language::faust: return 5;
    }

    return 1;
}

Language languageFromName (const juce::String& name)
{
    const auto normalised = name.toLowerCase();
    if (normalised == "sc" || normalised == "supercollider")
        return Language::supercollider;
    if (normalised == "rtcmix" || normalised == "rt")
        return Language::rtcmix;
    if (normalised == "csound" || normalised == "cs")
        return Language::csound;
    if (normalised == "faust")
        return Language::faust;

    return Language::chuck;
}

double beatsPerSecond (double bpm) noexcept
{
    return juce::jlimit (1.0, 999.0, bpm) / 60.0;
}

double oneBarBeats (const ProjectModel& project) noexcept
{
    return juce::jmax (1, project.globalTimeSigNumerator) * (4.0 / juce::jmax (1, project.globalTimeSigDenominator));
}

std::vector<int> defaultArrangementOrder (const ProjectModel& project)
{
    std::vector<int> order;
    order.reserve (project.states.size());

    for (int i = 0; i < static_cast<int> (project.states.size()); ++i)
        order.push_back (i);

    return order;
}

std::vector<int> playableArrangementOrder (const ProjectModel& project)
{
    if (project.arrangementOrder.empty())
        return defaultArrangementOrder (project);

    std::vector<int> order;
    order.reserve (project.arrangementOrder.size());
    for (const auto stateIndex : project.arrangementOrder)
        if (stateIndex >= 0 && stateIndex < static_cast<int> (project.states.size()))
            order.push_back (stateIndex);

    return order.empty() ? defaultArrangementOrder (project) : order;
}

double totalDurationBeats (const ProjectModel& project) noexcept
{
    double total = 0.0;

    if (project.arrangementOrder.empty())
    {
        for (const auto& state : project.states)
            total += juce::jmax (0.0, state.durationBeats);
    }
    else
    {
        for (const auto stateIndex : project.arrangementOrder)
            if (stateIndex >= 0 && stateIndex < static_cast<int> (project.states.size()))
                total += juce::jmax (0.0, project.states[static_cast<size_t> (stateIndex)].durationBeats);
    }

    if (project.scheduledStopBeat >= 0.0)
        return juce::jmin (total, project.scheduledStopBeat);

    return total;
}

bool projectHasSoloedTrack (const ProjectModel& project)
{
    for (const auto& state : project.states)
        for (const auto& track : state.tracks)
            if (track.soloed)
                return true;

    return false;
}

float effectiveTrackGain (const ProjectModel& project, int stateIndex, int trackIndex)
{
    if (stateIndex < 0 || stateIndex >= static_cast<int> (project.states.size()))
        return 0.0f;

    const auto& state = project.states[static_cast<size_t> (stateIndex)];
    if (trackIndex < 0 || trackIndex >= static_cast<int> (state.tracks.size()))
        return 0.0f;

    const auto& track = state.tracks[static_cast<size_t> (trackIndex)];
    if (track.muted || (projectHasSoloedTrack (project) && ! track.soloed))
        return 0.0f;

    return juce::jlimit (0.0f, 4.0f, track.level);
}

std::vector<int> resolveProbabilisticArrangement (const ProjectModel& project)
{
    std::vector<int> order;
    if (project.states.empty())
        return order;

    std::mt19937 rng (std::random_device{}());
    constexpr int maximumResolvedStateVisits = 64;
    const auto hasExplicitTransitions = std::any_of (project.states.begin(),
                                                     project.states.end(),
                                                     [] (const StateModel& state)
                                                     {
                                                         return ! state.transitions.empty();
                                                     });
    auto stateIndex = 0;

    for (int visit = 0; visit < maximumResolvedStateVisits; ++visit)
    {
        if (stateIndex < 0 || stateIndex >= static_cast<int> (project.states.size()))
            break;

        order.push_back (stateIndex);
        const auto& transitions = project.states[static_cast<size_t> (stateIndex)].transitions;

        if (transitions.empty())
        {
            if (hasExplicitTransitions)
                break;

            const auto nextState = stateIndex + 1;
            if (nextState >= static_cast<int> (project.states.size()))
                break;

            stateIndex = nextState;
            continue;
        }

        auto totalWeight = 0.0;
        for (const auto& transition : transitions)
            if (transition.targetStateIndex >= 0
                && transition.targetStateIndex < static_cast<int> (project.states.size())
                && transition.weight > 0.0
                && std::isfinite (transition.weight))
                totalWeight += transition.weight;

        if (totalWeight <= 0.0)
            break;

        std::uniform_real_distribution<double> distribution (0.0, totalWeight);
        auto roll = distribution (rng);
        auto selectedState = -1;
        auto lastValidState = -1;

        for (const auto& transition : transitions)
        {
            if (transition.targetStateIndex < 0
                || transition.targetStateIndex >= static_cast<int> (project.states.size())
                || transition.weight <= 0.0
                || ! std::isfinite (transition.weight))
                continue;

            lastValidState = transition.targetStateIndex;
            roll -= transition.weight;
            if (roll <= 0.0)
            {
                selectedState = transition.targetStateIndex;
                break;
            }
        }

        if (selectedState < 0)
            selectedState = lastValidState;

        stateIndex = selectedState;
    }

    return order;
}

bool projectHasExplicitTransitions (const ProjectModel& project)
{
    return std::any_of (project.states.begin(),
                        project.states.end(),
                        [] (const StateModel& state)
                        {
                            return ! state.transitions.empty();
                        });
}

void arrangeStatesAsFiniteStateMachine (ProjectModel& project)
{
    if (project.states.empty())
        return;

    constexpr int columnSpacing = 260;
    constexpr int leftMargin = 70;
    constexpr int centreY = 160;
    constexpr int branchSpacing = 130;

    if (! projectHasExplicitTransitions (project))
    {
        for (int i = 0; i < static_cast<int> (project.states.size()); ++i)
        {
            auto& state = project.states[static_cast<size_t> (i)];
            state.canvasX = leftMargin + i * columnSpacing;
            state.canvasY = centreY;
        }

        return;
    }

    const auto stateCount = static_cast<int> (project.states.size());
    std::vector<int> depth (project.states.size(), -1);
    std::vector<int> queue;
    queue.reserve (project.states.size());
    depth[0] = 0;
    queue.push_back (0);

    for (size_t read = 0; read < queue.size(); ++read)
    {
        const auto from = queue[read];
        for (const auto& transition : project.states[static_cast<size_t> (from)].transitions)
        {
            const auto to = transition.targetStateIndex;
            if (to < 0 || to >= stateCount || depth[static_cast<size_t> (to)] >= 0)
                continue;

            depth[static_cast<size_t> (to)] = depth[static_cast<size_t> (from)] + 1;
            queue.push_back (to);
        }
    }

    auto maxDepth = 0;
    for (int i = 0; i < stateCount; ++i)
    {
        if (depth[static_cast<size_t> (i)] < 0)
            depth[static_cast<size_t> (i)] = ++maxDepth;
        else
            maxDepth = juce::jmax (maxDepth, depth[static_cast<size_t> (i)]);
    }

    std::vector<std::vector<int>> columns (static_cast<size_t> (maxDepth + 1));
    for (int i = 0; i < stateCount; ++i)
        columns[static_cast<size_t> (depth[static_cast<size_t> (i)])].push_back (i);

    for (int columnIndex = 0; columnIndex < static_cast<int> (columns.size()); ++columnIndex)
    {
        const auto& column = columns[static_cast<size_t> (columnIndex)];
        const auto count = static_cast<int> (column.size());
        for (int slot = 0; slot < count; ++slot)
        {
            auto& state = project.states[static_cast<size_t> (column[static_cast<size_t> (slot)])];
            state.canvasX = leftMargin + columnIndex * columnSpacing;
            state.canvasY = count <= 1 ? centreY : centreY - ((count - 1) * branchSpacing) / 2 + slot * branchSpacing;
        }
    }
}

int meterIdFor (int numerator, int denominator) noexcept
{
    if (numerator == 3 && denominator == 4)
        return 1;
    if (numerator == 5 && denominator == 4)
        return 3;
    if (numerator == 7 && denominator == 8)
        return 4;
    if (numerator == 4 && denominator == 4)
        return 2;

    return 0;
}

juce::Colour languageColour (Language language)
{
    switch (language)
    {
        case Language::chuck: return juce::Colour (0xff69b1c9);
        case Language::supercollider: return juce::Colour (0xffc98569);
        case Language::rtcmix: return juce::Colour (0xff86bd72);
        case Language::csound: return juce::Colour (0xffc1a85d);
        case Language::faust: return juce::Colour (0xffb07ac7);
    }

    return juce::Colours::white;
}

void configureCodeEditor (juce::TextEditor& editor, float fontSize = 13.0f)
{
    editor.setMultiLine (true);
    editor.setReturnKeyStartsNewLine (true);
    editor.setScrollbarsShown (true);
    editor.setTabKeyUsedAsCharacter (true);
    editor.setPopupMenuEnabled (true);
    editor.setFont (juce::FontOptions (fontSize));
    editor.setIndents (8, 6);
    editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0b0f0e));
    editor.setColour (juce::TextEditor::textColourId, juce::Colour (0xffe6eee9));
    editor.setColour (juce::TextEditor::highlightColourId, juce::Colour (0xff49665f));
    editor.setColour (juce::TextEditor::highlightedTextColourId, juce::Colours::white);
    editor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff33403c));
    editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0xff84c7b6));
}

void setCodeEditorFontSize (juce::TextEditor& editor, float fontSize)
{
    editor.setFont (juce::FontOptions (fontSize));
}

class CodeEditorPane final : public juce::Component
{
public:
    std::function<void()> onTextChange;

    CodeEditorPane()
    {
        addAndMakeVisible (languageLabel);
        languageLabel.setJustificationType (juce::Justification::centredLeft);
        languageLabel.setColour (juce::Label::textColourId, juce::Colour (0xffcfd8d3));

        addAndMakeVisible (search);
        search.setMultiLine (false);
        search.setReturnKeyStartsNewLine (false);
        search.setTextToShowWhenEmpty ("Search", juce::Colour (0xff74817c));
        search.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111715));
        search.setColour (juce::TextEditor::textColourId, juce::Colour (0xffe6eee9));
        search.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff33403c));
        search.onTextChange = [this]
        {
            searchMatchIndex = 0;
            refreshSearch();
        };

        addAndMakeVisible (previousMatch);
        previousMatch.setButtonText ("<");
        previousMatch.onClick = [this] { moveSearchMatch (-1); };

        addAndMakeVisible (nextMatch);
        nextMatch.setButtonText (">");
        nextMatch.onClick = [this] { moveSearchMatch (1); };

        addAndMakeVisible (statusLabel);
        statusLabel.setJustificationType (juce::Justification::centredRight);
        statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8fa19a));

        addAndMakeVisible (gutter);

        configureCodeEditor (editor, codeFontSize);
        editor.onTextChange = [this]
        {
            gutter.repaint();
            refreshStatus();
            refreshSearch();
            if (onTextChange != nullptr)
                onTextChange();
        };
        addAndMakeVisible (editor);

        setLanguage ("Code", juce::Colour (0xff69b1c9));
        refreshStatus();
    }

    void setText (const juce::String& text, bool sendNotification)
    {
        editor.setText (text, sendNotification);
        gutter.repaint();
        refreshStatus();
        refreshSearch();
    }

    juce::String getText() const
    {
        return editor.getText();
    }

    void setFontSize (float fontSize)
    {
        codeFontSize = juce::jlimit (10.0f, 24.0f, fontSize);
        setCodeEditorFontSize (editor, codeFontSize);
        gutter.repaint();
    }

    void setLanguage (const juce::String& language, juce::Colour colour)
    {
        accent = colour;
        languageLabel.setText (language, juce::dontSendNotification);
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto toolbar = area.removeFromTop (26);
        languageLabel.setBounds (toolbar.removeFromLeft (112));
        statusLabel.setBounds (toolbar.removeFromRight (122));
        nextMatch.setBounds (toolbar.removeFromRight (30).reduced (3, 2));
        previousMatch.setBounds (toolbar.removeFromRight (30).reduced (3, 2));
        search.setBounds (toolbar.removeFromRight (juce::jmin (180, toolbar.getWidth())).reduced (4, 2));

        area.removeFromTop (4);
        gutter.setBounds (area.removeFromLeft (44));
        editor.setBounds (area);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0b0f0e));
        g.setColour (accent.withAlpha (0.8f));
        g.fillRect (getLocalBounds().removeFromLeft (3));
        g.setColour (juce::Colour (0xff33403c));
        g.drawRect (getLocalBounds());
    }

private:
    static int countLines (const juce::String& text)
    {
        auto count = 1;
        for (const auto character : text)
            if (character == '\n')
                ++count;

        return count;
    }

    class LineNumberGutter final : public juce::Component
    {
    public:
        explicit LineNumberGutter (CodeEditorPane& ownerToUse)
            : owner (ownerToUse)
        {
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff111715));
            g.setColour (juce::Colour (0xff2b3531));
            g.drawVerticalLine (getWidth() - 1, 0.0f, static_cast<float> (getHeight()));

            g.setColour (juce::Colour (0xff718079));
            g.setFont (juce::FontOptions (owner.codeFontSize * 0.82f));
            const auto lineHeight = juce::jmax (14, juce::roundToInt (owner.codeFontSize * 1.42f));
            const auto lines = countLines (owner.editor.getText());
            for (int line = 0; line < lines; ++line)
            {
                const auto y = 6 + line * lineHeight;
                if (y > getHeight())
                    break;

                g.drawText (juce::String (line + 1),
                            0,
                            y,
                            getWidth() - 7,
                            lineHeight,
                            juce::Justification::centredRight,
                            true);
            }
        }

    private:
        CodeEditorPane& owner;
    };

    void refreshStatus()
    {
        const auto text = editor.getText();
        const auto lines = countLines (text);
        statusLabel.setText (juce::String (lines) + (lines == 1 ? " line" : " lines"), juce::dontSendNotification);
    }

    void refreshSearch()
    {
        searchMatches.clear();
        const auto needle = search.getText();
        if (needle.isEmpty())
        {
            statusLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8fa19a));
            refreshStatus();
            return;
        }

        const auto haystack = editor.getText();
        auto offset = 0;
        while (offset < haystack.length())
        {
            const auto found = haystack.indexOfIgnoreCase (offset, needle);
            if (found < 0)
                break;

            searchMatches.push_back (found);
            offset = found + juce::jmax (1, needle.length());
        }

        searchMatchIndex = searchMatches.empty() ? 0 : juce::jlimit (0, static_cast<int> (searchMatches.size()) - 1, searchMatchIndex);
        statusLabel.setColour (juce::Label::textColourId, searchMatches.empty() ? juce::Colour (0xffd58b73) : juce::Colour (0xfffff0a8));
        statusLabel.setText (searchMatches.empty()
                                ? "No matches"
                                : juce::String (searchMatchIndex + 1) + "/" + juce::String (searchMatches.size()),
                             juce::dontSendNotification);
    }

    void moveSearchMatch (int delta)
    {
        refreshSearch();
        if (searchMatches.empty())
            return;

        searchMatchIndex = (searchMatchIndex + delta + static_cast<int> (searchMatches.size())) % static_cast<int> (searchMatches.size());
        const auto start = searchMatches[static_cast<size_t> (searchMatchIndex)];
        editor.setHighlightedRegion ({ start, start + search.getText().length() });
        editor.grabKeyboardFocus();
        refreshSearch();
    }

    juce::Label languageLabel;
    juce::TextEditor search;
    juce::TextButton previousMatch;
    juce::TextButton nextMatch;
    juce::Label statusLabel;
    LineNumberGutter gutter { *this };
    juce::TextEditor editor;
    juce::Colour accent = juce::Colour (0xff69b1c9);
    float codeFontSize = 13.0f;
    std::vector<int> searchMatches;
    int searchMatchIndex = 0;
};

juce::String demoChucKMotifCode()
{
    return "SinOsc root => Gain rootGain => Gain mix => dac;\n"
           "SinOsc fifth => Gain fifthGain => mix;\n"
           "0.62 => rootGain.gain;\n"
           "0.38 => fifthGain.gain;\n"
           "while (true) {\n"
           "    Math.floor(hostStateBeat) $ int => int beat;\n"
           "    0 => int semitone;\n"
           "    beat % 8 => int step;\n"
           "    if (step == 1) 3 => semitone;\n"
           "    if (step == 2) 7 => semitone;\n"
           "    if (step == 3) 10 => semitone;\n"
           "    if (step == 4) 12 => semitone;\n"
           "    if (step == 5) 7 => semitone;\n"
           "    if (step == 6) 3 => semitone;\n"
           "    if (step == 7) 5 => semitone;\n"
           "    110.0 * Math.pow(2.0, semitone / 12.0) => float base;\n"
           "    base * 2.0 => root.freq;\n"
           "    base * 3.0 => fifth.freq;\n"
           "    0.72 + (0.18 * Math.sin(hostBarPhase * 6.2831853)) => float motion;\n"
           "    Math.max(0.0, Math.min(hostGain, 0.052)) * hostStateGain * hostTrackGain * motion => mix.gain;\n"
           "    1::samp => now;\n"
           "}\n";
}

juce::String demoChucKCodaCode()
{
    return "SinOsc answer => Gain answerGain => Gain mix => dac;\n"
           "SinOsc glow => Gain glowGain => mix;\n"
           "0.7 => answerGain.gain;\n"
           "0.3 => glowGain.gain;\n"
           "while (true) {\n"
           "    Math.floor(hostStateBeat * 0.5) $ int => int phrase;\n"
           "    12 => int semitone;\n"
           "    phrase % 4 => int step;\n"
           "    if (step == 1) 10 => semitone;\n"
           "    if (step == 2) 7 => semitone;\n"
           "    if (step == 3) 3 => semitone;\n"
           "    110.0 * Math.pow(2.0, semitone / 12.0) => float base;\n"
           "    base * 2.0 => answer.freq;\n"
           "    base * 4.0 => glow.freq;\n"
           "    Math.max(0.0, Math.min(hostGain, 0.048)) * hostStateGain * hostTrackGain => mix.gain;\n"
           "    1::samp => now;\n"
           "}\n";
}

juce::String demoChucKLayerCode (double ratio, double gainCap, double motionDepth, int offsetBeats)
{
    juce::String code;
    code << "SinOsc a => Gain ag => Gain mix => dac;\n"
         << "SinOsc b => Gain bg => mix;\n"
         << "0.58 => ag.gain;\n"
         << "0.42 => bg.gain;\n"
         << "while (true) {\n"
         << "    Math.floor(hostStateBeat + " << offsetBeats << ") $ int => int beat;\n"
         << "    0 => int semitone;\n"
         << "    beat % 8 => int step;\n"
         << "    if (step == 1) 5 => semitone;\n"
         << "    if (step == 2) 7 => semitone;\n"
         << "    if (step == 3) 10 => semitone;\n"
         << "    if (step == 4) 12 => semitone;\n"
         << "    if (step == 5) 15 => semitone;\n"
         << "    if (step == 6) 10 => semitone;\n"
         << "    if (step == 7) 7 => semitone;\n"
         << "    110.0 * Math.pow(2.0, semitone / 12.0) => float base;\n"
         << "    base * " << juce::String (ratio, 4) << " => a.freq;\n"
         << "    base * " << juce::String (ratio * 1.5, 4) << " => b.freq;\n"
         << "    0.32 => float gate;\n"
         << "    if ((beat + " << offsetBeats << ") % 4 == 0) 1.45 => gate;\n"
         << "    0.74 + (" << juce::String (motionDepth, 4) << " * Math.sin((hostBarPhase + " << juce::String (offsetBeats * 0.125, 4) << ") * 6.2831853)) => float motion;\n"
         << "    Math.max(0.0, Math.min(hostGain, " << juce::String (gainCap, 4) << ")) * hostStateGain * hostTrackGain * motion * gate => mix.gain;\n"
         << "    1::samp => now;\n"
         << "}\n";
    return code;
}

juce::String demoSuperColliderHarmonicCode()
{
    return "{ |freq = 330, gain = 0.04, blend = 0.5, stateGate = 1, stateGain = 1, tempoBpm = 120,\n"
           "   stateBeat = 0, globalBeat = 0, timeSigNumerator = 4, timeSigDenominator = 4,\n"
           "   barBeat = 0, barPhase = 0, phaseRotation = 0, trackGain = 1, trackTempoBpm = 120,\n"
           "   trackTimeSigNumerator = 4, trackTimeSigDenominator = 4, trackPhaseRotation = 0|\n"
           "    var base = freq.max(40);\n"
           "    var amp = gain.min(0.032) * stateGain * trackGain;\n"
           "    var breathe = SinOsc.kr(0.07, 0, 0.18, 0.82);\n"
           "    SinOsc.ar([base * 1.25, base * 1.5], 0, amp * breathe)\n"
           "        + SinOsc.ar([base * 2.0, base * 2.01], 0, amp * 0.24)\n"
           "}\n";
}

juce::String demoRTcmixBassCode()
{
    return "bus_config(\"WAVETABLE\", \"out 0\")\n"
           "freq = makeconnection(\"inlet\", 1, 220)\n"
           "gain = makeconnection(\"inlet\", 2, 0.02)\n"
           "stategain = makeconnection(\"inlet\", 5, 1.0)\n"
           "trackgain = makeconnection(\"inlet\", 14, 1.0)\n"
           "bassfreq = freq * 0.5\n"
           "fifth = bassfreq * 1.5\n"
           "wave = maketable(\"wave\", 4096, \"sine\")\n"
           "WAVETABLE(0, 3600, gain * 0.82 * stategain * trackgain * 32767.0, bassfreq, 0.43, wave)\n"
           "WAVETABLE(0, 3600, gain * 0.30 * stategain * trackgain * 32767.0, fifth, 0.62, wave)\n";
}

juce::String demoCsoundBellCode()
{
    return "giWeldSine ftgen 1, 0, 4096, 10, 1\n"
           "\n"
           "instr 1\n"
           "    kfreq chnget \"hostFreq\"\n"
           "    kgain chnget \"hostGain\"\n"
           "    kstate chnget \"hostStateGain\"\n"
           "    ktrack chnget \"hostTrackGain\"\n"
           "    kblend chnget \"hostBlend\"\n"
           "    a1 oscili kgain * 0.20 * kstate * ktrack, kfreq * 2.25, giWeldSine\n"
           "    a2 oscili kgain * 0.12 * kstate * ktrack, kfreq * (2.5 + (kblend * 0.04)), giWeldSine\n"
           "    outs a1, a2\n"
           "endin\n";
}

juce::String defaultChucKScoreScript()
{
    return "score.clear();\n"
           "tempo(120);\n"
           "meter(4, 4);\n"
           "state.add(\"Motif\", 32, 0);\n"
           "track.add(1, \"ChucK pulse canon\", \"chuck\");\n"
           "track.gain(1, 1, 0.46);\n"
           "track.add(1, \"Upper response\", \"chuck\");\n"
           "track.gain(1, 2, 0.42);\n"
           "track.add(1, \"Csound bright edge\", \"csound\");\n"
           "track.gain(1, 3, 0.44);\n"
           "track.add(1, \"Inner fifth\", \"chuck\");\n"
           "track.gain(1, 4, 0.40);\n"
           "state.add(\"Glass branch\", 24, 0);\n"
           "track.add(2, \"SC glass harmonics\", \"supercollider\");\n"
           "track.gain(2, 1, 0.46);\n"
           "track.add(2, \"ChucK halo\", \"chuck\");\n"
           "track.gain(2, 2, 0.40);\n"
           "track.add(2, \"ChucK slow answer\", \"chuck\");\n"
           "track.gain(2, 3, 0.38);\n"
           "track.add(2, \"ChucK high pin\", \"chuck\");\n"
           "track.gain(2, 4, 0.34);\n"
           "state.add(\"Low branch\", 40, 0);\n"
           "track.add(3, \"RTcmix low pedal\", \"rtcmix\");\n"
           "track.gain(3, 1, 0.44);\n"
           "track.add(3, \"ChucK pedal octave\", \"chuck\");\n"
           "track.gain(3, 2, 0.40);\n"
           "track.add(3, \"ChucK pulse shadow\", \"chuck\");\n"
           "track.gain(3, 3, 0.38);\n"
           "track.add(3, \"ChucK low shimmer\", \"chuck\");\n"
           "track.gain(3, 4, 0.36);\n"
           "state.add(\"Coda\", 20, 0);\n"
           "track.add(4, \"ChucK answer\", \"chuck\");\n"
           "track.gain(4, 1, 0.42);\n"
           "track.add(4, \"ChucK coda glow\", \"chuck\");\n"
           "track.gain(4, 2, 0.40);\n"
           "track.add(4, \"Csound closing bell\", \"csound\");\n"
           "track.gain(4, 3, 0.38);\n"
           "track.add(4, \"ChucK floor\", \"chuck\");\n"
           "track.gain(4, 4, 0.34);\n"
           "state.connect(1, 2, 70);\n"
           "state.connect(1, 3, 30);\n"
           "state.connect(2, 3, 50);\n"
           "state.connect(2, 4, 50);\n"
           "state.connect(3, 4, 1.0);\n"
           "play();\n";
}

juce::String quoteScoreString (juce::String text)
{
    text = text.replace ("\\", "\\\\");
    text = text.replace ("\"", "\\\"");
    text = text.replace ("\r", "\\r");
    text = text.replace ("\n", "\\n");
    text = text.replace ("\t", "\\t");
    return "\"" + text + "\"";
}

juce::String encodeScoreCode (const juce::String& code)
{
    return "base64:" + juce::Base64::toBase64 (code);
}

juce::String languageToken (Language language)
{
    switch (language)
    {
        case Language::supercollider: return "supercollider";
        case Language::rtcmix: return "rtcmix";
        case Language::csound: return "csound";
        case Language::faust: return "faust";
        case Language::chuck: break;
    }

    return "chuck";
}

juce::String generateChucKScoreScript (const ProjectModel& project)
{
    juce::String script;
    script << "score.clear();\n";
    script << "tempo(" << juce::String (project.globalTempoBpm, 3) << ");\n";
    script << "meter(" << project.globalTimeSigNumerator << ", " << project.globalTimeSigDenominator << ");\n";
    script << "phase(" << juce::String (project.globalPhaseBeats, 3) << ");\n\n";

    for (size_t stateIndex = 0; stateIndex < project.states.size(); ++stateIndex)
    {
        const auto& state = project.states[stateIndex];
        script << "state.add(" << quoteScoreString (state.name)
               << ", " << juce::String (state.durationBeats, 3)
               << ", " << juce::String (state.tailBeats, 3) << ");\n";

        const auto stateNumber = static_cast<int> (stateIndex) + 1;
        for (size_t trackIndex = 0; trackIndex < state.tracks.size(); ++trackIndex)
        {
            const auto& track = state.tracks[trackIndex];
            const auto trackNumber = static_cast<int> (trackIndex) + 1;
            script << "track.add(" << stateNumber
                   << ", " << quoteScoreString (track.name)
                   << ", " << quoteScoreString (languageToken (track.language)) << ");\n";
            script << "track.gain(" << stateNumber << ", " << trackNumber << ", " << juce::String (track.level, 3) << ");\n";
            script << "track.sync(" << stateNumber << ", " << trackNumber << ", " << (track.tightlySynced ? 1 : 0) << ");\n";
            script << "track.mute(" << stateNumber << ", " << trackNumber << ", " << (track.muted ? 1 : 0) << ");\n";
            script << "track.solo(" << stateNumber << ", " << trackNumber << ", " << (track.soloed ? 1 : 0) << ");\n";

            if (! track.tightlySynced)
            {
                script << "track.tempo(" << stateNumber << ", " << trackNumber << ", " << juce::String (track.tempoBpm, 3) << ");\n";
                script << "track.meter(" << stateNumber << ", " << trackNumber << ", "
                       << track.timeSigNumerator << ", " << track.timeSigDenominator << ");\n";
                script << "track.phase(" << stateNumber << ", " << trackNumber << ", " << juce::String (track.phaseBeats, 3) << ");\n";
            }

            if (track.code.isNotEmpty())
                script << "track.code(" << stateNumber << ", " << trackNumber << ", " << quoteScoreString (encodeScoreCode (track.code)) << ");\n";
            else
                script << "track.clear(" << stateNumber << ", " << trackNumber << ");\n";
        }

        script << "\n";
    }

    for (size_t stateIndex = 0; stateIndex < project.states.size(); ++stateIndex)
    {
        const auto stateNumber = static_cast<int> (stateIndex) + 1;
        for (const auto& transition : project.states[stateIndex].transitions)
            if (transition.targetStateIndex >= 0
                && transition.targetStateIndex < static_cast<int> (project.states.size())
                && transition.weight > 0.0
                && std::isfinite (transition.weight))
            {
                script << "state.connect(" << stateNumber
                       << ", " << (transition.targetStateIndex + 1)
                       << ", " << juce::String (transition.weight, 3) << ");\n";
            }
    }

    script << "play();\n";
    return script;
}

juce::String defaultCodeForLanguage (Language language)
{
    switch (language)
    {
        case Language::supercollider:
            return demoSuperColliderHarmonicCode();

        case Language::rtcmix:
            return demoRTcmixBassCode();

        case Language::csound:
            return demoCsoundBellCode();

        case Language::faust:
            return "import(\"stdfaust.lib\");\n"
                   "freq = hslider(\"hostFreq\", 220, 40, 4000, 0.01);\n"
                   "gain = hslider(\"hostGain\", 0.04, 0, 0.1, 0.001);\n"
                   "process = os.osc(freq) * gain <: _, _;\n";

        case Language::chuck:
            break;
    }

    return demoChucKMotifCode();
}

juce::String chuckFallbackCodeForLanguage (Language originalLanguage)
{
    switch (originalLanguage)
    {
        case Language::supercollider:
            return "SinOsc a => Gain ag => Gain g => dac;\n"
                   "SinOsc b => Gain bg => g;\n"
                   "0.62 => ag.gain;\n"
                   "0.38 => bg.gain;\n"
                   "while (true) {\n"
                   "    Math.max(40.0, hostFreq * 1.5) => float base;\n"
                   "    base => a.freq;\n"
                   "    base * 1.5 => b.freq;\n"
                   "    Math.max(0.0, Math.min(hostGain, 0.07)) * hostStateGain * hostTrackGain => g.gain;\n"
                   "    1::samp => now;\n"
                   "}\n";

        case Language::rtcmix:
            return "SinOsc s => Gain g => dac;\n"
                   "while (true) {\n"
                   "    Math.max(40.0, hostFreq * 0.5) => s.freq;\n"
                   "    Math.max(0.0, Math.min(hostGain, 0.075)) * hostStateGain * hostTrackGain => g.gain;\n"
                   "    1::samp => now;\n"
                   "}\n";

        case Language::csound:
            return "SinOsc s => Gain g => dac;\n"
                   "while (true) {\n"
                   "    Math.max(40.0, hostFreq * 1.25) => s.freq;\n"
                   "    Math.max(0.0, Math.min(hostGain, 0.07)) * hostStateGain * hostTrackGain => g.gain;\n"
                   "    1::samp => now;\n"
                   "}\n";

        case Language::faust:
            return "SinOsc s => Gain g => dac;\n"
                   "while (true) {\n"
                   "    Math.max(40.0, hostFreq * 2.5) => s.freq;\n"
                   "    Math.max(0.0, Math.min(hostGain, 0.055)) * hostStateGain * hostTrackGain => g.gain;\n"
                   "    1::samp => now;\n"
                   "}\n";

        case Language::chuck:
            break;
    }

    return defaultCodeForLanguage (Language::chuck);
}

TrackModel makeTrack (juce::String name, Language language)
{
    TrackModel track;
    track.name = std::move (name);
    track.language = language;

    if (language == Language::chuck)
    {
        if (track.name.containsIgnoreCase ("answer"))
            track.code = demoChucKCodaCode();
        else if (track.name.containsIgnoreCase ("upper"))
            track.code = demoChucKLayerCode (3.0, 0.046, 0.16, 1);
        else if (track.name.containsIgnoreCase ("low clock") || track.name.containsIgnoreCase ("floor") || track.name.containsIgnoreCase ("pedal"))
            track.code = demoChucKLayerCode (0.5, 0.050, 0.09, 4);
        else if (track.name.containsIgnoreCase ("fifth"))
            track.code = demoChucKLayerCode (1.5, 0.044, 0.12, 2);
        else if (track.name.containsIgnoreCase ("halo") || track.name.containsIgnoreCase ("glow"))
            track.code = demoChucKLayerCode (2.0, 0.044, 0.18, 0);
        else if (track.name.containsIgnoreCase ("high"))
            track.code = demoChucKLayerCode (4.0, 0.036, 0.08, 5);
        else if (track.name.containsIgnoreCase ("shadow") || track.name.containsIgnoreCase ("shimmer") || track.name.containsIgnoreCase ("slow"))
            track.code = demoChucKLayerCode (1.0, 0.042, 0.12, 3);
        else
            track.code = demoChucKMotifCode();
    }
    else
    {
        track.code = defaultCodeForLanguage (language);
    }

    return track;
}

TrackModel makeDemoTrack (juce::String name, Language language, float level, juce::String code)
{
    auto track = makeTrack (std::move (name), language);
    track.level = level;
    track.code = std::move (code);
    return track;
}

StateModel makeState (int index)
{
    StateModel state;
    state.name = "State " + juce::String (index);
    state.durationBeats = 16.0;
    state.tailBeats = 4.0;
    state.canvasX = 70 + ((index - 1) % 5) * 220;
    state.canvasY = 40 + ((index - 1) / 5) * 88;
    state.tracks.push_back (makeTrack ("Track 1", Language::chuck));
    return state;
}

ProjectModel makeInitialProject()
{
    ProjectModel project;
    project.chuckScoreScript = defaultChucKScoreScript();
    project.states.reserve (4);

    StateModel chuck;
    chuck.name = "Motif";
    chuck.durationBeats = 32.0;
    chuck.tailBeats = 0.0;
    chuck.canvasX = 70;
    chuck.canvasY = 160;
    chuck.tracks.push_back (makeDemoTrack ("ChucK pulse canon", Language::chuck, 0.46f, demoChucKMotifCode()));
    chuck.tracks.push_back (makeDemoTrack ("Upper response", Language::chuck, 0.42f, demoChucKLayerCode (3.0, 0.046, 0.16, 1)));
    chuck.tracks.push_back (makeDemoTrack ("Csound bright edge", Language::csound, 0.44f, demoCsoundBellCode()));
    chuck.tracks.push_back (makeDemoTrack ("Inner fifth", Language::chuck, 0.40f, demoChucKLayerCode (1.5, 0.044, 0.12, 2)));

    StateModel sc;
    sc.name = "Glass branch";
    sc.durationBeats = 24.0;
    sc.tailBeats = 0.0;
    sc.canvasX = 330;
    sc.canvasY = 60;
    sc.tracks.push_back (makeDemoTrack ("SC glass harmonics", Language::supercollider, 0.46f, demoSuperColliderHarmonicCode()));
    sc.tracks.push_back (makeDemoTrack ("ChucK halo", Language::chuck, 0.40f, demoChucKLayerCode (2.0, 0.044, 0.18, 0)));
    sc.tracks.push_back (makeDemoTrack ("ChucK slow answer", Language::chuck, 0.38f, demoChucKLayerCode (1.0, 0.046, 0.10, 3)));
    sc.tracks.push_back (makeDemoTrack ("ChucK high pin", Language::chuck, 0.34f, demoChucKLayerCode (4.0, 0.036, 0.08, 5)));

    StateModel rtcmix;
    rtcmix.name = "Low branch";
    rtcmix.durationBeats = 40.0;
    rtcmix.tailBeats = 0.0;
    rtcmix.canvasX = 330;
    rtcmix.canvasY = 230;
    rtcmix.tracks.push_back (makeDemoTrack ("RTcmix low pedal", Language::rtcmix, 0.44f, demoRTcmixBassCode()));
    rtcmix.tracks.push_back (makeDemoTrack ("ChucK pedal octave", Language::chuck, 0.40f, demoChucKLayerCode (0.5, 0.050, 0.09, 0)));
    rtcmix.tracks.push_back (makeDemoTrack ("ChucK pulse shadow", Language::chuck, 0.38f, demoChucKLayerCode (1.0, 0.042, 0.12, 2)));
    rtcmix.tracks.push_back (makeDemoTrack ("ChucK low shimmer", Language::chuck, 0.36f, demoChucKLayerCode (1.5, 0.040, 0.14, 6)));

    StateModel coda;
    coda.name = "Coda";
    coda.durationBeats = 20.0;
    coda.tailBeats = 0.0;
    coda.canvasX = 590;
    coda.canvasY = 160;
    coda.tracks.push_back (makeDemoTrack ("ChucK answer", Language::chuck, 0.42f, demoChucKCodaCode()));
    coda.tracks.push_back (makeDemoTrack ("ChucK coda glow", Language::chuck, 0.40f, demoChucKLayerCode (2.0, 0.042, 0.12, 1)));
    coda.tracks.push_back (makeDemoTrack ("Csound closing bell", Language::csound, 0.38f, demoCsoundBellCode()));
    coda.tracks.push_back (makeDemoTrack ("ChucK floor", Language::chuck, 0.34f, demoChucKLayerCode (0.5, 0.044, 0.08, 5)));

    project.states.push_back (std::move (chuck));
    project.states.push_back (std::move (sc));
    project.states.push_back (std::move (rtcmix));
    project.states.push_back (std::move (coda));
    project.states[0].transitions.push_back ({ 1, 70.0 });
    project.states[0].transitions.push_back ({ 2, 30.0 });
    project.states[1].transitions.push_back ({ 2, 50.0 });
    project.states[1].transitions.push_back ({ 3, 50.0 });
    project.states[2].transitions.push_back ({ 3, 1.0 });
    arrangeStatesAsFiniteStateMachine (project);
    project.arrangementOrder = defaultArrangementOrder (project);
    return project;
}

class SectionHeader final : public juce::Component
{
public:
    void setText (juce::String value)
    {
        text = std::move (value);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xffdce3e1));
        g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        g.drawFittedText (text, getLocalBounds(), juce::Justification::centredLeft, 1);
    }

private:
    juce::String text;
};

class ScoreMachineComponent final : public juce::Component
{
public:
    std::function<void()> onPlay;
    std::function<void()> onStop;
    std::function<void()> onAddState;
    std::function<void()> onRemoveState;
    std::function<void()> onRunChucKScore;
    std::function<void()> onSyncChucKScore;
    std::function<void()> onResetChucKScore;
    std::function<void (int)> onSelectState;

    explicit ScoreMachineComponent (ProjectModel& projectToUse)
        : project (projectToUse)
    {
        addAndMakeVisible (title);
        title.setText ("Score / State Machine");

        addAndMakeVisible (zoomOutButton);
        zoomOutButton.setButtonText ("-");
        zoomOutButton.onClick = [this]
        {
            userAdjustedGraphView = true;
            zoomGraphFromCentre (1.0f / 1.18f);
        };

        addAndMakeVisible (zoomInButton);
        zoomInButton.setButtonText ("+");
        zoomInButton.onClick = [this]
        {
            userAdjustedGraphView = true;
            zoomGraphFromCentre (1.18f);
        };

        addAndMakeVisible (fitGraphButton);
        fitGraphButton.setButtonText ("Fit");
        fitGraphButton.onClick = [this]
        {
            userAdjustedGraphView = true;
            fitGraphToView();
        };

        addAndMakeVisible (arrangeGraphButton);
        arrangeGraphButton.setButtonText ("Arrange");
        arrangeGraphButton.onClick = [this]
        {
            arrangeStatesAsFiniteStateMachine (project);
            userAdjustedGraphView = true;
            fitGraphToView();
            repaint();
        };

        addAndMakeVisible (playButton);
        playButton.setButtonText ("Play");
        playButton.onClick = [this]
        {
            if (onPlay != nullptr)
                onPlay();
        };

        addAndMakeVisible (stopButton);
        stopButton.setButtonText ("Stop");
        stopButton.onClick = [this]
        {
            if (onStop != nullptr)
                onStop();
        };

        addAndMakeVisible (addStateButton);
        addStateButton.setButtonText ("+ State");
        addStateButton.onClick = [this]
        {
            if (onAddState != nullptr)
                onAddState();
        };

        addAndMakeVisible (removeStateButton);
        removeStateButton.setButtonText ("- State");
        removeStateButton.onClick = [this]
        {
            if (onRemoveState != nullptr)
                onRemoveState();
        };

        addAndMakeVisible (tempo);
        tempo.setSliderStyle (juce::Slider::LinearHorizontal);
        tempo.setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 24);
        tempo.setRange (20.0, 300.0, 0.1);
        tempo.setValue (project.globalTempoBpm);
        tempo.onValueChange = [this]
        {
            project.globalTempoBpm = tempo.getValue();
            repaint();
        };

        addAndMakeVisible (meter);
        meter.addItem ("3/4", 1);
        meter.addItem ("4/4", 2);
        meter.addItem ("5/4", 3);
        meter.addItem ("7/8", 4);
        meter.setSelectedId (meterIdFor (project.globalTimeSigNumerator, project.globalTimeSigDenominator),
                             juce::dontSendNotification);
        meter.onChange = [this]
        {
            switch (meter.getSelectedId())
            {
                case 1: project.globalTimeSigNumerator = 3; project.globalTimeSigDenominator = 4; break;
                case 3: project.globalTimeSigNumerator = 5; project.globalTimeSigDenominator = 4; break;
                case 4: project.globalTimeSigNumerator = 7; project.globalTimeSigDenominator = 8; break;
                default: project.globalTimeSigNumerator = 4; project.globalTimeSigDenominator = 4; break;
            }

            repaint();
        };

        addAndMakeVisible (phase);
        phase.setSliderStyle (juce::Slider::LinearHorizontal);
        phase.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 24);
        phase.setRange (-8.0, 8.0, 0.01);
        phase.setValue (project.globalPhaseBeats);
        phase.onValueChange = [this]
        {
            project.globalPhaseBeats = phase.getValue();
            repaint();
        };

        addAndMakeVisible (runScoreButton);
        runScoreButton.setButtonText ("Run");
        runScoreButton.onClick = [this]
        {
            if (onRunChucKScore != nullptr)
                onRunChucKScore();
        };

        addAndMakeVisible (syncScoreButton);
        syncScoreButton.setButtonText ("Sync");
        syncScoreButton.onClick = [this]
        {
            if (onSyncChucKScore != nullptr)
                onSyncChucKScore();
        };

        addAndMakeVisible (resetScoreButton);
        resetScoreButton.setButtonText ("Template");
        resetScoreButton.onClick = [this]
        {
            if (onResetChucKScore != nullptr)
                onResetChucKScore();
        };

        addAndMakeVisible (clearScoreButton);
        clearScoreButton.setButtonText ("Clear");
        clearScoreButton.onClick = [this]
        {
            project.chuckScoreScript.clear();
            scoreScript.setText ({}, false);
        };

        addAndMakeVisible (scoreScript);
        configureCodeEditor (scoreScript);
        scoreScript.setText (project.chuckScoreScript, false);
        scoreScript.onTextChange = [this] { project.chuckScoreScript = scoreScript.getText(); };
    }

    void setTransport (bool isCountingInToUse, bool isPlayingToUse, double beatToUse)
    {
        isCountingIn = isCountingInToUse;
        isPlaying = isPlayingToUse;
        currentBeat = beatToUse;
        playButton.setToggleState (isCountingIn || isPlaying, juce::dontSendNotification);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff151817));
        auto area = getLocalBounds().reduced (18);
        area.removeFromTop (42);
        const auto editorWidth = scoreEditorWidthFor (area);
        area.removeFromRight (editorWidth + 12);

        drawPatchGrid (g, area, viewportZoom, viewportPan);

        auto timeline = area.removeFromBottom (34).reduced (14, 7);
        auto patchArea = area.reduced (14);
        patchArea.removeFromBottom (14);
        const auto totalBeats = juce::jmax (1.0, totalDurationBeats (project));

        const auto nodes = transformedStateNodeBounds (patchArea, currentNodeWidth(), currentNodeHeight());
        const auto activeState = activeStateForBeat (currentBeat);

        {
            juce::Graphics::ScopedSaveState graphState (g);
            g.reduceClipRegion (patchArea);

            auto drewExplicitConnection = false;
            for (size_t i = 0; i < project.states.size(); ++i)
            {
                const auto& state = project.states[i];
                auto totalWeight = 0.0;
                for (const auto& transition : state.transitions)
                    if (transition.targetStateIndex >= 0
                        && transition.targetStateIndex < static_cast<int> (nodes.size())
                        && transition.weight > 0.0
                        && std::isfinite (transition.weight))
                        totalWeight += transition.weight;

                for (const auto& transition : state.transitions)
                {
                    if (transition.targetStateIndex < 0
                        || transition.targetStateIndex >= static_cast<int> (nodes.size())
                        || transition.weight <= 0.0
                        || ! std::isfinite (transition.weight))
                        continue;

                    const auto probability = totalWeight > 0.0 ? transition.weight / totalWeight : transition.weight;
                    drawPatchCord (g,
                                   nodes[i].getRight(),
                                   nodes[i].getCentreY(),
                                   nodes[static_cast<size_t> (transition.targetStateIndex)].getX(),
                                   nodes[static_cast<size_t> (transition.targetStateIndex)].getCentreY(),
                                   probability);
                    drewExplicitConnection = true;
                }
            }

            if (! drewExplicitConnection)
                for (size_t i = 1; i < nodes.size(); ++i)
                    drawPatchCord (g, nodes[i - 1].getRight(), nodes[i - 1].getCentreY(), nodes[i].getX(), nodes[i].getCentreY(), 1.0);

            for (size_t i = 0; i < project.states.size(); ++i)
            {
                const auto index = static_cast<int> (i);
                drawStateNode (g,
                               project.states[i],
                               nodes[i],
                               index,
                               index == selectedStateIndex,
                               activeState.index == index,
                               activeState.progress);
            }
        }

        double stateStart = 0.0;
        const auto order = playableArrangementOrder (project);
        for (const auto stateIndex : order)
        {
            if (stateIndex < 0 || stateIndex >= static_cast<int> (project.states.size()))
                continue;

            const auto& state = project.states[static_cast<size_t> (stateIndex)];
            const auto stateWidth = juce::jmax (28, juce::roundToInt (timeline.getWidth() * state.durationBeats / totalBeats));
            const auto x = timeline.getX() + juce::roundToInt (timeline.getWidth() * stateStart / totalBeats);
            auto block = juce::Rectangle<int> (x, timeline.getY(), stateWidth - 2, timeline.getHeight());
            const auto base = languageColour (state.tracks.empty() ? Language::chuck : state.tracks.front().language);
            g.setColour (base.withAlpha (0.28f));
            g.fillRect (block);
            g.setColour (base.withAlpha (0.8f));
            g.drawRect (block);

            if ((isCountingIn || isPlaying) && currentBeat >= stateStart && currentBeat <= stateStart + state.durationBeats)
                drawProgressLine (g, timeline, currentBeat / totalBeats);

            stateStart += state.durationBeats;
        }

        if (isCountingIn)
        {
            g.setColour (juce::Colour (0xffffd36e));
            g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            g.drawText ("Count-in", area.reduced (14).removeFromBottom (26), juce::Justification::centredRight, true);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (18);
        auto top = area.removeFromTop (36);
        const auto editorWidth = scoreEditorWidthFor (area);
        auto editorTop = top.removeFromRight (editorWidth);
        top.removeFromRight (12);
        title.setBounds (top.removeFromLeft (230));
        zoomOutButton.setBounds (top.removeFromLeft (32).reduced (3, 4));
        zoomInButton.setBounds (top.removeFromLeft (32).reduced (3, 4));
        fitGraphButton.setBounds (top.removeFromLeft (54).reduced (4, 4));
        arrangeGraphButton.setBounds (top.removeFromLeft (86).reduced (4, 4));
        playButton.setBounds (top.removeFromRight (68));
        stopButton.setBounds (top.removeFromRight (68).reduced (6, 0));
        removeStateButton.setBounds (top.removeFromRight (82).reduced (6, 0));
        addStateButton.setBounds (top.removeFromRight (82).reduced (6, 0));
        phase.setBounds (top.removeFromRight (138).reduced (6, 0));
        meter.setBounds (top.removeFromRight (76).reduced (6, 0));
        tempo.setBounds (top.removeFromRight (164).reduced (6, 0));

        const auto scoreButtonWidth = editorTop.getWidth() / 4;
        runScoreButton.setBounds (editorTop.removeFromLeft (scoreButtonWidth).reduced (0, 0));
        syncScoreButton.setBounds (editorTop.removeFromLeft (scoreButtonWidth).reduced (6, 0));
        resetScoreButton.setBounds (editorTop.removeFromLeft (scoreButtonWidth).reduced (6, 0));
        clearScoreButton.setBounds (editorTop.reduced (6, 0));

        area.removeFromTop (6);
        auto editorArea = area.removeFromRight (editorWidth);
        scoreScript.setBounds (editorArea);

        const auto patchArea = getPatchArea();
        if (! userAdjustedGraphView && ! patchArea.isEmpty() && patchArea != lastAutoFitPatchArea)
        {
            lastAutoFitPatchArea = patchArea;
            fitGraphToView();
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        const auto graphArea = getGraphArea();
        if (! graphArea.contains (event.getPosition()))
            return;

        if (event.mods.isRightButtonDown())
        {
            userAdjustedGraphView = true;
            isPanningGraph = true;
            panStart = viewportPan;
            return;
        }

        const auto nodes = transformedStateNodeBounds (getPatchArea(), currentNodeWidth(), currentNodeHeight());
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            if (nodes[i].contains (event.getPosition()))
            {
                if (onSelectState != nullptr)
                    onSelectState (static_cast<int> (i));

                draggedStateIndex = static_cast<int> (i);
                dragStartStateCanvas = { static_cast<float> (project.states[i].canvasX),
                                         static_cast<float> (project.states[i].canvasY) };
                return;
            }
        }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (isPanningGraph && event.mods.isRightButtonDown())
        {
            viewportPan = panStart + event.getOffsetFromDragStart().toFloat();
            repaint();
            return;
        }

        if (draggedStateIndex >= 0 && draggedStateIndex < static_cast<int> (project.states.size()))
        {
            const auto delta = event.getOffsetFromDragStart().toFloat() / viewportZoom;
            auto& state = project.states[static_cast<size_t> (draggedStateIndex)];
            state.canvasX = juce::roundToInt (dragStartStateCanvas.x + delta.x);
            state.canvasY = juce::roundToInt (dragStartStateCanvas.y + delta.y);
            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        isPanningGraph = false;
        draggedStateIndex = -1;
    }

    void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        const auto graphArea = getGraphArea();
        if (! graphArea.contains (event.getPosition()) || std::abs (wheel.deltaY) < 0.0001f)
            return;

        const auto oldZoom = viewportZoom;
        const auto zoomStep = wheel.deltaY > 0.0f ? 1.12f : 1.0f / 1.12f;
        userAdjustedGraphView = true;
        viewportZoom = juce::jlimit (0.45f, 3.0f, viewportZoom * zoomStep);

        const auto patchArea = getPatchArea();
        const auto origin = patchArea.getPosition().toFloat();
        const auto mouse = event.position;
        const auto graphPoint = (mouse - origin - viewportPan) / oldZoom;
        viewportPan = mouse - origin - graphPoint * viewportZoom;
        repaint();
    }

    void setSelectedStateIndex (int index)
    {
        selectedStateIndex = index;
        repaint();
    }

    void setScoreScriptText (const juce::String& text)
    {
        scoreScript.setText (text, false);
    }

    void syncControlsFromProject()
    {
        tempo.setValue (project.globalTempoBpm, juce::dontSendNotification);
        meter.setSelectedId (meterIdFor (project.globalTimeSigNumerator, project.globalTimeSigDenominator),
                             juce::dontSendNotification);
        phase.setValue (project.globalPhaseBeats, juce::dontSendNotification);
    }

private:
    static int scoreEditorWidthFor (juce::Rectangle<int> area)
    {
        return juce::jlimit (280, 430, area.getWidth() / 3);
    }

    struct ActiveState
    {
        int index = -1;
        double progress = 0.0;
    };

    juce::Rectangle<int> getGraphArea() const
    {
        auto area = getLocalBounds().reduced (18);
        area.removeFromTop (42);
        const auto editorWidth = scoreEditorWidthFor (area);
        area.removeFromRight (editorWidth + 12);
        return area;
    }

    juce::Rectangle<int> getPatchArea() const
    {
        auto area = getGraphArea();
        area.removeFromBottom (34);
        auto patchArea = area.reduced (14);
        patchArea.removeFromBottom (14);
        return patchArea;
    }

    int currentNodeWidth() const
    {
        return 188;
    }

    int currentNodeHeight() const
    {
        return 92;
    }

    std::vector<juce::Rectangle<int>> transformedStateNodeBounds (juce::Rectangle<int> patchArea,
                                                                  int nodeWidth,
                                                                  int nodeHeight) const
    {
        std::vector<juce::Rectangle<int>> nodes;
        nodes.reserve (project.states.size());

        for (const auto& state : project.states)
        {
            const auto x = static_cast<float> (patchArea.getX()) + viewportPan.x + static_cast<float> (state.canvasX) * viewportZoom;
            const auto y = static_cast<float> (patchArea.getY()) + viewportPan.y + static_cast<float> (state.canvasY) * viewportZoom;
            nodes.push_back ({ juce::roundToInt (x),
                               juce::roundToInt (y),
                               juce::roundToInt (static_cast<float> (nodeWidth) * viewportZoom),
                               juce::roundToInt (static_cast<float> (nodeHeight) * viewportZoom) });
        }

        return nodes;
    }

    juce::Rectangle<float> graphContentBounds() const
    {
        if (project.states.empty())
            return {};

        auto bounds = juce::Rectangle<float> (static_cast<float> (project.states.front().canvasX),
                                             static_cast<float> (project.states.front().canvasY),
                                             static_cast<float> (currentNodeWidth()),
                                             static_cast<float> (currentNodeHeight()));

        for (const auto& state : project.states)
        {
            bounds = bounds.getUnion ({ static_cast<float> (state.canvasX),
                                        static_cast<float> (state.canvasY),
                                        static_cast<float> (currentNodeWidth()),
                                        static_cast<float> (currentNodeHeight()) });
        }

        return bounds;
    }

    void fitGraphToView()
    {
        const auto patchArea = getPatchArea().toFloat();
        const auto content = graphContentBounds();
        if (patchArea.isEmpty() || content.isEmpty())
            return;

        constexpr auto initialZoomMultiplier = 1.2f;
        const auto paddedArea = patchArea.reduced (18.0f);
        const auto scaleX = paddedArea.getWidth() / juce::jmax (1.0f, content.getWidth());
        const auto scaleY = paddedArea.getHeight() / juce::jmax (1.0f, content.getHeight());
        viewportZoom = juce::jlimit (0.45f, 3.0f, juce::jmin (scaleX, scaleY) * initialZoomMultiplier);
        viewportPan = { (patchArea.getWidth() - content.getWidth() * viewportZoom) * 0.5f - content.getX() * viewportZoom,
                        (patchArea.getHeight() - content.getHeight() * viewportZoom) * 0.5f - content.getY() * viewportZoom };
        repaint();
    }

    void zoomGraphFromCentre (float factor)
    {
        const auto patchArea = getPatchArea();
        if (patchArea.isEmpty())
            return;

        const auto oldZoom = viewportZoom;
        viewportZoom = juce::jlimit (0.45f, 3.0f, viewportZoom * factor);
        const auto origin = patchArea.getPosition().toFloat();
        const auto centre = patchArea.getCentre().toFloat();
        const auto graphPoint = (centre - origin - viewportPan) / oldZoom;
        viewportPan = centre - origin - graphPoint * viewportZoom;
        repaint();
    }

    ActiveState activeStateForBeat (double beat) const
    {
        if (! (isCountingIn || isPlaying))
            return {};

        auto stateStart = 0.0;
        const auto order = playableArrangementOrder (project);
        for (size_t i = 0; i < order.size(); ++i)
        {
            const auto stateIndex = order[i];
            if (stateIndex < 0 || stateIndex >= static_cast<int> (project.states.size()))
                continue;

            const auto duration = juce::jmax (0.0001, project.states[static_cast<size_t> (stateIndex)].durationBeats);
            const auto isLast = i + 1 == order.size();
            if (beat >= stateStart && (beat < stateStart + duration || isLast))
                return { stateIndex, juce::jlimit (0.0, 1.0, (beat - stateStart) / duration) };

            stateStart += duration;
        }

        return {};
    }

    static void drawPatchGrid (juce::Graphics& g,
                               juce::Rectangle<int> area,
                               float zoom,
                               juce::Point<float> pan)
    {
        g.setColour (juce::Colour (0xff202624));
        g.fillRect (area);

        g.setColour (juce::Colour (0xff2a312f));
        const auto spacing = juce::jlimit (8.0f, 72.0f, 24.0f * zoom);
        auto firstX = static_cast<float> (area.getX()) + std::fmod (pan.x, spacing);
        if (firstX > static_cast<float> (area.getX()))
            firstX -= spacing;

        auto firstY = static_cast<float> (area.getY()) + std::fmod (pan.y, spacing);
        if (firstY > static_cast<float> (area.getY()))
            firstY -= spacing;

        for (auto x = firstX; x < static_cast<float> (area.getRight()); x += spacing)
            g.drawLine (x,
                        static_cast<float> (area.getY()),
                        x,
                        static_cast<float> (area.getBottom()),
                        1.0f);

        for (auto y = firstY; y < static_cast<float> (area.getBottom()); y += spacing)
            g.drawLine (static_cast<float> (area.getX()),
                        y,
                        static_cast<float> (area.getRight()),
                        y,
                        1.0f);

        g.setColour (juce::Colour (0xff3a4642));
        g.drawRect (area);
    }

    static void drawPatchCord (juce::Graphics& g, int x1, int y1, int x2, int y2, double weight)
    {
        juce::Path cord;
        cord.startNewSubPath (static_cast<float> (x1), static_cast<float> (y1));
        const auto tension = juce::jmax (32.0f, std::abs (static_cast<float> (x2 - x1)) * 0.45f);
        cord.cubicTo (static_cast<float> (x1) + tension,
                      static_cast<float> (y1),
                      static_cast<float> (x2) - tension,
                      static_cast<float> (y2),
                      static_cast<float> (x2),
                      static_cast<float> (y2));

        g.setColour (juce::Colour (0xffcfd8d3).withAlpha (0.72f));
        g.strokePath (cord, juce::PathStrokeType (2.0f));

        if (std::abs (weight - 1.0) > 0.001)
        {
            const auto label = juce::String (juce::roundToInt (weight * 100.0)) + "%";
            const auto labelX = (x1 + x2) / 2 - 18;
            const auto labelY = (y1 + y2) / 2 - 22;
            auto labelBounds = juce::Rectangle<int> (labelX, labelY, 36, 18);
            g.setColour (juce::Colour (0xff101211).withAlpha (0.92f));
            g.fillRect (labelBounds);
            g.setColour (juce::Colour (0xfffff0a8));
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (label, labelBounds, juce::Justification::centred, true);
        }
    }

    static void drawStateNode (juce::Graphics& g,
                               const StateModel& state,
                               juce::Rectangle<int> node,
                               int index,
                               bool selected,
                               bool active,
                               double progress)
    {
        const auto language = state.tracks.empty() ? Language::chuck : state.tracks.front().language;
        const auto base = languageColour (language);

        g.setColour (juce::Colour (0xff121615));
        g.fillRect (node);
        if (active)
        {
            g.setColour (base.withAlpha (0.18f));
            g.fillRect (node.reduced (3));
            g.setColour (juce::Colour (0xfffff0a8).withAlpha (0.35f));
            g.drawRect (node.expanded (7), 2);
        }

        g.setColour (base.withAlpha (0.92f));
        g.drawRect (node, 2);
        if (selected)
        {
            g.setColour (juce::Colour (0xfffff0a8));
            g.drawRect (node.expanded (3), 2);
        }

        if (active)
        {
            auto progressBar = node.reduced (6).removeFromBottom (4);
            progressBar.setWidth (juce::roundToInt (progressBar.getWidth() * juce::jlimit (0.0, 1.0, progress)));
            g.setColour (juce::Colour (0xfffff0a8));
            g.fillRect (progressBar);
        }

        const auto inlet = juce::Rectangle<int> (node.getX() - 4, node.getCentreY() - 4, 8, 8);
        const auto outlet = juce::Rectangle<int> (node.getRight() - 4, node.getCentreY() - 4, 8, 8);
        g.fillRect (inlet);
        g.fillRect (outlet);

        auto text = node.reduced (10, 7);
        g.setColour (juce::Colour (0xffeef4f1));
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (juce::String (index + 1) + "  " + state.name, text.removeFromTop (22), juce::Justification::centredLeft, true);
        g.setColour (base.brighter (0.25f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (languageName (language), text.removeFromTop (18), juce::Justification::centredLeft, true);
        g.setColour (juce::Colour (0xffcdd6d1));
        g.drawText (juce::String (state.tracks.size()) + " tracks  "
                        + juce::String (state.durationBeats, 1) + " + "
                        + juce::String (state.tailBeats, 1),
                    text,
                    juce::Justification::centredLeft,
                    true);
    }

    static void drawProgressLine (juce::Graphics& g, juce::Rectangle<int> lane, double normalised)
    {
        const auto x = lane.getX() + juce::roundToInt (lane.getWidth() * juce::jlimit (0.0, 1.0, normalised));
        g.setColour (juce::Colour (0xfffff0a8));
        g.drawLine (static_cast<float> (x), static_cast<float> (lane.getY()), static_cast<float> (x), static_cast<float> (lane.getBottom()), 2.0f);
    }

    ProjectModel& project;
    SectionHeader title;
    juce::TextButton zoomOutButton;
    juce::TextButton zoomInButton;
    juce::TextButton fitGraphButton;
    juce::TextButton arrangeGraphButton;
    juce::TextButton playButton;
    juce::TextButton stopButton;
    juce::TextButton addStateButton;
    juce::TextButton removeStateButton;
    juce::TextButton runScoreButton;
    juce::TextButton syncScoreButton;
    juce::TextButton resetScoreButton;
    juce::TextButton clearScoreButton;
    juce::Slider tempo;
    juce::ComboBox meter;
    juce::Slider phase;
    juce::TextEditor scoreScript;
    bool isCountingIn = false;
    bool isPlaying = false;
    int selectedStateIndex = 0;
    double currentBeat = 0.0;
    float viewportZoom = 1.0f;
    juce::Point<float> viewportPan;
    juce::Point<float> panStart;
    juce::Point<float> dragStartStateCanvas;
    int draggedStateIndex = -1;
    bool isPanningGraph = false;
    bool userAdjustedGraphView = false;
    juce::Rectangle<int> lastAutoFitPatchArea;
};

class TrackEditorComponent final : public juce::Component
{
public:
    explicit TrackEditorComponent (TrackModel& trackToUse)
        : track (trackToUse)
    {
        name.setText (track.name, juce::dontSendNotification);
        name.onTextChange = [this] { track.name = name.getText(); };
        addAndMakeVisible (name);

        language.addItem ("ChucK", 1);
        language.addItem ("SuperCollider", 2);
        language.addItem ("RTcmix", 3);
        language.addItem ("Csound", 4);
        language.addItem ("Faust", 5);
        language.setSelectedId (indexForLanguage (track.language), juce::dontSendNotification);
        language.onChange = [this]
        {
            track.language = languageFromIndex (language.getSelectedId());
            updateCodeLanguage();
            if (track.code.trim().isEmpty())
            {
                track.code = defaultCodeForLanguage (track.language);
                code.setText (track.code, false);
            }

            repaint();
        };
        addAndMakeVisible (language);

        sync.setButtonText ("Sync");
        sync.setToggleState (track.tightlySynced, juce::dontSendNotification);
        sync.onClick = [this]
        {
            track.tightlySynced = sync.getToggleState();
            updateTimingEnablement();
        };
        addAndMakeVisible (sync);

        configureTimingSlider (tempo, 20.0, 300.0, track.tempoBpm);
        tempo.onValueChange = [this] { track.tempoBpm = tempo.getValue(); };
        addAndMakeVisible (tempo);

        timeSig.addItem ("3/4", 1);
        timeSig.addItem ("4/4", 2);
        timeSig.addItem ("5/4", 3);
        timeSig.addItem ("7/8", 4);
        timeSig.setSelectedId (meterIdFor (track.timeSigNumerator, track.timeSigDenominator),
                               juce::dontSendNotification);
        timeSig.onChange = [this]
        {
            switch (timeSig.getSelectedId())
            {
                case 1: track.timeSigNumerator = 3; track.timeSigDenominator = 4; break;
                case 3: track.timeSigNumerator = 5; track.timeSigDenominator = 4; break;
                case 4: track.timeSigNumerator = 7; track.timeSigDenominator = 8; break;
                default: track.timeSigNumerator = 4; track.timeSigDenominator = 4; break;
            }
        };
        addAndMakeVisible (timeSig);

        configureTimingSlider (phase, -8.0, 8.0, track.phaseBeats);
        phase.onValueChange = [this] { track.phaseBeats = phase.getValue(); };
        addAndMakeVisible (phase);

        templateButton.setButtonText ("Template");
        templateButton.onClick = [this]
        {
            track.code = defaultCodeForLanguage (track.language);
            code.setText (track.code, false);
        };
        addAndMakeVisible (templateButton);

        clearButton.setButtonText ("Clear");
        clearButton.onClick = [this]
        {
            track.code.clear();
            code.setText ({}, false);
        };
        addAndMakeVisible (clearButton);

        fontDownButton.setButtonText ("A-");
        fontDownButton.onClick = [this] { setTrackCodeFontSize (codeFontSize - 1.0f); };
        addAndMakeVisible (fontDownButton);

        fontUpButton.setButtonText ("A+");
        fontUpButton.onClick = [this] { setTrackCodeFontSize (codeFontSize + 1.0f); };
        addAndMakeVisible (fontUpButton);

        code.setFontSize (codeFontSize);
        updateCodeLanguage();
        code.setText (track.code, false);
        code.onTextChange = [this] { track.code = code.getText(); };
        addAndMakeVisible (code);

        updateTimingEnablement();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds();
        g.setColour (juce::Colour (0xff1d2220));
        g.fillRect (area);
        g.setColour (languageColour (track.language).withAlpha (0.8f));
        g.fillRect (area.removeFromLeft (4));
        g.setColour (juce::Colour (0xff38433f));
        g.drawRect (getLocalBounds());
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        auto mainControls = area.removeFromTop (34);
        name.setBounds (mainControls.removeFromLeft (juce::jmin (230, juce::jmax (130, mainControls.getWidth() / 5))));
        language.setBounds (mainControls.removeFromLeft (150).reduced (8, 0));
        sync.setBounds (mainControls.removeFromLeft (74).reduced (4, 0));
        fontUpButton.setBounds (mainControls.removeFromRight (44).reduced (4, 0));
        fontDownButton.setBounds (mainControls.removeFromRight (44).reduced (4, 0));
        clearButton.setBounds (mainControls.removeFromRight (68).reduced (4, 0));
        templateButton.setBounds (mainControls.removeFromRight (100).reduced (8, 0));

        area.removeFromTop (6);
        auto timingControls = area.removeFromTop (30);
        tempo.setBounds (timingControls.removeFromLeft (juce::jmin (220, timingControls.getWidth() / 3)).reduced (0, 2));
        timeSig.setBounds (timingControls.removeFromLeft (94).reduced (8, 2));
        phase.setBounds (timingControls.removeFromLeft (juce::jmin (220, timingControls.getWidth())).reduced (8, 2));

        code.setBounds (area.withTrimmedTop (8));
    }

private:
    static void configureTimingSlider (juce::Slider& slider, double min, double max, double value)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 22);
        slider.setRange (min, max, 0.01);
        slider.setValue (value, juce::dontSendNotification);
    }

    void updateTimingEnablement()
    {
        const auto independent = ! track.tightlySynced;
        tempo.setEnabled (independent);
        timeSig.setEnabled (independent);
        phase.setEnabled (independent);
    }

    void setTrackCodeFontSize (float fontSize)
    {
        codeFontSize = juce::jlimit (10.0f, 24.0f, fontSize);
        code.setFontSize (codeFontSize);
    }

    void updateCodeLanguage()
    {
        code.setLanguage (languageName (track.language), languageColour (track.language));
    }

    TrackModel& track;
    juce::TextEditor name;
    juce::ComboBox language;
    juce::ToggleButton sync;
    juce::Slider tempo;
    juce::ComboBox timeSig;
    juce::Slider phase;
    juce::TextButton templateButton;
    juce::TextButton clearButton;
    juce::TextButton fontDownButton;
    juce::TextButton fontUpButton;
    CodeEditorPane code;
    float codeFontSize = 14.0f;
};

class StateEditorComponent final : public juce::Component
{
public:
    StateEditorComponent (StateModel& stateToUse, std::function<void()> addTrackCallback, std::function<void()> removeTrackCallback)
        : state (stateToUse),
          onAddTrack (std::move (addTrackCallback)),
          onRemoveTrack (std::move (removeTrackCallback))
    {
        addAndMakeVisible (header);
        header.setText (state.name);

        duration.setSliderStyle (juce::Slider::LinearHorizontal);
        duration.setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 22);
        duration.setRange (1.0, 256.0, 0.25);
        duration.setValue (state.durationBeats);
        duration.onValueChange = [this] { state.durationBeats = duration.getValue(); };
        addAndMakeVisible (duration);

        tail.setSliderStyle (juce::Slider::LinearHorizontal);
        tail.setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 22);
        tail.setRange (0.0, 64.0, 0.25);
        tail.setValue (state.tailBeats);
        tail.onValueChange = [this] { state.tailBeats = tail.getValue(); };
        addAndMakeVisible (tail);

        addAndMakeVisible (addTrackButton);
        addTrackButton.setButtonText ("+ Track");
        addTrackButton.onClick = [this]
        {
            if (onAddTrack != nullptr)
                onAddTrack();
        };

        addAndMakeVisible (removeTrackButton);
        removeTrackButton.setButtonText ("- Track");
        removeTrackButton.onClick = [this]
        {
            if (onRemoveTrack != nullptr)
                onRemoveTrack();
        };

        rebuildTrackEditors();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff101211));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16);
        auto top = area.removeFromTop (34);
        header.setBounds (top.removeFromLeft (190));
        duration.setBounds (top.removeFromLeft (210).reduced (8, 0));
        tail.setBounds (top.removeFromLeft (190).reduced (8, 0));
        removeTrackButton.setBounds (top.removeFromRight (92).reduced (6, 0));
        addTrackButton.setBounds (top.removeFromRight (92).reduced (6, 0));
        area.removeFromTop (10);

        const auto trackHeight = juce::jmax (154, area.getHeight() / juce::jmax (1, static_cast<int> (trackEditors.size())));
        for (auto& editor : trackEditors)
            editor->setBounds (area.removeFromTop (trackHeight).reduced (0, 5));
    }

private:
    void rebuildTrackEditors()
    {
        trackEditors.clear();

        for (auto& track : state.tracks)
        {
            auto editor = std::make_unique<TrackEditorComponent> (track);
            addAndMakeVisible (*editor);
            trackEditors.push_back (std::move (editor));
        }
    }

    StateModel& state;
    std::function<void()> onAddTrack;
    std::function<void()> onRemoveTrack;
    SectionHeader header;
    juce::Slider duration;
    juce::Slider tail;
    juce::TextButton addTrackButton;
    juce::TextButton removeTrackButton;
    std::vector<std::unique_ptr<TrackEditorComponent>> trackEditors;
};

class ArrangementComponent final : public juce::Component
{
public:
    explicit ArrangementComponent (ProjectModel& projectToUse)
        : project (projectToUse),
          trackLanes (projectToUse)
    {
        addAndMakeVisible (zoomOutButton);
        zoomOutButton.setButtonText ("-");
        zoomOutButton.onClick = [this] { setLaneZoom (laneZoom / 1.25); };

        addAndMakeVisible (zoomInButton);
        zoomInButton.setButtonText ("+");
        zoomInButton.onClick = [this] { setLaneZoom (laneZoom * 1.25); };

        addAndMakeVisible (fitButton);
        fitButton.setButtonText ("Fit");
        fitButton.onClick = [this] { setLaneZoom (1.0); };

        addAndMakeVisible (trackViewport);
        trackViewport.setViewedComponent (&trackLanes, false);
        trackViewport.setScrollBarsShown (true, true);
    }

    void setTransport (bool countingInToUse, bool playingToUse, double countInBeatToUse, double beatToUse)
    {
        countingIn = countingInToUse;
        playing = playingToUse;
        countInBeat = countInBeatToUse;
        currentBeat = beatToUse;
        trackLanes.setTransport (playing, currentBeat);
        repaint();
    }

    void refresh()
    {
        updateTrackLaneBounds();
        trackLanes.repaint();
        repaint();
    }

    void resized() override
    {
        updateTrackLaneBounds();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff101211));
        auto area = getLocalBounds().reduced (18);
        auto header = area.removeFromTop (30);
        g.setColour (juce::Colour (0xffdce3e1));
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        g.drawText (countingIn ? "Arrangement: count-in" : "Arrangement", header, juce::Justification::centredLeft, true);
        g.setColour (juce::Colour (0xff8fa19a));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (juce::String (laneZoom, 2) + "x", header.removeFromRight (64), juce::Justification::centredRight, true);

        const auto totalBeats = juce::jmax (1.0, totalDurationBeats (project));
        auto timeline = area.removeFromTop (juce::jmax (80, area.getHeight() / 4));
        auto stateStartBeat = 0.0;
        const auto order = playableArrangementOrder (project);

        for (const auto stateIndex : order)
        {
            if (stateIndex < 0 || stateIndex >= static_cast<int> (project.states.size()))
                continue;

            const auto& state = project.states[static_cast<size_t> (stateIndex)];
            const auto startX = beatToX (timeline, stateStartBeat, totalBeats);
            const auto endX = beatToX (timeline, stateStartBeat + state.durationBeats, totalBeats);
            auto block = juce::Rectangle<int> (startX, timeline.getY(), juce::jmax (1, endX - startX - 4), timeline.getHeight());
            g.setColour (juce::Colour (0xff202725));
            g.fillRect (block);
            g.setColour (juce::Colour (0xff53605c));
            g.drawRect (block, 1);
            g.setColour (juce::Colour (0xffedf2ef));
            g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
            g.drawText (state.name, block.reduced (10), juce::Justification::centredLeft, true);
            stateStartBeat += state.durationBeats;
        }

        if (playing)
            drawPlayhead (g, timeline, currentBeat / totalBeats);

        if (countingIn)
        {
            const auto countInProgress = countInBeat / juce::jmax (1.0, oneBarBeats (project));
            g.setColour (juce::Colour (0xffffd36e));
            g.fillRect (timeline.withWidth (juce::roundToInt (timeline.getWidth() * juce::jlimit (0.0, 1.0, countInProgress))).removeFromBottom (5));
        }

        area.removeFromTop (14);
    }

private:
    class TrackLanesComponent final : public juce::Component
    {
    public:
        explicit TrackLanesComponent (ProjectModel& projectToUse)
            : project (projectToUse)
        {
        }

        void setTransport (bool playingToUse, double beatToUse)
        {
            playing = playingToUse;
            currentBeat = beatToUse;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff101211));
            drawTrackLanes (g, getLocalBounds());
        }

        static int laneCountFor (const ProjectModel& project)
        {
            const auto order = playableArrangementOrder (project);
            auto laneCount = 0;
            for (const auto stateIndex : order)
                if (stateIndex >= 0 && stateIndex < static_cast<int> (project.states.size()))
                    laneCount += static_cast<int> (project.states[static_cast<size_t> (stateIndex)].tracks.size());

            return laneCount;
        }

    private:
        static int beatToX (juce::Rectangle<int> area, double beat, double totalBeats)
        {
            return area.getX() + juce::roundToInt (area.getWidth() * juce::jlimit (0.0, 1.0, beat / juce::jmax (1.0, totalBeats)));
        }

        void drawTrackLanes (juce::Graphics& g, juce::Rectangle<int> area)
        {
            const auto order = playableArrangementOrder (project);
            const auto laneCount = juce::jmax (1, laneCountFor (project));
            const auto laneHeight = juce::jmax (28, area.getHeight() / laneCount);
            const auto totalBeats = juce::jmax (1.0, totalDurationBeats (project));

            auto stateStartBeat = 0.0;
            for (const auto stateIndex : order)
            {
                if (stateIndex < 0 || stateIndex >= static_cast<int> (project.states.size()))
                    continue;

                const auto& state = project.states[static_cast<size_t> (stateIndex)];
                for (const auto& track : state.tracks)
                {
                    auto lane = area.removeFromTop (laneHeight).reduced (0, 3);
                    const auto laneBounds = lane;
                    g.setColour (juce::Colour (0xff1b201f));
                    g.fillRect (laneBounds);

                    const auto stateStartX = beatToX (laneBounds, stateStartBeat, totalBeats);
                    const auto stateEndX = beatToX (laneBounds, stateStartBeat + state.durationBeats, totalBeats);
                    const auto tailEndX = beatToX (laneBounds, stateStartBeat + state.durationBeats + state.tailBeats, totalBeats);
                    auto clip = juce::Rectangle<int> (stateStartX,
                                                      laneBounds.getY(),
                                                      juce::jmax (1, stateEndX - stateStartX),
                                                      laneBounds.getHeight());
                    auto tail = juce::Rectangle<int> (stateEndX,
                                                      laneBounds.getY(),
                                                      juce::jmax (0, tailEndX - stateEndX),
                                                      laneBounds.getHeight());

                    const auto colour = languageColour (track.language);
                    if (! tail.isEmpty())
                    {
                        g.setColour (colour.withAlpha (0.26f));
                        g.fillRect (tail);
                    }

                    g.setColour (colour.withAlpha (0.82f));
                    g.fillRect (clip);
                    g.setColour (colour.withAlpha (0.95f));
                    g.drawRect (clip);
                    if (! tail.isEmpty())
                        g.drawRect (tail);

                    g.setColour (juce::Colour (0xff101211));
                    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
                    g.drawText (state.name + " / " + track.name + " / " + languageName (track.language),
                                clip.reduced (8, 0).withRight (juce::jmax (clip.getRight(), laneBounds.getX() + 180)),
                                juce::Justification::centredLeft,
                                true);
                }

                stateStartBeat += state.durationBeats;
            }

            if (playing)
            {
                const auto playheadX = beatToX (getLocalBounds(), currentBeat, totalBeats);
                g.setColour (juce::Colour (0xfffff0a8));
                g.drawLine (static_cast<float> (playheadX),
                            static_cast<float> (getLocalBounds().getY()),
                            static_cast<float> (playheadX),
                            static_cast<float> (getLocalBounds().getBottom()),
                            1.5f);
            }
        }

        ProjectModel& project;
        bool playing = false;
        double currentBeat = 0.0;
    };

    void updateTrackLaneBounds()
    {
        auto area = getLocalBounds().reduced (18);
        auto header = area.removeFromTop (30);
        fitButton.setBounds (header.removeFromRight (54).reduced (4, 2));
        zoomInButton.setBounds (header.removeFromRight (34).reduced (4, 2));
        zoomOutButton.setBounds (header.removeFromRight (34).reduced (4, 2));
        area.removeFromTop (juce::jmax (80, area.getHeight() / 4));
        area.removeFromTop (14);

        trackViewport.setBounds (area);

        const auto laneHeight = 42;
        const auto contentHeight = juce::jmax (area.getHeight(),
                                               TrackLanesComponent::laneCountFor (project) * laneHeight);
        const auto contentWidth = juce::jmax (1, juce::roundToInt (static_cast<double> (juce::jmax (1, area.getWidth() - 14)) * laneZoom));
        trackLanes.setBounds (0, 0, contentWidth, contentHeight);
    }

    void setLaneZoom (double zoom)
    {
        laneZoom = juce::jlimit (1.0, 6.0, zoom);
        updateTrackLaneBounds();
        trackLanes.repaint();
        repaint();
    }

    static void drawPlayhead (juce::Graphics& g, juce::Rectangle<int> timeline, double normalised)
    {
        const auto x = timeline.getX() + juce::roundToInt (timeline.getWidth() * juce::jlimit (0.0, 1.0, normalised));
        g.setColour (juce::Colour (0xfffff0a8));
        g.drawLine (static_cast<float> (x), static_cast<float> (timeline.getY()), static_cast<float> (x), static_cast<float> (timeline.getBottom()), 2.5f);
    }

    static int beatToX (juce::Rectangle<int> area, double beat, double totalBeats)
    {
        return area.getX() + juce::roundToInt (area.getWidth() * juce::jlimit (0.0, 1.0, beat / juce::jmax (1.0, totalBeats)));
    }

    ProjectModel& project;
    juce::TextButton zoomOutButton;
    juce::TextButton zoomInButton;
    juce::TextButton fitButton;
    juce::Viewport trackViewport;
    TrackLanesComponent trackLanes;
    bool countingIn = false;
    bool playing = false;
    double countInBeat = 0.0;
    double currentBeat = 0.0;
    double laneZoom = 1.0;
};

class MixerComponent final : public juce::Component
{
public:
    std::function<void()> onControlChange;

    explicit MixerComponent (ProjectModel& projectToUse)
        : project (projectToUse)
    {
        addAndMakeVisible (channelViewport);
        channelViewport.setViewedComponent (&channelContent, false);
        channelViewport.setScrollBarsShown (false, true);
        rebuild();
    }

    void refresh()
    {
        rebuild();
        resized();
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16);
        auto title = area.removeFromTop (28);
        titleBounds = title;
        area.removeFromTop (12);
        channelViewport.setBounds (area);

        const auto channelWidth = 112;
        const auto contentWidth = juce::jmax (area.getWidth(), channelWidth * static_cast<int> (channels.size()));
        channelContent.setBounds (0, 0, contentWidth, area.getHeight());
        auto channelArea = channelContent.getLocalBounds();
        for (auto& channel : channels)
            channel->setBounds (channelArea.removeFromLeft (channelWidth).reduced (5, 0));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff101211));
        g.setColour (juce::Colour (0xffdce3e1));
        g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        g.drawText ("Mixer", titleBounds, juce::Justification::centredLeft, true);
        g.setColour (juce::Colour (0xff8fa19a));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (juce::String (channels.size()) + " tracks", titleBounds, juce::Justification::centredRight, true);
    }

private:
    class Channel final : public juce::Component
    {
    public:
        Channel (TrackModel& trackToUse, juce::String stateNameToUse, std::function<void()> controlChangeCallback)
            : track (trackToUse),
              stateName (std::move (stateNameToUse)),
              onControlChange (std::move (controlChangeCallback))
        {
            fader.setSliderStyle (juce::Slider::LinearVertical);
            fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 58, 22);
            fader.setRange (0.0, 1.0, 0.001);
            fader.setValue (track.level);
            fader.onValueChange = [this]
            {
                track.level = static_cast<float> (fader.getValue());
                repaint();
                notifyControlChange();
            };
            addAndMakeVisible (fader);

            mute.setButtonText ("M");
            mute.setToggleState (track.muted, juce::dontSendNotification);
            mute.onClick = [this]
            {
                track.muted = mute.getToggleState();
                repaint();
                notifyControlChange();
            };
            addAndMakeVisible (mute);

            solo.setButtonText ("S");
            solo.setToggleState (track.soloed, juce::dontSendNotification);
            solo.onClick = [this]
            {
                track.soloed = solo.getToggleState();
                repaint();
                notifyControlChange();
            };
            addAndMakeVisible (solo);
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (juce::Colour (0xff1b201f));
            g.fillRect (getLocalBounds());
            const auto colour = languageColour (track.language);
            g.setColour (colour);
            g.fillRect (getLocalBounds().removeFromTop (4));

            auto meter = getLocalBounds().reduced (10);
            meter.removeFromTop (76);
            meter.removeFromBottom (42);
            auto levelFill = meter.withTop (meter.getBottom()
                                            - juce::roundToInt (static_cast<float> (meter.getHeight())
                                                                * juce::jlimit (0.0f, 1.0f, track.level)));
            g.setColour (colour.withAlpha (track.muted ? 0.14f : 0.28f));
            g.fillRect (meter);
            g.setColour (colour.withAlpha (track.muted ? 0.25f : 0.78f));
            g.fillRect (levelFill);

            g.setColour (juce::Colour (0xffdce3e1));
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            auto label = getLocalBounds().reduced (7).removeFromTop (62);
            g.drawFittedText (track.name, label.removeFromTop (34), juce::Justification::centred, 2);
            g.setColour (juce::Colour (0xff9fb0aa));
            g.setFont (juce::FontOptions (10.5f));
            g.drawFittedText (stateName + " / " + languageName (track.language), label, juce::Justification::centred, 2);

            if (track.muted || track.soloed)
            {
                g.setColour (track.soloed ? juce::Colour (0xfffff0a8) : juce::Colour (0xffd58b73));
                g.drawRect (getLocalBounds().reduced (2), 2);
            }
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (7);
            area.removeFromTop (68);
            auto buttons = area.removeFromBottom (30);
            mute.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (2));
            solo.setBounds (buttons.reduced (2));
            fader.setBounds (area.reduced (6, 0));
        }

    private:
        void notifyControlChange()
        {
            if (onControlChange != nullptr)
                onControlChange();
        }

        TrackModel& track;
        juce::String stateName;
        std::function<void()> onControlChange;
        juce::Slider fader;
        juce::TextButton mute;
        juce::TextButton solo;
    };

    void rebuild()
    {
        channels.clear();
        channelContent.removeAllChildren();

        for (auto& state : project.states)
        {
            for (auto& track : state.tracks)
            {
                auto channel = std::make_unique<Channel> (track, state.name, [this]
                {
                    if (onControlChange != nullptr)
                        onControlChange();
                });
                channelContent.addAndMakeVisible (*channel);
                channels.push_back (std::move (channel));
            }
        }
    }

    ProjectModel& project;
    juce::Rectangle<int> titleBounds;
    juce::Viewport channelViewport;
    juce::Component channelContent;
    std::vector<std::unique_ptr<Channel>> channels;
};

class PerformanceController final : public juce::AudioIODeviceCallback
{
public:
    PerformanceController()
    {
        deviceManager.initialise (0, 2, nullptr, true);
        deviceManager.addAudioCallback (this);
    }

    ~PerformanceController() override
    {
        deviceManager.removeAudioCallback (this);
        performance.release();
    }

    bool loadProject (const ProjectModel& project)
    {
        const juce::ScopedLock lock (engineLock);
        pendingProject = project;
        needsRebuild = true;

        if (deviceSampleRate > 0.0)
            return rebuildLocked();

        return true;
    }

    bool updateTrackGains (const ProjectModel& project)
    {
        const juce::ScopedLock lock (engineLock);
        pendingProject = project;

        if (deviceSampleRate <= 0.0 || deviceBlockSize <= 0 || ! performance.isReady())
            return true;

        for (int stateIndex = 0; stateIndex < static_cast<int> (pendingProject.states.size()); ++stateIndex)
        {
            const auto& state = pendingProject.states[static_cast<size_t> (stateIndex)];
            for (int trackIndex = 0; trackIndex < static_cast<int> (state.tracks.size()); ++trackIndex)
            {
                if (! performance.setTrackGain (stateIndex,
                                                trackIndex,
                                                effectiveTrackGain (pendingProject, stateIndex, trackIndex)))
                {
                    lastError = performance.getLastError();
                    return false;
                }
            }
        }

        lastError.clear();
        return true;
    }

    bool start()
    {
        const juce::ScopedLock lock (engineLock);

        if (needsRebuild && ! rebuildLocked())
            return false;

        performance.resetToStart();
        return performance.start();
    }

    void stop()
    {
        const juce::ScopedLock lock (engineLock);
        performance.stop();
    }

    juce::String getLastError() const
    {
        const juce::ScopedLock lock (engineLock);
        return lastError;
    }

    bool isPlaying() const
    {
        const juce::ScopedLock lock (engineLock);
        return performance.isPlaying();
    }

    double getCurrentBeat() const
    {
        const juce::ScopedLock lock (engineLock);
        return performance.getCurrentBeat();
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        const juce::ScopedLock lock (engineLock);
        deviceSampleRate = device != nullptr ? device->getCurrentSampleRate() : 44100.0;
        deviceBlockSize = device != nullptr ? device->getCurrentBufferSizeSamples() : 512;
        deviceBlockSize = juce::jlimit (64, EmbeddedChucKEngine::maximumBlockSizeLimit, deviceBlockSize);
        needsRebuild = true;
        rebuildLocked();
    }

    void audioDeviceStopped() override
    {
        const juce::ScopedLock lock (engineLock);
        performance.release();
        scratchInput.setSize (0, 0);
        scratchOutput.setSize (0, 0);
        deviceSampleRate = 0.0;
        deviceBlockSize = 0;
        needsRebuild = true;
    }

    void audioDeviceIOCallbackWithContext (const float* const*,
                                           int,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        juce::ScopedNoDenormals noDenormals;
        clearOutputs (outputChannelData, numOutputChannels, numSamples);

        const juce::ScopedLock lock (engineLock);
        if (! performance.isReady() || ! performance.isPlaying() || numOutputChannels <= 0 || numSamples <= 0)
            return;

        if (numSamples > scratchOutput.getNumSamples())
        {
            performance.stop();
            lastError = "Host audio block exceeded prepared GUI performance buffer";
            return;
        }

        for (int channel = 0; channel < scratchOutput.getNumChannels(); ++channel)
            scratchOutputPointers[static_cast<size_t> (channel)] = scratchOutput.getWritePointer (channel);

        juce::AudioBuffer<float> outputView (scratchOutputPointers.data(),
                                             scratchOutput.getNumChannels(),
                                             numSamples);
        outputView.clear();
        performance.process (scratchInput, outputView);

        const auto channelsToCopy = juce::jmin (numOutputChannels, scratchOutput.getNumChannels());
        for (int channel = 0; channel < channelsToCopy; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::copy (outputChannelData[channel],
                                                   scratchOutput.getReadPointer (channel),
                                                   numSamples);
    }

private:
    static bool sequenceHasFallbackCandidate (const std::vector<EmbeddedPerformanceEngine::State>& sequence) noexcept
    {
        for (const auto& state : sequence)
            for (const auto& track : state.tracks)
                if (track.language != Language::chuck)
                    return true;

        return false;
    }

    static std::vector<EmbeddedPerformanceEngine::State>
    makeChucKFallbackSequence (std::vector<EmbeddedPerformanceEngine::State> sequence)
    {
        for (auto& state : sequence)
            for (auto& track : state.tracks)
                if (track.language != Language::chuck)
                {
                    const auto originalLanguage = track.language;
                    track.language = Language::chuck;
                    track.programBody = chuckFallbackCodeForLanguage (originalLanguage);
                    track.parameterBindings = EmbeddedChucKEngine::getDefaultParameterBindings();
                }

        return sequence;
    }

    bool rebuildLocked()
    {
        needsRebuild = false;
        performance.release();

        if (deviceSampleRate <= 0.0 || deviceBlockSize <= 0)
            return true;

        scratchInput.setSize (0, deviceBlockSize, false, false, true);
        scratchOutput.setSize (2, deviceBlockSize, false, false, true);
        scratchInput.clear();
        scratchOutput.clear();

        if (! performance.prepare (deviceSampleRate, deviceBlockSize, 0, 2))
        {
            lastError = performance.getLastError();
            return false;
        }

        performance.setTempoBpm (pendingProject.globalTempoBpm);
        performance.setTempoMap (pendingProject.tempoMap.empty()
                                    ? std::vector<EmbeddedPerformanceEngine::TempoEvent> { { 0.0, pendingProject.globalTempoBpm } }
                                    : pendingProject.tempoMap);
        performance.setTimeSignatureMap (pendingProject.timeSignatureMap.empty()
                                            ? std::vector<EmbeddedPerformanceEngine::TimeSignatureEvent> { { 0.0, pendingProject.globalTimeSigNumerator, pendingProject.globalTimeSigDenominator } }
                                            : pendingProject.timeSignatureMap);
        performance.setPhaseRotationMap (pendingProject.phaseRotationMap.empty()
                                            ? std::vector<EmbeddedPerformanceEngine::PhaseRotationEvent> { { 0.0, pendingProject.globalPhaseBeats } }
                                            : pendingProject.phaseRotationMap);
        performance.setStopBeat (pendingProject.scheduledStopBeat);

        std::vector<EmbeddedPerformanceEngine::State> sequence;
        sequence.reserve (pendingProject.states.size());

        const auto order = playableArrangementOrder (pendingProject);
        for (const auto stateIndex : order)
        {
            if (stateIndex < 0 || stateIndex >= static_cast<int> (pendingProject.states.size()))
                continue;

            const auto& state = pendingProject.states[static_cast<size_t> (stateIndex)];
            EmbeddedPerformanceEngine::State performanceState;
            performanceState.name = state.name;
            performanceState.durationBeats = state.durationBeats;
            performanceState.tailBeats = state.tailBeats;
            performanceState.tracks.reserve (state.tracks.size());

            for (int trackIndex = 0; trackIndex < static_cast<int> (state.tracks.size()); ++trackIndex)
            {
                const auto& track = state.tracks[static_cast<size_t> (trackIndex)];
                performanceState.tracks.push_back ({ track.name,
                                                     track.language,
                                                     track.code,
                                                     EmbeddedChucKEngine::getDefaultParameterBindings(),
                                                     effectiveTrackGain (pendingProject, stateIndex, trackIndex),
                                                     track.tightlySynced,
                                                     track.tempoBpm,
                                                     track.timeSigNumerator,
                                                     track.timeSigDenominator,
                                                     track.phaseBeats,
                                                     track.gainEvents });
            }

            if (! performanceState.tracks.empty())
                sequence.push_back (std::move (performanceState));
        }

        if (sequence.empty())
        {
            lastError = "GUI project has no playable tracks";
            return false;
        }

        if (! performance.loadSequence (sequence))
        {
            const auto nativeError = performance.getLastError();

            if (sequenceHasFallbackCandidate (sequence))
            {
                if (performance.loadSequence (makeChucKFallbackSequence (sequence)))
                {
                    lastError = "Native sequence failed; using ChucK fallback tracks: " + nativeError;
                    juce::Logger::writeToLog (lastError);
                    return true;
                }

                lastError = nativeError + " / ChucK fallback also failed: " + performance.getLastError();
                return false;
            }

            lastError = nativeError;
            return false;
        }

        lastError.clear();
        return true;
    }

    static void clearOutputs (float* const* outputChannelData, int numOutputChannels, int numSamples) noexcept
    {
        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[channel], numSamples);
    }

    mutable juce::CriticalSection engineLock;
    juce::AudioDeviceManager deviceManager;
    EmbeddedPerformanceEngine performance;
    ProjectModel pendingProject = makeInitialProject();
    juce::AudioBuffer<float> scratchInput;
    juce::AudioBuffer<float> scratchOutput;
    std::array<float*, EmbeddedChucKEngine::maximumChannelLimit> scratchOutputPointers {};
    juce::String lastError;
    double deviceSampleRate = 0.0;
    int deviceBlockSize = 0;
    bool needsRebuild = true;
};

class SplitterComponent final : public juce::Component
{
public:
    std::function<void()> onDragStart;
    std::function<void (int)> onDrag;
    std::function<void()> onReset;

    SplitterComponent()
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds();
        g.fillAll (juce::Colour (0xff0f1211));
        g.setColour (isMouseOverOrDragging() ? juce::Colour (0xff84c7b6) : juce::Colour (0xff38433f));
        g.fillRoundedRectangle (area.withSizeKeepingCentre (juce::jmax (80, area.getWidth() / 8), 3).toFloat(), 1.5f);
    }

    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (onDragStart != nullptr)
            onDragStart();
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (onDrag != nullptr)
            onDrag (event.getDistanceFromDragStartY());
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (onReset != nullptr)
            onReset();
    }
};

class MainComponent final : public juce::Component, private juce::Timer
{
public:
    MainComponent()
        : project (makeInitialProject()),
          score (project),
          tabs (juce::TabbedButtonBar::TabsAtTop),
          arrangement (project),
          mixer (project)
    {
        setWantsKeyboardFocus (true);

        addAndMakeVisible (score);
        addAndMakeVisible (splitter);
        splitter.onDragStart = [this] { dragStartTopHeight = score.getHeight(); };
        splitter.onDrag = [this] (int deltaY) { setTopPanelHeight (dragStartTopHeight + deltaY); };
        splitter.onReset = [this]
        {
            topPanelRatio = defaultTopPanelRatio;
            resized();
        };

        score.onPlay = [this] { startPlayback(); };
        score.onStop = [this] { stopPlayback(); };
        score.onAddState = [this] { addState(); };
        score.onRemoveState = [this] { removeCurrentState(); };
        score.onRunChucKScore = [this] { runChucKScoreScript (project.chuckScoreScript); };
        score.onSyncChucKScore = [this] { syncChucKScoreScriptFromProject(); };
        score.onResetChucKScore = [this]
        {
            project.chuckScoreScript = defaultChucKScoreScript();
            score.setScoreScriptText (project.chuckScoreScript);
        };
        score.onSelectState = [this] (int index) { selectState (index); };

        tabs.setOutline (0);
        addAndMakeVisible (tabs);
        rebuildTabs();

        addChildComponent (arrangement);
        addChildComponent (mixer);
        mixer.onControlChange = [this] { refreshMixerControlChange (false); };

        audio.loadProject (project);
        prepareChucKScoreRunner();

        startTimerHz (30);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const auto topHeight = limitedTopPanelHeight (juce::roundToInt (area.getHeight() * topPanelRatio));
        score.setBounds (area.removeFromTop (topHeight));
        splitter.setBounds (area.removeFromTop (splitterHeight));
        bottomBounds = area;
        layoutBottomView();
    }

    void parentHierarchyChanged() override
    {
        grabKeyboardFocus();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key.getTextCharacter() == 'm' || key.getTextCharacter() == 'M')
        {
            mixerVisible = ! mixerVisible;
            layoutBottomView();
            return true;
        }

        if (key == juce::KeyPress::spaceKey)
        {
            if (playing || countingIn)
                stopPlayback();
            else
                startPlayback();

            return true;
        }

        return false;
    }

private:
    using ScoreCommandId = ChucKScoreScript::CommandId;

    enum class BottomView
    {
        stateTabs,
        arrangement,
        mixer
    };

    static constexpr int scoreChucKSampleRate = 48000;
    static constexpr int scoreChucKBlockSize = 256;
    static constexpr int splitterHeight = 9;
    static constexpr double defaultTopPanelRatio = 1.0 / 3.0;

    struct ScoreCommandEvent
    {
        ScoreCommandId command = ScoreCommandId::none;
        int64_t frame = 0;
        std::array<double, ChucKScoreScript::argumentCount> args {};
        std::array<juce::String, 2> text;
    };

    int limitedTopPanelHeight (int proposedHeight) const
    {
        const auto height = getHeight();
        if (height <= splitterHeight)
            return 0;

        const auto minTop = juce::jmin (220, juce::jmax (80, height / 3));
        const auto minBottom = juce::jmin (260, juce::jmax (100, height / 3));
        const auto maxTop = juce::jmax (minTop, height - splitterHeight - minBottom);
        return juce::jlimit (minTop, maxTop, proposedHeight);
    }

    void setTopPanelHeight (int proposedHeight)
    {
        const auto limitedHeight = limitedTopPanelHeight (proposedHeight);
        if (getHeight() > 0)
            topPanelRatio = static_cast<double> (limitedHeight) / static_cast<double> (getHeight());

        resized();
    }

    void rebuildTabs()
    {
        const auto previousTab = tabs.getCurrentTabIndex();
        tabs.clearTabs();

        const auto count = juce::jmin (maxStateCount, static_cast<int> (project.states.size()));
        for (int i = 0; i < count; ++i)
        {
            auto* editor = new StateEditorComponent (project.states[static_cast<size_t> (i)],
                                                     [this, i] { addTrackToState (i); },
                                                     [this, i] { removeTrackFromState (i); });
            tabs.addTab (project.states[static_cast<size_t> (i)].name,
                         juce::Colour (0xff1a1f1d),
                         editor,
                         true);
        }

        if (count > 0)
            tabs.setCurrentTabIndex (juce::jlimit (0, count - 1, previousTab));
    }

    void selectState (int index)
    {
        if (index < 0 || index >= tabs.getNumTabs())
            return;

        tabs.setCurrentTabIndex (index);
        score.setSelectedStateIndex (index);
        currentView = BottomView::stateTabs;
        mixerVisible = false;
        layoutBottomView();
    }

    void syncChucKScoreScriptFromProject()
    {
        project.chuckScoreScript = generateChucKScoreScript (project);
        score.setScoreScriptText (project.chuckScoreScript);
    }

    void addState()
    {
        addStateFromCommand (juce::String(), 16.0, 4.0, true);
    }

    bool addStateFromCommand (juce::String name, double durationBeats, double tailBeats, bool includeDefaultTrack)
    {
        if (static_cast<int> (project.states.size()) >= maxStateCount)
            return false;

        quietTransportForStructureEdit();
        auto state = makeState (static_cast<int> (project.states.size()) + 1);
        if (! includeDefaultTrack)
            state.tracks.clear();
        if (name.isNotEmpty())
            state.name = std::move (name);
        state.durationBeats = juce::jlimit (1.0, 256.0, durationBeats);
        state.tailBeats = juce::jlimit (0.0, 64.0, tailBeats);
        const auto newStateIndex = static_cast<int> (project.states.size());
        if (includeDefaultTrack
            && newStateIndex > 0
            && project.states[static_cast<size_t> (newStateIndex - 1)].transitions.empty())
            project.states[static_cast<size_t> (newStateIndex - 1)].transitions.push_back ({ newStateIndex, 1.0 });
        project.states.push_back (std::move (state));
        arrangeStatesAsFiniteStateMachine (project);
        project.arrangementOrder = defaultArrangementOrder (project);
        refreshStructure (static_cast<int> (project.states.size()) - 1);
        return true;
    }

    void removeCurrentState()
    {
        if (project.states.size() <= 1)
            return;

        const auto index = juce::jlimit (0, static_cast<int> (project.states.size()) - 1, tabs.getCurrentTabIndex());
        removeStateAt (index);
    }

    bool removeStateAt (int index)
    {
        if (project.states.size() <= 1 || index < 0 || index >= static_cast<int> (project.states.size()))
            return false;

        quietTransportForStructureEdit();
        project.states.erase (project.states.begin() + index);
        for (auto& state : project.states)
        {
            state.transitions.erase (std::remove_if (state.transitions.begin(),
                                                     state.transitions.end(),
                                                     [index] (const StateTransitionModel& transition)
                                                     {
                                                         return transition.targetStateIndex == index;
                                                     }),
                                     state.transitions.end());

            for (auto& transition : state.transitions)
                if (transition.targetStateIndex > index)
                    --transition.targetStateIndex;
        }

        project.arrangementOrder = defaultArrangementOrder (project);
        arrangeStatesAsFiniteStateMachine (project);
        refreshStructure (juce::jlimit (0, static_cast<int> (project.states.size()) - 1, index));
        return true;
    }

    void addTrackToState (int stateIndex)
    {
        static_cast<void> (addTrackFromCommand (stateIndex, Language::chuck, juce::String()));
    }

    bool addTrackFromCommand (int stateIndex, Language language, juce::String name)
    {
        if (stateIndex < 0 || stateIndex >= static_cast<int> (project.states.size()))
            return false;

        auto& state = project.states[static_cast<size_t> (stateIndex)];
        if (static_cast<int> (state.tracks.size()) >= maxTrackCountPerState)
            return false;

        quietTransportForStructureEdit();
        if (name.isEmpty())
            name = "Track " + juce::String (static_cast<int> (state.tracks.size()) + 1);

        state.tracks.push_back (makeTrack (std::move (name), language));
        refreshStructure (stateIndex);
        return true;
    }

    void removeTrackFromState (int stateIndex)
    {
        if (stateIndex < 0 || stateIndex >= static_cast<int> (project.states.size()))
            return;

        auto& state = project.states[static_cast<size_t> (stateIndex)];
        static_cast<void> (removeTrackAt (stateIndex, static_cast<int> (state.tracks.size()) - 1));
    }

    bool removeTrackAt (int stateIndex, int trackIndex)
    {
        if (stateIndex < 0 || stateIndex >= static_cast<int> (project.states.size()))
            return false;

        auto& state = project.states[static_cast<size_t> (stateIndex)];
        if (state.tracks.size() <= 1 || trackIndex < 0 || trackIndex >= static_cast<int> (state.tracks.size()))
            return false;

        quietTransportForStructureEdit();
        state.tracks.erase (state.tracks.begin() + trackIndex);
        refreshStructure (stateIndex);
        return true;
    }

    void quietTransportForStructureEdit()
    {
        countingIn = false;
        playing = false;
        countInBeat = 0.0;
        currentBeat = 0.0;
        audio.stop();
        currentView = BottomView::stateTabs;
        mixerVisible = false;
    }

    void refreshStructure (int preferredTab)
    {
        rebuildTabs();
        if (tabs.getNumTabs() > 0)
            tabs.setCurrentTabIndex (juce::jlimit (0, tabs.getNumTabs() - 1, preferredTab));
        score.setSelectedStateIndex (tabs.getNumTabs() > 0 ? tabs.getCurrentTabIndex() : 0);

        mixer.refresh();
        score.repaint();
        arrangement.refresh();
        audio.loadProject (project);
        layoutBottomView();
        updateTransportViews();
    }

    void refreshMixerControlChange (bool rebuildMixer)
    {
        if (rebuildMixer)
            mixer.refresh();

        static_cast<void> (audio.updateTrackGains (project));
        score.repaint();
        arrangement.refresh();
        updateTransportViews();
    }

    void refreshScoreControlChange()
    {
        mixer.refresh();
        score.repaint();
        arrangement.refresh();
        audio.loadProject (project);
        updateTransportViews();
    }

    void startPlayback()
    {
        countingIn = true;
        playing = false;
        countInBeat = 0.0;
        currentBeat = 0.0;
        project.arrangementOrder = resolveProbabilisticArrangement (project);
        audio.loadProject (project);
        currentView = BottomView::arrangement;
        layoutBottomView();
        updateTransportViews();
    }

    void stopPlayback()
    {
        countingIn = false;
        playing = false;
        countInBeat = 0.0;
        currentBeat = 0.0;
        audio.stop();
        currentView = BottomView::stateTabs;
        layoutBottomView();
        updateTransportViews();
    }

    void timerCallback() override
    {
        const auto deltaBeats = beatsPerSecond (project.globalTempoBpm) / 30.0;

        if (chuckScoreRunning)
        {
            advanceChucKScoreRunner (1.0 / 30.0);
        }

        if (countingIn)
        {
            countInBeat += deltaBeats;

            if (countInBeat >= oneBarBeats (project))
            {
                countingIn = false;
                playing = audio.start();
                currentBeat = 0.0;

                if (! playing)
                    currentView = BottomView::stateTabs;
            }
        }
        else if (playing)
        {
            currentBeat = audio.getCurrentBeat();
            playing = audio.isPlaying();

            if (! playing || currentBeat >= totalDurationBeats (project))
            {
                playing = false;
                currentBeat = juce::jmin (currentBeat, totalDurationBeats (project));
            }
        }

        updateTransportViews();
    }

    void updateTransportViews()
    {
        score.setTransport (countingIn, playing, currentBeat);
        arrangement.setTransport (countingIn, playing, countInBeat, currentBeat);
    }

    static juce::String decodeScoreText (juce::String text)
    {
        text = text.trim().unquoted();
        if (! text.startsWithIgnoreCase ("base64:"))
            return text;

        juce::MemoryOutputStream decoded;
        if (juce::Base64::convertFromBase64 (decoded, text.fromFirstOccurrenceOf (":", false, false)))
            return decoded.toString();

        return {};
    }

    void prepareChucKScoreRunner()
    {
        if (chuckScorePrepared)
            return;

        chuckScoreInput.setSize (0, scoreChucKBlockSize);
        chuckScoreOutput.setSize (1, scoreChucKBlockSize);
        chuckScorePrepared = scoreChucK.prepare (scoreChucKSampleRate, scoreChucKBlockSize, 0, 1);

        if (! chuckScorePrepared)
            juce::Logger::writeToLog ("ChucK score runner prepare failed: " + scoreChucK.getLastError());
    }

    static bool isAudioTimelineCommand (ScoreCommandId command) noexcept
    {
        return command == ScoreCommandId::tempo
            || command == ScoreCommandId::meter
            || command == ScoreCommandId::phase
            || command == ScoreCommandId::trackGain
            || command == ScoreCommandId::trackMute
            || command == ScoreCommandId::trackSolo
            || command == ScoreCommandId::stop;
    }

    bool captureChucKScoreScript (const juce::String& script, std::vector<ScoreCommandEvent>& events, juce::String& error)
    {
        EmbeddedChucKEngine captureEngine;
        if (! captureEngine.prepare (scoreChucKSampleRate, scoreChucKBlockSize, 0, 1))
        {
            error = captureEngine.getLastError();
            return false;
        }

        const auto program = ChucKScoreScript::buildProgram (script);
        if (! captureEngine.loadProgram (program.source, ChucKScoreScript::getParameterBindings()))
        {
            error = captureEngine.getLastError();
            return false;
        }

        const auto commandIndex = captureEngine.getParameterIndex ("weldScoreCommand");
        const auto frameIndex = captureEngine.getParameterIndex ("weldScoreFrame");
        if (commandIndex < 0 || frameIndex < 0)
        {
            error = "ChucK score bridge command/frame slots are unavailable";
            return false;
        }

        std::array<int, ChucKScoreScript::argumentCount> argIndices {};
        for (int i = 0; i < ChucKScoreScript::argumentCount; ++i)
        {
            argIndices[static_cast<size_t> (i)] = captureEngine.getParameterIndex ("weldScoreArg" + juce::String (i));
            if (argIndices[static_cast<size_t> (i)] < 0)
            {
                error = "ChucK score bridge argument slots are unavailable";
                return false;
            }
        }

        juce::AudioBuffer<float> input (0, scoreChucKBlockSize);
        juce::AudioBuffer<float> output (1, scoreChucKBlockSize);
        constexpr int64_t maxScoreFrames = static_cast<int64_t> (scoreChucKSampleRate) * 60 * 30;

        while (captureEngine.isReady() && static_cast<int64_t> (captureEngine.getRenderedFrameCount()) < maxScoreFrames)
        {
            output.clear();
            captureEngine.process (input, output);
            captureEngine.pullParameterValuesFromGlobals();

            const auto commandId = juce::roundToInt (captureEngine.getParameterValue (commandIndex));
            if (commandId == 0)
                continue;

            ScoreCommandEvent event;
            event.command = static_cast<ScoreCommandId> (commandId);
            event.frame = static_cast<int64_t> (std::llround (captureEngine.getGlobalFloatValue ("weldScoreFrame")));
            event.text[0] = captureEngine.getGlobalStringValue ("weldScoreText0");
            event.text[1] = captureEngine.getGlobalStringValue ("weldScoreText1");

            for (int i = 0; i < ChucKScoreScript::argumentCount; ++i)
                event.args[static_cast<size_t> (i)] = captureEngine.getParameterValue (argIndices[static_cast<size_t> (i)]);

            events.push_back (std::move (event));
            captureEngine.setParameterValue (commandIndex, 0.0f);

            juce::AudioBuffer<float> ackInput (0, 1);
            juce::AudioBuffer<float> ackOutput (1, 1);
            captureEngine.process (ackInput, ackOutput);
            captureEngine.pullParameterValuesFromGlobals();

            if (commandId == static_cast<int> (ScoreCommandId::scoreComplete))
                return true;
        }

        error = "ChucK score script did not finish within the 30 minute capture limit";
        return false;
    }

    void executeCapturedScoreCommand (const ScoreCommandEvent& event)
    {
        scoreCommandText = event.text;
        executeChucKScoreCommand (event.command, event.args);
    }

    void applyCapturedTimelineMaps (const std::vector<ScoreCommandEvent>& events, size_t playEventIndex)
    {
        project.tempoMap.clear();
        project.timeSignatureMap.clear();
        project.phaseRotationMap.clear();
        project.scheduledStopBeat = -1.0;

        for (auto& state : project.states)
            for (auto& track : state.tracks)
                track.gainEvents.clear();

        auto bpm = project.globalTempoBpm;
        auto numerator = project.globalTimeSigNumerator;
        auto denominator = project.globalTimeSigDenominator;
        auto phaseBeats = project.globalPhaseBeats;
        auto beat = 0.0;
        auto lastFrame = events[playEventIndex].frame;

        project.tempoMap.push_back ({ 0.0, bpm });
        project.timeSignatureMap.push_back ({ 0.0, numerator, denominator });
        project.phaseRotationMap.push_back ({ 0.0, phaseBeats });

        const auto stateIndex = [] (double value) { return juce::roundToInt (value) - 1; };
        const auto trackIndex = [] (double value) { return juce::roundToInt (value) - 1; };

        for (size_t i = playEventIndex + 1; i < events.size(); ++i)
        {
            const auto& event = events[i];
            const auto elapsedFrames = juce::jmax<int64_t> (0, event.frame - lastFrame);
            beat += (static_cast<double> (elapsedFrames) / static_cast<double> (scoreChucKSampleRate)) * (bpm / 60.0);
            lastFrame = event.frame;

            if (event.command == ScoreCommandId::tempo)
            {
                bpm = juce::jlimit (20.0, 300.0, event.args[0]);
                project.tempoMap.push_back ({ beat, bpm });
            }
            else if (event.command == ScoreCommandId::meter)
            {
                numerator = juce::jlimit (1, 32, juce::roundToInt (event.args[0]));
                denominator = juce::jlimit (1, 32, juce::roundToInt (event.args[1]));
                project.timeSignatureMap.push_back ({ beat, numerator, denominator });
            }
            else if (event.command == ScoreCommandId::phase)
            {
                phaseBeats = event.args[0];
                project.phaseRotationMap.push_back ({ beat, phaseBeats });
            }
            else if (event.command == ScoreCommandId::trackGain
                     || event.command == ScoreCommandId::trackMute
                     || event.command == ScoreCommandId::trackSolo)
            {
                const auto s = stateIndex (event.args[0]);
                const auto t = trackIndex (event.args[1]);
                if (s >= 0 && s < static_cast<int> (project.states.size()))
                {
                    auto& state = project.states[static_cast<size_t> (s)];
                    if (t >= 0 && t < static_cast<int> (state.tracks.size()))
                    {
                        auto& track = state.tracks[static_cast<size_t> (t)];
                        if (event.command == ScoreCommandId::trackGain)
                            track.level = juce::jlimit (0.0f, 1.0f, static_cast<float> (event.args[2]));
                        else if (event.command == ScoreCommandId::trackMute)
                            track.muted = event.args[2] != 0.0;
                        else if (event.command == ScoreCommandId::trackSolo)
                            track.soloed = event.args[2] != 0.0;

                        if (event.command == ScoreCommandId::trackGain)
                            track.gainEvents.push_back ({ beat, effectiveTrackGain (project, s, t) });
                        else
                            for (int stateToUpdate = 0; stateToUpdate < static_cast<int> (project.states.size()); ++stateToUpdate)
                            {
                                auto& stateModel = project.states[static_cast<size_t> (stateToUpdate)];
                                for (int trackToUpdate = 0; trackToUpdate < static_cast<int> (stateModel.tracks.size()); ++trackToUpdate)
                                    stateModel.tracks[static_cast<size_t> (trackToUpdate)].gainEvents.push_back (
                                        { beat, effectiveTrackGain (project, stateToUpdate, trackToUpdate) });
                            }
                    }
                }
            }
            else if (event.command == ScoreCommandId::stop || event.command == ScoreCommandId::scoreComplete)
            {
                if (event.command == ScoreCommandId::stop)
                    project.scheduledStopBeat = beat;
                break;
            }
            else if (! isAudioTimelineCommand (event.command))
            {
                juce::Logger::writeToLog ("ChucK score command after play is not sample-scheduled in the current GUI: "
                                          + juce::String (static_cast<int> (event.command)));
            }
        }
    }

    void applyCapturedChucKScoreScript (const std::vector<ScoreCommandEvent>& events)
    {
        auto playEventIndex = events.size();

        for (size_t i = 0; i < events.size(); ++i)
        {
            if (events[i].command == ScoreCommandId::play)
            {
                playEventIndex = i;
                break;
            }

            if (events[i].command != ScoreCommandId::scoreComplete)
                executeCapturedScoreCommand (events[i]);
        }

        if (playEventIndex >= events.size())
            return;

        applyCapturedTimelineMaps (events, playEventIndex);
        mixer.refresh();
        score.syncControlsFromProject();
        score.repaint();
        arrangement.refresh();
        startPlayback();
    }

    void runChucKScoreScript (const juce::String& script)
    {
        std::vector<ScoreCommandEvent> events;
        juce::String captureError;
        if (captureChucKScoreScript (script, events, captureError))
        {
            chuckScoreRunning = false;
            applyCapturedChucKScoreScript (events);
            return;
        }

        juce::Logger::writeToLog ("ChucK score exact capture failed; falling back to live runner: " + captureError);
        prepareChucKScoreRunner();
        if (! chuckScorePrepared)
        {
            chuckScoreRunning = false;
            return;
        }

        auto program = ChucKScoreScript::buildProgram (script);
        const auto bindings = ChucKScoreScript::getParameterBindings();

        chuckScoreRunning = false;
        if (! scoreChucK.loadProgram (program.source, bindings))
        {
            juce::Logger::writeToLog ("ChucK score script compile failed: " + scoreChucK.getLastError());
            return;
        }

        scoreCommandParameterIndex = scoreChucK.getParameterIndex ("weldScoreCommand");
        for (int i = 0; i < ChucKScoreScript::argumentCount; ++i)
            scoreArgumentParameterIndices[static_cast<size_t> (i)] = scoreChucK.getParameterIndex ("weldScoreArg" + juce::String (i));

        if (scoreCommandParameterIndex < 0)
        {
            juce::Logger::writeToLog ("ChucK score script bridge command slot is unavailable");
            return;
        }

        for (const auto index : scoreArgumentParameterIndices)
        {
            if (index < 0)
            {
                juce::Logger::writeToLog ("ChucK score script bridge argument slots are unavailable");
                return;
            }
        }

        scoreChucK.setParameterValue (scoreCommandParameterIndex, 0.0f);
        chuckScoreRunning = true;
        advanceChucKScoreRunner (1.0 / 30.0);
    }

    juce::String scoreText (int index) const
    {
        if (index >= 0 && index < static_cast<int> (scoreCommandText.size()))
            return scoreCommandText[static_cast<size_t> (index)];

        return scoreChucK.getGlobalStringValue ("weldScoreText" + juce::String (index));
    }

    void advanceChucKScoreRunner (double seconds)
    {
        if (! chuckScoreRunning || ! scoreChucK.isReady())
            return;

        auto framesRemaining = juce::jlimit (1, scoreChucKSampleRate, juce::roundToInt (seconds * scoreChucKSampleRate));
        int guard = scoreChucKSampleRate;

        while (chuckScoreRunning && framesRemaining > 0 && guard-- > 0)
        {
            if (drainChucKScoreCommand())
                continue;

            const auto framesThisChunk = juce::jmin (64, framesRemaining);
            chuckScoreInput.setSize (0, framesThisChunk, false, false, true);
            chuckScoreOutput.setSize (1, framesThisChunk, false, false, true);
            chuckScoreOutput.clear();
            scoreChucK.process (chuckScoreInput, chuckScoreOutput);
            scoreChucK.pullParameterValuesFromGlobals();
            framesRemaining -= framesThisChunk;
        }

        static_cast<void> (drainChucKScoreCommand());
    }

    bool drainChucKScoreCommand()
    {
        if (scoreCommandParameterIndex < 0)
            return false;

        const auto commandId = juce::roundToInt (scoreChucK.getParameterValue (scoreCommandParameterIndex));
        if (commandId == 0)
            return false;

        std::array<double, ChucKScoreScript::argumentCount> args {};
        for (int i = 0; i < ChucKScoreScript::argumentCount; ++i)
        {
            const auto parameterIndex = scoreArgumentParameterIndices[static_cast<size_t> (i)];
            if (parameterIndex >= 0)
                args[static_cast<size_t> (i)] = scoreChucK.getParameterValue (parameterIndex);
        }

        scoreCommandText[0] = scoreChucK.getGlobalStringValue ("weldScoreText0");
        scoreCommandText[1] = scoreChucK.getGlobalStringValue ("weldScoreText1");
        executeChucKScoreCommand (static_cast<ScoreCommandId> (commandId), args);
        scoreChucK.setParameterValue (scoreCommandParameterIndex, 0.0f);
        if (commandId == static_cast<int> (ScoreCommandId::scoreComplete))
            acknowledgeChucKScoreCommand();
        return true;
    }

    void acknowledgeChucKScoreCommand()
    {
        chuckScoreInput.setSize (0, 1, false, false, true);
        chuckScoreOutput.setSize (1, 1, false, false, true);
        chuckScoreOutput.clear();
        scoreChucK.process (chuckScoreInput, chuckScoreOutput);
        scoreChucK.pullParameterValuesFromGlobals();
    }

    void executeChucKScoreCommand (ScoreCommandId command, const std::array<double, ChucKScoreScript::argumentCount>& args)
    {
        const auto stateIndex = [] (double value) { return juce::roundToInt (value) - 1; };
        const auto trackIndex = [] (double value) { return juce::roundToInt (value) - 1; };

        switch (command)
        {
            case ScoreCommandId::scoreClear:
                quietTransportForStructureEdit();
                project.states.clear();
                project.tempoMap.clear();
                project.timeSignatureMap.clear();
                project.phaseRotationMap.clear();
                project.scheduledStopBeat = -1.0;
                project.arrangementOrder.clear();
                refreshStructure (0);
                break;

            case ScoreCommandId::scoreComplete:
                arrangeStatesAsFiniteStateMachine (project);
                score.repaint();
                arrangement.refresh();
                chuckScoreRunning = false;
                break;

            case ScoreCommandId::play:
                startPlayback();
                break;

            case ScoreCommandId::stop:
                stopPlayback();
                break;

            case ScoreCommandId::tempo:
                project.globalTempoBpm = juce::jlimit (20.0, 300.0, args[0]);
                audio.loadProject (project);
                score.syncControlsFromProject();
                score.repaint();
                break;

            case ScoreCommandId::meter:
                project.globalTimeSigNumerator = juce::jlimit (1, 32, juce::roundToInt (args[0]));
                project.globalTimeSigDenominator = juce::jlimit (1, 32, juce::roundToInt (args[1]));
                audio.loadProject (project);
                score.syncControlsFromProject();
                score.repaint();
                break;

            case ScoreCommandId::phase:
                project.globalPhaseBeats = args[0];
                audio.loadProject (project);
                score.syncControlsFromProject();
                score.repaint();
                break;

            case ScoreCommandId::stateAdd:
                static_cast<void> (addStateFromCommand (scoreText (0), args[1], args[2], false));
                break;

            case ScoreCommandId::stateRemove:
                static_cast<void> (removeStateAt (stateIndex (args[0])));
                break;

            case ScoreCommandId::stateDuration:
            case ScoreCommandId::stateTail:
            {
                const auto index = stateIndex (args[0]);
                if (index >= 0 && index < static_cast<int> (project.states.size()))
                {
                    auto& stateToEdit = project.states[static_cast<size_t> (index)];
                    quietTransportForStructureEdit();
                    if (command == ScoreCommandId::stateDuration)
                        stateToEdit.durationBeats = juce::jlimit (1.0, 256.0, args[1]);
                    else
                        stateToEdit.tailBeats = juce::jlimit (0.0, 64.0, args[1]);
                    refreshStructure (index);
                }
                break;
            }

            case ScoreCommandId::stateName:
            {
                const auto index = stateIndex (args[0]);
                if (index >= 0 && index < static_cast<int> (project.states.size()))
                {
                    project.states[static_cast<size_t> (index)].name = scoreText (0);
                    refreshStructure (index);
                }
                break;
            }

            case ScoreCommandId::stateSelect:
                selectState (stateIndex (args[0]));
                break;

            case ScoreCommandId::stateConnect:
            {
                const auto from = stateIndex (args[0]);
                const auto to = stateIndex (args[1]);
                if (from >= 0 && from < static_cast<int> (project.states.size())
                    && to >= 0 && to < static_cast<int> (project.states.size())
                    && args[2] > 0.0
                    && std::isfinite (args[2]))
                {
                    auto& transitions = project.states[static_cast<size_t> (from)].transitions;
                    auto existing = std::find_if (transitions.begin(),
                                                  transitions.end(),
                                                  [to] (const StateTransitionModel& transition)
                                                  {
                                                      return transition.targetStateIndex == to;
                                                  });

                    if (existing != transitions.end())
                        existing->weight = args[2];
                    else
                        transitions.push_back ({ to, args[2] });

                    project.arrangementOrder = defaultArrangementOrder (project);
                    arrangeStatesAsFiniteStateMachine (project);
                    score.repaint();
                    arrangement.refresh();
                }
                break;
            }

            case ScoreCommandId::stateDisconnect:
            {
                const auto from = stateIndex (args[0]);
                const auto to = stateIndex (args[1]);
                if (from >= 0 && from < static_cast<int> (project.states.size()))
                {
                    auto& transitions = project.states[static_cast<size_t> (from)].transitions;
                    transitions.erase (std::remove_if (transitions.begin(),
                                                       transitions.end(),
                                                       [to] (const StateTransitionModel& transition)
                                                       {
                                                           return transition.targetStateIndex == to;
                                                       }),
                                       transitions.end());
                    project.arrangementOrder = defaultArrangementOrder (project);
                    arrangeStatesAsFiniteStateMachine (project);
                    score.repaint();
                    arrangement.refresh();
                }
                break;
            }

            case ScoreCommandId::stateClearConnections:
            {
                const auto index = stateIndex (args[0]);
                if (index >= 0 && index < static_cast<int> (project.states.size()))
                {
                    project.states[static_cast<size_t> (index)].transitions.clear();
                    project.arrangementOrder = defaultArrangementOrder (project);
                    arrangeStatesAsFiniteStateMachine (project);
                    score.repaint();
                    arrangement.refresh();
                }
                break;
            }

            case ScoreCommandId::trackAdd:
                static_cast<void> (addTrackFromCommand (stateIndex (args[0]),
                                                        languageFromName (scoreText (1)),
                                                        scoreText (0)));
                break;

            case ScoreCommandId::trackRemove:
                static_cast<void> (removeTrackAt (stateIndex (args[0]), trackIndex (args[1])));
                break;

            case ScoreCommandId::trackName:
            case ScoreCommandId::trackLanguage:
            case ScoreCommandId::trackGain:
            case ScoreCommandId::trackSync:
            case ScoreCommandId::trackTempo:
            case ScoreCommandId::trackMeter:
            case ScoreCommandId::trackPhase:
            case ScoreCommandId::trackCode:
            case ScoreCommandId::trackTemplate:
            case ScoreCommandId::trackClear:
            case ScoreCommandId::trackMute:
            case ScoreCommandId::trackSolo:
                applyChucKScoreTrackCommand (command, stateIndex (args[0]), trackIndex (args[1]), args);
                break;

            case ScoreCommandId::mixer:
                mixerVisible = args[0] != 0.0;
                layoutBottomView();
                break;

            case ScoreCommandId::none:
                break;
        }
    }

    void applyChucKScoreTrackCommand (ScoreCommandId command,
                                      int stateIndex,
                                      int trackIndex,
                                      const std::array<double, ChucKScoreScript::argumentCount>& args)
    {
        if (stateIndex < 0 || stateIndex >= static_cast<int> (project.states.size()))
            return;

        auto& stateToEdit = project.states[static_cast<size_t> (stateIndex)];
        if (trackIndex < 0 || trackIndex >= static_cast<int> (stateToEdit.tracks.size()))
            return;

        auto& track = stateToEdit.tracks[static_cast<size_t> (trackIndex)];
        const auto structuralEdit = command == ScoreCommandId::trackName
                                 || command == ScoreCommandId::trackLanguage
                                 || command == ScoreCommandId::trackCode
                                 || command == ScoreCommandId::trackTemplate
                                 || command == ScoreCommandId::trackClear;

        if (structuralEdit)
            quietTransportForStructureEdit();

        if (command == ScoreCommandId::trackName)
            track.name = scoreText (0);
        else if (command == ScoreCommandId::trackLanguage)
        {
            track.language = languageFromName (scoreText (0));
            if (track.code.trim().isEmpty())
                track.code = defaultCodeForLanguage (track.language);
        }
        else if (command == ScoreCommandId::trackGain)
            track.level = juce::jlimit (0.0f, 1.0f, static_cast<float> (args[2]));
        else if (command == ScoreCommandId::trackSync)
            track.tightlySynced = args[2] != 0.0;
        else if (command == ScoreCommandId::trackTempo)
            track.tempoBpm = juce::jlimit (20.0, 300.0, args[2]);
        else if (command == ScoreCommandId::trackMeter)
        {
            track.timeSigNumerator = juce::jlimit (1, 32, juce::roundToInt (args[2]));
            track.timeSigDenominator = juce::jlimit (1, 32, juce::roundToInt (args[3]));
        }
        else if (command == ScoreCommandId::trackPhase)
            track.phaseBeats = args[2];
        else if (command == ScoreCommandId::trackCode)
            track.code = decodeScoreText (scoreText (0));
        else if (command == ScoreCommandId::trackTemplate)
            track.code = defaultCodeForLanguage (track.language);
        else if (command == ScoreCommandId::trackClear)
            track.code.clear();
        else if (command == ScoreCommandId::trackMute)
            track.muted = args[2] != 0.0;
        else if (command == ScoreCommandId::trackSolo)
            track.soloed = args[2] != 0.0;

        if (structuralEdit)
            refreshStructure (stateIndex);
        else if (command == ScoreCommandId::trackGain
                 || command == ScoreCommandId::trackMute
                 || command == ScoreCommandId::trackSolo)
            refreshMixerControlChange (true);
        else
            refreshScoreControlChange();
    }

    void layoutBottomView()
    {
        const auto view = mixerVisible ? BottomView::mixer : currentView;

        tabs.setVisible (view == BottomView::stateTabs);
        arrangement.setVisible (view == BottomView::arrangement);
        mixer.setVisible (view == BottomView::mixer);

        tabs.setBounds (bottomBounds);
        arrangement.setBounds (bottomBounds);
        mixer.setBounds (bottomBounds);
    }

    ProjectModel project;
    PerformanceController audio;
    EmbeddedChucKEngine scoreChucK;
    ScoreMachineComponent score;
    SplitterComponent splitter;
    juce::TabbedComponent tabs;
    ArrangementComponent arrangement;
    MixerComponent mixer;
    juce::AudioBuffer<float> chuckScoreInput;
    juce::AudioBuffer<float> chuckScoreOutput;
    juce::Rectangle<int> bottomBounds;
    BottomView currentView = BottomView::stateTabs;
    bool mixerVisible = false;
    bool countingIn = false;
    bool playing = false;
    bool chuckScorePrepared = false;
    bool chuckScoreRunning = false;
    int dragStartTopHeight = 0;
    double topPanelRatio = defaultTopPanelRatio;
    double countInBeat = 0.0;
    double currentBeat = 0.0;
    int scoreCommandParameterIndex = -1;
    std::array<int, ChucKScoreScript::argumentCount> scoreArgumentParameterIndices {};
    std::array<juce::String, 2> scoreCommandText;
};

class MainWindow final : public juce::DocumentWindow
{
public:
    explicit MainWindow (juce::String name)
        : DocumentWindow (std::move (name),
                          juce::Colour (0xff101211),
                          juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (false);
        setResizable (true, false);
        setContentOwned (new MainComponent(), true);
        centreWithSize (1280, 800);
        setFullScreen (true);
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class AlchemyApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Alchemy"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace

START_JUCE_APPLICATION (AlchemyApplication)

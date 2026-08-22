#include "NassauAnaloguePlugin.h"
#include "IPlug_include_in_plug_src.h"
#include "nassau_presets.h"
#include "nassau_state.h"
#include <algorithm>
#include <cstdint>

using namespace iplug;

// nassau_presets.h's param-index constants are dependency-free plain ints
// (so they can be shared with the SDK-free Tests/preset_tests.cpp) and MUST
// match this file's EParams enum value-for-value (R4: append-only, so the
// enum is frozen -- this just guards against ever getting the two out of
// sync again). Mirrors NassauZermattPlugin.cpp's own static_assert block
// verbatim (docs/GATES.md G9's "copy the shape" instruction), extended to
// all 53 indices.
static_assert(nassau_presets::kMasterVolume == kMasterVolume, "preset index drift: kMasterVolume");
static_assert(nassau_presets::kOutputClip == kOutputClip, "preset index drift: kOutputClip");
static_assert(nassau_presets::kOsc1Wave == kOsc1Wave, "preset index drift: kOsc1Wave");
static_assert(nassau_presets::kOsc1Octave == kOsc1Octave, "preset index drift: kOsc1Octave");
static_assert(nassau_presets::kOsc1Fine == kOsc1Fine, "preset index drift: kOsc1Fine");
static_assert(nassau_presets::kOsc1PW == kOsc1PW, "preset index drift: kOsc1PW");
static_assert(nassau_presets::kOsc1Level == kOsc1Level, "preset index drift: kOsc1Level");
static_assert(nassau_presets::kOsc2Wave == kOsc2Wave, "preset index drift: kOsc2Wave");
static_assert(nassau_presets::kOsc2Octave == kOsc2Octave, "preset index drift: kOsc2Octave");
static_assert(nassau_presets::kOsc2Semi == kOsc2Semi, "preset index drift: kOsc2Semi");
static_assert(nassau_presets::kOsc2Fine == kOsc2Fine, "preset index drift: kOsc2Fine");
static_assert(nassau_presets::kOsc2PW == kOsc2PW, "preset index drift: kOsc2PW");
static_assert(nassau_presets::kOsc2Level == kOsc2Level, "preset index drift: kOsc2Level");
static_assert(nassau_presets::kOsc2Sync == kOsc2Sync, "preset index drift: kOsc2Sync");
static_assert(nassau_presets::kOsc2KeyTrack == kOsc2KeyTrack, "preset index drift: kOsc2KeyTrack");
static_assert(nassau_presets::kSubOctave == kSubOctave, "preset index drift: kSubOctave");
static_assert(nassau_presets::kSubLevel == kSubLevel, "preset index drift: kSubLevel");
static_assert(nassau_presets::kNoiseColor == kNoiseColor, "preset index drift: kNoiseColor");
static_assert(nassau_presets::kNoiseLevel == kNoiseLevel, "preset index drift: kNoiseLevel");
static_assert(nassau_presets::kEnvFAttack == kEnvFAttack, "preset index drift: kEnvFAttack");
static_assert(nassau_presets::kEnvFDecay == kEnvFDecay, "preset index drift: kEnvFDecay");
static_assert(nassau_presets::kEnvFSustain == kEnvFSustain, "preset index drift: kEnvFSustain");
static_assert(nassau_presets::kEnvFRelease == kEnvFRelease, "preset index drift: kEnvFRelease");
static_assert(nassau_presets::kEnvAAttack == kEnvAAttack, "preset index drift: kEnvAAttack");
static_assert(nassau_presets::kEnvADecay == kEnvADecay, "preset index drift: kEnvADecay");
static_assert(nassau_presets::kEnvASustain == kEnvASustain, "preset index drift: kEnvASustain");
static_assert(nassau_presets::kEnvARelease == kEnvARelease, "preset index drift: kEnvARelease");
static_assert(nassau_presets::kLfoWave == kLfoWave, "preset index drift: kLfoWave");
static_assert(nassau_presets::kLfoRate == kLfoRate, "preset index drift: kLfoRate");
static_assert(nassau_presets::kLfoDelay == kLfoDelay, "preset index drift: kLfoDelay");
static_assert(nassau_presets::kLfoPitchAmount == kLfoPitchAmount, "preset index drift: kLfoPitchAmount");
static_assert(nassau_presets::kLfoPwmAmount == kLfoPwmAmount, "preset index drift: kLfoPwmAmount");
static_assert(nassau_presets::kLpfSlope == kLpfSlope, "preset index drift: kLpfSlope");
static_assert(nassau_presets::kLpfCutoff == kLpfCutoff, "preset index drift: kLpfCutoff");
static_assert(nassau_presets::kLpfResonance == kLpfResonance, "preset index drift: kLpfResonance");
static_assert(nassau_presets::kLpfEnvAmount == kLpfEnvAmount, "preset index drift: kLpfEnvAmount");
static_assert(nassau_presets::kLpfKeyFollow == kLpfKeyFollow, "preset index drift: kLpfKeyFollow");
static_assert(nassau_presets::kLpfLfoAmount == kLpfLfoAmount, "preset index drift: kLpfLfoAmount");
static_assert(nassau_presets::kDrive == kDrive, "preset index drift: kDrive");
static_assert(nassau_presets::kHpfSlope == kHpfSlope, "preset index drift: kHpfSlope");
static_assert(nassau_presets::kHpfCutoff == kHpfCutoff, "preset index drift: kHpfCutoff");
static_assert(nassau_presets::kHpfKeyFollow == kHpfKeyFollow, "preset index drift: kHpfKeyFollow");
static_assert(nassau_presets::kPmEnvFToOsc2 == kPmEnvFToOsc2, "preset index drift: kPmEnvFToOsc2");
static_assert(nassau_presets::kPmEnvFToPw == kPmEnvFToPw, "preset index drift: kPmEnvFToPw");
static_assert(nassau_presets::kPolyphony == kPolyphony, "preset index drift: kPolyphony");
static_assert(nassau_presets::kVoiceMode == kVoiceMode, "preset index drift: kVoiceMode");
static_assert(nassau_presets::kGlideTime == kGlideTime, "preset index drift: kGlideTime");
static_assert(nassau_presets::kBendRange == kBendRange, "preset index drift: kBendRange");
static_assert(nassau_presets::kVelToVca == kVelToVca, "preset index drift: kVelToVca");
static_assert(nassau_presets::kVelToFilter == kVelToFilter, "preset index drift: kVelToFilter");
static_assert(nassau_presets::kStereoMode == kStereoMode, "preset index drift: kStereoMode");
static_assert(nassau_presets::kStereoDetune == kStereoDetune, "preset index drift: kStereoDetune");
static_assert(nassau_presets::kStereoSpread == kStereoSpread, "preset index drift: kStereoSpread");
static_assert(nassau_presets::kParamCount == kNumParams, "preset index drift: kNumParams");
static_assert(nassau_presets::kPresetCount == kNumPresets, "preset index drift: kNumPresets");

namespace {

// Translates one host IMidiMsg into a framework-free NoteEvent (DESIGN.md
// §10.1), preserving `mOffset` EXACTLY (G9.7: "an IMidiMsg at frame offset s
// reaches the core as a NoteEvent with sampleOffset == s -- the queue must
// not collapse offsets to 0"). Returns false for a message type that has no
// NoteEvent equivalent -- NoteOn/NoteOff/ControlChange(sustain/all-notes-off/
// all-sound-off)/PitchWheel is this instrument's ENTIRE MIDI surface
// (DESIGN.md §10.1: "the IPlug2 wrapper's entire MIDI job is translating
// IMidiMsg into that struct"). Channel is deliberately IGNORED (omni, G9.8):
// NoteEvent has no channel field and PLUG_DOES_MPE is 0 (config.h) -- MPE/
// per-channel routing is out of scope for v1 (DESIGN.md §13).
//
// UNVERIFIED (DESIGN.md §0.4, docs/GATES.md G0's own note): iPlug2's exact
// IMidiMsg member/enum spelling (StatusMsg()/NoteNumber()/Velocity()/
// PitchWheel()/kNoteOn/kNoteOff/kControlChange/kPitchWheel, and mOffset/
// mData1/mData2 as public fields) could not be confirmed against the actual
// SDK headers -- the SDK's iplug2 submodule is unchecked-out on this Linux
// dev box. This is written against the well-established, publicly
// documented iPlug2 IPlugMidi.h convention (also the shape used by iPlug2's
// own IPlugInstrument example project); it compiles and links on NEITHER
// platform this gate ran on, so it is unbuilt and unverified pending a
// Windows/macOS host -- see this gate's own report.
bool TranslateMidiMsg(const IMidiMsg& msg, NoteEvent& out) {
    out.sampleOffset = msg.mOffset;
    out.note = 0;
    out.value = 0.f;

    switch (msg.StatusMsg()) {
        case IMidiMsg::kNoteOn:
            // A NoteOn with velocity 0 is, by long-standing MIDI convention,
            // a note-off (some controllers send it this way instead of a
            // real 0x80 status to exploit running status) -- G9.8.
            if (msg.Velocity() <= 0) {
                out.type = NoteEvent::NoteOff;
                out.note = msg.NoteNumber();
                out.value = 0.f;
            } else {
                out.type = NoteEvent::NoteOn;
                out.note = msg.NoteNumber();
                out.value = static_cast<float>(msg.Velocity()) / 127.f;
            }
            return true;
        case IMidiMsg::kNoteOff:
            out.type = NoteEvent::NoteOff;
            out.note = msg.NoteNumber();
            // Note-off velocity is accepted (G9.8) even though DESIGN.md's
            // NoteEvent/synth_core.h do not currently use it for anything --
            // it is captured here so a future release-velocity feature needs
            // no wrapper change, and so this translation is a complete,
            // lossless mapping of the message.
            out.value = static_cast<float>(msg.Velocity()) / 127.f;
            return true;
        case IMidiMsg::kControlChange: {
            const int cc = static_cast<int>(msg.mData1);
            if (cc == 64) {  // sustain pedal (DESIGN.md §10.3)
                out.type = NoteEvent::Sustain;
                out.value = msg.mData2 >= 64 ? 1.f : 0.f;
                return true;
            }
            if (cc == 123) {  // all notes off (DESIGN.md §10.3)
                out.type = NoteEvent::AllNotesOff;
                return true;
            }
            if (cc == 120) {  // all sound off (DESIGN.md §10.3/§10.4)
                out.type = NoteEvent::AllSoundOff;
                return true;
            }
            return false;  // every other CC: no NoteEvent destination, ignored
        }
        case IMidiMsg::kPitchWheel:
            out.type = NoteEvent::PitchBend;
            out.value = static_cast<float>(msg.PitchWheel());  // -1..+1, DESIGN.md §10.1
            return true;
        default:
            // PolyAftertouch/ProgramChange/ChannelAftertouch: no NoteEvent
            // destination exists for any of these (DESIGN.md §10.1's closed
            // NoteEvent::Type set), ignored.
            return false;
    }
}

}  // namespace

Config NassauAnaloguePlugin::MakePluginConfig()
{
    return Config(
        PLUG_N_PARAMS,
        PLUG_N_PRESETS,
        PLUG_CHANNEL_IO,
        PLUG_NAME,
        PLUG_NAME,
        PLUG_MFR,
        PLUG_VERSION_HEX,
        PLUG_UNIQUE_ID,
        PLUG_MFR_ID,
        PLUG_LATENCY,
        PLUG_DOES_MIDI_IN,
        PLUG_DOES_MIDI_OUT,
        PLUG_DOES_MPE,
        PLUG_DOES_STATE_CHUNKS,
        // kInstrument, NOT kEffect (DESIGN.md §11 "Channel configuration":
        // "This is an instrument"; NassauZermatt always passes kEffect).
        // UNVERIFIED (DESIGN.md §0.4): see this file's own top-of-file note
        // and NassauAnaloguePlugin.h's G0-era comment -- the SDK's iplug2
        // submodule is unchecked-out on this dev box, so kInstrument's exact
        // spelling and existence in iplug::Config could not be confirmed.
        kInstrument,
        PLUG_HAS_UI,
        PLUG_WIDTH,
        PLUG_HEIGHT,
        false,
        0, 0, 0, 0,
        // BUNDLE_ID is iPlug2's per-format bundle identifier (e.g.
        // com.Nassau.audiounit.NassauAnalogue for AU). It MUST match each
        // format's CFBundleIdentifier: the AU Cocoa-UI query looks the
        // bundle up by this id, so a mismatch (as with a hand-set
        // PLUG_BUNDLE_ID) crashes auval.
        BUNDLE_ID,
        ""
    );
}

NassauAnaloguePlugin::NassauAnaloguePlugin(const InstanceInfo& info)
    // Qualify iplug::Plugin: in the CLAP build the CLAP-helpers headers also
    // define a `Plugin` template, so the unqualified name would be ambiguous.
    : iplug::Plugin(info, MakePluginConfig())
{
    // ---- Full 53-param surface (DESIGN.md §11) -----------------------------
    // Ranges/defaults transcribed verbatim from DESIGN.md §11's table.
    // Grouped exactly along the same gate boundaries as the EParams enum
    // comment in NassauAnaloguePlugin.h for cross-reference, though every
    // group below is wired together now (docs/GATES.md G9).

    // G0
    GetParam(kMasterVolume)->InitDouble("Volume", -6., -60., 12., 0.1, "dB");
    GetParam(kOutputClip)->InitBool("Clip", true);

    // G2 (oscillators, sub, noise)
    GetParam(kOsc1Wave)->InitEnum("Osc1 Wave", 0, {"Saw", "Pulse", "Tri"});
    GetParam(kOsc1Octave)->InitEnum("Osc1 Range", 1, {"16'", "8'", "4'", "2'"});
    GetParam(kOsc1Fine)->InitDouble("Osc1 Fine", 0., -25., 25., 0.1, "cents");
    GetParam(kOsc1PW)->InitDouble("Osc1 PW", 50., 5., 95., 0.1, "%");
    GetParam(kOsc1Level)->InitDouble("Osc1 Level", 100., 0., 100., 0.1, "%");
    GetParam(kOsc2Wave)->InitEnum("Osc2 Wave", 0, {"Saw", "Pulse", "Tri"});
    GetParam(kOsc2Octave)->InitEnum("Osc2 Range", 1, {"16'", "8'", "4'", "2'"});
    GetParam(kOsc2Semi)->InitInt("Osc2 Semi", 0, -12, 12, "semi");
    GetParam(kOsc2Fine)->InitDouble("Osc2 Fine", -3.5, -25., 25., 0.1, "cents");
    GetParam(kOsc2PW)->InitDouble("Osc2 PW", 50., 5., 95., 0.1, "%");
    GetParam(kOsc2Level)->InitDouble("Osc2 Level", 80., 0., 100., 0.1, "%");
    GetParam(kOsc2Sync)->InitBool("Osc2 Sync", false);
    GetParam(kOsc2KeyTrack)->InitBool("Osc2 KeyTrack", true);
    GetParam(kSubOctave)->InitEnum("Sub Range", 0, {"-1 Oct", "-2 Oct"});
    GetParam(kSubLevel)->InitDouble("Sub Level", 0., 0., 100., 0.1, "%");
    GetParam(kNoiseColor)->InitEnum("Noise Color", 0, {"White", "Pink"});
    GetParam(kNoiseLevel)->InitDouble("Noise Level", 0., 0., 100., 0.1, "%");

    // G3 (envelopes, LFO)
    GetParam(kEnvFAttack)->InitDouble("EnvF Attack", 2., 1., 10000., 0.1, "ms");
    GetParam(kEnvFDecay)->InitDouble("EnvF Decay", 400., 1., 10000., 0.1, "ms");
    GetParam(kEnvFSustain)->InitDouble("EnvF Sustain", 30., 0., 100., 0.1, "%");
    GetParam(kEnvFRelease)->InitDouble("EnvF Release", 300., 1., 10000., 0.1, "ms");
    GetParam(kEnvAAttack)->InitDouble("EnvA Attack", 2., 1., 10000., 0.1, "ms");
    GetParam(kEnvADecay)->InitDouble("EnvA Decay", 800., 1., 10000., 0.1, "ms");
    GetParam(kEnvASustain)->InitDouble("EnvA Sustain", 80., 0., 100., 0.1, "%");
    GetParam(kEnvARelease)->InitDouble("EnvA Release", 250., 1., 10000., 0.1, "ms");
    GetParam(kLfoWave)->InitEnum("LFO Wave", 0, {"Tri", "Saw", "Ramp", "Square", "S&H"});
    GetParam(kLfoRate)->InitDouble("LFO Rate", 5., 0.05, 30., 0.01, "Hz");
    GetParam(kLfoDelay)->InitDouble("LFO Delay", 0., 0., 3000., 0.1, "ms");
    GetParam(kLfoPitchAmount)->InitDouble("LFO->Pitch", 0., 0., 100., 0.1, "%");
    GetParam(kLfoPwmAmount)->InitDouble("LFO->PWM", 0., 0., 100., 0.1, "%");

    // G4 (low-pass filter)
    GetParam(kLpfSlope)->InitEnum("LPF Slope", 0, {"24 dB", "12 dB"});
    GetParam(kLpfCutoff)->InitDouble("LPF Cutoff", 2000., 20., 18000., 1.0, "Hz");
    GetParam(kLpfResonance)->InitDouble("LPF Resonance", 20., 0., 100., 0.1, "%");

    // G5 (LPF modulation)
    GetParam(kLpfEnvAmount)->InitDouble("LPF EnvF Amt", 40., -100., 100., 0.1, "%");
    GetParam(kLpfKeyFollow)->InitDouble("LPF KeyFollow", 50., 0., 100., 0.1, "%");
    GetParam(kLpfLfoAmount)->InitDouble("LPF LFO Amt", 0., 0., 100., 0.1, "%");

    // G6 (HPF, drive, Poly-Mod)
    GetParam(kDrive)->InitDouble("Drive", 15., 0., 100., 0.1, "%");
    GetParam(kHpfSlope)->InitEnum("HPF Slope", 0, {"12 dB", "24 dB"});
    GetParam(kHpfCutoff)->InitDouble("HPF Cutoff", 20., 20., 2000., 0.1, "Hz");
    GetParam(kHpfKeyFollow)->InitDouble("HPF KeyFollow", 0., 0., 100., 0.1, "%");
    GetParam(kPmEnvFToOsc2)->InitDouble("PolyMod EnvF->Osc2", 0., -100., 100., 0.1, "%");
    GetParam(kPmEnvFToPw)->InitDouble("PolyMod EnvF->PW", 0., -100., 100., 0.1, "%");

    // G7 (voice allocation, MIDI, polyphony, glide, velocity)
    GetParam(kPolyphony)->InitEnum("Polyphony", 2, {"4", "6", "8", "12", "16"});
    GetParam(kVoiceMode)->InitEnum("Voice Mode", 0, {"Poly", "Unison", "Mono"});
    GetParam(kGlideTime)->InitDouble("Glide", 0., 0., 2000., 0.1, "ms");
    GetParam(kBendRange)->InitInt("Bend Range", 2, 0, 24, "semi");
    GetParam(kVelToVca)->InitDouble("Vel->VCA", 40., 0., 100., 0.1, "%");
    GetParam(kVelToFilter)->InitDouble("Vel->Filter", 20., 0., 100., 0.1, "%");

    // G8 (stereo dual chain)
    GetParam(kStereoMode)->InitBool("Stereo", false);
    GetParam(kStereoDetune)->InitDouble("Stereo Detune", 6., 0., 25., 0.1, "cents");
    GetParam(kStereoSpread)->InitDouble("Stereo Spread", 70., 0., 100., 0.1, "%");

    // ---- Factory preset bank (kNumPresets == PLUG_N_PRESETS) -----------------
    // The full 12-preset bank, defined ONCE in Source/Plugin/nassau_presets.h
    // (also consumed directly by Tests/preset_tests.cpp, which has no SDK
    // link -- G9.5). Each preset's stored array is exactly the RAW
    // positional-double layout MakePresetFromNamedParams itself builds
    // internally (53 doubles, one per param, in enum order), so
    // MakePresetFromChunk reproduces it exactly -- this sidesteps
    // MakePresetFromNamedParams' own int-vs-double vararg pitfall entirely
    // (every value here is already the correct double for its param's stored
    // representation, enum/bool included) while still recalling through the
    // SAME "unframed chunk -> legacy fallback" path nassau_state.h's
    // UnserializeState already handles (see its file header and
    // Tests/state_tests.cpp's bad-magic case). Copied verbatim from
    // NassauZermattPlugin.cpp's own proven ctor loop.
    for (const auto& preset : nassau_presets::Presets()) {
        IByteChunk chunk;
        for (double v : preset.values) chunk.Put(&v);
        MakePresetFromChunk(preset.name, chunk);
    }

    // Seatbelt: the number of presets actually created must match the
    // compile-time count (and thus PLUG_N_PRESETS). Catches a forgotten
    // config.h bump.
    assert(NPresets() == kNumPresets && "Preset count out of sync with PLUG_N_PRESETS");

#if IPLUG_EDITOR
    using namespace iplug::igraphics;
    mMakeGraphicsFunc = [&]() {
        return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS,
                            GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
    };

    mLayoutFunc = [&](IGraphics* pGraphics) {
        NassauAnalogueUI::Layout(*pGraphics, *this);
    };
#endif
}

void NassauAnaloguePlugin::OnReset()
{
    const double sr = GetSampleRate();
    if (sr >= 8000. && sr <= 192000.) {
        mCore.init(static_cast<float>(sr));
        // Re-push every param so the freshly-initialised core matches host
        // state (mirrors NassauZermattPlugin::OnReset()'s own loop, now over
        // the FULL 53-param surface -- every index is wired, unlike G0).
        for (int i = 0; i < kNumParams; ++i)
            OnParamChange(i);
        // PLUG_LATENCY stays 0, permanently (DESIGN.md §2.1/§12, G9.13): no
        // oversampling anywhere in this design, so unlike NassauZermatt there
        // is no dynamic latency to (re-)report here -- SetLatency() is never
        // called.
    }
}

void NassauAnaloguePlugin::OnParamChange(int p)
{
    const IParam* param = GetParam(p);
    const float   f     = static_cast<float>(param->Value());
    switch (p) {
        case kMasterVolume: mCore.setMasterVolumeDb(f); break;
        case kOutputClip:   mCore.setOutputClip(f != 0.f); break;

        case kOsc1Wave:     mCore.setOsc1Wave(static_cast<SynthCore::Wave>(static_cast<int>(f))); break;
        case kOsc1Octave:   mCore.setOsc1Octave(static_cast<SynthCore::Octave>(static_cast<int>(f))); break;
        case kOsc1Fine:     mCore.setOsc1FineCents(f); break;
        case kOsc1PW:       mCore.setOsc1PwPercent(f); break;
        case kOsc1Level:    mCore.setOsc1LevelPercent(f); break;
        case kOsc2Wave:     mCore.setOsc2Wave(static_cast<SynthCore::Wave>(static_cast<int>(f))); break;
        case kOsc2Octave:   mCore.setOsc2Octave(static_cast<SynthCore::Octave>(static_cast<int>(f))); break;
        case kOsc2Semi:     mCore.setOsc2Semi(static_cast<int>(f)); break;
        case kOsc2Fine:     mCore.setOsc2FineCents(f); break;
        case kOsc2PW:       mCore.setOsc2PwPercent(f); break;
        case kOsc2Level:    mCore.setOsc2LevelPercent(f); break;
        case kOsc2Sync:     mCore.setOsc2Sync(f != 0.f); break;
        case kOsc2KeyTrack: mCore.setOsc2KeyTrack(f != 0.f); break;
        case kSubOctave:    mCore.setSubOctave(static_cast<SynthCore::SubOctave>(static_cast<int>(f))); break;
        case kSubLevel:     mCore.setSubLevelPercent(f); break;
        case kNoiseColor:   mCore.setNoiseColor(static_cast<SynthCore::NoiseColor>(static_cast<int>(f))); break;
        case kNoiseLevel:   mCore.setNoiseLevelPercent(f); break;

        case kEnvFAttack:   mCore.setEnvFAttackMs(f); break;
        case kEnvFDecay:    mCore.setEnvFDecayMs(f); break;
        case kEnvFSustain:  mCore.setEnvFSustainPercent(f); break;
        case kEnvFRelease:  mCore.setEnvFReleaseMs(f); break;
        case kEnvAAttack:   mCore.setEnvAAttackMs(f); break;
        case kEnvADecay:    mCore.setEnvADecayMs(f); break;
        case kEnvASustain:  mCore.setEnvASustainPercent(f); break;
        case kEnvARelease:  mCore.setEnvAReleaseMs(f); break;
        case kLfoWave:      mCore.setLfoWave(static_cast<SynthCore::LfoWave>(static_cast<int>(f))); break;
        case kLfoRate:      mCore.setLfoRateHz(f); break;
        case kLfoDelay:     mCore.setLfoDelayMs(f); break;
        case kLfoPitchAmount: mCore.setLfoPitchAmountPercent(f); break;
        case kLfoPwmAmount:   mCore.setLfoPwmAmountPercent(f); break;

        case kLpfSlope:     mCore.setLpfSlope(static_cast<SynthCore::LpfSlope>(static_cast<int>(f))); break;
        case kLpfCutoff:    mCore.setLpfCutoffHz(f); break;
        case kLpfResonance: mCore.setLpfResonancePercent(f); break;

        case kLpfEnvAmount:  mCore.setLpfEnvAmountPercent(f); break;
        case kLpfKeyFollow:  mCore.setLpfKeyFollowPercent(f); break;
        case kLpfLfoAmount:  mCore.setLpfLfoAmountPercent(f); break;

        case kDrive:        mCore.setDrivePercent(f); break;
        case kHpfSlope:      mCore.setHpfSlope(static_cast<SynthCore::HpfSlope>(static_cast<int>(f))); break;
        case kHpfCutoff:     mCore.setHpfCutoffHz(f); break;
        case kHpfKeyFollow:  mCore.setHpfKeyFollowPercent(f); break;
        case kPmEnvFToOsc2:  mCore.setPmEnvFToOsc2Percent(f); break;
        case kPmEnvFToPw:    mCore.setPmEnvFToPwPercent(f); break;

        case kPolyphony:  mCore.setPolyphony(static_cast<SynthCore::Polyphony>(static_cast<int>(f))); break;
        case kVoiceMode:  mCore.setVoiceMode(static_cast<SynthCore::VoiceMode>(static_cast<int>(f))); break;
        case kGlideTime:  mCore.setGlideTimeMs(f); break;
        case kBendRange:  mCore.setBendRangeSemitones(static_cast<int>(f)); break;
        case kVelToVca:    mCore.setVelToVcaPercent(f); break;
        case kVelToFilter: mCore.setVelToFilterPercent(f); break;

        case kStereoMode:    mCore.setStereoMode(f != 0.f); break;
        case kStereoDetune:  mCore.setStereoDetuneCents(f); break;
        case kStereoSpread:  mCore.setStereoSpreadPercent(f); break;

        default: break;
    }
}

// ---- Versioned, forward-compatible state (docs/GATES.md G9) ---------------
// Layout (see nassau_state.h): magic 'NsAn', uint32 version, uint32
// paramCount, then paramCount doubles via the base SerializeParams. The
// append-only param discipline (documented at the EParams enum) is what
// makes the tolerant read below safe. Design copied verbatim from
// NassauZermattPlugin::SerializeState/UnserializeState (proven; see
// docs/GATES.md G9 deliverables).

bool NassauAnaloguePlugin::SerializeState(IByteChunk& chunk) const
{
    chunk.PutBytes(nassau_state::kMagic, 4);
    const uint32_t version = nassau_state::kStateVersion;
    const uint32_t count   = static_cast<uint32_t>(NParams());
    chunk.Put(&version);
    chunk.Put(&count);
    return SerializeParams(chunk);
}

int NassauAnaloguePlugin::UnserializeState(const IByteChunk& chunk, int startPos)
{
    // Read + validate the magic. On mismatch, fall back to a legacy raw param
    // chunk -- this is ALSO the path a factory-preset chunk takes (built by
    // MakePresetFromChunk above, which is unframed raw doubles, exactly like
    // MakePresetFromNamedParams' own internal representation), not just a
    // hypothetical pre-versioning save (see nassau_state.h's file header).
    unsigned char magic[4] = {0, 0, 0, 0};
    int pos = chunk.GetBytes(magic, 4, startPos);
    if (pos < 0 || !nassau_state::MagicMatches(magic))
        return UnserializeParams(chunk, startPos);

    uint32_t version = 0, storedCount = 0;
    pos = chunk.Get(&version, pos);
    pos = chunk.Get(&storedCount, pos);
    if (pos < 0)
        return pos;
    (void) version; // only one framing version so far; kept for future migration.

    const auto plan = nassau_state::PlanUnserialize(
        storedCount, static_cast<uint32_t>(NParams()));

    // Read the params we understand; any current params beyond paramsToRead keep
    // their (already-initialised) defaults. Never read past the stored block.
    ENTER_PARAMS_MUTEX
    for (int i = 0; i < plan.paramsToRead && pos >= 0; ++i) {
        double v = 0.0;
        pos = chunk.Get(&v, pos);
        if (pos >= 0)
            GetParam(i)->Set(v);
    }
    LEAVE_PARAMS_MUTEX

    // Skip any trailing params a NEWER build wrote that this build doesn't know.
    if (pos >= 0 && plan.skipBytes > 0)
        pos += plan.skipBytes;

    // R9 note: OnParamReset(kPresetRecall) below re-derives mCore's state from
    // the just-restored IParam values via OnParamChange() -- it does not
    // itself call SetParameterValue or otherwise write a param, so this stays
    // within R9 ("the plugin never writes its own params": no
    // processor-INITIATED SetParameterValue). This mirrors
    // NassauZermattPlugin::UnserializeState's identical call.
    OnParamReset(kPresetRecall);
    return pos;
}

void NassauAnaloguePlugin::ProcessMidiMsg(const IMidiMsg& msg)
{
    // Queue exactly the message types TranslateMidiMsg() understands (see its
    // own comment for the full, closed list and the UNVERIFIED note on the
    // IMidiMsg API this is written against). G9.8: MIDI channel is ignored
    // (omni) -- no channel filtering here, matching PLUG_DOES_MPE 0.
    switch (msg.StatusMsg()) {
        case IMidiMsg::kNoteOn:
        case IMidiMsg::kNoteOff:
        case IMidiMsg::kControlChange:
        case IMidiMsg::kPitchWheel:
            mMidiQueue.Add(msg);
            break;
        default:
            break;
    }
}

void NassauAnaloguePlugin::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
    // PLUG_CHANNEL_IO "0-2": this is an instrument with NO audio input at all
    // (DESIGN.md §11) -- `inputs` is unused, unlike an effect wrapper.
    (void) inputs;

    const int n = std::min(nFrames, kMaxBlockSize);

    // Drain mMidiQueue (filled by ProcessMidiMsg() above, sample-accurately,
    // before this call -- standard iPlug2 sequencing) into the framework-free
    // NoteEvent array SynthCore::process() consumes (DESIGN.md §10.1). Every
    // message's mOffset is preserved EXACTLY into NoteEvent::sampleOffset
    // (G9.7) -- IMidiQueue keeps messages in mOffset order and Peek()/
    // Remove() drain them in that order, so no reordering or offset
    // collapsing happens here.
    int numEvents = 0;
    while (!mMidiQueue.Empty() && numEvents < kMaxEventsPerBlock) {
        const IMidiMsg& msg = mMidiQueue.Peek();
        if (TranslateMidiMsg(msg, mEventBuf[numEvents]))
            ++numEvents;
        mMidiQueue.Remove();
    }
    // Advance the queue past this block (iPlug2 convention: Flush(nFrames)
    // rebases any remaining offsets -- none should remain here since every
    // queued message's mOffset is < nFrames by construction, but this is the
    // same unconditional call NassauZermattPlugin-shaped wrappers make every
    // block regardless).
    mMidiQueue.Flush(n);

    mCore.process(numEvents > 0 ? mEventBuf : nullptr, numEvents, mOutL, mOutR, n);

    for (int i = 0; i < n; ++i) {
        outputs[0][i] = static_cast<sample>(mOutL[i]);
        outputs[1][i] = static_cast<sample>(mOutR[i]);
    }
}

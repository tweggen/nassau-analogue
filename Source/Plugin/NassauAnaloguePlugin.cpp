#include "NassauAnaloguePlugin.h"
#include "IPlug_include_in_plug_src.h"
#include "nassau_state.h"
#include <algorithm>
#include <cstdint>

using namespace iplug;

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
        // UNVERIFIED (DESIGN.md §0.4): the SDK's iplug2 submodule is
        // unchecked-out on this dev box, so kInstrument's exact spelling and
        // existence in iplug::Config could not be confirmed while G0 was
        // written. docs/GATES.md's own G0 note flags this as one of four
        // "received wisdom, not verified fact" assumptions; G9 MUST verify
        // it (and the other three) before writing anything else there.
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
    // ---- G0 param surface (indices 0-1 of the FINAL 53-param enum) --------
    // Ranges/defaults transcribed verbatim from DESIGN.md §11. Every other
    // index (2-52) is a frozen POSITION (R4) but has no live IParam yet --
    // see NassauAnaloguePlugin.h's EParams comment and kNumWiredParams. Each
    // later gate appends its own InitXxx() call(s) here, in enum order,
    // never reordering, and bumps kNumWiredParams to match.
    GetParam(kMasterVolume)->InitDouble("Volume", -6., -60., 12., 0.1, "dB");
    GetParam(kOutputClip)->InitBool("Clip", true);

    // No factory preset bank yet (G9 -- see config.h's PLUG_N_PRESETS note
    // and NassauAnaloguePlugin.h's kNumPresets note: nassau_presets.h and the
    // MakePresetFromChunk loop both land there, once the full 53-param
    // surface each preset needs to specify actually exists).

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
        // Re-push every WIRED param (kNumWiredParams, G0: just the two
        // below) so the freshly-initialised core matches host state. NOT a
        // loop to kNumParams (53) -- indices past kNumWiredParams have no
        // live IParam yet (see NassauAnaloguePlugin.h's EParams comment).
        for (int i = 0; i < kNumWiredParams; ++i)
            OnParamChange(i);
        // Unlike NassauZermatt there is no dynamic latency to (re-)report
        // here: PLUG_LATENCY stays 0 permanently (DESIGN.md §2.1/§12 -- no
        // oversampling anywhere in this design, so SetLatency() is never
        // called, matching G6/G9/G11's own "PLUG_LATENCY stays 0" ACs).
    }
}

void NassauAnaloguePlugin::OnParamChange(int p)
{
    const IParam* param = GetParam(p);
    const float   f     = static_cast<float>(param->Value());
    switch (p) {
        case kMasterVolume: mCore.setMasterVolumeDb(f);  break;
        case kOutputClip:   mCore.setOutputClip(f != 0.f); break;
        // Indices 2-52: not yet wired (their gate hasn't landed the
        // corresponding SynthCore setter call here yet) -- see
        // NassauAnaloguePlugin.h's EParams/kNumWiredParams comment. Silently
        // ignored rather than asserting: OnReset() never calls this for an
        // index >= kNumWiredParams, but a host is free to probe any
        // registered-or-not param index and R9 forbids the processor from
        // reacting in a way that could surprise it.
        default: break;
    }
}

// ---- Versioned, forward-compatible state (docs/GATES.md, lands fully G9) --
// Layout (see nassau_state.h): magic 'NsAn', uint32 version, uint32
// paramCount, then paramCount doubles via the base SerializeParams. The
// append-only param discipline (documented at the EParams enum) is what
// makes the tolerant read below safe. Design copied verbatim from
// NassauZermatt's own proven NassauZermattPlugin::SerializeState/
// UnserializeState (docs/GATES.md G0's config.h note: "PLUG_DOES_STATE_CHUNKS
// 0, yet SerializeState IS overridden -- copy zermatt's stance verbatim, it
// is proven").

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
    // chunk -- this is ALSO the path a factory-preset chunk takes once G9
    // builds one via MakePresetFromChunk (unframed raw doubles, exactly like
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

    // Read the params we understand; any current params beyond paramsToRead
    // keep their (already-initialised) defaults. Never read past the stored
    // block.
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

    OnParamReset(kPresetRecall);
    return pos;
}

void NassauAnaloguePlugin::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
    // PLUG_CHANNEL_IO "0-2": this is an instrument with NO audio input at all
    // (DESIGN.md §11) -- `inputs` is unused, unlike NassauZermatt's effect
    // wrapper.
    (void) inputs;

    const int n = std::min(nFrames, kMaxBlockSize);

    // G0: MIDI (ProcessMidiMsg -> IMidiQueue -> NoteEvent array) is not wired
    // yet -- that is G9's job (see NassauAnaloguePlugin.h's own note; one of
    // the four iPlug2 assumptions docs/GATES.md's G0 flags as unverified,
    // DESIGN.md §0.4). SynthCore::process() ignores events entirely at G0
    // regardless (synth_core.h's class comment) and writes exact silence, so
    // passing events == nullptr here changes nothing observable yet.
    mCore.process(nullptr, 0, mOutL, mOutR, n);

    for (int i = 0; i < n; ++i) {
        outputs[0][i] = static_cast<sample>(mOutL[i]);
        outputs[1][i] = static_cast<sample>(mOutR[i]);
    }
}

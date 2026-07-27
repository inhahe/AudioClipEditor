// Headless self-test of the audio backend (decode / encode / peaks / slicing).
// Run with:  AudioClipEditor.exe --selftest
// Results are written to selftest.log next to the exe.
#include "audio_buffer.h"
#include "decoder.h"
#include "encoder.h"
#include "dsp.h"
#include "document.h"
#include "engine.h"
#include "layout.h"
#include "transport.h"
#include "selhistory.h"
#include "snap.h"
#include "waveform.h"
#include <windows.h>
#include <shlwapi.h>
#include <string>
#include <cmath>
#include <cstdio>
#include <vector>

static std::wstring exeDir() {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path); return path;
}

static AudioBufferPtr makeSine(int rate, double seconds, double freq) {
    auto b = std::make_shared<AudioBuffer>();
    b->sampleRate = rate; b->channels = 2;
    int64_t nf = (int64_t)(rate * seconds);
    b->samples.resize((size_t)nf * 2);
    for (int64_t i = 0; i < nf; ++i) {
        float v = (float)(0.5 * std::sin(2.0 * 3.14159265358979 * freq * i / rate));
        b->samples[i * 2] = v; b->samples[i * 2 + 1] = v;
    }
    return b;
}

static double rms(const AudioBuffer& b) {
    double s = 0; for (float v : b.samples) s += (double)v * v;
    return b.samples.empty() ? 0 : std::sqrt(s / b.samples.size());
}

int runSelfTest() {
    std::wstring dir = exeDir();
    std::wstring logPath = dir + L"\\selftest.log";
    FILE* log = _wfopen(logPath.c_str(), L"w, ccs=UTF-8");
    auto out = [&](const std::wstring& s) { if (log) { fwprintf(log, L"%ls\n", s.c_str()); fflush(log); } };

    int pass = 0, fail = 0;
    auto check = [&](bool ok, const std::wstring& name, const std::wstring& detail = L"") {
        if (ok) { pass++; out(L"[PASS] " + name + (detail.empty() ? L"" : L"  (" + detail + L")")); }
        else    { fail++; out(L"[FAIL] " + name + (detail.empty() ? L"" : L"  (" + detail + L")")); }
    };

    if (!mfio::startup()) { out(L"[FAIL] MFStartup"); if (log) fclose(log); return 2; }
    out(L"Media Foundation started.");

    const int rate = 48000;
    auto sine = makeSine(rate, 1.0, 440.0);
    out(L"Generated 1s stereo 440Hz sine, frames=" + std::to_wstring(sine->frames()) +
        L" rms=" + std::to_wstring(rms(*sine)));

    // PeakCache
    PeakCache pc; pc.build(*sine);
    check(!pc.empty() && pc.peakInRange(0, sine->frames()) > 0.3f, L"PeakCache build",
          L"peak=" + std::to_wstring(pc.peakInRange(0, sine->frames())));

    // WAV round-trip
    std::wstring wav = dir + L"\\_selftest.wav";
    mfio::ExportOptions o; o.format = mfio::ExportFormat::WAV; o.sampleRate = rate; o.channels = 2;
    std::wstring err;
    bool wok = mfio::encodeFile(wav, *sine, o, &err);
    check(wok, L"WAV encode", err);
    if (wok) {
        auto dec = mfio::decodeFile(wav, rate, &err);
        bool ok = dec && std::llabs(dec->frames() - sine->frames()) < rate / 10 && rms(*dec) > 0.1;
        check(ok, L"WAV decode round-trip",
              dec ? (L"frames=" + std::to_wstring(dec->frames()) + L" rms=" + std::to_wstring(rms(*dec))) : err);
    }

    // Mono downmix WAV
    std::wstring wavm = dir + L"\\_selftest_mono.wav";
    mfio::ExportOptions om = o; om.channels = 1;
    bool mok = mfio::encodeFile(wavm, *sine, om, &err);
    check(mok, L"WAV mono downmix encode", err);
    if (mok) {
        auto dec = mfio::decodeFile(wavm, rate, &err);
        check(dec && rms(*dec) > 0.1, L"WAV mono decode", dec ? L"" : err);
    }

    // Compressed formats (best-effort; may be unsupported on some systems)
    struct { mfio::ExportFormat f; const wchar_t* ext; const wchar_t* name; } fmts[] = {
        { mfio::ExportFormat::MP3, L"_selftest.mp3", L"MP3 encode" },
        { mfio::ExportFormat::AAC, L"_selftest.m4a", L"AAC encode" },
        { mfio::ExportFormat::WMA, L"_selftest.wma", L"WMA encode" },
    };
    for (auto& F : fmts) {
        std::wstring p = dir + L"\\" + F.ext;
        mfio::ExportOptions co; co.format = F.f; co.sampleRate = rate; co.channels = 2; co.bitrateKbps = 192;
        std::wstring e2;
        bool ok = mfio::encodeFile(p, *sine, co, &e2);
        // These are informational, not hard failures.
        out((ok ? L"[ OK ] " : L"[SKIP] ") + std::wstring(F.name) + (ok ? L"" : L"  (" + e2 + L")"));
        if (ok) {
            auto dec = mfio::decodeFile(p, rate, &e2);
            out(L"       decode " + std::wstring(F.name) + L": " +
                (dec ? (L"frames=" + std::to_wstring(dec->frames())) : (L"failed - " + e2)));
        }
    }

    // DSP: speech loudness + noise reduction
    {
        double loud = dsp::speechLoudness(*sine);
        check(loud > 0.05, L"speechLoudness on sine", L"loud=" + std::to_wstring(loud));

        // sine + white noise
        auto noisy = makeSine(rate, 1.0, 300.0);
        unsigned seed = 12345u;
        for (auto& s : noisy->samples) {
            seed = seed * 1103515245u + 12345u;
            float n = (float)((int)((seed >> 16) & 0x7fff)) / 32768.0f - 0.5f;
            s += n * 0.15f;
        }
        dsp::NROptions nro; nro.algorithm = dsp::NRAlgorithm::SpectralSubtraction; nro.strength = dsp::NRStrength::Medium;
        auto clean = dsp::denoise(*noisy, nro);
        bool ok = clean && clean->channels == noisy->channels &&
                  std::llabs(clean->frames() - noisy->frames()) < rate / 10;
        check(ok, L"denoise spectral-subtraction",
              clean ? (L"frames=" + std::to_wstring(clean->frames()) + L" rms=" + std::to_wstring(rms(*clean))) : L"null");

        dsp::NROptions nrw; nrw.algorithm = dsp::NRAlgorithm::Wiener; nrw.strength = dsp::NRStrength::Aggressive;
        auto clean2 = dsp::denoise(*noisy, nrw);
        check(clean2 && clean2->frames() > 0, L"denoise wiener", clean2 ? L"" : L"null");
    }

    // DSP: Audacity-style profile-based noise reduction
    {
        // 2 s stereo buffer: constant white noise + a 440 Hz tone only in the middle.
        const double ampNoise = 0.05, ampTone = 0.4;
        const int64_t nf = rate * 2;
        const int64_t toneA = (int64_t)(rate * 0.75), toneB = (int64_t)(rate * 1.25);
        auto buf = std::make_shared<AudioBuffer>();
        buf->sampleRate = rate; buf->channels = 2;
        buf->samples.resize((size_t)nf * 2);
        unsigned seed = 777u;
        for (int64_t i = 0; i < nf; ++i) {
            float t = 0.0f;
            if (i >= toneA && i < toneB)
                t = (float)(ampTone * std::sin(2.0 * 3.14159265358979 * 440.0 * i / rate));
            for (int c = 0; c < 2; ++c) {
                seed = seed * 1103515245u + 12345u;
                float n = ((float)((seed >> 16) & 0x7fff) / 32768.0f - 0.5f) * 2.0f;
                buf->samples[i * 2 + c] = t + n * (float)ampNoise;
            }
        }

        // RMS over a frame range of one buffer (both channels).
        auto rmsRange = [](const AudioBuffer& b, int64_t a, int64_t z) {
            double s = 0; int64_t n = 0;
            for (int64_t i = a; i < z; ++i)
                for (int c = 0; c < b.channels; ++c) { double v = b.samples[i * b.channels + c]; s += v * v; n++; }
            return n ? std::sqrt(s / n) : 0.0;
        };

        // Profile from the first 0.5 s (noise only).
        auto noiseSlice = std::make_shared<AudioBuffer>();
        noiseSlice->sampleRate = rate; noiseSlice->channels = 2;
        noiseSlice->samples.assign(buf->samples.begin(), buf->samples.begin() + (size_t)(rate / 2) * 2);
        dsp::NoiseProfile prof = dsp::computeNoiseProfile(*noiseSlice);
        check(prof.valid() && prof.sampleRate == rate, L"noise profile capture",
              L"windows=" + std::to_wstring(prof.windows) + L" bins=" + std::to_wstring(prof.means.size()));

        // Too-short selection (< one STFT window) -> invalid profile.
        auto shortSlice = std::make_shared<AudioBuffer>();
        shortSlice->sampleRate = rate; shortSlice->channels = 2;
        shortSlice->samples.assign(buf->samples.begin(), buf->samples.begin() + 1000 * 2);
        dsp::NoiseProfile shortProf = dsp::computeNoiseProfile(*shortSlice);
        check(!shortProf.valid(), L"noise profile too short rejected");

        // Reduce at 20 dB: noise floor drops ~20 dB, tone survives.
        dsp::NRProfileOptions po; po.reductionDb = 20.0f; po.sensitivity = 6.0f; po.freqSmoothingBands = 0;
        auto red = dsp::denoiseWithProfile(*buf, prof, po);
        bool okRed = red && red->frames() == buf->frames() && red->channels == 2;
        check(okRed, L"profile NR output shape",
              red ? (L"frames=" + std::to_wstring(red->frames())) : L"null");
        if (okRed) {
            // Noise-only region: measure away from edges/tone (0.1 s .. 0.6 s).
            double nBefore = rmsRange(*buf, rate / 10, (int64_t)(rate * 0.6));
            double nAfter  = rmsRange(*red, rate / 10, (int64_t)(rate * 0.6));
            double attenDb = 20.0 * std::log10(nAfter / nBefore);
            check(attenDb < -14.0, L"profile NR noise attenuation",
                  L"atten=" + std::to_wstring(attenDb) + L" dB (want <= -14, target -20)");

            // Tone region interior (skip 0.1 s at each edge for attack/release).
            double tBefore = rmsRange(*buf, toneA + rate / 10, toneB - rate / 10);
            double tAfter  = rmsRange(*red, toneA + rate / 10, toneB - rate / 10);
            check(tAfter > 0.7 * tBefore, L"profile NR preserves tone",
                  L"before=" + std::to_wstring(tBefore) + L" after=" + std::to_wstring(tAfter));
        }

        // Reduce - residue == original (defaults: 6 dB / 6.00 / 6 bands).
        dsp::NRProfileOptions pd;
        auto r1 = dsp::denoiseWithProfile(*buf, prof, pd);
        dsp::NRProfileOptions pr = pd; pr.residue = true;
        auto r2 = dsp::denoiseWithProfile(*buf, prof, pr);
        if (r1 && r2 && r1->samples.size() == buf->samples.size() && r2->samples.size() == buf->samples.size()) {
            double maxDiff = 0;
            for (size_t i = 0; i < buf->samples.size(); ++i) {
                double d = std::fabs((double)r1->samples[i] - (double)r2->samples[i] - (double)buf->samples[i]);
                if (d > maxDiff) maxDiff = d;
            }
            check(maxDiff < 1e-3, L"profile NR reduce-residue identity",
                  L"maxDiff=" + std::to_wstring(maxDiff));
        } else {
            check(false, L"profile NR reduce-residue identity", L"null or size mismatch");
        }

        // Sample-rate mismatch -> rejected.
        dsp::NoiseProfile wrongRate = prof; wrongRate.sampleRate = 44100;
        check(dsp::denoiseWithProfile(*buf, wrongRate, pd) == nullptr, L"profile NR rate mismatch rejected");

        // Dispatch through dsp::denoise with algorithm=Profile.
        dsp::NROptions nrp; nrp.algorithm = dsp::NRAlgorithm::Profile;
        nrp.profile = pd; nrp.noiseProfile = &prof;
        auto viaDenoise = dsp::denoise(*buf, nrp);
        check(viaDenoise && viaDenoise->frames() == buf->frames(), L"denoise dispatch profile algo",
              viaDenoise ? L"" : L"null");
        nrp.noiseProfile = nullptr;
        check(dsp::denoise(*buf, nrp) == nullptr, L"denoise profile without capture rejected");
    }

    // Export sample-rate conversion + bit depths
    {
        struct { int rate; int bits; const wchar_t* tag; } cases[] = {
            { 44100, 16, L"WAV 44100/16" },
            { 48000, 24, L"WAV 48000/24" },
            { 96000, 32, L"WAV 96000/32f" },
        };
        for (auto& C : cases) {
            std::wstring p = dir + L"\\_selftest_" + std::to_wstring(C.rate) + L"_" +
                             std::to_wstring(C.bits) + L".wav";
            mfio::ExportOptions eo; eo.format = mfio::ExportFormat::WAV;
            eo.sampleRate = C.rate; eo.bitsPerSample = C.bits; eo.channels = 2;
            std::wstring e2;
            bool wok2 = mfio::encodeFile(p, *sine, eo, &e2);
            if (wok2) {
                auto dec = mfio::decodeFile(p, rate, &e2);   // decode back to project rate
                // duration must be preserved (within 0.1s) despite the rate change
                bool ok = dec && std::llabs(dec->frames() - sine->frames()) < rate / 10 && rms(*dec) > 0.1;
                check(ok, std::wstring(L"export ") + C.tag,
                      dec ? (L"frames=" + std::to_wstring(dec->frames()) + L" rms=" + std::to_wstring(rms(*dec))) : e2);
            } else {
                check(false, std::wstring(L"export ") + C.tag, e2);
            }
        }
    }

    // Slicing (crop) sanity
    auto slice = std::make_shared<AudioBuffer>();
    slice->sampleRate = rate; slice->channels = 2;
    int64_t f0 = rate / 4, f1 = rate / 2;
    slice->samples.assign(sine->samples.begin() + f0 * 2, sine->samples.begin() + f1 * 2);
    check(slice->frames() == (f1 - f0), L"Buffer slice length",
          L"got=" + std::to_wstring(slice->frames()) + L" want=" + std::to_wstring(f1 - f0));

    // Document project round-trip (.acep v3) + library sort / reorder
    {
        Document d; d.init(rate);
        auto mk = [&](const wchar_t* nm, uint64_t ts) {
            int id = d.addClip(nm, makeSine(rate, 0.2, 300.0), L"", L"add");
            if (Clip* c = d.project().findClip(id)) c->timestamp = ts;
            return id;
        };
        int charlieId = mk(L"Charlie", 300); mk(L"alpha", 100); mk(L"Bravo", 200);

        // View state (selection + playhead) rides along with the project.
        d.view().selClipId = charlieId;
        d.view().selStart = 1000; d.view().selEnd = 5000;
        d.view().playheadFrame = 7777;

        std::wstring proj = dir + L"\\_selftest.acep";
        check(d.saveProject(proj), L"project save (.acep)");
        Document d2; d2.init(rate);
        bool lok = d2.loadProject(proj);
        auto& lib = d2.project().library;
        check(lok && lib.size() == 3, L"project load (.acep)", L"clips=" + std::to_wstring(lib.size()));
        if (lok && lib.size() == 3) {
            check(lib[0].name == L"Charlie" && lib[1].name == L"alpha" && lib[2].name == L"Bravo",
                  L"project load preserves order");
            check(lib[0].timestamp == 300 && lib[1].timestamp == 100 && lib[2].timestamp == 200,
                  L"project load preserves timestamps");
            const ViewState& v = d2.view();
            check(v.selClipId == charlieId && v.selStart == 1000 && v.selEnd == 5000 &&
                  v.playheadFrame == 7777, L"project load preserves selection + playhead",
                  L"clip=" + std::to_wstring(v.selClipId) + L" sel=" +
                  std::to_wstring(v.selStart) + L".." + std::to_wstring(v.selEnd) +
                  L" playhead=" + std::to_wstring(v.playheadFrame));
            d2.sortLibrary(true);   // case-insensitive name: alpha, Bravo, Charlie
            auto& L2 = d2.project().library;
            check(L2[0].name == L"alpha" && L2[1].name == L"Bravo" && L2[2].name == L"Charlie",
                  L"sortLibrary by name");
            d2.sortLibrary(false);  // time oldest-first: 100, 200, 300
            auto& L3 = d2.project().library;
            check(L3[0].timestamp == 100 && L3[1].timestamp == 200 && L3[2].timestamp == 300,
                  L"sortLibrary by time");
            int firstId = L3[0].id;
            d2.moveClipInLibrary(firstId, 3);   // move head to the end
            auto& L4 = d2.project().library;
            check(L4[2].id == firstId, L"moveClipInLibrary reorder");

            // A selection pointing at a clip that no longer exists is dropped.
            d2.view().selClipId = 4242; d2.view().selStart = 10; d2.view().selEnd = 20;
            d2.saveProject(proj);
            Document d3; d3.init(rate);
            check(d3.loadProject(proj) && d3.view().selClipId == -1 &&
                  d3.view().selEnd == 0, L"project load drops a stale selection");
        }
        DeleteFileW(proj.c_str());
    }

    // clampSelection: the rule that decides whether a waveform selection survives an
    // edit. It is only allowed to narrow a selection, never to discard one that still
    // works -- removing a clip's placement from a track used to wipe the selection on
    // the corresponding library clip even though its audio was untouched.
    {
        Document d; d.init(rate);
        int keep = d.addClip(L"keep", makeSine(rate, 1.0, 300.0), L"", L"add");
        int other = d.addClip(L"other", makeSine(rate, 1.0, 400.0), L"", L"add");
        const int64_t nf = d.project().findClip(keep)->frames();
        int trackId = d.project().tracks[0].id;
        d.placeClip(trackId, other, 0, L"place");

        // The reported bug: an edit elsewhere in the project leaves it alone.
        int id = keep; int64_t s = 1000, e = 5000;
        d.removePlaced(trackId, 0, L"remove placement");
        clampSelection(d.project(), id, s, e);
        check(id == keep && s == 1000 && e == 5000,
              L"selection survives removing an unrelated placement",
              L"clip=" + std::to_wstring(id) + L" " + std::to_wstring(s) +
              L".." + std::to_wstring(e));

        // Deleting a *different* clip likewise leaves it alone.
        id = keep; s = 1000; e = 5000;
        d.removeClip(other);
        clampSelection(d.project(), id, s, e);
        check(id == keep && s == 1000 && e == 5000,
              L"selection survives deleting another clip");

        // Deleting the clip it belongs to drops it.
        id = keep; s = 1000; e = 5000;
        Document d2; d2.init(rate);
        int gone = d2.addClip(L"gone", makeSine(rate, 1.0, 300.0), L"", L"add");
        id = gone;
        d2.removeClip(gone);
        clampSelection(d2.project(), id, s, e);
        check(id == -1 && s == 0 && e == 0, L"selection dropped when its clip is deleted");

        // A range running past the end of a shortened buffer is narrowed, not lost.
        id = keep; s = nf - 100; e = nf + 5000;
        clampSelection(d.project(), id, s, e);
        check(id == keep && s == nf - 100 && e == nf,
              L"selection past the buffer end is clamped, not dropped",
              L"clip=" + std::to_wstring(id) + L" " + std::to_wstring(s) +
              L".." + std::to_wstring(e) + L" (nf=" + std::to_wstring(nf) + L")");

        // ...but one that survives as an empty range is dropped.
        id = keep; s = nf + 10; e = nf + 20;
        clampSelection(d.project(), id, s, e);
        check(id == -1 && s == 0 && e == 0, L"selection entirely past the end is dropped");

        // "No selection" stays that way.
        id = -1; s = 0; e = 0;
        clampSelection(d.project(), id, s, e);
        check(id == -1 && s == 0 && e == 0, L"clampSelection leaves an empty selection alone");
    }

    // Library drop geometry (layout.h): a 5-card grid reflowed 3-per-row.
    //   row 0: [0][1][2]   x = 0,100,200   y = 0..80
    //   row 1: [3][4]      x = 0,100       y = 100..180
    // Row 0's tail (x >= 300) and row 1's head are the same *index* but different
    // *places*; the caret must follow the cursor, or dropping at the end of a row
    // looks impossible.
    {
        std::vector<RECT> g;
        for (int i = 0; i < 5; ++i) {
            int col = i % 3, row = i / 3;
            g.push_back(RECT{ col * 100, row * 100, col * 100 + 90, row * 100 + 80 });
        }
        auto idx = [&](int x, int y) { return layout::insertIndex(g, POINT{ x, y }); };
        auto anc = [&](int x, int y) {
            POINT p{ x, y }; return layout::caretAnchor(g, p, layout::insertIndex(g, p));
        };
        check(idx(10, 40) == 0, L"library drop: left of the first card inserts at 0");
        check(idx(160, 40) == 2, L"library drop: past a card's centre inserts after it");
        check(idx(350, 40) == 3, L"library drop: row 0's empty tail inserts at 3");
        check(idx(10, 140) == 3, L"library drop: row 1's head also inserts at 3");
        check(idx(350, 140) == 5, L"library drop: past the last card appends");
        check(idx(200, 400) == 5, L"library drop: below every row appends");

        // The regression: aiming at the end of row 0 used to draw the caret at the
        // head of row 1, so there appeared to be no way to drop there.
        layout::CaretAnchor tail = anc(350, 40);
        check(tail.card == 2 && tail.trailing,
              L"library caret: row 0's tail trails card 2",
              L"card=" + std::to_wstring(tail.card) +
              L" trailing=" + std::to_wstring((int)tail.trailing));
        layout::CaretAnchor head = anc(10, 140);
        check(head.card == 3 && !head.trailing,
              L"library caret: row 1's head leads card 3",
              L"card=" + std::to_wstring(head.card) +
              L" trailing=" + std::to_wstring((int)head.trailing));
        // Same index, two different carets -- that is the whole point.
        check(idx(350, 40) == idx(10, 140) && tail.card != head.card,
              L"library caret: one index, two places");

        layout::CaretAnchor end = anc(350, 140);
        check(end.card == 4 && end.trailing, L"library caret: append trails the last card");
        layout::CaretAnchor first = anc(10, 40);
        check(first.card == 0 && !first.trailing, L"library caret: index 0 leads card 0");

        std::vector<RECT> none;
        check(layout::caretAnchor(none, POINT{ 0, 0 }, 0).card < 0,
              L"library caret: an empty library has no caret");
    }

    // Unsaved-changes flag: edits and selection changes count, playhead doesn't.
    {
        Document d; d.init(rate);
        std::wstring proj = dir + L"\\_selftest_dirty.acep";
        int id = d.addClip(L"clip", makeSine(rate, 0.2, 300.0), L"", L"add");
        check(d.isModified(), L"dirty after an edit");
        check(d.saveProject(proj) && !d.isModified(), L"clean after save");
        d.view().selClipId = id; d.view().selStart = 100; d.view().selEnd = 900;
        check(d.isModified(), L"selection change marks the project modified");
        check(d.saveProject(proj) && !d.isModified(), L"clean after saving the selection");
        d.view().playheadFrame = 4321;
        check(!d.isModified(), L"playhead move does not mark the project modified");
        DeleteFileW(proj.c_str());
    }

    // Sub-range preview source: the UI arms a BufferSource over [begin,end) and
    // maps clip frames to source frames with (frame - begin), so position() must
    // stay relative to begin and rendering must stop at end.
    {
        const int64_t begin = 1000, end = 5000, from = 1250;
        BufferSource src(sine, begin, end, 1.0f);
        check(src.total() == end - begin && src.position() == 0,
              L"preview source spans the selection",
              L"total=" + std::to_wstring(src.total()));
        src.seek(from - begin);                       // start playback at clip frame `from`
        check(src.position() == from - begin, L"preview source seek is begin-relative",
              L"pos=" + std::to_wstring(src.position()));
        float got[16] = { 0 };
        int n = src.render(got, 8);
        bool sameAudio = (n == 8);
        for (int i = 0; i < 8 && sameAudio; ++i)
            sameAudio = std::fabs(got[i * 2] - sine->samples[(size_t)(from + i) * 2]) < 1e-6f;
        check(sameAudio, L"preview source renders from the seek point");
        src.seek(end - begin - 4);                    // 4 frames left before the end
        float tail[64] = { 0 };
        check(src.render(tail, 32) == 4, L"preview source stops at the selection end");
        check(src.render(tail, 32) == 0, L"preview source drains once past the end");
    }

    // Voice isolation ("remove non-voice"): a harmonic speech-like stretch plus a
    // low-frequency bump and a broadband shuffle burst, separated by room tone.
    {
        const double dur = 5.2;
        auto sig = std::make_shared<AudioBuffer>();
        sig->sampleRate = rate; sig->channels = 2;
        const int64_t nf = (int64_t)(rate * dur);
        sig->samples.assign((size_t)nf * 2, 0.0f);
        uint32_t rng = 12345u;
        auto urand = [&]() {                      // deterministic white noise in [-1,1)
            rng = rng * 1664525u + 1013904223u;
            return (double)(int32_t)rng / 2147483648.0;
        };
        const double vA = 0.80, vB = 2.00;        // voice
        const double bA = 3.00, bB = 3.30;        // bump (60 Hz thump)
        const double sA = 4.00, sB = 4.50;        // shuffle (broadband burst)
        for (int64_t i = 0; i < nf; ++i) {
            const double t = (double)i / rate;
            double v = 0.004 * urand();           // room tone everywhere
            if (t >= vA && t < vB) {              // harmonic voice: F0 140 Hz + 12 harmonics
                double s = 0;
                for (int k = 1; k <= 12; ++k)
                    s += std::sin(2.0 * 3.14159265358979 * 140.0 * k * (t - vA)) / k;
                v += 0.18 * s;
            }
            if (t >= bA && t < bB)                // decaying low thump
                v += 0.6 * std::exp(-(t - bA) * 12.0) * std::sin(2.0 * 3.14159265358979 * 60.0 * (t - bA));
            if (t >= sA && t < sB)                // shuffling / rustle
                v += 0.15 * urand();
            sig->samples[i * 2] = (float)v;
            sig->samples[i * 2 + 1] = (float)v;
        }
        auto regionRms = [](const AudioBuffer& b, double t0, double t1) {
            int64_t a = (int64_t)(t0 * b.sampleRate) * b.channels;
            int64_t z = std::min<int64_t>((int64_t)(t1 * b.sampleRate) * b.channels, (int64_t)b.samples.size());
            double s = 0; int64_t n = 0;
            for (int64_t i = a; i < z; ++i) { s += (double)b.samples[i] * b.samples[i]; ++n; }
            return n ? std::sqrt(s / n) : 0.0;
        };
        auto dbDrop = [](double before, double after) {
            return 20.0 * std::log10(std::max(after, 1e-12) / std::max(before, 1e-12));
        };

        dsp::VoiceIsolateOptions vio;             // defaults: balanced, 60 dB, 200 ms hold
        dsp::VoiceIsolateStats vst;
        LARGE_INTEGER qf, q0, q1; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
        auto clean = dsp::isolateVoice(*sig, vio, &vst);
        QueryPerformanceCounter(&q1);
        const double secs = (double)(q1.QuadPart - q0.QuadPart) / qf.QuadPart;
        out(L"Voice isolation: " + std::to_wstring(secs) + L" s for " + std::to_wstring(dur) +
            L" s of audio (" + std::to_wstring(dur / std::max(secs, 1e-9)) + L"x realtime)");
        check(clean && clean->frames() == nf, L"voice isolation preserves length",
              L"frames=" + std::to_wstring(clean ? clean->frames() : 0));
        if (clean) {
            double vKept = regionRms(*clean, 1.0, 1.9) / std::max(1e-12, regionRms(*sig, 1.0, 1.9));
            check(vKept > 0.95, L"voice isolation keeps speech",
                  L"kept=" + std::to_wstring(vKept * 100.0) + L"%");
            double bump = dbDrop(regionRms(*sig, bA + 0.02, bB), regionRms(*clean, bA + 0.02, bB));
            check(bump < -20.0, L"voice isolation removes bumps",
                  std::to_wstring(bump) + L" dB");
            double shuf = dbDrop(regionRms(*sig, sA + 0.05, sB - 0.05), regionRms(*clean, sA + 0.05, sB - 0.05));
            check(shuf < -20.0, L"voice isolation removes shuffling",
                  std::to_wstring(shuf) + L" dB");
            double tone = dbDrop(regionRms(*sig, 0.0, 0.6), regionRms(*clean, 0.0, 0.6));
            check(tone < -20.0, L"voice isolation removes room tone",
                  std::to_wstring(tone) + L" dB");
            check(vst.segments == 1 && vst.voiceSeconds > 0.9 && vst.voiceSeconds < 2.5,
                  L"voice isolation segment stats",
                  L"segments=" + std::to_wstring(vst.segments) +
                  L" voice=" + std::to_wstring(vst.voiceSeconds) + L"s");

            // reduce + residue == original (nothing invented, nothing lost)
            dsp::VoiceIsolateOptions rio = vio; rio.residue = true;
            auto res = dsp::isolateVoice(*sig, rio, nullptr);
            double maxDiff = 0.0;
            if (res && res->samples.size() == sig->samples.size())
                for (size_t i = 0; i < sig->samples.size(); ++i)
                    maxDiff = std::max(maxDiff,
                        std::fabs((double)clean->samples[i] + res->samples[i] - sig->samples[i]));
            check(res && maxDiff < 1e-5, L"voice isolation reduce+residue == original",
                  L"maxDiff=" + std::to_wstring(maxDiff));

            // "Only the selection": the effect still runs over the whole clip, then
            // blendProcessedRange keeps just part of that result. Chosen range
            // covers the bump (3.00-3.30) but not the shuffle (4.00-4.50).
            const int64_t r0 = (int64_t)(2.90 * rate), r1 = (int64_t)(3.50 * rate);
            auto partial = dsp::blendProcessedRange(*sig, *clean, r0, r1);
            check(partial && partial->frames() == nf && partial->channels == sig->channels,
                  L"range blend preserves length and channels");
            if (partial) {
                double outside = 0.0;
                for (int64_t i = 0; i < nf; ++i) {
                    if (i >= r0 && i < r1) continue;
                    for (int c = 0; c < 2; ++c)
                        outside = std::max(outside, std::fabs((double)partial->samples[i * 2 + c] -
                                                              sig->samples[i * 2 + c]));
                }
                check(outside == 0.0, L"range blend leaves audio outside the range untouched",
                      L"maxDiff=" + std::to_wstring(outside));

                // Past the crossfade edges the range holds the processed result exactly.
                double inside = 0.0;
                const int64_t guard = (int64_t)(0.02 * rate);
                for (int64_t i = r0 + guard; i < r1 - guard; ++i)
                    for (int c = 0; c < 2; ++c)
                        inside = std::max(inside, std::fabs((double)partial->samples[i * 2 + c] -
                                                             clean->samples[i * 2 + c]));
                check(inside < 1e-6, L"range blend keeps the processed result inside the range",
                      L"maxDiff=" + std::to_wstring(inside));

                double pb = dbDrop(regionRms(*sig, bA + 0.02, bB), regionRms(*partial, bA + 0.02, bB));
                check(pb < -20.0, L"range blend removes non-voice inside the range",
                      std::to_wstring(pb) + L" dB");
                double ps = dbDrop(regionRms(*sig, sA + 0.05, sB - 0.05),
                                   regionRms(*partial, sA + 0.05, sB - 0.05));
                check(std::fabs(ps) < 0.01, L"range blend keeps non-voice outside the range",
                      std::to_wstring(ps) + L" dB");
            }
            check(dsp::blendProcessedRange(*sig, *clean, r1, r0) == nullptr,
                  L"range blend rejects an empty range");
            AudioBuffer shortBuf; shortBuf.sampleRate = rate; shortBuf.channels = 2;
            shortBuf.samples.assign(1024, 0.0f);
            check(dsp::blendProcessedRange(*sig, shortBuf, r0, r1) == nullptr,
                  L"range blend rejects a mismatched buffer");

            // Statistics follow that range instead of covering the whole clip:
            // 0.0-0.6 s is room tone only, so nothing there is voice.
            dsp::VoiceIsolateStats rst;
            dsp::isolateVoice(*sig, vio, &rst, 0, (int64_t)(0.6 * rate));
            check(rst.segments == 0 && rst.voiceSeconds == 0.0 &&
                  std::fabs(rst.removedSeconds - 0.6) < 0.01,
                  L"voice isolation stats follow the range",
                  L"segments=" + std::to_wstring(rst.segments) +
                  L" removed=" + std::to_wstring(rst.removedSeconds) + L"s");
        }
    }

    // Clip names are free text but get offered as the default export file name.
    {
        auto sfn = [](const wchar_t* s) { return mfio::safeFileName(s); };
        check(sfn(L"take 2") == L"take 2", L"safeFileName leaves a clean name alone",
              sfn(L"take 2"));
        check(sfn(L"re: take 2/3 <best?>") == L"re_ take 2_3 _best__",
              L"safeFileName replaces path-illegal characters", sfn(L"re: take 2/3 <best?>"));
        // Windows drops these silently, so the file would not match the dialog.
        check(sfn(L"take 2. ") == L"take 2", L"safeFileName drops trailing dots and spaces",
              L"[" + sfn(L"take 2. ") + L"]");
        check(sfn(L"  take 2") == L"take 2", L"safeFileName drops leading spaces",
              L"[" + sfn(L"  take 2") + L"]");
        // Illegal characters still leave a usable name; only a name that sanitises
        // away to nothing falls back.
        check(sfn(L"???") == L"___", L"safeFileName keeps a fully-substituted name",
              sfn(L"???"));
        check(sfn(L"") == L"clip" && sfn(L"...") == L"clip" && sfn(L"   ") == L"clip",
              L"safeFileName never returns an empty name", sfn(L"..."));
    }

    // A bump next to speech must be removed just like one in the middle of a
    // silence. The gate's pre-roll / hold / gap-merge used to protect it, so a
    // take whose bumps cluster around the speech -- the normal case, and exactly
    // what a user selects when they want one gone -- came back barely changed
    // (measured -0.9 dB) while an identical isolated bump went to -60 dB.
    {
        const int rate = 48000;
        const double dur = 7.0;
        const int64_t nf = (int64_t)(dur * rate);
        auto sig = std::make_shared<AudioBuffer>();
        sig->sampleRate = rate; sig->channels = 2; sig->samples.assign(nf * 2, 0.0f);
        uint32_t rng = 99u;
        auto urand = [&]() { rng = rng * 1664525u + 1013904223u;
                             return (double)(int32_t)rng / 2147483648.0; };
        auto voice = [&](double t, double a, double b) {
            if (t < a || t >= b) return 0.0;
            double s = 0;
            for (int k = 1; k <= 12; ++k) s += std::sin(2.0 * 3.14159265358979 * 140.0 * k * (t - a)) / k;
            return 0.18 * s;
        };
        auto thump = [&](double t, double a, double b) {
            if (t < a || t >= b) return 0.0;
            return 0.6 * std::exp(-(t - a) * 12.0) * std::sin(2.0 * 3.14159265358979 * 60.0 * (t - a));
        };
        for (int64_t i = 0; i < nf; ++i) {
            const double t = (double)i / rate;
            double v = 0.004 * urand();
            v += voice(t, 0.50, 2.00);      // speech
            v += thump(t, 2.15, 2.45);      // bump 150 ms after speech ends
            v += voice(t, 3.00, 4.50);      // speech
            v += thump(t, 6.00, 6.30);      // bump far from any speech
            sig->samples[i * 2] = (float)v; sig->samples[i * 2 + 1] = (float)v;
        }
        auto regionRms = [](const AudioBuffer& b, double t0, double t1) {
            int64_t a = (int64_t)(t0 * b.sampleRate) * b.channels;
            int64_t z = std::min<int64_t>((int64_t)(t1 * b.sampleRate) * b.channels, (int64_t)b.samples.size());
            double s = 0; int64_t n = 0;
            for (int64_t i = a; i < z; ++i) { s += (double)b.samples[i] * b.samples[i]; ++n; }
            return n ? std::sqrt(s / n) : 0.0;
        };
        dsp::VoiceIsolateOptions vio;
        auto clean = dsp::isolateVoice(*sig, vio, nullptr);
        auto db = [&](double t0, double t1) {
            return 20.0 * std::log10(std::max(regionRms(*clean, t0, t1), 1e-12) /
                                     std::max(regionRms(*sig, t0, t1), 1e-12));
        };
        check(clean != nullptr, L"near-speech bump: isolation ran");
        if (clean) {
            // 150 ms after the speech ends, i.e. inside the 200 ms hold window.
            const double adjacent = db(2.17, 2.45);
            check(adjacent < -15.0, L"bump 150 ms after speech is removed",
                  std::to_wstring(adjacent) + L" dB");
            // The same bump far from any speech, as the upper bound to compare to.
            // The near one can't quite reach this: its energy is concentrated in
            // the first few ms and the gate needs fadeMs to close.
            const double isolated = db(6.02, 6.30);
            check(isolated < -20.0, L"bump far from speech is removed",
                  std::to_wstring(isolated) + L" dB");
            // The override must not eat the speech it sits next to.
            const double sp = db(0.60, 1.90);
            check(sp > -0.5, L"speech next to a bump is kept",
                  std::to_wstring(sp) + L" dB");
        }
    }

    // ---- playback position (device frames played -> source frames heard)
    {
        // A 48 kHz device fed in 480-frame (10 ms) chunks, no resampling.
        RenderTimeline t;
        t.reset(0);
        check(t.sourceAt(0) == 0, L"playback position: empty history reads the reset point");
        for (int i = 0; i < 5; ++i) t.push(480, 480, 480 * (i + 1));
        // The whole point: a query landing *inside* a chunk interpolates rather
        // than snapping to the chunk boundary. Without this the playhead steps
        // by 10 ms at a time however fast the UI repaints.
        check(t.sourceAt(1200) == 1200, L"playback position interpolates inside a chunk",
              std::to_wstring(t.sourceAt(1200)));
        check(t.sourceAt(480) == 480, L"playback position is exact on a chunk boundary",
              std::to_wstring(t.sourceAt(480)));
        // It must never read ahead of the audio: 5 chunks written, 2 played
        // means the listener is at 960, not at the render position of 2400.
        check(t.sourceAt(960) == 960, L"playback position lags the render position",
              std::to_wstring(t.sourceAt(960)));
        check(t.sourceAt(99999) == 2400, L"playback position saturates once everything has played",
              std::to_wstring(t.sourceAt(99999)));

        // Resampling: a 44.1 kHz source into a 48 kHz device consumes fewer
        // source frames than it writes device frames, and the mapping has to
        // follow the *source*, which is what the playhead is drawn against.
        RenderTimeline r;
        r.reset(0);
        for (int i = 0; i < 4; ++i) r.push(480, 441, 441 * (i + 1));
        check(r.sourceAt(480) == 441, L"playback position follows a resampled source",
              std::to_wstring(r.sourceAt(480)));
        // 720 device frames is 720 * 441/480 = 661.5 source frames.
        check(r.sourceAt(720) == 661, L"playback position interpolates a resampled chunk",
              std::to_wstring(r.sourceAt(720)));

        // A seek drops the history, so the position reads as the new one
        // immediately rather than tracking the stale audio still draining out of
        // the device buffer -- that is what makes seeking feel instant.
        RenderTimeline s;
        s.reset(0);
        for (int i = 0; i < 3; ++i) s.push(480, 480, 480 * (i + 1));
        s.reset(96000);
        check(s.sourceAt(480) == 96000, L"playback position jumps immediately on a seek",
              std::to_wstring(s.sourceAt(480)));
        check(s.devWritten == 1440, L"playback position keeps counting device frames across a seek",
              std::to_wstring(s.devWritten));

        // History older than the ring falls back to the reset point rather than
        // reading a recycled entry.
        RenderTimeline w;
        w.reset(7);
        for (int i = 0; i < RenderTimeline::kRecs + 20; ++i) w.push(480, 480, 480 * (i + 1));
        check(w.sourceAt(0) == 7, L"playback position falls back when history has been recycled",
              std::to_wstring(w.sourceAt(0)));
        const int64_t newestSrc = 480 * (RenderTimeline::kRecs + 20);
        check(w.sourceAt(w.devWritten) == newestSrc, L"playback position still tracks after wrapping",
              std::to_wstring(w.sourceAt(w.devWritten)));

        // A paused or silent fill consumes no source, so the position must hold
        // still rather than sliding forward with the device clock.
        RenderTimeline p;
        p.reset(0);
        p.push(480, 480, 480);
        p.push(480, 0, 480);
        check(p.sourceAt(720) == 480, L"playback position holds still through a silent fill",
              std::to_wstring(p.sourceAt(720)));
    }

    // ---- playback position, end to end through a real device
    // RenderTimeline above covers the arithmetic; this covers the wiring that
    // feeds it -- the audio clock's frequency units and the QPC extrapolation,
    // which are exactly the parts that would silently report nonsense. Plays one
    // second of digital silence, so it is inaudible.
    {
        PlaybackEngine eng;
        std::wstring engErr;
        LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
        auto nowSec = [&] {
            LARGE_INTEGER q; QueryPerformanceCounter(&q);
            return (double)q.QuadPart / (double)qf.QuadPart;
        };
        if (!eng.init(&engErr)) {
            out(L"[SKIP] playback engine: no audio device (" + engErr + L")");
        } else {
            // init() is what calls IAudioClient::Start(), so this is the moment
            // the device clock starts running -- the anchor the startup settling
            // window below is measured from.
            const double tStreamStart = nowSec();
            auto silence = std::make_shared<AudioBuffer>();
            silence->sampleRate = eng.sampleRate(); silence->channels = 2;
            silence->samples.assign((size_t)eng.sampleRate() * 2 * 2, 0.0f);   // 2 s
            eng.setSourceRate(silence->sampleRate);
            eng.play(std::make_shared<BufferSource>(silence, 0, silence->frames()), 0);

            // Sample the position repeatedly, timing each sample with QPC --
            // Sleep(20) really sleeps ~30 ms at the default timer resolution, so
            // the ticks have to be measured rather than assumed.
            std::vector<int64_t> pos; std::vector<double> at;
            std::vector<PlaybackEngine::PosDiag> dg;
            const double t0 = nowSec();
            for (int i = 0; i < 35; ++i) {
                Sleep(20);
                pos.push_back(eng.position()); dg.push_back(eng.posDiag()); at.push_back(nowSec());
            }
            eng.stop();

            bool monotonic = true;
            double worstRate = 0.0, bestRate = 1e9;
            for (size_t i = 1; i < pos.size(); ++i) {
                if (pos[i] < pos[i - 1]) monotonic = false;
                // Skip the stream's startup settling, measured from the moment
                // IAudioClient::Start() ran rather than as a tick count.
                //
                // Two separate things happen in that window. The buffer is still
                // filling, so the heard position legitimately lags. And the audio
                // driver's own clock is not yet linear: IAudioClock reports one
                // ~12 ms (~510-530 frame) downward step, exactly once, 60-130 ms
                // after Start(), after which it tracks QPC to within 0.1% for the
                // rest of the stream. That step is in the *device* clock reading
                // itself -- not the render timeline, not either clamp in
                // position() -- so the engine has nothing to correct: it is
                // faithfully reporting what the hardware says.
                //
                // This used to be `i < 4`, which made the check fail about one run
                // in six. The transient happens at a fixed time after Start(), but
                // a tick count anchors to whenever this loop happened to begin, so
                // the step landed inside the skip on most runs and just past it on
                // the rest. (Raising the process priority made it *more* frequent,
                // not less -- which is what ruled out CPU starvation as the cause.)
                if (at[i - 1] - tStreamStart < 0.35) continue;
                const double dt = at[i] - at[i - 1];
                if (dt <= 0) continue;
                // Frames advanced per second of wall clock, as a multiple of the
                // sample rate. 1.0 means the playhead is keeping perfect time.
                const double rate = (double)(pos[i] - pos[i - 1]) / dt / eng.sampleRate();
                worstRate = std::max(worstRate, rate);
                bestRate = std::min(bestRate, rate);
            }
            check(monotonic, L"playback position never goes backwards");
            const double overall = (double)pos.back() / eng.sampleRate() / (at.back() - t0);
            check(overall > 0.9 && overall < 1.1, L"playback position keeps real time",
                  std::to_wstring(overall) + L"x");
            // The actual regression, and the reason the average above is not
            // enough on its own: every tick must advance by about its own
            // duration. Reporting the render position instead gets the average
            // right (0.99x) while swinging between 0.63x and 1.31x tick to tick,
            // which is what a stuttering playhead looks like. Measured here:
            // 0.9991x to 1.0008x. The thresholds sit between the two.
            const bool steady = bestRate > 0.8;
            check(steady, L"playback position never stalls between ticks",
                  std::to_wstring(bestRate) + L"x");
            check(worstRate < 1.25, L"playback position never lurches between ticks",
                  std::to_wstring(worstRate) + L"x");
            // This one fails intermittently and only on a loaded machine, so when
            // it does, dump the series that produced it: a plateau followed by a
            // catch-up looks quite different from a single lost increment, and
            // without the samples the two are indistinguishable after the fact.
            if (!steady) {
                for (size_t i = 1; i < pos.size(); ++i) {
                    const double dt = at[i] - at[i - 1];
                    wchar_t line[160];
                    swprintf(line, 160,
                             L"       tick %2d  dt %6.2f ms  dpos %7lld  rate %6.3fx  "
                             L"dev %8lld (+%5lld)  heard %8lld  rend %8lld  lead %6lld  recs %2d%s%s",
                             (int)i, dt * 1000.0, (long long)(pos[i] - pos[i - 1]),
                             dt > 0 ? (double)(pos[i] - pos[i - 1]) / dt / eng.sampleRate() : 0.0,
                             (long long)dg[i].devPlayed,
                             (long long)(dg[i].devPlayed - dg[i - 1].devPlayed),
                             (long long)dg[i].heard, (long long)dg[i].rendered,
                             (long long)(dg[i].rendered - dg[i].heard), dg[i].recCount,
                             dg[i].clamped ? L"  CLAMPED" : L"", dg[i].drained ? L"  DRAINED" : L"");
                    out(line);
                }
            }
            eng.shutdown();
        }
    }

    // ---- editor toolbar wrapping
    {
        // The real editor toolbar: the widths computeEditorLayout passes, scaled
        // the way S() would. The negative entry is the transport/edit group gap.
        auto toolbar = [](float sc, int winW) {
            auto S = [&](int v) { return (int)(v * sc + 0.5f); };
            const std::vector<int> w = { S(96), S(140), S(150), -S(16), S(120),
                                         S(130), S(130), S(150), S(150), S(120) };
            const int pad = S(16), doneW = S(96);
            const int avail = std::max(S(200), winW - pad - doneW - S(16));
            return layout::flowButtons(w, pad, S(10), S(34), S(8), avail, S(10));
        };
        // 100% DPI, wide window: everything still fits on one row. The height
        // comes out just under the toolbar's historical 56 px, which is why
        // computeEditorLayout floors it there -- so nothing below it moves.
        auto wide = toolbar(1.0f, 1900);
        check(wide.rows == 1, L"editor toolbar: one row when it fits",
              std::to_wstring(wide.rows));
        check(wide.height <= 56, L"editor toolbar: single row still fits the old height",
              std::to_wstring(wide.height));
        // 150% DPI at the default 1500 px window -- the case that motivated this.
        auto dpi150 = toolbar(1.5f, 1500);
        check(dpi150.rows == 2, L"editor toolbar wraps at 150% DPI",
              std::to_wstring(dpi150.rows));
        // Every button must land somewhere reachable, i.e. inside the window.
        bool allVisible = true;
        for (const RECT& r : dpi150.rects)
            if (r.right > 1500) allVisible = false;
        check(allVisible, L"editor toolbar: no button runs off the window edge");
        // A wrapped row restarts at the left margin rather than continuing.
        int secondRowLeft = -1;
        for (const RECT& r : dpi150.rects)
            if (r.right > r.left && r.top != dpi150.rects[0].top) { secondRowLeft = r.left; break; }
        check(secondRowLeft == (int)(16 * 1.5f + 0.5f),
              L"editor toolbar: wrapped row starts at the left margin",
              std::to_wstring(secondRowLeft));
        // Absurdly narrow: still lays out, one button per row, nothing lost.
        auto tiny = toolbar(1.0f, 300);
        bool everyPlaced = true;
        for (size_t i = 0; i < tiny.rects.size(); ++i)
            if (i != 3 && tiny.rects[i].right == tiny.rects[i].left) everyPlaced = false;
        check(everyPlaced, L"editor toolbar: every button is placed even when very narrow");
        check(tiny.rows > 2, L"editor toolbar: keeps wrapping when very narrow",
              std::to_wstring(tiny.rows));

        // The labels and the widths above are declared in different places, so
        // they can drift -- and the buttons draw with DT_CENTER and no ellipsis,
        // so a label that outgrows its width is silently clipped at both ends.
        // Measure every label, including the alternates a button swaps to while
        // playing or toggled, with the real UI font at both 100% and 150%.
        //
        // The transport buttons are sized for their *widest* alternate on purpose:
        // resizing a button the moment playback starts would re-flow the whole
        // toolbar under the user's cursor.
        struct Lbl { size_t idx; const wchar_t* text; };
        static const Lbl labels[] = {
            { 0, L"\u25B6 Play" }, { 0, L"\u275A\u275A Pause" },
            { 1, L"\u25B6 Play selection" }, { 1, L"\u275A\u275A Pause selection" },
            { 2, L"Fine-tune edges" }, { 2, L"\u2713 Fine-tune edges" },
            { 4, L"Crop to selection\u2026" },
            { 5, L"Silence selection" }, { 6, L"Delete selection" },
            { 7, L"Save selection as clip" },
            { 8, L"Capture noise (sel)" }, { 8, L"Capture noise (clip)" },
            { 9, L"Clear selection" },
        };
        for (float sc : { 1.0f, 1.5f }) {
            auto S = [&](int v) { return (int)(v * sc + 0.5f); };
            const std::vector<int> w = { S(96), S(140), S(150), -S(16), S(120),
                                         S(130), S(130), S(150), S(150), S(120) };
            HDC sdc = GetDC(nullptr);
            HFONT f = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                  OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  VARIABLE_PITCH, L"Segoe UI");
            HGDIOBJ of = SelectObject(sdc, f);
            std::wstring worst; int worstOver = -100000;
            for (const Lbl& l : labels) {
                SIZE sz{};
                GetTextExtentPoint32W(sdc, l.text, (int)wcslen(l.text), &sz);
                int over = sz.cx - w[l.idx];
                if (over > worstOver) { worstOver = over; worst = l.text; }
            }
            SelectObject(sdc, of); DeleteObject(f); ReleaseDC(nullptr, sdc);
            // A few px of breathing room, since the button also draws a rounded edge.
            check(worstOver <= -6,
                  std::wstring(L"editor toolbar: every label fits its button at ") +
                      (sc == 1.0f ? L"100%" : L"150%") + L" DPI",
                  L"tightest \"" + worst + L"\" with " + std::to_wstring(-worstOver) + L" px spare");
        }
    }

    // ---- preview transport (which control does what to the engine)
    //
    // transport::decide is what stands behind the play/pause buttons. The reported
    // bug was that pressing the editor's "Play selection" turned the neighbouring
    // "Play" into Pause -- both buttons shared one rule, so a selection audition
    // was indistinguishable from a whole-clip play. The two-button case is now
    // ownRangeOnly, and these checks pin both callers' behaviour down.
    {
        using namespace transport;
        const int C = 7;                 // the clip under the buttons
        const int64_t SB = 1000, SE = 5000;   // its selection

        auto armed = [&](bool isSel, bool playing) {
            State s; s.clipId = C; s.isSel = isSel;
            s.begin = isSel ? SB : 0; s.end = isSel ? SE : 20000;
            s.playing = playing; s.paused = !playing;
            return s;
        };
        auto press = [&](bool wantSel, bool ownRangeOnly) {
            Press p; p.clipId = C; p.wantSel = wantSel;
            p.ownRangeOnly = ownRangeOnly; p.selBegin = SB; p.selEnd = SE;
            return p;
        };

        // The editor pair: each button pauses only what it started...
        check(decide(armed(true, true), press(true, true)).act == Act::Pause,
              L"transport: Play selection pauses its own audition");
        check(decide(armed(false, true), press(false, true)).act == Act::Pause,
              L"transport: Play pauses its own whole-clip playback");
        // ...and switches playback to itself rather than pausing the other one.
        // This is the reported bug: pressing one used to stop the other.
        {
            Plan p = decide(armed(false, true), press(true, true));
            check(p.act == Act::Restart && p.from == From::SelStart,
                  L"transport: Play selection during whole-clip play switches to the selection");
        }
        {
            Plan p = decide(armed(true, true), press(false, true));
            check(p.act == Act::Restart && p.from == From::Cursor,
                  L"transport: Play during a selection audition switches to the whole clip");
        }

        // The single combined control (library card / Space) still pauses whatever
        // is running, whichever range that is -- otherwise its pause button would
        // restart a selection audition instead of pausing it.
        check(decide(armed(true, true), press(false, false)).act == Act::Pause,
              L"transport: the combined control pauses a selection audition");
        check(decide(armed(false, true), press(true, false)).act == Act::Pause,
              L"transport: the combined control pauses whole-clip playback");

        // Paused and asked for the same range again => resume in place, not restart,
        // so pause/play doesn't jump back to the start of the range.
        check(decide(armed(true, false), press(true, true)).act == Act::Resume,
              L"transport: pressing Play selection again resumes in place");
        check(decide(armed(false, false), press(false, true)).act == Act::Resume,
              L"transport: pressing Play again resumes in place");

        // A seek while paused must re-arm rather than resume, or playback would
        // carry on from where it stopped and ignore the click.
        { State s = armed(true, false); s.seekPending = true;
          check(decide(s, press(true, true)).act == Act::Restart,
                L"transport: a pending seek forces a restart instead of a resume"); }

        // A selection edited since the audition was armed is a different range,
        // so it must re-arm at the new head.
        { State s = armed(true, false); s.begin = SB + 500;
          Plan p = decide(s, press(true, true));
          check(p.act == Act::Restart && p.from == From::SelStart,
                L"transport: a moved selection edge re-arms at the new selection start"); }

        // A different clip, and the timeline owning the engine, both start fresh.
        { Press p = press(false, true); p.clipId = C + 1;
          Plan pl = decide(armed(false, true), p);
          check(pl.act == Act::Restart && pl.from == From::ClipStart,
                L"transport: playing a different clip starts at its head"); }
        { State s = armed(false, true); s.timeline = true;
          check(decide(s, press(false, true)).act == Act::Restart,
                L"transport: a preview press during timeline playback takes the engine over"); }
    }

    // ---- sticky snapping (dragging a clip along a lane)
    {
        using snapping::Sticky;
        // Neighbours make 1000 and 2000 interesting; 0 is the timeline start.
        const std::vector<int64_t> tg{ 0, 1000, 2000 };
        const int64_t tol = 50;

        // The point of the whole exercise: approaching a snap point must not
        // pull, so a position just short of one is actually reachable.
        { Sticky s; s.begin(500);
          check(s.update(900, tg, tol) == 900, L"snap: approaching a target does not pull");
          check(s.update(990, tg, tol) == 990, L"snap: stopping just short of a target keeps the gap");
          check(s.update(999, tg, tol) == 999, L"snap: one frame short of a target is reachable");
          check(!s.engaged(), L"snap: nothing is engaged while merely approaching"); }

        // ...but crossing one lands flush, so snapping still costs no aim.
        { Sticky s; s.begin(500);
          check(s.update(1010, tg, tol) == 1000, L"snap: crossing a target lands flush on it");
          check(s.engaged(), L"snap: crossing engages"); }
        { Sticky s; s.begin(1500);
          check(s.update(995, tg, tol) == 1000, L"snap: crossing from the far side lands flush too"); }

        // Engaged, it resists until the mouse is more than tol away.
        { Sticky s; s.begin(500);
          s.update(1010, tg, tol);
          check(s.update(1040, tg, tol) == 1000, L"snap: an engaged target holds while dragging away");
          check(s.update(960, tg, tol) == 1000, L"snap: it holds on the other side as well");
          check(s.update(1051, tg, tol) == 1051, L"snap: past the tolerance it lets go");
          check(!s.engaged(), L"snap: letting go disengages"); }

        // Having let go, the far side is approachable -- which is how a position
        // inside the tolerance band, unreachable on the way in, is reached.
        { Sticky s; s.begin(500);
          s.update(1010, tg, tol);
          s.update(1051, tg, tol);              // pull free
          check(s.update(1005, tg, tol) == 1005, L"snap: coming back toward a target does not re-pull");
          check(s.update(1001, tg, tol) == 1001, L"snap: a position inside the band is reachable from outside"); }

        // A clip already sitting flush must resist the first nudge, or one butted
        // against a neighbour would drift off on a stray pixel.
        { Sticky s; s.begin(1000);
          check(s.update(1020, tg, tol) == 1000, L"snap: a clip starting flush resists being nudged off");
          check(s.update(1060, tg, tol) == 1060, L"snap: ...and still releases when pulled properly"); }

        // A fast drag that sweeps over a target and ends far past it is a move
        // through, not a landing; grabbing it would strand the clip behind the mouse.
        { Sticky s; s.begin(0);
          check(s.update(5000, tg, tol) == 5000, L"snap: sweeping past targets does not grab any of them");
          check(!s.engaged(), L"snap: a sweep leaves nothing engaged"); }

        // With two targets crossed in one step, the one ending nearest wins.
        { Sticky s; s.begin(980);
          const std::vector<int64_t> pair{ 1000, 1030 };
          check(s.update(1040, pair, tol) == 1030, L"snap: the nearest of several crossed targets wins");
          // A target the step stopped short of is not crossed, so it cannot win
          // even when it is the nearer one.
          Sticky s2; s2.begin(980);
          check(s2.update(1025, pair, tol) == 1000, L"snap: a target not yet reached is not a candidate"); }

        // Painting must not be able to move the clip: repeating a position is inert.
        { Sticky s; s.begin(500);
          const int64_t a = s.update(1010, tg, tol);
          check(s.update(1010, tg, tol) == a, L"snap: repeating a position changes nothing");
          check(s.update(1010, tg, tol) == a, L"snap: ...however many times it repeats"); }

        // begin() must wipe the previous gesture, or the next drag inherits a
        // stuck target it never touched.
        { Sticky s; s.begin(500);
          s.update(1010, tg, tol);
          s.begin(3000);
          check(!s.engaged(), L"snap: a new drag starts disengaged");
          check(s.update(3010, tg, tol) == 3010, L"snap: a new drag is free of the old target"); }
    }

    // ---- selection history (Ctrl+Z stepping back through selections)
    {
        using selhist::History; using selhist::Sel; using selhist::same;
        // Stand-ins for document undo-tree nodes; only their identity matters.
        const int nodeA = 1, nodeB = 2;   // distinct values, so the addresses cannot be folded
        const void* A = &nodeA; const void* B = &nodeB;

        const Sel none{ -1, 0, 0 }, s1{ 7, 100, 200 }, s2{ 7, 300, 900 }, s3{ 9, 0, 50 };

        { History h; check(!h.canUndo(A) && !h.canRedo(A),
                           L"selection history: nothing to undo before any selection"); }

        // The basic ask: two selections, then step back to the first.
        { History h;
          h.record(A, none, s1);
          h.record(A, s1, s2);
          check(h.canUndo(A), L"selection history: a change is undoable");
          check(same(h.undo(s2), s1), L"selection history: undo returns the previous selection");
          check(same(h.undo(s1), none), L"selection history: undo again reaches the empty selection");
          check(!h.canUndo(A), L"selection history: undo stops at the start"); }

        // Redo has to retrace exactly, including the state undone from.
        { History h;
          h.record(A, none, s1); h.record(A, s1, s2);
          h.undo(s2);
          check(h.canRedo(A), L"selection history: an undone change is redoable");
          check(same(h.redo(s1), s2), L"selection history: redo returns the undone selection"); }

        // A gesture that ends where it began (a click re-picking the same range)
        // must not leave a dead entry that Ctrl+Z appears to ignore.
        { History h; h.record(A, s1, s1);
          check(!h.canUndo(A), L"selection history: an unchanged selection records nothing"); }

        // Making a new selection after undoing abandons the redo branch.
        { History h;
          h.record(A, none, s1); h.record(A, s1, s2);
          h.undo(s2);
          h.record(A, s1, s3);
          check(!h.canRedo(A), L"selection history: a new selection clears the redo branch");
          check(same(h.undo(s3), s1), L"selection history: the new change is itself undoable"); }

        // The whole point of the base token: once the document moves, these
        // entries describe states that are no longer reachable.
        { History h;
          h.record(A, none, s1); h.record(A, s1, s2);
          check(!h.canUndo(B), L"selection history: a document edit invalidates the stack");
          check(h.canUndo(A), L"selection history: the stack is keyed to its own history node");
          // Recording against the new node rebuilds from scratch rather than
          // appending to entries belonging to the old one.
          h.record(B, s2, s3);
          check(h.undoDepth() == 1 && same(h.undo(s3), s2),
                L"selection history: recording after an edit starts a fresh stack"); }

        { History h;
          h.record(A, none, s1);
          h.reset(A);
          check(!h.canUndo(A) && h.undoDepth() == 0, L"selection history: reset empties the stack"); }
    }

    // ---- waveform rendering, off-screen
    //
    // wf::draw is GDI, but it draws into whatever DC it is given, so a DIB
    // section makes it fully testable: render, then read the pixels back. Worth
    // testing because the envelope path was rewritten from a single ~3000-vertex
    // Polygon to one PolyPolyline of per-column vertical segments -- a 10x speed
    // -up (11 ms -> 1 ms on a 20 s clip at 1478 px, which is what let the
    // playhead reach the screen refresh rate) whose correctness is entirely
    // about the column geometry.
    {
        const int W = 400, H = 100;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;   // top-down
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ oldb = SelectObject(dc, dib);
        RECT rc = { 0, 0, W, H };

        // Left half loud, right half silent: the envelope must be tall on one
        // side and reduced to the centre line on the other.
        auto wbuf = std::make_shared<AudioBuffer>();
        wbuf->sampleRate = 48000; wbuf->channels = 1;
        const int64_t wn = 200000;
        wbuf->samples.assign((size_t)wn, 0.0f);
        for (int64_t i = 0; i < wn / 2; ++i)
            wbuf->samples[(size_t)i] = (float)std::sin(2 * 3.14159265358979 * 300.0 * i / 48000.0);
        PeakCache wpc; wpc.build(*wbuf);

        const COLORREF wcol = RGB(255, 0, 0);
        auto colHeight = [&](int x) {
            int n = 0;
            for (int y = 0; y < H; ++y) {
                DWORD px = ((DWORD*)bits)[(size_t)y * W + x];
                if ((px & 0x00FFFFFF) == 0x00FF0000) ++n;   // BGRA: red
            }
            return n;
        };

        // Zoomed out: 200000 frames across 400 px is 500 frames/px, well past the
        // 256-frame bucket size, so this exercises the peak-cache envelope path.
        memset(bits, 0, (size_t)W * H * 4);
        wf::draw(dc, rc, *wbuf, wpc, 0, wn, wcol);
        GdiFlush();
        check(colHeight(50) > H / 2, L"waveform: loud column fills most of the height",
              std::to_wstring(colHeight(50)));
        // The silent half must still leave a mark: a zero-height column would
        // make a quiet passage disappear entirely rather than read as a flat line.
        check(colHeight(350) >= 1 && colHeight(350) <= 3,
              L"waveform: silent column collapses to the centre line",
              std::to_wstring(colHeight(350)));
        int painted = 0;
        for (int x = 0; x < W; ++x) if (colHeight(x) > 0) ++painted;
        check(painted == W, L"waveform: every pixel column is drawn",
              std::to_wstring(painted));

        // Zoomed in past the bucket size takes the raw-sample branch; and fewer
        // frames than pixels takes the sub-sample scope branch. Both must still
        // put ink in the rectangle and nothing outside it.
        memset(bits, 0, (size_t)W * H * 4);
        wf::draw(dc, rc, *wbuf, wpc, 0, 4000, wcol);        // 10 frames/px, raw
        GdiFlush();
        check(colHeight(200) > 1, L"waveform: raw-sample zoom draws an envelope",
              std::to_wstring(colHeight(200)));

        memset(bits, 0, (size_t)W * H * 4);
        wf::draw(dc, rc, *wbuf, wpc, 0, 100, wcol);         // 4 px/frame, scope
        GdiFlush();
        int inked = 0;
        for (int x = 0; x < W; ++x) inked += colHeight(x);
        check(inked > W, L"waveform: sub-sample zoom draws a scope trace",
              std::to_wstring(inked));

        // An empty or inverted range must draw nothing rather than assert.
        memset(bits, 0, (size_t)W * H * 4);
        wf::draw(dc, rc, *wbuf, wpc, 5000, 5000, wcol);
        GdiFlush();
        int blank = 0;
        for (int x = 0; x < W; ++x) blank += colHeight(x);
        check(blank == 0, L"waveform: empty range draws nothing", std::to_wstring(blank));

        SelectObject(dc, oldb); DeleteObject(dib); DeleteDC(dc);
    }

    // ---- manual region edits (the fallback for non-voice the detector can't see)
    {
        // 1 s of a steady tone at 48 kHz stereo, so any level change is obvious
        // and the crossfade has something continuous to be judged against.
        auto src = std::make_shared<AudioBuffer>();
        src->sampleRate = 48000; src->channels = 2;
        const int64_t n = 48000;
        src->samples.resize((size_t)n * 2);
        for (int64_t i = 0; i < n; ++i) {
            const float v = (float)(0.5 * std::sin(2.0 * 3.14159265358979 * 220.0 * i / 48000.0));
            src->samples[(size_t)i * 2] = v; src->samples[(size_t)i * 2 + 1] = v;
        }
        auto rms = [](const AudioBuffer& b, double t0, double t1) {
            int64_t a = (int64_t)(t0 * b.sampleRate) * b.channels;
            int64_t z = std::min<int64_t>((int64_t)(t1 * b.sampleRate) * b.channels, (int64_t)b.samples.size());
            double s = 0; int64_t c = 0;
            for (int64_t i = std::max<int64_t>(0, a); i < z; ++i) { s += (double)b.samples[i] * b.samples[i]; ++c; }
            return c ? std::sqrt(s / c) : 0.0;
        };

        // --- silenceRange: [0.4, 0.6) goes quiet, everything else is untouched,
        //     and the clip keeps its length.
        auto sil = dsp::silenceRange(*src, (int64_t)(0.4 * 48000), (int64_t)(0.6 * 48000));
        check(sil != nullptr, L"silenceRange: returned a buffer");
        if (sil) {
            check(sil->frames() == n, L"silenceRange keeps the clip length",
                  std::to_wstring(sil->frames()));
            // Skip the 5 ms ramps at each edge when measuring the silent part.
            check(rms(*sil, 0.41, 0.59) < 1e-6, L"silenceRange silences the range",
                  std::to_wstring(rms(*sil, 0.41, 0.59)));
            bool outsideIntact = true;
            for (int64_t i = 0; i < (int64_t)(0.4 * 48000) * 2 && outsideIntact; ++i)
                outsideIntact = (sil->samples[(size_t)i] == src->samples[(size_t)i]);
            for (int64_t i = (int64_t)(0.6 * 48000) * 2;
                 i < (int64_t)src->samples.size() && outsideIntact; ++i)
                outsideIntact = (sil->samples[(size_t)i] == src->samples[(size_t)i]);
            check(outsideIntact, L"silenceRange leaves audio outside the range bit-identical");
            // The ramp has to actually be a ramp, not a hard edge: audio just
            // inside the boundary still carries most of the original signal.
            check(rms(*sil, 0.400, 0.401) > 0.2, L"silenceRange ramps in rather than cutting",
                  std::to_wstring(rms(*sil, 0.400, 0.401)));
        }

        // --- deleteRange: [0.4, 0.6) is cut and the gap closed. The result is
        //     the 0.2 s range shorter, less the 5 ms crossfade overlap.
        auto del = dsp::deleteRange(*src, (int64_t)(0.4 * 48000), (int64_t)(0.6 * 48000));
        check(del != nullptr, L"deleteRange: returned a buffer");
        if (del) {
            const int64_t want = n - (int64_t)(0.2 * 48000) - (int64_t)(0.005 * 48000);
            check(del->frames() == want, L"deleteRange shortens by the range plus the crossfade",
                  std::to_wstring(del->frames()) + L" vs " + std::to_wstring(want));
            // The level across the splice must not collapse, which is how a bad
            // overlap or an off-by-one would show up. The window is deliberately
            // wide: the two sides here are the *same* 220 Hz tone a whole number
            // of cycles apart, so they add coherently and equal-power weights
            // give up to +3 dB (0.354 -> 0.5). Real splices join unrelated room
            // tone, which is the case equal-power is chosen for.
            const double atJoin = rms(*del, 0.396, 0.400);
            check(atJoin > 0.2 && atJoin < 0.55, L"deleteRange splice holds its level",
                  std::to_wstring(atJoin));
            check(del->channels == 2 && del->sampleRate == 48000,
                  L"deleteRange preserves format");
        }

        // --- degenerate ranges are refused rather than producing an empty clip
        check(dsp::silenceRange(*src, 100, 100) == nullptr, L"silenceRange rejects an empty range");
        check(dsp::deleteRange(*src, 100, 100) == nullptr, L"deleteRange rejects an empty range");
        check(dsp::deleteRange(*src, 0, n) == nullptr, L"deleteRange refuses to delete everything");
        // Deleting from the very start leaves no audio before the cut to fade
        // against, so it must splice cleanly instead of reading out of bounds.
        auto head = dsp::deleteRange(*src, 0, (int64_t)(0.1 * 48000));
        check(head && head->frames() == n - (int64_t)(0.1 * 48000),
              L"deleteRange at the clip start needs no crossfade",
              head ? std::to_wstring(head->frames()) : L"(null)");
        auto tailCut = dsp::deleteRange(*src, n - (int64_t)(0.1 * 48000), n);
        check(tailCut && tailCut->frames() == n - (int64_t)(0.1 * 48000),
              L"deleteRange at the clip end needs no crossfade",
              tailCut ? std::to_wstring(tailCut->frames()) : L"(null)");
    }

    out(L"");
    out(L"==== " + std::to_wstring(pass) + L" passed, " + std::to_wstring(fail) + L" failed ====");
    if (log) fclose(log);

    mfio::shutdown();
    return fail == 0 ? 0 : 1;
}

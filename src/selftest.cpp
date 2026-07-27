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
#include <windows.h>
#include <shlwapi.h>
#include <string>
#include <cmath>
#include <cstdio>

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

    out(L"");
    out(L"==== " + std::to_wstring(pass) + L" passed, " + std::to_wstring(fail) + L" failed ====");
    if (log) fclose(log);

    mfio::shutdown();
    return fail == 0 ? 0 : 1;
}

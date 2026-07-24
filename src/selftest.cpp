// Headless self-test of the audio backend (decode / encode / peaks / slicing).
// Run with:  AudioClipEditor.exe --selftest
// Results are written to selftest.log next to the exe.
#include "audio_buffer.h"
#include "decoder.h"
#include "encoder.h"
#include "dsp.h"
#include "document.h"
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

    // Document project round-trip (.acep v2) + library sort / reorder
    {
        Document d; d.init(rate);
        auto mk = [&](const wchar_t* nm, uint64_t ts) {
            int id = d.addClip(nm, makeSine(rate, 0.2, 300.0), L"", L"add");
            if (Clip* c = d.project().findClip(id)) c->timestamp = ts;
            return id;
        };
        mk(L"Charlie", 300); mk(L"alpha", 100); mk(L"Bravo", 200);

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
        }
        DeleteFileW(proj.c_str());
    }

    out(L"");
    out(L"==== " + std::to_wstring(pass) + L" passed, " + std::to_wstring(fail) + L" failed ====");
    if (log) fclose(log);

    mfio::shutdown();
    return fail == 0 ? 0 : 1;
}

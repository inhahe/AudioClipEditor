#pragma once
#include "audio_buffer.h"
#include <string>

namespace mfio {

// Call once at startup / shutdown.
bool startup();
void shutdown();

// Decode any Media-Foundation-supported file (wav/mp3/m4a/aac/flac/wma/ogg*) to
// canonical stereo float PCM at `targetRate`. Returns nullptr on failure and
// fills `err` with a short message.
AudioBufferPtr decodeFile(const std::wstring& path, int targetRate, std::wstring* err = nullptr);

} // namespace mfio

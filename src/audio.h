#pragma once

#include <cstddef>
#include <string>

// Audio format: 48,000 Hz, stereo, interleaved float samples.
bool AudioStart();
bool AudioSubmitPCM(const float* samples, std::size_t frames);
void AudioClearQueue(); // <--- Add this
void AudioStop();
std::string AudioGetError();
bool AudioIsRunning();
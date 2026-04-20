#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"

#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <vector>

struct AudioData {

  // Actual playback object
  ma_engine engine;

  // The sound file/object being played
  ma_sound sound;

  // Stores recent audio samples
  std::vector<float> ringBuffer;

  // Mark where the next sample should be written in the write buffer
  size_t writeIndex = 0;

  // Protect the writebuffer from being accessed by two threads at the same time
  std::mutex bufferMutex;
};

// Fills pOutput with audio data from the engine and also stores it in the ring
// buffer
void data_callback(ma_device *pDevice, void *pOutput, const void *pInput,
                   ma_uint32 frameCount) {
  // (void)pInput;

  // Create a pointer that refereces our actual audio. I casted it to AudioData
  AudioData *audio = (AudioData *)pDevice->pUserData;

  // If somehow audio doesn't exit, fill the output with silence and return
  if (audio == nullptr) {
    ma_silence_pcm_frames(pOutput, frameCount, pDevice->playback.format,
                          pDevice->playback.channels);
    return;
  }

  // Read the next chunk of audio data from the engine into the output buffer.
  ma_engine_read_pcm_frames(&audio->engine, pOutput, frameCount, nullptr);

  float *out = (float *)pOutput;
  ma_uint32 channels = pDevice->playback.channels;
  size_t sampleCount = (size_t)frameCount * channels;

  std::lock_guard<std::mutex> lock(audio->bufferMutex);

  for (size_t i = 0; i < sampleCount; ++i) {
    audio->ringBuffer[audio->writeIndex] = out[i];
    audio->writeIndex = (audio->writeIndex + 1) % audio->ringBuffer.size();
  }
}

static std::vector<float> getLatestInterleavedSamples(AudioData &audio,
                                                      size_t count) {
  std::vector<float> samples;

  // If we haven't filled the buffer yet, just return an empty vector.
  if (audio.ringBuffer.empty())
    return samples;

  // Make sure user doesn't ask for more samples than the buffer can hold.
  count = std::min(count, audio.ringBuffer.size());

  // Resize the output vector to hold the requested number of samples.
  samples.resize(count);

  // Lock the buffer while we read from it to avoid conflicts with the audio
  std::lock_guard<std::mutex> lock(audio.bufferMutex);

  size_t bufferSize = audio.ringBuffer.size();

  size_t start = (audio.writeIndex + bufferSize - count) % bufferSize;

  for (size_t i = 0; i < count; ++i)
    samples[i] = audio.ringBuffer[(start + i) % bufferSize];

  return samples;
}

static std::vector<float> stereoToMono(const std::vector<float> &interleaved) {
  std::vector<float> mono;
  mono.reserve(interleaved.size() / 2);

  for (size_t i = 0; i + 1 < interleaved.size(); i += 2) {
    mono.push_back(0.5f * (interleaved[i] + interleaved[i + 1]));
  }

  return mono;
}

static float computeRMS(const std::vector<float> &samples) {
  if (samples.empty())
    return 0.0f;

  double sum = 0.0;
  for (float s : samples) {
    sum += (double)s * (double)s;
  }

  return (float)std::sqrt(sum / (double)samples.size());
}

static std::vector<float>
computeBarLevels(const std::vector<float> &monoSamples, int barCount) {
  std::vector<float> levels(barCount, 0.0f);
  if (monoSamples.empty() || barCount <= 0)
    return levels;

  const size_t windowSize =
      std::max<size_t>(1, monoSamples.size() / (size_t)barCount);

  for (int bar = 0; bar < barCount; ++bar) {
    const size_t start = (size_t)bar * windowSize;
    const size_t end = std::min(monoSamples.size(), start + windowSize);

    if (start >= monoSamples.size())
      break;

    float acc = 0.0f;
    for (size_t i = start; i < end; ++i) {
      acc += std::fabs(monoSamples[i]);
    }

    float avg = acc / (float)std::max<size_t>(1, end - start);

    // Mild shaping so quiet motion still shows up visually.
    levels[bar] = std::pow(std::min(avg * 3.0f, 1.0f), 0.8f);
  }

  return levels;
}

static float damp(float current, float target, float factor) {
  return current + (target - current) * factor;
}

int main() {
  ma_result result;
  AudioData audio{};

  // ~1 second of stereo float samples at 48kHz.
  audio.ringBuffer.resize(48000 * 2, 0.0f);

  // Engine with no device: we will pull samples manually in the callback.
  ma_engine_config engineConfig = ma_engine_config_init();
  engineConfig.noDevice = MA_TRUE;
  engineConfig.sampleRate = 48000;
  engineConfig.channels = 2;

  result = ma_engine_init(&engineConfig, &audio.engine);
  if (result != MA_SUCCESS) {
    std::printf("Failed to init engine: %d\n", result);
    return -1;
  }

  result = ma_sound_init_from_file(&audio.engine, "sound.wav", 0, NULL, NULL,
                                   &audio.sound);
  if (result != MA_SUCCESS) {
    std::printf("Failed to load sound.wav: %d\n", result);
    ma_engine_uninit(&audio.engine);
    return -1;
  }

  ma_device_config deviceConfig =
      ma_device_config_init(ma_device_type_playback);
  deviceConfig.playback.format = ma_format_f32;
  deviceConfig.playback.channels = 2;
  deviceConfig.sampleRate = 48000;
  deviceConfig.dataCallback = data_callback;
  deviceConfig.pUserData = &audio;

  ma_device device;
  result = ma_device_init(NULL, &deviceConfig, &device);
  if (result != MA_SUCCESS) {
    std::printf("Failed to init playback device: %d\n", result);
    ma_sound_uninit(&audio.sound);
    ma_engine_uninit(&audio.engine);
    return -1;
  }

  result = ma_sound_start(&audio.sound);
  if (result != MA_SUCCESS) {
    std::printf("Failed to start sound: %d\n", result);
    ma_device_uninit(&device);
    ma_sound_uninit(&audio.sound);
    ma_engine_uninit(&audio.engine);
    return -1;
  }

  result = ma_device_start(&device);
  if (result != MA_SUCCESS) {
    std::printf("Failed to start device: %d\n", result);
    ma_device_uninit(&device);
    ma_sound_uninit(&audio.sound);
    ma_engine_uninit(&audio.engine);
    return -1;
  }

  // ---- raylib setup ----
  const int screenWidth = 1280;
  const int screenHeight = 720;

  InitWindow(screenWidth, screenHeight, "3D Audio Visualizer");
  SetTargetFPS(144);

  Camera3D camera = {0};
  camera.position = {6.0f, 4.0f, 6.0f};
  camera.target = {0.0f, 1.2f, 0.0f};
  camera.up = {0.0f, 1.0f, 0.0f};
  camera.fovy = 45.0f;
  camera.projection = CAMERA_PERSPECTIVE;

  const int barCount = 48;
  std::vector<float> smoothedBars(barCount, 0.0f);
  float smoothedRms = 0.0f;

  while (!WindowShouldClose()) {
    const float t = (float)GetTime();

    // Pull latest audio data for this frame.
    std::vector<float> interleaved =
        getLatestInterleavedSamples(audio, 2048 * 2);
    std::vector<float> mono = stereoToMono(interleaved);

    float rms = computeRMS(mono);
    std::vector<float> barLevels = computeBarLevels(mono, barCount);

    // Smooth visuals so they feel less jittery.
    smoothedRms = damp(smoothedRms, rms, 0.12f);
    for (int i = 0; i < barCount; ++i) {
      smoothedBars[i] = damp(smoothedBars[i], barLevels[i], 0.18f);
    }

    // Simple slow orbit camera.
    camera.position = {std::sinf(t * 0.25f) * 7.0f,
                       3.5f + std::sinf(t * 0.17f) * 0.8f,
                       std::cosf(t * 0.25f) * 7.0f};
    camera.target = {0.0f, 1.0f + smoothedRms * 2.0f, 0.0f};

    // Visual parameters.
    float sphereRadius = 1.0f + smoothedRms * 3.5f;
    float sphereY = 1.1f + smoothedRms * 0.4f;
    Color coreColor = ColorFromHSV(200.0f + smoothedRms * 120.0f, 0.75f, 0.95f);
    Color wireColor = ColorFromHSV(320.0f - smoothedRms * 100.0f, 0.45f, 1.0f);

    BeginDrawing();
    ClearBackground(Color{8, 10, 18, 255});

    BeginMode3D(camera);

    // Floor grid
    DrawGrid(20, 1.0f);

    // Central pulsing sphere
    DrawSphere({0.0f, sphereY, 0.0f}, sphereRadius, coreColor);
    DrawSphereWires({0.0f, sphereY, 0.0f}, sphereRadius * 1.03f, 16, 16,
                    wireColor);

    // Ring of reactive bars
    for (int i = 0; i < barCount; ++i) {
      float angle = ((float)i / (float)barCount) * 2.0f * PI;
      float radius = 3.6f;
      float level = smoothedBars[i];

      float h = 0.15f + level * 5.0f;
      float x = std::cosf(angle) * radius;
      float z = std::sinf(angle) * radius;

      Vector3 pos = {x, h * 0.5f, z};
      Color c = ColorFromHSV((float)i * (360.0f / (float)barCount) + t * 20.0f,
                             0.8f, 0.5f + level * 0.5f);

      DrawCube(pos, 0.18f, h, 0.18f, c);
      DrawCubeWires(pos, 0.18f, h, 0.18f, Fade(WHITE, 0.15f));
    }

    // Small orbiting satellites for extra motion
    for (int i = 0; i < 8; ++i) {
      float a = t * (0.6f + 0.08f * i) + i * (2.0f * PI / 8.0f);
      float r = 1.8f + smoothedRms * 0.8f;
      Vector3 p = {std::cosf(a) * r,
                   1.0f + std::sinf(a * 2.0f) * 0.4f + smoothedRms * 1.2f,
                   std::sinf(a) * r};
      DrawSphere(p, 0.08f + smoothedRms * 0.15f,
                 ColorFromHSV(40.0f + i * 35.0f + t * 30.0f, 0.5f, 1.0f));
    }

    EndMode3D();

    DrawText("raylib starter visualizer", 20, 20, 24, RAYWHITE);
    DrawText(TextFormat("RMS: %.4f", smoothedRms), 20, 52, 20,
             Fade(RAYWHITE, 0.85f));
    DrawText("Next upgrade: FFT + shaders + postprocessing", 20, 78, 18,
             Fade(SKYBLUE, 0.8f));
    DrawFPS(screenWidth - 95, 20);

    EndDrawing();
  }

  CloseWindow();

  ma_device_uninit(&device);
  ma_sound_uninit(&audio.sound);
  ma_engine_uninit(&audio.engine);

  return 0;
}

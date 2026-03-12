#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "dsp/wav.h"

namespace {
constexpr int kBufferSize = 64;

struct Config {
  std::string model_path;
  std::string input_wav_path;
  std::string output_wav_path = "nam_output.wav";
};

void PrintUsage(const char* program_name) {
  std::cerr
      << "usage: " << program_name << " --model PATH --input-wav PATH [--output-wav PATH]\n";
}

bool ParseStringArg(const std::string& arg,
                    const std::string& flag,
                    int& index,
                    int argc,
                    char** argv,
                    std::string& value_out) {
  const std::string prefix = flag + "=";
  if (arg == flag) {
    if (index + 1 >= argc) {
      std::cerr << "Missing value for " << flag << "\n";
      return false;
    }
    value_out = argv[++index];
    return true;
  }
  if (arg.rfind(prefix, 0) == 0) {
    value_out = arg.substr(prefix.size());
    return true;
  }
  return false;
}

bool SaveWavFloat32(const std::string& file_name,
                    const std::vector<float>& samples,
                    double sample_rate_hz) {
  std::ofstream out(file_name, std::ios::binary);
  if (!out) {
    std::cerr << "Failed to open output WAV " << file_name << "\n";
    return false;
  }

  const std::uint32_t data_size =
      static_cast<std::uint32_t>(samples.size() * sizeof(float));
  const std::uint32_t chunk_size = 36 + data_size;
  const std::uint16_t audio_format = 3;
  const std::uint16_t num_channels = 1;
  const std::uint32_t sample_rate = static_cast<std::uint32_t>(sample_rate_hz);
  const std::uint32_t byte_rate = sample_rate * sizeof(float);
  const std::uint16_t block_align = sizeof(float);
  const std::uint16_t bits_per_sample = 32;
  const std::uint32_t fmt_size = 16;

  out.write("RIFF", 4);
  out.write(reinterpret_cast<const char*>(&chunk_size), 4);
  out.write("WAVE", 4);

  out.write("fmt ", 4);
  out.write(reinterpret_cast<const char*>(&fmt_size), 4);
  out.write(reinterpret_cast<const char*>(&audio_format), 2);
  out.write(reinterpret_cast<const char*>(&num_channels), 2);
  out.write(reinterpret_cast<const char*>(&sample_rate), 4);
  out.write(reinterpret_cast<const char*>(&byte_rate), 4);
  out.write(reinterpret_cast<const char*>(&block_align), 2);
  out.write(reinterpret_cast<const char*>(&bits_per_sample), 2);

  out.write("data", 4);
  out.write(reinterpret_cast<const char*>(&data_size), 4);
  out.write(reinterpret_cast<const char*>(samples.data()), data_size);

  return static_cast<bool>(out);
}
}  // namespace

int main(int argc, char** argv) {
  Config config;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 0;
    }
    if (ParseStringArg(arg, "--model", i, argc, argv, config.model_path) ||
        ParseStringArg(arg, "--input-wav", i, argc, argv, config.input_wav_path) ||
        ParseStringArg(arg, "--output-wav", i, argc, argv, config.output_wav_path)) {
      continue;
    }

    std::cerr << "Unknown argument: " << arg << "\n";
    PrintUsage(argv[0]);
    return 1;
  }

  if (config.model_path.empty() || config.input_wav_path.empty()) {
    PrintUsage(argv[0]);
    return 1;
  }

  auto model = nam::get_dsp(std::filesystem::path(config.model_path));
  if (!model) {
    std::cerr << "Failed to load NAM model from " << config.model_path << "\n";
    return 1;
  }

  std::vector<float> input_audio;
  double input_sample_rate_hz = 0.0;
  const auto load_result =
      dsp::wav::Load(config.input_wav_path.c_str(), input_audio, input_sample_rate_hz);
  if (load_result != dsp::wav::LoadReturnCode::SUCCESS) {
    std::cerr << "Failed to load input WAV: "
              << dsp::wav::GetMsgForLoadReturnCode(load_result) << "\n";
    return 1;
  }

  const double expected_rate_hz = model->GetExpectedSampleRate();
  if (expected_rate_hz > 0.0 &&
      std::abs(input_sample_rate_hz - expected_rate_hz) > 0.5) {
    std::cerr << "Input WAV sample rate (" << input_sample_rate_hz
              << " Hz) does not match model expected rate (" << expected_rate_hz
              << " Hz)\n";
    return 1;
  }

  const double sample_rate_hz =
      expected_rate_hz > 0.0 ? expected_rate_hz : input_sample_rate_hz;
  model->ResetAndPrewarm(sample_rate_hz, kBufferSize);

  if (model->NumInputChannels() != 1) {
    std::cerr << "Only mono-input NAM models are supported right now\n";
    return 1;
  }

  std::vector<std::vector<NAM_SAMPLE>> input_buffers(model->NumInputChannels());
  std::vector<std::vector<NAM_SAMPLE>> output_buffers(model->NumOutputChannels());
  std::vector<NAM_SAMPLE*> input_ptrs(model->NumInputChannels());
  std::vector<NAM_SAMPLE*> output_ptrs(model->NumOutputChannels());

  for (int ch = 0; ch < model->NumInputChannels(); ++ch) {
    input_buffers[ch].resize(kBufferSize, 0.0);
    input_ptrs[ch] = input_buffers[ch].data();
  }
  for (int ch = 0; ch < model->NumOutputChannels(); ++ch) {
    output_buffers[ch].resize(kBufferSize, 0.0);
    output_ptrs[ch] = output_buffers[ch].data();
  }

  std::vector<float> output_audio;
  output_audio.reserve(input_audio.size());

  std::size_t read_pos = 0;
  while (read_pos < input_audio.size()) {
    const std::size_t to_read =
        std::min<std::size_t>(kBufferSize, input_audio.size() - read_pos);

    for (std::size_t i = 0; i < to_read; ++i) {
      input_buffers[0][i] = static_cast<NAM_SAMPLE>(input_audio[read_pos + i]);
    }
    for (std::size_t i = to_read; i < static_cast<std::size_t>(kBufferSize); ++i) {
      input_buffers[0][i] = 0.0;
    }

    model->process(input_ptrs.data(), output_ptrs.data(), static_cast<int>(to_read));

    for (std::size_t i = 0; i < to_read; ++i) {
      output_audio.push_back(static_cast<float>(output_buffers[0][i]));
    }

    read_pos += to_read;
  }

  if (!SaveWavFloat32(config.output_wav_path, output_audio, sample_rate_hz)) {
    return 1;
  }

  std::cout << "Rendered " << config.input_wav_path
            << " through " << config.model_path
            << " to " << config.output_wav_path << "\n";
  return 0;
}

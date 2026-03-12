#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../tube_stage.h"

namespace {
constexpr double kPi = 3.14159265358979323846;

struct Config {
  std::string output_prefix = "tube_stage_test";
  std::string preset = "marshall";
  double frequency_hz = 82.41;
  double duration_seconds = 2.0;
  double sample_rate_hz = 48000.0;
  double amplitude = 0.25;
  double drive_db = 16.0;
  double level_db = -6.0;
  double bright_db = 3.0;
  double bias_trim = 0.02;
};

void PrintUsage(const char* program_name) {
  std::cerr
      << "usage: " << program_name << " [options]\n"
      << "  --output-prefix NAME   Base name for *_input.wav and *_output.wav\n"
      << "  --preset NAME          marshall or fender\n"
      << "  --frequency-hz VALUE   Input sine frequency, default 82.41\n"
      << "  --duration VALUE       Duration in seconds, default 2.0\n"
      << "  --sample-rate VALUE    Sample rate, default 48000\n"
      << "  --amplitude VALUE      Input amplitude 0..1, default 0.25\n"
      << "  --drive-db VALUE       Stage drive, default 16\n"
      << "  --level-db VALUE       Output trim, default -6\n"
      << "  --bright-db VALUE      Bright boost, default 3\n"
      << "  --bias-trim VALUE      Bias trim, default 0.02\n";
}

bool ParseDoubleArg(const std::string& arg,
                    const std::string& flag,
                    int& index,
                    int argc,
                    char** argv,
                    double& value_out) {
  const std::string prefix = flag + "=";
  try {
    if (arg == flag) {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for " << flag << "\n";
        return false;
      }
      value_out = std::stod(argv[++index]);
      return true;
    }
    if (arg.rfind(prefix, 0) == 0) {
      value_out = std::stod(arg.substr(prefix.size()));
      return true;
    }
  } catch (const std::exception&) {
    std::cerr << "Invalid value for " << flag << "\n";
    return false;
  }
  return false;
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

std::vector<float> GenerateSineWave(const Config& config) {
  const std::size_t frame_count =
      static_cast<std::size_t>(config.duration_seconds * config.sample_rate_hz);
  std::vector<float> samples(frame_count, 0.0f);

  const std::size_t fade_samples =
      static_cast<std::size_t>(0.010 * config.sample_rate_hz);

  for (std::size_t i = 0; i < frame_count; ++i) {
    const double t = static_cast<double>(i) / config.sample_rate_hz;
    double gain = config.amplitude;

    if (fade_samples > 0 && i < fade_samples) {
      gain *= static_cast<double>(i) / static_cast<double>(fade_samples);
    }
    if (fade_samples > 0 && i + fade_samples >= frame_count) {
      const std::size_t tail_index = frame_count - i - 1;
      gain *= static_cast<double>(tail_index) / static_cast<double>(fade_samples);
    }

    samples[i] = static_cast<float>(
        gain * std::sin(2.0 * kPi * config.frequency_hz * t));
  }

  return samples;
}

void WriteLe16(std::ofstream& stream, std::uint16_t value) {
  stream.put(static_cast<char>(value & 0xff));
  stream.put(static_cast<char>((value >> 8) & 0xff));
}

void WriteLe32(std::ofstream& stream, std::uint32_t value) {
  stream.put(static_cast<char>(value & 0xff));
  stream.put(static_cast<char>((value >> 8) & 0xff));
  stream.put(static_cast<char>((value >> 16) & 0xff));
  stream.put(static_cast<char>((value >> 24) & 0xff));
}

bool WriteMonoWav(const std::string& path,
                  const std::vector<float>& samples,
                  std::uint32_t sample_rate_hz) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::cerr << "Failed to open " << path << " for writing\n";
    return false;
  }

  const std::uint16_t num_channels = 1;
  const std::uint16_t bits_per_sample = 16;
  const std::uint32_t byte_rate =
      sample_rate_hz * num_channels * bits_per_sample / 8;
  const std::uint16_t block_align = num_channels * bits_per_sample / 8;
  const std::uint32_t data_size =
      static_cast<std::uint32_t>(samples.size() * block_align);
  const std::uint32_t riff_size = 36 + data_size;

  out.write("RIFF", 4);
  WriteLe32(out, riff_size);
  out.write("WAVE", 4);

  out.write("fmt ", 4);
  WriteLe32(out, 16);
  WriteLe16(out, 1);
  WriteLe16(out, num_channels);
  WriteLe32(out, sample_rate_hz);
  WriteLe32(out, byte_rate);
  WriteLe16(out, block_align);
  WriteLe16(out, bits_per_sample);

  out.write("data", 4);
  WriteLe32(out, data_size);

  for (float sample : samples) {
    const float clamped = std::clamp(sample, -1.0f, 1.0f);
    const auto pcm = static_cast<std::int16_t>(std::lrint(clamped * 32767.0f));
    WriteLe16(out, static_cast<std::uint16_t>(pcm));
  }

  return static_cast<bool>(out);
}

TubeStageSpec PresetSpec(const std::string& preset_name) {
  if (preset_name == "fender") {
    return FenderStage1Spec();
  }
  return MarshallStage1Spec();
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
    if (ParseStringArg(arg, "--output-prefix", i, argc, argv, config.output_prefix) ||
        ParseStringArg(arg, "--preset", i, argc, argv, config.preset) ||
        ParseDoubleArg(arg, "--frequency-hz", i, argc, argv, config.frequency_hz) ||
        ParseDoubleArg(arg, "--duration", i, argc, argv, config.duration_seconds) ||
        ParseDoubleArg(arg, "--sample-rate", i, argc, argv, config.sample_rate_hz) ||
        ParseDoubleArg(arg, "--amplitude", i, argc, argv, config.amplitude) ||
        ParseDoubleArg(arg, "--drive-db", i, argc, argv, config.drive_db) ||
        ParseDoubleArg(arg, "--level-db", i, argc, argv, config.level_db) ||
        ParseDoubleArg(arg, "--bright-db", i, argc, argv, config.bright_db) ||
        ParseDoubleArg(arg, "--bias-trim", i, argc, argv, config.bias_trim)) {
      continue;
    }

    std::cerr << "Unknown argument: " << arg << "\n";
    PrintUsage(argv[0]);
    return 1;
  }

  if (config.sample_rate_hz <= 0.0 || config.duration_seconds <= 0.0) {
    std::cerr << "Sample rate and duration must be positive\n";
    return 1;
  }

  auto input_samples = GenerateSineWave(config);
  auto output_samples = input_samples;

  TubeStage stage;
  stage.SetSampleRate(config.sample_rate_hz);
  stage.SetSpec(PresetSpec(config.preset));

  TubeStageControls controls;
  controls.drive_db = config.drive_db;
  controls.level_db = config.level_db;
  controls.bright_db = config.bright_db;
  controls.bias_trim = config.bias_trim;
  stage.SetControls(controls);

  for (std::size_t i = 0; i < output_samples.size(); ++i) {
    output_samples[i] = stage.Process(output_samples[i]);
  }

  const std::string input_path = config.output_prefix + "_input.wav";
  const std::string output_path = config.output_prefix + "_output.wav";
  if (!WriteMonoWav(input_path, input_samples,
                    static_cast<std::uint32_t>(config.sample_rate_hz)) ||
      !WriteMonoWav(output_path, output_samples,
                    static_cast<std::uint32_t>(config.sample_rate_hz))) {
    return 1;
  }

  std::cout << "Wrote " << input_path << " and " << output_path << "\n";
  std::cout << "Preset: " << config.preset
            << ", frequency: " << config.frequency_hz
            << " Hz, duration: " << config.duration_seconds
            << " s\n";
  return 0;
}

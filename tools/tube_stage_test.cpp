#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "../amp_profile.h"
#include "../effects/klon_effect.h"
#include "../effects/plate_reverb_effect.h"
#include "../effects/tubescreamer_effect.h"
#include "../power_stage.h"
#include "../preamp.h"
#include "../preamp_profile.h"
#include "../tube_stage.h"

namespace {
constexpr double kPi = 3.14159265358979323846;

struct Config {
  std::string output_prefix = "tube_stage_test";
  std::string amp_name;
  std::string amp_file;
  std::string preamp_name;
  std::string preamp_file;
  std::string preset = "marshall";
  std::string effect = "none";
  std::string input_wav;
  int input_channel = 0;
  double frequency_hz = 82.41;
  double duration_seconds = 2.0;
  double sample_rate_hz = 48000.0;
  double amplitude = 0.25;
  std::optional<double> drive_db;
  std::optional<double> level_db;
  std::optional<double> bright_db;
  std::optional<double> bias_trim;
  std::optional<double> bass;
  std::optional<double> mid;
  std::optional<double> treble;
  std::optional<double> presence;
  std::optional<PowerTubeType> power_tube_type;
  std::optional<double> power_drive_db;
  std::optional<double> power_level_db;
  std::optional<double> power_bias_trim;
  double effect_drive = 0.5;
  double effect_tone = 0.5;
  double effect_level_db = 0.0;
  double effect_clean_blend = 0.45;
};

void PrintUsage(const char* program_name) {
  std::cerr
      << "usage: " << program_name << " [options]\n"
      << "  --output-prefix NAME   Base name for *_input.wav and *_output.wav\n"
      << "  --amp NAME             Amp profile from ./amps, e.g. marshall_jtm45\n"
      << "  --amp-file PATH        Explicit amp profile file path\n"
      << "  --preamp NAME          Preamp profile from ./preamps\n"
      << "  --preamp-file PATH     Explicit preamp profile file path\n"
      << "  --power-tube NAME      6V6, 6L6, EL34, or EL84\n"
      << "  --preset NAME          marshall or fender\n"
      << "  --effect NAME          none, klon, tubescreamer, or plate, default none\n"
      << "  --input-wav PATH       Use a WAV file instead of generating a sine\n"
      << "  --input-channel N      Channel to read from WAV input, default 0\n"
      << "  --frequency-hz VALUE   Input sine frequency, default 82.41\n"
      << "  --duration VALUE       Duration in seconds, default 2.0\n"
      << "  --sample-rate VALUE    Sample rate, default 48000\n"
      << "  --amplitude VALUE      Input amplitude 0..1, default 0.25\n"
      << "  --drive-db VALUE       Stage drive, default 16\n"
      << "  --level-db VALUE       Output trim, default -6\n"
      << "  --bright-db VALUE      Bright boost, default 3\n"
      << "  --bias-trim VALUE      Bias trim, default 0.02\n"
      << "  --bass VALUE           Tone stack bass 0..10, default 5\n"
      << "  --mid VALUE            Tone stack mid 0..10, default 5\n"
      << "  --treble VALUE         Tone stack treble 0..10, default 5\n"
      << "  --presence VALUE       Tone stack presence 0..10, default 5\n"
      << "  --power-drive-db VAL   Master / phase inverter drive, amp default\n"
      << "  --power-level-db VAL   Final power-stage output trim, amp default\n"
      << "  --power-bias-trim VAL  Power-stage bias trim, amp default\n"
      << "  --effect-drive VALUE   Pedal drive, or plate mix 0..1, default 0.5\n"
      << "  --effect-tone VALUE    Pedal tone, or plate brightness 0..1, default 0.5\n"
      << "  --effect-level-db VAL  Output trim for effect, default 0\n"
      << "  --effect-clean-blend V Klon clean blend, or plate decay 0..1, default 0.45\n";
}

std::optional<PreampProfile> ResolvePreampProfile(const Config& config) {
  if (!config.preamp_file.empty()) {
    std::string error;
    auto profile = LoadPreampProfileFromFile(config.preamp_file, &error);
    if (!profile && !error.empty()) {
      std::cerr << error << "\n";
    }
    return profile;
  }

  if (!config.preamp_name.empty()) {
    std::string error;
    auto profile =
        LoadPreampProfileFromFile("preamps/" + config.preamp_name + ".preamp", &error);
    if (!profile && !error.empty()) {
      std::cerr << error << "\n";
    }
    return profile;
  }

  return std::nullopt;
}

std::optional<AmpProfile> ResolveAmpProfile(const Config& config) {
  if (!config.amp_file.empty()) {
    std::string error;
    auto profile = LoadAmpProfileFromFile(config.amp_file, &error);
    if (!profile && !error.empty()) {
      std::cerr << error << "\n";
    }
    return profile;
  }

  if (!config.amp_name.empty()) {
    const std::string path = "amps/" + config.amp_name + ".amp";
    std::string error;
    auto profile = LoadAmpProfileFromFile(path, &error);
    if (!profile && !error.empty()) {
      std::cerr << error << "\n";
    }
    return profile;
  }

  return BuiltinPresetProfile(config.preset);
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

bool ParseOptionalDoubleArg(const std::string& arg,
                            const std::string& flag,
                            int& index,
                            int argc,
                            char** argv,
                            std::optional<double>& value_out) {
  double parsed = 0.0;
  if (!ParseDoubleArg(arg, flag, index, argc, argv, parsed)) {
    return false;
  }
  value_out = parsed;
  return true;
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

bool ParseIntArg(const std::string& arg,
                 const std::string& flag,
                 int& index,
                 int argc,
                 char** argv,
                 int& value_out) {
  const std::string prefix = flag + "=";
  try {
    if (arg == flag) {
      if (index + 1 >= argc) {
        std::cerr << "Missing value for " << flag << "\n";
        return false;
      }
      value_out = std::stoi(argv[++index]);
      return true;
    }
    if (arg.rfind(prefix, 0) == 0) {
      value_out = std::stoi(arg.substr(prefix.size()));
      return true;
    }
  } catch (const std::exception&) {
    std::cerr << "Invalid value for " << flag << "\n";
    return false;
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

std::uint16_t ReadLe16(std::ifstream& stream) {
  unsigned char bytes[2] = {};
  stream.read(reinterpret_cast<char*>(bytes), 2);
  return static_cast<std::uint16_t>(bytes[0] | (bytes[1] << 8));
}

std::uint32_t ReadLe32(std::ifstream& stream) {
  unsigned char bytes[4] = {};
  stream.read(reinterpret_cast<char*>(bytes), 4);
  return static_cast<std::uint32_t>(bytes[0] |
                                    (bytes[1] << 8) |
                                    (bytes[2] << 16) |
                                    (bytes[3] << 24));
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

bool ReadInputWav(const std::string& path,
                  int requested_channel,
                  std::vector<float>& samples_out,
                  std::uint32_t& sample_rate_out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::cerr << "Failed to open input WAV: " << path << "\n";
    return false;
  }

  char riff[4] = {};
  in.read(riff, 4);
  if (std::strncmp(riff, "RIFF", 4) != 0) {
    std::cerr << "Input is not a RIFF WAV file\n";
    return false;
  }

  (void)ReadLe32(in);  // RIFF size

  char wave[4] = {};
  in.read(wave, 4);
  if (std::strncmp(wave, "WAVE", 4) != 0) {
    std::cerr << "Input is not a WAVE file\n";
    return false;
  }

  std::uint16_t audio_format = 0;
  std::uint16_t num_channels = 0;
  std::uint16_t bits_per_sample = 0;
  std::vector<char> data_chunk;

  while (in && (!audio_format || data_chunk.empty())) {
    char chunk_id[4] = {};
    in.read(chunk_id, 4);
    if (!in) {
      break;
    }
    const std::uint32_t chunk_size = ReadLe32(in);

    if (std::strncmp(chunk_id, "fmt ", 4) == 0) {
      audio_format = ReadLe16(in);
      num_channels = ReadLe16(in);
      sample_rate_out = ReadLe32(in);
      (void)ReadLe32(in);  // byte rate
      (void)ReadLe16(in);  // block align
      bits_per_sample = ReadLe16(in);
      if (chunk_size > 16) {
        in.seekg(static_cast<std::streamoff>(chunk_size - 16), std::ios::cur);
      }
    } else if (std::strncmp(chunk_id, "data", 4) == 0) {
      data_chunk.resize(chunk_size);
      in.read(data_chunk.data(), static_cast<std::streamsize>(chunk_size));
    } else {
      in.seekg(static_cast<std::streamoff>(chunk_size), std::ios::cur);
    }

    if (chunk_size % 2 == 1) {
      in.seekg(1, std::ios::cur);
    }
  }

  if (!audio_format || !num_channels || bits_per_sample == 0 || data_chunk.empty()) {
    std::cerr << "Input WAV is missing fmt or data chunks\n";
    return false;
  }

  if (requested_channel < 0 || requested_channel >= num_channels) {
    std::cerr << "Requested channel " << requested_channel
              << " is out of range for " << num_channels << "-channel input\n";
    return false;
  }

  const std::size_t bytes_per_sample = bits_per_sample / 8;
  const std::size_t frame_size = bytes_per_sample * num_channels;
  if (frame_size == 0 || data_chunk.size() % frame_size != 0) {
    std::cerr << "Unsupported or malformed WAV frame layout\n";
    return false;
  }

  const std::size_t frame_count = data_chunk.size() / frame_size;
  samples_out.resize(frame_count);

  const char* raw = data_chunk.data();
  for (std::size_t i = 0; i < frame_count; ++i) {
    const std::size_t base = i * frame_size + requested_channel * bytes_per_sample;

    if (audio_format == 1 && bits_per_sample == 16) {
      std::int16_t sample = 0;
      std::memcpy(&sample, raw + base, sizeof(sample));
      samples_out[i] = static_cast<float>(sample / 32768.0f);
    } else if (audio_format == 3 && bits_per_sample == 32) {
      float sample = 0.0f;
      std::memcpy(&sample, raw + base, sizeof(sample));
      samples_out[i] = sample;
    } else {
      std::cerr << "Unsupported WAV format. Supported: PCM16 or float32 WAV\n";
      return false;
    }
  }

  return true;
}

bool IsEffectEnabled(const std::string& effect_name) {
  return effect_name == "klon" ||
         effect_name == "tubescreamer" ||
         effect_name == "plate";
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
        ParseStringArg(arg, "--amp", i, argc, argv, config.amp_name) ||
        ParseStringArg(arg, "--amp-file", i, argc, argv, config.amp_file) ||
        ParseStringArg(arg, "--preamp", i, argc, argv, config.preamp_name) ||
        ParseStringArg(arg, "--preamp-file", i, argc, argv, config.preamp_file) ||
        ParseStringArg(arg, "--preset", i, argc, argv, config.preset) ||
        ParseStringArg(arg, "--effect", i, argc, argv, config.effect) ||
        ParseStringArg(arg, "--input-wav", i, argc, argv, config.input_wav) ||
        ParseIntArg(arg, "--input-channel", i, argc, argv, config.input_channel) ||
        ParseDoubleArg(arg, "--frequency-hz", i, argc, argv, config.frequency_hz) ||
        ParseDoubleArg(arg, "--duration", i, argc, argv, config.duration_seconds) ||
        ParseDoubleArg(arg, "--sample-rate", i, argc, argv, config.sample_rate_hz) ||
        ParseDoubleArg(arg, "--amplitude", i, argc, argv, config.amplitude) ||
        ParseOptionalDoubleArg(arg, "--drive-db", i, argc, argv, config.drive_db) ||
        ParseOptionalDoubleArg(arg, "--level-db", i, argc, argv, config.level_db) ||
        ParseOptionalDoubleArg(arg, "--bright-db", i, argc, argv, config.bright_db) ||
        ParseOptionalDoubleArg(arg, "--bias-trim", i, argc, argv, config.bias_trim) ||
        ParseOptionalDoubleArg(arg, "--bass", i, argc, argv, config.bass) ||
        ParseOptionalDoubleArg(arg, "--mid", i, argc, argv, config.mid) ||
        ParseOptionalDoubleArg(arg, "--treble", i, argc, argv, config.treble) ||
        ParseOptionalDoubleArg(arg, "--presence", i, argc, argv, config.presence) ||
        ParseOptionalDoubleArg(arg, "--power-drive-db", i, argc, argv, config.power_drive_db) ||
        ParseOptionalDoubleArg(arg, "--power-level-db", i, argc, argv, config.power_level_db) ||
        ParseOptionalDoubleArg(arg, "--power-bias-trim", i, argc, argv, config.power_bias_trim) ||
        ParseDoubleArg(arg, "--effect-drive", i, argc, argv, config.effect_drive) ||
        ParseDoubleArg(arg, "--effect-tone", i, argc, argv, config.effect_tone) ||
        ParseDoubleArg(arg, "--effect-level-db", i, argc, argv, config.effect_level_db) ||
        ParseDoubleArg(arg, "--effect-clean-blend", i, argc, argv, config.effect_clean_blend)) {
      continue;
    }
    if (arg == "--power-tube" || arg.rfind("--power-tube=", 0) == 0) {
      std::string value;
      if (!ParseStringArg(arg, "--power-tube", i, argc, argv, value)) {
        return 1;
      }
      PowerTubeType parsed;
      if (!ParsePowerTubeType(value, parsed)) {
        std::cerr << "Unknown power tube: " << value << "\n";
        return 1;
      }
      config.power_tube_type = parsed;
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

  std::vector<float> input_samples;
  if (!config.input_wav.empty()) {
    std::uint32_t wav_sample_rate = 0;
    if (!ReadInputWav(config.input_wav, config.input_channel, input_samples, wav_sample_rate)) {
      return 1;
    }
    config.sample_rate_hz = static_cast<double>(wav_sample_rate);
  } else {
    input_samples = GenerateSineWave(config);
  }
  auto output_samples = input_samples;

  if (config.effect != "none" && !IsEffectEnabled(config.effect)) {
    std::cerr << "Unknown effect: " << config.effect << "\n";
    return 1;
  }

  KlonEffect effect;
  TubeScreamerEffect tubescreamer;
  PlateReverbEffect plate;
  if (IsEffectEnabled(config.effect)) {
    if (config.effect == "klon") {
      effect.SetSampleRate(config.sample_rate_hz);
      KlonControls controls;
      controls.drive = config.effect_drive;
      controls.tone = config.effect_tone;
      controls.level_db = config.effect_level_db;
      controls.clean_blend = config.effect_clean_blend;
      effect.SetControls(controls);
    } else if (config.effect == "tubescreamer") {
      tubescreamer.SetSampleRate(config.sample_rate_hz);
      TubeScreamerControls controls;
      controls.drive = config.effect_drive;
      controls.tone = config.effect_tone;
      controls.level_db = config.effect_level_db;
      tubescreamer.SetControls(controls);
    } else {
      plate.SetSampleRate(config.sample_rate_hz);
      PlateReverbControls controls;
      controls.mix = config.effect_drive;
      controls.brightness = config.effect_tone;
      controls.level_db = config.effect_level_db;
      controls.decay = config.effect_clean_blend;
      plate.SetControls(controls);
    }
  }

  auto preamp_profile = ResolvePreampProfile(config);
  auto amp_profile = ResolveAmpProfile(config);
  if (!preamp_profile && !amp_profile) {
    std::cerr << "No valid preamp or amp profile selected\n";
    return 1;
  }
  if (!preamp_profile && amp_profile) {
    preamp_profile = amp_profile->preamp;
  }

  PreampProcessor preamp;
  preamp.SetSampleRate(config.sample_rate_hz);
  preamp.SetProfile(*preamp_profile);

  TubeStageControls controls = preamp_profile->defaults;
  if (config.drive_db.has_value()) {
    controls.drive_db = *config.drive_db;
  }
  if (config.level_db.has_value()) {
    controls.level_db = *config.level_db;
  }
  if (config.bright_db.has_value()) {
    controls.bright_db = *config.bright_db;
  }
  if (config.bias_trim.has_value()) {
    controls.bias_trim = *config.bias_trim;
  }
  if (config.bass.has_value()) {
    controls.bass = *config.bass;
  }
  if (config.mid.has_value()) {
    controls.mid = *config.mid;
  }
  if (config.treble.has_value()) {
    controls.treble = *config.treble;
  }
  if (config.presence.has_value()) {
    controls.presence = *config.presence;
  }
  preamp.SetControls(controls);

  PowerStage power_stage;
  const bool has_power_stage =
      config.power_tube_type.has_value() || (amp_profile && amp_profile->has_power_stage);
  const PowerTubeType power_tube_type =
      config.power_tube_type.value_or(
          amp_profile ? amp_profile->power_tube_type : PowerTubeType::k6V6);
  PowerStageControls power_controls =
      amp_profile ? amp_profile->power_defaults : PowerStageControls{};
  if (config.power_drive_db.has_value()) {
    power_controls.drive_db = *config.power_drive_db;
  }
  if (config.power_level_db.has_value()) {
    power_controls.level_db = *config.power_level_db;
  }
  if (config.power_bias_trim.has_value()) {
    power_controls.bias_trim = *config.power_bias_trim;
  }
  if (has_power_stage) {
    power_stage.SetSampleRate(config.sample_rate_hz);
    power_stage.SetTubeType(power_tube_type);
    power_stage.SetControls(power_controls);
  }

  for (std::size_t i = 0; i < output_samples.size(); ++i) {
    float s = output_samples[i];
    if (config.effect == "klon") {
      s = effect.Process(s);
    } else if (config.effect == "tubescreamer") {
      s = tubescreamer.Process(s);
    }
    s = preamp.Process(s);
    if (has_power_stage) {
      s = power_stage.Process(s);
    }
    if (config.effect == "plate") {
      s = plate.Process(s);
    }
    output_samples[i] = s;
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
  std::cout << "Preamp: " << preamp_profile->name;
  if (!preamp_profile->circuit.empty()) {
    std::cout << " (" << preamp_profile->circuit << ")";
  }
  if (has_power_stage) {
    std::cout << ", power tube: " << PowerTubeTypeName(power_tube_type);
  }
  std::cout << ", effect: " << config.effect;
  if (!config.input_wav.empty()) {
    std::cout << ", input WAV: " << config.input_wav
              << ", channel: " << config.input_channel;
  } else {
    std::cout << ", frequency: " << config.frequency_hz
              << " Hz, duration: " << config.duration_seconds << " s";
  }
  std::cout << "\n";
  return 0;
}

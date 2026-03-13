#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <portaudio.h>
#ifdef __linux__
#include <pa_linux_alsa.h>
#endif

#include "../amp_profile.h"
#include "../effects/klon_effect.h"
#include "../effects/tubescreamer_effect.h"
#include "../live_control.h"
#include "../power_stage.h"
#include "../preamp_profile.h"
#include "../tube_stage.h"

namespace {
constexpr unsigned long kFramesPerBuffer = 128;
constexpr int kInputChannels = 2;
constexpr int kOutputChannels = 2;
constexpr double kSampleRateHz = 48000.0;

std::atomic<bool> g_running{true};

struct Config {
  std::string effect = "none";
  std::string amp_name;
  std::string amp_file;
  std::string preamp_name;
  std::string preamp_file;
  std::string preset = "marshall";
  std::string device_name = "default";
  std::string input_device_name;
  std::string output_device_name;
  std::string control_file;
  std::string alsa_input;
  std::string alsa_output;
  std::optional<double> drive_db;
  std::optional<double> level_db;
  std::optional<double> bright_db;
  std::optional<double> bias_trim;
  std::optional<double> bass;
  std::optional<double> mid;
  std::optional<double> treble;
  std::optional<double> presence;
  std::optional<PowerTubeType> power_tube_type;
  double effect_drive = 0.5;
  double effect_tone = 0.5;
  double effect_level_db = 0.0;
  double effect_clean_blend = 0.45;
};

struct RuntimeSettings {
  std::string amp_name;
  std::string preamp_name;
  EffectType effect = EffectType::kNone;
  TubeStageSpec stage_spec;
  TubeStageControls stage_controls;
  bool has_power_stage = false;
  PowerTubeType power_tube_type = PowerTubeType::k6V6;
  PowerStageControls power_controls;
  KlonControls klon_controls;
  TubeScreamerControls tubescreamer_controls;
};

struct AppState {
  TubeStage stage;
  PowerStage power_stage;
  KlonEffect effect;
  TubeScreamerEffect tubescreamer;
  std::shared_ptr<RuntimeSettings> settings;
  std::string applied_amp_name;
  std::string applied_preamp_name;
  EffectType applied_effect = EffectType::kNone;
  bool applied_power_stage_enabled = false;
  PowerTubeType applied_power_tube_type = PowerTubeType::k6V6;
};

void OnSignal(int) {
  g_running = false;
}

void PrintUsage(const char* program_name) {
  std::cerr
      << "usage: " << program_name << " [options]\n"
      << "  --amp NAME               Amp profile from ./amps, e.g. marshall_jtm45\n"
      << "  --amp-file PATH          Explicit amp profile file path\n"
      << "  --preamp NAME            Preamp profile from ./preamps\n"
      << "  --preamp-file PATH       Explicit preamp profile file path\n"
      << "  --power-tube NAME        6V6, 6L6, EL34, or EL84\n"
      << "  --preset NAME            marshall or fender\n"
      << "  --effect NAME            none, klon, or tubescreamer\n"
      << "  --device NAME            Duplex PortAudio device substring, default default\n"
      << "  --input-device NAME      PortAudio input device substring\n"
      << "  --output-device NAME     PortAudio output device substring\n"
      << "  --control-file PATH      Live control file to poll for updates\n"
      << "  --alsa-device NAME       Linux ALSA device for in/out, e.g. plughw:2,0\n"
      << "  --alsa-input NAME        Linux ALSA input device\n"
      << "  --alsa-output NAME       Linux ALSA output device\n"
      << "  --drive-db VALUE         Tube stage drive override\n"
      << "  --level-db VALUE         Tube stage level override\n"
      << "  --bright-db VALUE        Tube stage bright override\n"
      << "  --bias-trim VALUE        Tube stage bias override\n"
      << "  --bass VALUE             Tone stack bass 0..10 override\n"
      << "  --mid VALUE              Tone stack mid 0..10 override\n"
      << "  --treble VALUE           Tone stack treble 0..10 override\n"
      << "  --presence VALUE         Tone stack presence 0..10 override\n"
      << "  --effect-drive VALUE     Effect drive 0..1, default 0.5\n"
      << "  --effect-tone VALUE      Effect tone 0..1, default 0.5\n"
      << "  --effect-level-db VALUE  Effect output trim, default 0\n"
      << "  --effect-clean-blend V   Klon clean blend 0..1, default 0.45\n"
      << "  --list-devices           Print PortAudio devices and exit\n";
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

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

void CheckPa(PaError err, const char* what) {
  if (err != paNoError) {
    std::cerr << what << ": " << Pa_GetErrorText(err) << "\n";
    std::exit(1);
  }
}

void PrintDevices() {
  const int count = Pa_GetDeviceCount();
  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info) {
      continue;
    }
    std::cerr << "[" << i << "] " << info->name
              << " in=" << info->maxInputChannels
              << " out=" << info->maxOutputChannels << "\n";
  }
}

PaDeviceIndex FindDuplexDevice(const std::string& needle) {
  const std::string lowered_needle = ToLower(needle);
  const int count = Pa_GetDeviceCount();
  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info) {
      continue;
    }
    const std::string name = ToLower(info->name ? info->name : "");
    if (name.find(lowered_needle) != std::string::npos &&
        info->maxInputChannels >= kInputChannels &&
        info->maxOutputChannels >= kOutputChannels) {
      return i;
    }
  }
  return paNoDevice;
}

PaDeviceIndex FindInputDevice(const std::string& needle) {
  const std::string lowered_needle = ToLower(needle);
  const int count = Pa_GetDeviceCount();
  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info) {
      continue;
    }
    const std::string name = ToLower(info->name ? info->name : "");
    if (name.find(lowered_needle) != std::string::npos &&
        info->maxInputChannels >= kInputChannels) {
      return i;
    }
  }
  return paNoDevice;
}

PaDeviceIndex FindOutputDevice(const std::string& needle) {
  const std::string lowered_needle = ToLower(needle);
  const int count = Pa_GetDeviceCount();
  for (int i = 0; i < count; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info) {
      continue;
    }
    const std::string name = ToLower(info->name ? info->name : "");
    if (name.find(lowered_needle) != std::string::npos &&
        info->maxOutputChannels >= kOutputChannels) {
      return i;
    }
  }
  return paNoDevice;
}

bool IsEffectEnabled(const std::string& effect_name) {
  return effect_name == "klon" || effect_name == "tubescreamer";
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
    auto profile = LoadPreampProfileFromFile(
        "preamps/" + config.preamp_name + ".preamp", &error);
    if (!profile && !error.empty()) {
      std::cerr << error << "\n";
    }
    return profile;
  }

  return std::nullopt;
}

std::optional<PreampProfile> ResolvePreampProfileByName(
    const std::string& preamp_name) {
  std::string error;
  auto profile = LoadPreampProfileFromFile(
      "preamps/" + preamp_name + ".preamp", &error);
  if (!profile && !error.empty()) {
    std::cerr << error << "\n";
  }
  return profile;
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

std::optional<AmpProfile> ResolveAmpProfileByName(const std::string& amp_name) {
  std::string error;
  auto profile = LoadAmpProfileFromFile("amps/" + amp_name + ".amp", &error);
  if (!profile && !error.empty()) {
    std::cerr << error << "\n";
  }
  return profile;
}

std::shared_ptr<RuntimeSettings> ResolveRuntimeSettings(
    const AmpProfile& base_profile,
    const Config& config,
    const std::optional<LiveControlState>& live_state) {
  auto settings = std::make_shared<RuntimeSettings>();

  AmpProfile profile = base_profile;
  if (live_state && live_state->amp_name) {
    auto live_profile = ResolveAmpProfileByName(*live_state->amp_name);
    if (live_profile) {
      profile = *live_profile;
    }
  }

  PreampProfile preamp = profile.preamp;
  if (const auto explicit_preamp = ResolvePreampProfile(config)) {
    preamp = *explicit_preamp;
  }
  if (live_state && live_state->preamp_name) {
    auto live_preamp = ResolvePreampProfileByName(*live_state->preamp_name);
    if (live_preamp) {
      preamp = *live_preamp;
    }
  }

  settings->amp_name = profile.name;
  settings->preamp_name = preamp.name;
  settings->stage_spec = preamp.spec;
  settings->stage_controls = preamp.defaults;
  settings->has_power_stage = profile.has_power_stage;
  settings->power_tube_type = profile.power_tube_type;
  settings->power_controls = profile.power_defaults;
  if (config.power_tube_type) {
    settings->has_power_stage = true;
    settings->power_tube_type = *config.power_tube_type;
  }

  if (config.drive_db) settings->stage_controls.drive_db = *config.drive_db;
  if (config.level_db) settings->stage_controls.level_db = *config.level_db;
  if (config.bright_db) settings->stage_controls.bright_db = *config.bright_db;
  if (config.bias_trim) settings->stage_controls.bias_trim = *config.bias_trim;
  if (config.bass) settings->stage_controls.bass = *config.bass;
  if (config.mid) settings->stage_controls.mid = *config.mid;
  if (config.treble) settings->stage_controls.treble = *config.treble;
  if (config.presence) settings->stage_controls.presence = *config.presence;

  if (config.effect == "klon") {
    settings->effect = EffectType::kKlon;
  } else if (config.effect == "tubescreamer") {
    settings->effect = EffectType::kTubeScreamer;
  }

  settings->klon_controls.drive = config.effect_drive;
  settings->klon_controls.tone = config.effect_tone;
  settings->klon_controls.level_db = config.effect_level_db;
  settings->klon_controls.clean_blend = config.effect_clean_blend;

  settings->tubescreamer_controls.drive = config.effect_drive;
  settings->tubescreamer_controls.tone = config.effect_tone;
  settings->tubescreamer_controls.level_db = config.effect_level_db;

  if (live_state) {
    if (live_state->effect) settings->effect = *live_state->effect;
    if (live_state->power_tube_type) {
      settings->has_power_stage = true;
      settings->power_tube_type = *live_state->power_tube_type;
    }
    if (live_state->drive_db) settings->stage_controls.drive_db = *live_state->drive_db;
    if (live_state->level_db) settings->stage_controls.level_db = *live_state->level_db;
    if (live_state->bright_db) settings->stage_controls.bright_db = *live_state->bright_db;
    if (live_state->bias_trim) settings->stage_controls.bias_trim = *live_state->bias_trim;
    if (live_state->bass) settings->stage_controls.bass = *live_state->bass;
    if (live_state->mid) settings->stage_controls.mid = *live_state->mid;
    if (live_state->treble) settings->stage_controls.treble = *live_state->treble;
    if (live_state->presence) settings->stage_controls.presence = *live_state->presence;
    if (live_state->power_drive_db) settings->power_controls.drive_db = *live_state->power_drive_db;
    if (live_state->power_level_db) settings->power_controls.level_db = *live_state->power_level_db;
    if (live_state->power_bias_trim) settings->power_controls.bias_trim = *live_state->power_bias_trim;
    if (live_state->effect_drive) {
      settings->klon_controls.drive = *live_state->effect_drive;
      settings->tubescreamer_controls.drive = *live_state->effect_drive;
    }
    if (live_state->effect_tone) {
      settings->klon_controls.tone = *live_state->effect_tone;
      settings->tubescreamer_controls.tone = *live_state->effect_tone;
    }
    if (live_state->effect_level_db) {
      settings->klon_controls.level_db = *live_state->effect_level_db;
      settings->tubescreamer_controls.level_db = *live_state->effect_level_db;
    }
    if (live_state->effect_clean_blend) {
      settings->klon_controls.clean_blend = *live_state->effect_clean_blend;
    }
  }

  return settings;
}

LiveControlState BuildLiveControlState(const RuntimeSettings& settings) {
  LiveControlState state;
  state.amp_name = settings.amp_name;
  state.preamp_name = settings.preamp_name;
  state.power_tube_type = settings.power_tube_type;
  state.effect = settings.effect;
  state.drive_db = settings.stage_controls.drive_db;
  state.level_db = settings.stage_controls.level_db;
  state.bright_db = settings.stage_controls.bright_db;
  state.bias_trim = settings.stage_controls.bias_trim;
  state.bass = settings.stage_controls.bass;
  state.mid = settings.stage_controls.mid;
  state.treble = settings.stage_controls.treble;
  state.presence = settings.stage_controls.presence;
  if (settings.has_power_stage) {
    state.power_drive_db = settings.power_controls.drive_db;
    state.power_level_db = settings.power_controls.level_db;
    state.power_bias_trim = settings.power_controls.bias_trim;
  }
  state.effect_drive = settings.klon_controls.drive;
  state.effect_tone = settings.klon_controls.tone;
  state.effect_level_db = settings.klon_controls.level_db;
  state.effect_clean_blend = settings.klon_controls.clean_blend;
  return state;
}

int AudioCallback(const void* input_buffer,
                  void* output_buffer,
                  unsigned long frames_per_buffer,
                  const PaStreamCallbackTimeInfo*,
                  PaStreamCallbackFlags,
                  void* user_data) {
  auto* state = static_cast<AppState*>(user_data);
  const auto* in = static_cast<const std::int16_t*>(input_buffer);
  auto* out = static_cast<std::int16_t*>(output_buffer);

  const auto settings = std::atomic_load(&state->settings);
  if (settings) {
    if (state->applied_amp_name != settings->amp_name ||
        state->applied_preamp_name != settings->preamp_name) {
      state->stage.Reset();
      state->power_stage.Reset();
      state->effect.Reset();
      state->tubescreamer.Reset();
      state->applied_amp_name = settings->amp_name;
      state->applied_preamp_name = settings->preamp_name;
    }
    if (state->applied_effect != settings->effect) {
      state->effect.Reset();
      state->tubescreamer.Reset();
      state->applied_effect = settings->effect;
    }
    if (state->applied_power_stage_enabled != settings->has_power_stage ||
        state->applied_power_tube_type != settings->power_tube_type) {
      state->power_stage.Reset();
      state->applied_power_stage_enabled = settings->has_power_stage;
      state->applied_power_tube_type = settings->power_tube_type;
    }

    state->stage.SetSpec(settings->stage_spec);
    state->stage.SetControls(settings->stage_controls);
    if (settings->has_power_stage) {
      state->power_stage.SetTubeType(settings->power_tube_type);
      state->power_stage.SetControls(settings->power_controls);
    }
    state->effect.SetControls(settings->klon_controls);
    state->tubescreamer.SetControls(settings->tubescreamer_controls);
  }

  for (unsigned long i = 0; i < frames_per_buffer; ++i) {
    float s = 0.0f;
    if (in) {
      s = static_cast<float>(in[i * kInputChannels] / 32768.0f);
    }

    if (settings && settings->effect == EffectType::kKlon) {
      s = state->effect.Process(s);
    } else if (settings && settings->effect == EffectType::kTubeScreamer) {
      s = state->tubescreamer.Process(s);
    }

    s = state->stage.Process(s);
    if (settings && settings->has_power_stage) {
      s = state->power_stage.Process(s);
    }

    const float clamped = std::clamp(s, -1.0f, 1.0f);
    const auto pcm = static_cast<std::int16_t>(std::lrint(clamped * 32767.0f));
    out[i * 2 + 0] = pcm;
    out[i * 2 + 1] = pcm;
  }

  return g_running ? paContinue : paComplete;
}
}  // namespace

int main(int argc, char** argv) {
  Config config;
  bool list_devices = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      return 0;
    }
    if (arg == "--list-devices") {
      list_devices = true;
      continue;
    }
    if (ParseStringArg(arg, "--amp", i, argc, argv, config.amp_name) ||
        ParseStringArg(arg, "--amp-file", i, argc, argv, config.amp_file) ||
        ParseStringArg(arg, "--preamp", i, argc, argv, config.preamp_name) ||
        ParseStringArg(arg, "--preamp-file", i, argc, argv, config.preamp_file) ||
        ParseStringArg(arg, "--preset", i, argc, argv, config.preset) ||
        ParseStringArg(arg, "--effect", i, argc, argv, config.effect) ||
        ParseStringArg(arg, "--device", i, argc, argv, config.device_name) ||
        ParseStringArg(arg, "--input-device", i, argc, argv, config.input_device_name) ||
        ParseStringArg(arg, "--output-device", i, argc, argv, config.output_device_name) ||
        ParseStringArg(arg, "--control-file", i, argc, argv, config.control_file) ||
        ParseStringArg(arg, "--alsa-device", i, argc, argv, config.alsa_input) ||
        ParseStringArg(arg, "--alsa-input", i, argc, argv, config.alsa_input) ||
        ParseStringArg(arg, "--alsa-output", i, argc, argv, config.alsa_output) ||
        ParseOptionalDoubleArg(arg, "--drive-db", i, argc, argv, config.drive_db) ||
        ParseOptionalDoubleArg(arg, "--level-db", i, argc, argv, config.level_db) ||
        ParseOptionalDoubleArg(arg, "--bright-db", i, argc, argv, config.bright_db) ||
        ParseOptionalDoubleArg(arg, "--bias-trim", i, argc, argv, config.bias_trim) ||
        ParseOptionalDoubleArg(arg, "--bass", i, argc, argv, config.bass) ||
        ParseOptionalDoubleArg(arg, "--mid", i, argc, argv, config.mid) ||
        ParseOptionalDoubleArg(arg, "--treble", i, argc, argv, config.treble) ||
        ParseOptionalDoubleArg(arg, "--presence", i, argc, argv, config.presence) ||
        ParseDoubleArg(arg, "--effect-drive", i, argc, argv, config.effect_drive) ||
        ParseDoubleArg(arg, "--effect-tone", i, argc, argv, config.effect_tone) ||
        ParseDoubleArg(arg, "--effect-level-db", i, argc, argv, config.effect_level_db) ||
        ParseDoubleArg(arg, "--effect-clean-blend", i, argc, argv, config.effect_clean_blend)) {
      if (arg == "--alsa-device" || arg.rfind("--alsa-device=", 0) == 0) {
        config.alsa_output = config.alsa_input;
      }
      continue;
    }
    if (arg == "--power-tube" || arg.rfind("--power-tube=", 0) == 0) {
      std::string tube_name;
      if (!ParseStringArg(arg, "--power-tube", i, argc, argv, tube_name)) {
        return 1;
      }
      PowerTubeType type;
      if (!ParsePowerTubeType(tube_name, type)) {
        std::cerr << "Unknown power tube: " << tube_name << "\n";
        return 1;
      }
      config.power_tube_type = type;
      continue;
    }

    std::cerr << "Unknown argument: " << arg << "\n";
    PrintUsage(argv[0]);
    return 1;
  }

  if (config.effect != "none" && !IsEffectEnabled(config.effect)) {
    std::cerr << "Unknown effect: " << config.effect << "\n";
    return 1;
  }

  std::signal(SIGINT, OnSignal);

  const auto profile = ResolveAmpProfile(config);
  if (!profile) {
    return 1;
  }

  std::optional<LiveControlState> live_state;
  if (!config.control_file.empty() && std::filesystem::exists(config.control_file)) {
    LiveControlState loaded_state;
    std::string error;
    if (LoadLiveControlState(config.control_file, loaded_state, &error)) {
      live_state = loaded_state;
#ifdef __linux__
      if (config.alsa_input.empty() && loaded_state.alsa_input) {
        config.alsa_input = *loaded_state.alsa_input;
      }
      if (config.alsa_output.empty() && loaded_state.alsa_output) {
        config.alsa_output = *loaded_state.alsa_output;
      }
      if (config.alsa_input.empty() && !config.alsa_output.empty()) {
        config.alsa_input = config.alsa_output;
      }
      if (config.alsa_output.empty() && !config.alsa_input.empty()) {
        config.alsa_output = config.alsa_input;
      }
#endif
      if (config.input_device_name.empty() && loaded_state.input_device_name) {
        config.input_device_name = *loaded_state.input_device_name;
      }
      if (config.output_device_name.empty() && loaded_state.output_device_name) {
        config.output_device_name = *loaded_state.output_device_name;
      }
      if (config.device_name == "default" &&
          config.input_device_name.empty() &&
          config.output_device_name.empty() &&
          loaded_state.input_device_name &&
          loaded_state.output_device_name &&
          *loaded_state.input_device_name == *loaded_state.output_device_name) {
        config.device_name = *loaded_state.input_device_name;
      }
    } else {
      std::cerr << error << "\n";
    }
  }

  auto settings = ResolveRuntimeSettings(*profile, config, live_state);

  if (!config.control_file.empty() && !std::filesystem::exists(config.control_file)) {
    std::string error;
    if (!SaveLiveControlState(config.control_file, BuildLiveControlState(*settings), &error)) {
      std::cerr << error << "\n";
    }
  }

  AppState state;
  state.stage.SetSampleRate(kSampleRateHz);
  state.power_stage.SetSampleRate(kSampleRateHz);
  state.effect.SetSampleRate(kSampleRateHz);
  state.tubescreamer.SetSampleRate(kSampleRateHz);
  std::atomic_store(&state.settings, settings);

  CheckPa(Pa_Initialize(), "Pa_Initialize");

  if (list_devices) {
    PrintDevices();
    CheckPa(Pa_Terminate(), "Pa_Terminate");
    return 0;
  }

  PaStreamParameters input_params{};
  PaStreamParameters output_params{};
  input_params.channelCount = kInputChannels;
  input_params.sampleFormat = paInt16;
  output_params.channelCount = kOutputChannels;
  output_params.sampleFormat = paInt16;

  std::string selected_device_label;
#ifdef __linux__
  PaAlsaStreamInfo alsa_input_info{};
  PaAlsaStreamInfo alsa_output_info{};
  const bool use_explicit_alsa = !config.alsa_input.empty() || !config.alsa_output.empty();
  if (use_explicit_alsa) {
    if (config.alsa_input.empty()) {
      config.alsa_input = config.alsa_output;
    }
    if (config.alsa_output.empty()) {
      config.alsa_output = config.alsa_input;
    }

    PaAlsa_InitializeStreamInfo(&alsa_input_info);
    PaAlsa_InitializeStreamInfo(&alsa_output_info);
    alsa_input_info.deviceString = config.alsa_input.c_str();
    alsa_output_info.deviceString = config.alsa_output.c_str();

    input_params.device = paUseHostApiSpecificDeviceSpecification;
    input_params.suggestedLatency = 0.01;
    input_params.hostApiSpecificStreamInfo = &alsa_input_info;

    output_params.device = paUseHostApiSpecificDeviceSpecification;
    output_params.suggestedLatency = 0.01;
    output_params.hostApiSpecificStreamInfo = &alsa_output_info;

    selected_device_label =
        "ALSA in=" + config.alsa_input + " out=" + config.alsa_output;
  } else
#endif
  {
    const bool use_split_devices =
        !config.input_device_name.empty() || !config.output_device_name.empty();
    if (use_split_devices) {
      const std::string input_needle =
          config.input_device_name.empty() ? config.device_name : config.input_device_name;
      const std::string output_needle =
          config.output_device_name.empty() ? config.device_name : config.output_device_name;

      const PaDeviceIndex input_device = FindInputDevice(input_needle);
      if (input_device == paNoDevice) {
        std::cerr << "Could not find input device matching \"" << input_needle << "\"\n";
        PrintDevices();
        CheckPa(Pa_Terminate(), "Pa_Terminate");
        return 1;
      }

      const PaDeviceIndex output_device = FindOutputDevice(output_needle);
      if (output_device == paNoDevice) {
        std::cerr << "Could not find output device matching \"" << output_needle << "\"\n";
        PrintDevices();
        CheckPa(Pa_Terminate(), "Pa_Terminate");
        return 1;
      }

      const PaDeviceInfo* input_info = Pa_GetDeviceInfo(input_device);
      const PaDeviceInfo* output_info = Pa_GetDeviceInfo(output_device);
      input_params.device = input_device;
      input_params.suggestedLatency = input_info->defaultLowInputLatency;
      output_params.device = output_device;
      output_params.suggestedLatency = output_info->defaultLowOutputLatency;
      selected_device_label =
          "input=" + std::string(input_info->name) +
          ", output=" + std::string(output_info->name);
    } else {
      const PaDeviceIndex device = FindDuplexDevice(config.device_name);
      if (device == paNoDevice) {
        std::cerr << "Could not find duplex device matching \"" << config.device_name << "\"\n";
        PrintDevices();
        CheckPa(Pa_Terminate(), "Pa_Terminate");
        return 1;
      }

      const PaDeviceInfo* info = Pa_GetDeviceInfo(device);
      input_params.device = device;
      input_params.suggestedLatency = info->defaultLowInputLatency;
      output_params.device = device;
      output_params.suggestedLatency = info->defaultLowOutputLatency;
      selected_device_label = info->name;
    }
  }

  const PaError support =
      Pa_IsFormatSupported(&input_params, &output_params, kSampleRateHz);
  if (support != paFormatIsSupported) {
    std::cerr << "Device \"" << selected_device_label << "\" does not support "
              << kSampleRateHz << " Hz with "
              << kInputChannels << " input / "
              << kOutputChannels << " output int16.\n";
    CheckPa(Pa_Terminate(), "Pa_Terminate");
    return 1;
  }

  PaStream* stream = nullptr;
  CheckPa(Pa_OpenStream(&stream,
                        &input_params,
                        &output_params,
                        kSampleRateHz,
                        kFramesPerBuffer,
                        paNoFlag,
                        AudioCallback,
                        &state),
          "Pa_OpenStream");

#ifdef __linux__
  if (!config.alsa_input.empty() || !config.alsa_output.empty()) {
    PaAlsa_EnableRealtimeScheduling(stream, 1);
  }
#endif

  CheckPa(Pa_StartStream(stream), "Pa_StartStream");

  std::cout << "Running on " << selected_device_label
            << " at " << kSampleRateHz
            << " Hz, amp=" << settings->amp_name
            << ", preamp=" << settings->preamp_name
            << (settings->has_power_stage
                    ? std::string(", power=") + PowerTubeTypeName(settings->power_tube_type)
                    : std::string())
            << ", effect=" << EffectTypeName(settings->effect)
            << ". Press Ctrl+C to quit.\n";

  while (g_running && Pa_IsStreamActive(stream) == 1) {
    if (!config.control_file.empty() && std::filesystem::exists(config.control_file)) {
      static std::filesystem::file_time_type last_write_time;
      const auto current_write_time = std::filesystem::last_write_time(config.control_file);
      if (current_write_time != last_write_time) {
        last_write_time = current_write_time;
        LiveControlState loaded_state;
        std::string error;
        if (LoadLiveControlState(config.control_file, loaded_state, &error)) {
          auto next_settings = ResolveRuntimeSettings(*profile, config, loaded_state);
          std::atomic_store(&state.settings, next_settings);
        } else {
          std::cerr << error << "\n";
        }
      }
    }
    Pa_Sleep(100);
  }

  CheckPa(Pa_StopStream(stream), "Pa_StopStream");
  CheckPa(Pa_CloseStream(stream), "Pa_CloseStream");
  CheckPa(Pa_Terminate(), "Pa_Terminate");
  return 0;
}

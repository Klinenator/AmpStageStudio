#pragma once

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>

#include "power_stage.h"

enum class EffectType {
  kNone,
  kKlon,
  kTubeScreamer,
  kPlate
};

inline bool ParseEffectType(const std::string& value, EffectType& out) {
  if (value == "none") {
    out = EffectType::kNone;
    return true;
  }
  if (value == "klon") {
    out = EffectType::kKlon;
    return true;
  }
  if (value == "tubescreamer") {
    out = EffectType::kTubeScreamer;
    return true;
  }
  if (value == "plate") {
    out = EffectType::kPlate;
    return true;
  }
  return false;
}

inline const char* EffectTypeName(EffectType type) {
  switch (type) {
    case EffectType::kNone: return "none";
    case EffectType::kKlon: return "klon";
    case EffectType::kTubeScreamer: return "tubescreamer";
    case EffectType::kPlate: return "plate";
  }
  return "none";
}

struct LiveControlState {
  std::optional<std::string> amp_name;
  std::optional<std::string> preamp_name;
  std::optional<std::string> alsa_input;
  std::optional<std::string> alsa_output;
  std::optional<std::string> input_device_name;
  std::optional<std::string> output_device_name;
  std::optional<PowerTubeType> power_tube_type;
  std::optional<EffectType> effect;
  std::optional<double> drive_db;
  std::optional<double> level_db;
  std::optional<double> bright_db;
  std::optional<double> bias_trim;
  std::optional<double> bass;
  std::optional<double> mid;
  std::optional<double> treble;
  std::optional<double> presence;
  std::optional<double> power_drive_db;
  std::optional<double> power_level_db;
  std::optional<double> power_bias_trim;
  std::optional<double> effect_drive;
  std::optional<double> effect_tone;
  std::optional<double> effect_level_db;
  std::optional<double> effect_clean_blend;
};

inline bool ParseLiveControlDouble(const std::string& value, double& out) {
  try {
    std::size_t consumed = 0;
    out = std::stod(value, &consumed);
    return consumed == value.size();
  } catch (const std::exception&) {
    return false;
  }
}

inline std::string TrimLiveControlString(std::string value) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) {
    return !std::isspace(c);
  }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
    return !std::isspace(c);
  }).base(), value.end());
  return value;
}

inline bool LoadLiveControlState(const std::string& path,
                                 LiveControlState& state_out,
                                 std::string* error_out = nullptr) {
  std::ifstream in(path);
  if (!in) {
    if (error_out) {
      *error_out = "Failed to open live control file: " + path;
    }
    return false;
  }

  LiveControlState parsed;
  std::string line;
  int line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    const std::size_t comment_pos = line.find('#');
    if (comment_pos != std::string::npos) {
      line.resize(comment_pos);
    }
    line = TrimLiveControlString(line);
    if (line.empty()) {
      continue;
    }

    const std::size_t equals_pos = line.find('=');
    if (equals_pos == std::string::npos) {
      if (error_out) {
        *error_out = "Malformed control file line " + std::to_string(line_number);
      }
      return false;
    }

    const std::string key = TrimLiveControlString(line.substr(0, equals_pos));
    const std::string value = TrimLiveControlString(line.substr(equals_pos + 1));

    if (key == "amp") {
      parsed.amp_name = value;
      continue;
    }
    if (key == "preamp") {
      parsed.preamp_name = value;
      continue;
    }
    if (key == "alsa_input") {
      parsed.alsa_input = value;
      continue;
    }
    if (key == "alsa_output") {
      parsed.alsa_output = value;
      continue;
    }
    if (key == "input_device") {
      parsed.input_device_name = value;
      continue;
    }
    if (key == "output_device") {
      parsed.output_device_name = value;
      continue;
    }
    if (key == "power_tube_type") {
      PowerTubeType type;
      if (!ParsePowerTubeType(value, type)) {
        if (error_out) *error_out = "Unknown power tube value " + value;
        return false;
      }
      parsed.power_tube_type = type;
      continue;
    }
    if (key == "effect") {
      EffectType effect_type;
      if (!ParseEffectType(value, effect_type)) {
        if (error_out) {
          *error_out = "Unknown effect value " + value;
        }
        return false;
      }
      parsed.effect = effect_type;
      continue;
    }

    double parsed_double = 0.0;
    if (!ParseLiveControlDouble(value, parsed_double)) {
      if (error_out) {
        *error_out = "Invalid numeric value for " + key;
      }
      return false;
    }

    if (key == "drive_db") {
      parsed.drive_db = parsed_double;
    } else if (key == "level_db") {
      parsed.level_db = parsed_double;
    } else if (key == "bright_db") {
      parsed.bright_db = parsed_double;
    } else if (key == "bias_trim") {
      parsed.bias_trim = parsed_double;
    } else if (key == "bass") {
      parsed.bass = parsed_double;
    } else if (key == "mid") {
      parsed.mid = parsed_double;
    } else if (key == "treble") {
      parsed.treble = parsed_double;
    } else if (key == "presence") {
      parsed.presence = parsed_double;
    } else if (key == "power_drive_db") {
      parsed.power_drive_db = parsed_double;
    } else if (key == "power_level_db") {
      parsed.power_level_db = parsed_double;
    } else if (key == "power_bias_trim") {
      parsed.power_bias_trim = parsed_double;
    } else if (key == "effect_drive") {
      parsed.effect_drive = parsed_double;
    } else if (key == "effect_tone") {
      parsed.effect_tone = parsed_double;
    } else if (key == "effect_level_db") {
      parsed.effect_level_db = parsed_double;
    } else if (key == "effect_clean_blend") {
      parsed.effect_clean_blend = parsed_double;
    } else {
      if (error_out) {
        *error_out = "Unknown control key " + key;
      }
      return false;
    }
  }

  state_out = parsed;
  return true;
}

inline bool SaveLiveControlState(const std::string& path,
                                 const LiveControlState& state,
                                 std::string* error_out = nullptr) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    if (error_out) {
      *error_out = "Failed to write live control file: " + path;
    }
    return false;
  }

  if (state.amp_name) {
    out << "amp = " << *state.amp_name << "\n";
  }
  if (state.preamp_name) {
    out << "preamp = " << *state.preamp_name << "\n";
  }
  if (state.alsa_input) {
    out << "alsa_input = " << *state.alsa_input << "\n";
  }
  if (state.alsa_output) {
    out << "alsa_output = " << *state.alsa_output << "\n";
  }
  if (state.input_device_name) {
    out << "input_device = " << *state.input_device_name << "\n";
  }
  if (state.output_device_name) {
    out << "output_device = " << *state.output_device_name << "\n";
  }
  if (state.power_tube_type) {
    out << "power_tube_type = " << PowerTubeTypeName(*state.power_tube_type) << "\n";
  }
  if (state.effect) {
    out << "effect = " << EffectTypeName(*state.effect) << "\n";
  }
  if (state.drive_db) out << "drive_db = " << *state.drive_db << "\n";
  if (state.level_db) out << "level_db = " << *state.level_db << "\n";
  if (state.bright_db) out << "bright_db = " << *state.bright_db << "\n";
  if (state.bias_trim) out << "bias_trim = " << *state.bias_trim << "\n";
  if (state.bass) out << "bass = " << *state.bass << "\n";
  if (state.mid) out << "mid = " << *state.mid << "\n";
  if (state.treble) out << "treble = " << *state.treble << "\n";
  if (state.presence) out << "presence = " << *state.presence << "\n";
  if (state.power_drive_db) out << "power_drive_db = " << *state.power_drive_db << "\n";
  if (state.power_level_db) out << "power_level_db = " << *state.power_level_db << "\n";
  if (state.power_bias_trim) out << "power_bias_trim = " << *state.power_bias_trim << "\n";
  if (state.effect_drive) out << "effect_drive = " << *state.effect_drive << "\n";
  if (state.effect_tone) out << "effect_tone = " << *state.effect_tone << "\n";
  if (state.effect_level_db) out << "effect_level_db = " << *state.effect_level_db << "\n";
  if (state.effect_clean_blend) {
    out << "effect_clean_blend = " << *state.effect_clean_blend << "\n";
  }
  return static_cast<bool>(out);
}

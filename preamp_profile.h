#pragma once

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <string>

#include "tube_stage.h"

struct PreampProfile {
  std::string name;
  std::string circuit;
  TubeStageSpec spec;
  TubeStageControls defaults;
};

inline std::string TrimPreampProfileString(std::string value) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(
      std::find_if(value.rbegin(), value.rend(), not_space).base(),
      value.end());
  return value;
}

inline bool ParsePreampProfileDouble(const std::string& value, double& out) {
  try {
    std::size_t consumed = 0;
    out = std::stod(value, &consumed);
    return consumed == value.size();
  } catch (const std::exception&) {
    return false;
  }
}

inline std::optional<PreampProfile> LoadPreampProfileFromFile(
    const std::string& path,
    std::string* error_out = nullptr) {
  std::ifstream in(path);
  if (!in) {
    if (error_out) *error_out = "Failed to open preamp profile: " + path;
    return std::nullopt;
  }

  PreampProfile profile;
  std::string line;
  int line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    const std::size_t comment_pos = line.find('#');
    if (comment_pos != std::string::npos) line.resize(comment_pos);
    line = TrimPreampProfileString(line);
    if (line.empty()) continue;

    const std::size_t equals_pos = line.find('=');
    if (equals_pos == std::string::npos) {
      if (error_out) {
        *error_out = "Malformed preamp profile line " + std::to_string(line_number) +
                     " in " + path;
      }
      return std::nullopt;
    }

    const std::string key = TrimPreampProfileString(line.substr(0, equals_pos));
    const std::string value =
        TrimPreampProfileString(line.substr(equals_pos + 1));

    if (key == "name") {
      profile.name = value;
      continue;
    }
    if (key == "circuit") {
      profile.circuit = value;
      continue;
    }

    double parsed = 0.0;
    if (!ParsePreampProfileDouble(value, parsed)) {
      if (error_out) *error_out = "Invalid numeric value for " + key + " in " + path;
      return std::nullopt;
    }

    if (key == "input_hpf_hz") {
      profile.spec.input_hpf_hz = parsed;
    } else if (key == "bright_hpf_hz") {
      profile.spec.bright_hpf_hz = parsed;
    } else if (key == "plate_lpf_hz") {
      profile.spec.plate_lpf_hz = parsed;
    } else if (key == "output_hpf_hz") {
      profile.spec.output_hpf_hz = parsed;
    } else if (key == "nominal_bias") {
      profile.spec.nominal_bias = parsed;
    } else if (key == "positive_curve") {
      profile.spec.positive_curve = parsed;
    } else if (key == "negative_curve") {
      profile.spec.negative_curve = parsed;
    } else if (key == "asymmetry") {
      profile.spec.asymmetry = parsed;
    } else if (key == "cathode_memory_amount") {
      profile.spec.cathode_memory_amount = parsed;
    } else if (key == "default_drive_db") {
      profile.defaults.drive_db = parsed;
    } else if (key == "default_level_db") {
      profile.defaults.level_db = parsed;
    } else if (key == "default_bright_db") {
      profile.defaults.bright_db = parsed;
    } else if (key == "default_bias_trim") {
      profile.defaults.bias_trim = parsed;
    } else {
      if (error_out) *error_out = "Unknown preamp profile key " + key + " in " + path;
      return std::nullopt;
    }
  }

  if (profile.name.empty()) profile.name = path;
  return profile;
}

inline PreampProfile BuiltinPresetPreampProfile(const std::string& preset_name) {
  PreampProfile profile;
  if (preset_name == "fender") {
    profile.name = "fender";
    profile.circuit = "generic_fender_stage1";
    profile.spec = FenderStage1Spec();
    profile.defaults.drive_db = 10.0;
    profile.defaults.level_db = -4.0;
    profile.defaults.bright_db = 2.0;
    profile.defaults.bias_trim = 0.0;
    return profile;
  }

  profile.name = "marshall";
  profile.circuit = "generic_marshall_stage1";
  profile.spec = MarshallStage1Spec();
  profile.defaults.drive_db = 16.0;
  profile.defaults.level_db = -6.0;
  profile.defaults.bright_db = 3.0;
  profile.defaults.bias_trim = 0.02;
  return profile;
}

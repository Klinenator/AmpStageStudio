#pragma once

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "power_stage.h"
#include "preamp_profile.h"
#include "tube_stage.h"

struct AmpProfile {
  std::string name;
  std::string circuit;
  std::string preamp_name;
  PreampProfile preamp;
  bool has_power_stage = false;
  PowerTubeType power_tube_type = PowerTubeType::k6V6;
  PowerStageControls power_defaults;
};

inline std::string TrimAmpProfileString(std::string value) {
  const auto not_space = [](unsigned char c) {
    return !std::isspace(c);
  };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(
      std::find_if(value.rbegin(), value.rend(), not_space).base(),
      value.end());
  return value;
}

inline bool ParseAmpProfileDouble(const std::string& value, double& out) {
  try {
    std::size_t consumed = 0;
    out = std::stod(value, &consumed);
    return consumed == value.size();
  } catch (const std::exception&) {
    return false;
  }
}

inline std::optional<AmpProfile> LoadAmpProfileFromFile(
    const std::string& path,
    std::string* error_out = nullptr) {
  std::ifstream in(path);
  if (!in) {
    if (error_out) {
      *error_out = "Failed to open amp profile: " + path;
    }
    return std::nullopt;
  }

  AmpProfile profile;
  std::string line;
  int line_number = 0;
  while (std::getline(in, line)) {
    ++line_number;
    const std::size_t comment_pos = line.find('#');
    if (comment_pos != std::string::npos) {
      line.resize(comment_pos);
    }
    line = TrimAmpProfileString(line);
    if (line.empty()) {
      continue;
    }

    const std::size_t equals_pos = line.find('=');
    if (equals_pos == std::string::npos) {
      if (error_out) {
        *error_out = "Malformed amp profile line " + std::to_string(line_number) +
                     " in " + path;
      }
      return std::nullopt;
    }

    const std::string key = TrimAmpProfileString(line.substr(0, equals_pos));
    const std::string value =
        TrimAmpProfileString(line.substr(equals_pos + 1));

    if (key == "name") {
      profile.name = value;
      continue;
    }
    if (key == "circuit") {
      profile.circuit = value;
      continue;
    }
    if (key == "preamp_name") {
      profile.preamp_name = value;
      continue;
    }
    if (key == "power_tube_type") {
      PowerTubeType type;
      if (!ParsePowerTubeType(value, type)) {
        if (error_out) {
          *error_out = "Unknown power_tube_type " + value + " in " + path;
        }
        return std::nullopt;
      }
      profile.has_power_stage = true;
      profile.power_tube_type = type;
      continue;
    }

    double parsed = 0.0;
    if (!ParseAmpProfileDouble(value, parsed)) {
      if (error_out) {
        *error_out = "Invalid numeric value for " + key + " in " + path;
      }
      return std::nullopt;
    }

    if (key == "power_enabled") {
      profile.has_power_stage = parsed != 0.0;
    } else if (key == "power_default_drive_db") {
      profile.has_power_stage = true;
      profile.power_defaults.drive_db = parsed;
    } else if (key == "power_default_level_db") {
      profile.has_power_stage = true;
      profile.power_defaults.level_db = parsed;
    } else if (key == "power_default_bias_trim") {
      profile.has_power_stage = true;
      profile.power_defaults.bias_trim = parsed;
    } else {
      if (error_out) {
        *error_out = "Unknown amp profile key " + key + " in " + path;
      }
      return std::nullopt;
    }
  }

  if (!profile.preamp_name.empty()) {
    std::string error;
    auto preamp = LoadPreampProfileFromFile("preamps/" + profile.preamp_name + ".preamp", &error);
    if (!preamp) {
      if (error_out) *error_out = error;
      return std::nullopt;
    }
    profile.preamp = *preamp;
  }

  if (profile.name.empty()) {
    profile.name = path;
  }
  return profile;
}

inline AmpProfile BuiltinPresetProfile(const std::string& preset_name) {
  AmpProfile profile;
  if (preset_name == "fender") {
    profile.name = "fender";
    profile.circuit = "generic_fender_stage1";
    profile.preamp_name = "fender";
    profile.preamp = BuiltinPresetPreampProfile("fender");
    profile.has_power_stage = true;
    profile.power_tube_type = PowerTubeType::k6V6;
    profile.power_defaults.drive_db = 3.0;
    profile.power_defaults.level_db = -1.0;
    return profile;
  }

  profile.name = "marshall";
  profile.circuit = "generic_marshall_stage1";
  profile.preamp_name = "marshall";
  profile.preamp = BuiltinPresetPreampProfile("marshall");
  profile.has_power_stage = true;
  profile.power_tube_type = PowerTubeType::kEL34;
  profile.power_defaults.drive_db = 4.0;
  profile.power_defaults.level_db = -1.5;
  return profile;
}

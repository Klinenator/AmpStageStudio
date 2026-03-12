#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "tube_stage.h"

enum class PowerTubeType {
  k6L6,
  kEL34,
  kEL84,
  k6V6
};

struct PowerStageSpec {
  double input_hpf_hz = 35.0;
  double output_lpf_hz = 5200.0;
  double nominal_bias = 0.02;
  double positive_curve = 1.15;
  double negative_curve = 1.05;
  double asymmetry = 0.94;
  double sag_amount = 0.10;
};

struct PowerStageControls {
  double drive_db = 0.0;
  double level_db = 0.0;
  double bias_trim = 0.0;
};

inline bool ParsePowerTubeType(const std::string& value, PowerTubeType& out) {
  if (value == "6L6") {
    out = PowerTubeType::k6L6;
    return true;
  }
  if (value == "EL34") {
    out = PowerTubeType::kEL34;
    return true;
  }
  if (value == "EL84") {
    out = PowerTubeType::kEL84;
    return true;
  }
  if (value == "6V6") {
    out = PowerTubeType::k6V6;
    return true;
  }
  return false;
}

inline const char* PowerTubeTypeName(PowerTubeType type) {
  switch (type) {
    case PowerTubeType::k6L6: return "6L6";
    case PowerTubeType::kEL34: return "EL34";
    case PowerTubeType::kEL84: return "EL84";
    case PowerTubeType::k6V6: return "6V6";
  }
  return "unknown";
}

inline PowerStageSpec PowerStageSpecForType(PowerTubeType type) {
  PowerStageSpec spec;
  switch (type) {
    case PowerTubeType::k6L6:
      spec.input_hpf_hz = 32.0;
      spec.output_lpf_hz = 5600.0;
      spec.nominal_bias = 0.015;
      spec.positive_curve = 1.10;
      spec.negative_curve = 1.00;
      spec.asymmetry = 0.93;
      spec.sag_amount = 0.08;
      break;
    case PowerTubeType::kEL34:
      spec.input_hpf_hz = 40.0;
      spec.output_lpf_hz = 4700.0;
      spec.nominal_bias = 0.03;
      spec.positive_curve = 1.28;
      spec.negative_curve = 1.12;
      spec.asymmetry = 0.96;
      spec.sag_amount = 0.10;
      break;
    case PowerTubeType::kEL84:
      spec.input_hpf_hz = 46.0;
      spec.output_lpf_hz = 5000.0;
      spec.nominal_bias = 0.028;
      spec.positive_curve = 1.24;
      spec.negative_curve = 1.16;
      spec.asymmetry = 0.95;
      spec.sag_amount = 0.12;
      break;
    case PowerTubeType::k6V6:
      spec.input_hpf_hz = 38.0;
      spec.output_lpf_hz = 5000.0;
      spec.nominal_bias = 0.022;
      spec.positive_curve = 1.22;
      spec.negative_curve = 1.10;
      spec.asymmetry = 0.95;
      spec.sag_amount = 0.12;
      break;
  }
  return spec;
}

class PowerStage {
public:
  void SetSampleRate(double sample_rate_hz) {
    sample_rate_hz_ = sample_rate_hz;
    UpdateDerived();
    Reset();
  }

  void SetTubeType(PowerTubeType type) {
    type_ = type;
    spec_ = PowerStageSpecForType(type);
    UpdateDerived();
  }

  void SetControls(const PowerStageControls& controls) {
    controls_ = controls;
    UpdateDerived();
  }

  void Reset() {
    input_hpf_.Reset();
    output_lpf_.Reset();
    sag_env_ = 0.0;
  }

  float Process(float x) {
    double s = static_cast<double>(x);
    s = input_hpf_.Process(s);
    s *= drive_lin_;

    const double dynamic_bias =
        spec_.nominal_bias + controls_.bias_trim - spec_.sag_amount * sag_env_;
    s = ProcessNonlinear(s, dynamic_bias);
    s = output_lpf_.Process(s);
    s *= level_lin_;

    const double abs_s = std::abs(s);
    sag_env_ = sag_env_alpha_ * sag_env_ + (1.0 - sag_env_alpha_) * abs_s;
    return static_cast<float>(s);
  }

private:
  double ProcessNonlinear(double x, double bias) const {
    const double v = x + bias;
    double y = 0.0;

    if (v >= 0.0) {
      y = 0.88 * std::tanh(spec_.positive_curve * v);
    } else {
      const double n = -v;
      y = -spec_.asymmetry *
          std::tanh(spec_.negative_curve * n + 0.10 * n * n);
    }

    y += 0.015 * v * v * v;
    return 0.82 * y;
  }

  void UpdateDerived() {
    input_hpf_.SetCutoff(sample_rate_hz_, spec_.input_hpf_hz);
    output_lpf_.SetCutoff(sample_rate_hz_, spec_.output_lpf_hz);
    drive_lin_ = DbToLin(controls_.drive_db);
    level_lin_ = DbToLin(controls_.level_db);

    const double tau_seconds = 0.060;
    sag_env_alpha_ = std::exp(-1.0 / (sample_rate_hz_ * tau_seconds));
  }

  static double DbToLin(double db) {
    return std::pow(10.0, db / 20.0);
  }

  PowerTubeType type_ = PowerTubeType::k6V6;
  PowerStageSpec spec_ = PowerStageSpecForType(PowerTubeType::k6V6);
  PowerStageControls controls_;

  double sample_rate_hz_ = 48000.0;
  double drive_lin_ = 1.0;
  double level_lin_ = 1.0;
  double sag_env_ = 0.0;
  double sag_env_alpha_ = 0.999;

  OnePoleHPF input_hpf_;
  OnePoleLPF output_lpf_;
};

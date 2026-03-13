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
  double phase_inverter_hpf_hz = 22.0;
  double phase_inverter_lpf_hz = 8800.0;
  double phase_inverter_nominal_bias = 0.01;
  double phase_inverter_positive_curve = 1.18;
  double phase_inverter_negative_curve = 1.06;
  double phase_inverter_asymmetry = 0.95;
  double power_input_gain_db = 0.0;
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
      spec.phase_inverter_hpf_hz = 20.0;
      spec.phase_inverter_lpf_hz = 9000.0;
      spec.phase_inverter_nominal_bias = 0.008;
      spec.phase_inverter_positive_curve = 1.15;
      spec.phase_inverter_negative_curve = 1.04;
      spec.phase_inverter_asymmetry = 0.94;
      spec.power_input_gain_db = -0.8;
      spec.input_hpf_hz = 32.0;
      spec.output_lpf_hz = 5600.0;
      spec.nominal_bias = 0.015;
      spec.positive_curve = 1.10;
      spec.negative_curve = 1.00;
      spec.asymmetry = 0.93;
      spec.sag_amount = 0.08;
      break;
    case PowerTubeType::kEL34:
      spec.phase_inverter_hpf_hz = 24.0;
      spec.phase_inverter_lpf_hz = 8200.0;
      spec.phase_inverter_nominal_bias = 0.012;
      spec.phase_inverter_positive_curve = 1.24;
      spec.phase_inverter_negative_curve = 1.10;
      spec.phase_inverter_asymmetry = 0.96;
      spec.power_input_gain_db = 0.4;
      spec.input_hpf_hz = 40.0;
      spec.output_lpf_hz = 4700.0;
      spec.nominal_bias = 0.03;
      spec.positive_curve = 1.28;
      spec.negative_curve = 1.12;
      spec.asymmetry = 0.96;
      spec.sag_amount = 0.10;
      break;
    case PowerTubeType::kEL84:
      spec.phase_inverter_hpf_hz = 26.0;
      spec.phase_inverter_lpf_hz = 7600.0;
      spec.phase_inverter_nominal_bias = 0.014;
      spec.phase_inverter_positive_curve = 1.26;
      spec.phase_inverter_negative_curve = 1.14;
      spec.phase_inverter_asymmetry = 0.97;
      spec.power_input_gain_db = 1.0;
      spec.input_hpf_hz = 46.0;
      spec.output_lpf_hz = 5000.0;
      spec.nominal_bias = 0.028;
      spec.positive_curve = 1.24;
      spec.negative_curve = 1.16;
      spec.asymmetry = 0.95;
      spec.sag_amount = 0.12;
      break;
    case PowerTubeType::k6V6:
      spec.phase_inverter_hpf_hz = 24.0;
      spec.phase_inverter_lpf_hz = 7800.0;
      spec.phase_inverter_nominal_bias = 0.013;
      spec.phase_inverter_positive_curve = 1.23;
      spec.phase_inverter_negative_curve = 1.12;
      spec.phase_inverter_asymmetry = 0.96;
      spec.power_input_gain_db = 0.8;
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
    phase_inverter_hpf_.Reset();
    phase_inverter_lpf_.Reset();
    input_hpf_.Reset();
    output_lpf_.Reset();
    phase_inverter_env_ = 0.0;
    sag_env_ = 0.0;
  }

  float Process(float x) {
    double s = static_cast<double>(x);
    s = phase_inverter_hpf_.Process(s);
    s *= master_lin_;

    const double phase_inverter_bias =
        spec_.phase_inverter_nominal_bias - 0.04 * phase_inverter_env_;
    s = ProcessPhaseInverter(s, phase_inverter_bias);
    s = phase_inverter_lpf_.Process(s);

    const double phase_inverter_abs = std::abs(s);
    phase_inverter_env_ =
        phase_inverter_env_alpha_ * phase_inverter_env_ +
        (1.0 - phase_inverter_env_alpha_) * phase_inverter_abs;

    s = input_hpf_.Process(s);
    s *= power_input_gain_lin_;

    const double dynamic_bias =
        spec_.nominal_bias + controls_.bias_trim - spec_.sag_amount * sag_env_;
    s = ProcessNonlinear(s, dynamic_bias);
    s = output_lpf_.Process(s);
    s *= output_level_lin_;

    const double abs_s = std::abs(s);
    sag_env_ = sag_env_alpha_ * sag_env_ + (1.0 - sag_env_alpha_) * abs_s;
    return static_cast<float>(s);
  }

private:
  double ProcessPhaseInverter(double x, double bias) const {
    const double v = x + bias;
    if (v >= 0.0) {
      const double soft = std::tanh(spec_.phase_inverter_positive_curve * v);
      const double cutoff = std::tanh(2.4 * std::max(0.0, v - 0.35));
      return 0.92 * soft - 0.12 * cutoff + 0.010 * v * v * v;
    }

    const double n = -v;
    return -spec_.phase_inverter_asymmetry *
           std::tanh(spec_.phase_inverter_negative_curve * n + 0.08 * n * n);
  }

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
    phase_inverter_hpf_.SetCutoff(sample_rate_hz_, spec_.phase_inverter_hpf_hz);
    phase_inverter_lpf_.SetCutoff(sample_rate_hz_, spec_.phase_inverter_lpf_hz);
    input_hpf_.SetCutoff(sample_rate_hz_, spec_.input_hpf_hz);
    output_lpf_.SetCutoff(sample_rate_hz_, spec_.output_lpf_hz);
    master_lin_ = DbToLin(controls_.drive_db);
    power_input_gain_lin_ = DbToLin(spec_.power_input_gain_db);
    output_level_lin_ = DbToLin(controls_.level_db);

    const double phase_inverter_tau_seconds = 0.018;
    phase_inverter_env_alpha_ =
        std::exp(-1.0 / (sample_rate_hz_ * phase_inverter_tau_seconds));
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
  double master_lin_ = 1.0;
  double power_input_gain_lin_ = 1.0;
  double output_level_lin_ = 1.0;
  double phase_inverter_env_ = 0.0;
  double phase_inverter_env_alpha_ = 0.999;
  double sag_env_ = 0.0;
  double sag_env_alpha_ = 0.999;

  OnePoleHPF phase_inverter_hpf_;
  OnePoleLPF phase_inverter_lpf_;
  OnePoleHPF input_hpf_;
  OnePoleLPF output_lpf_;
};

#pragma once

#include <algorithm>
#include <cmath>

struct TubeStageSpec {
  double input_hpf_hz = 60.0;
  double bright_hpf_hz = 1800.0;
  double plate_lpf_hz = 4500.0;
  double output_hpf_hz = 80.0;

  double nominal_bias = 0.05;
  double positive_curve = 1.7;
  double negative_curve = 1.1;
  double asymmetry = 0.92;
  double cathode_memory_amount = 0.12;
};

struct TubeStageControls {
  double drive_db = 0.0;
  double level_db = 0.0;
  double bright_db = 0.0;
  double bias_trim = 0.0;
  double bass = 5.0;
  double mid = 5.0;
  double treble = 5.0;
  double presence = 5.0;
};

class OnePoleLPF {
public:
  void SetCutoff(double sample_rate_hz, double cutoff_hz) {
    const double clamped = std::clamp(cutoff_hz, 1.0, 0.45 * sample_rate_hz);
    a_ = std::exp(-2.0 * kPi * clamped / sample_rate_hz);
    b_ = 1.0 - a_;
  }

  void Reset(double value = 0.0) {
    z_ = value;
  }

  double Process(double x) {
    z_ = b_ * x + a_ * z_;
    return z_;
  }

private:
  static constexpr double kPi = 3.14159265358979323846;
  double a_ = 0.0;
  double b_ = 1.0;
  double z_ = 0.0;
};

class OnePoleHPF {
public:
  void SetCutoff(double sample_rate_hz, double cutoff_hz) {
    lpf_.SetCutoff(sample_rate_hz, cutoff_hz);
  }

  void Reset() {
    lpf_.Reset();
  }

  double Process(double x) {
    return x - lpf_.Process(x);
  }

private:
  OnePoleLPF lpf_;
};

class TubeStage {
public:
  void SetSampleRate(double sample_rate_hz) {
    sample_rate_hz_ = sample_rate_hz;
    UpdateDerived();
    Reset();
  }

  void SetSpec(const TubeStageSpec& spec) {
    spec_ = spec;
    UpdateDerived();
  }

  void SetControls(const TubeStageControls& controls) {
    controls_ = controls;
    UpdateDerived();
  }

  void Reset() {
    input_hpf_.Reset();
    bright_hpf_.Reset();
    tone_bass_lpf_.Reset();
    tone_mid_lpf_.Reset();
    tone_mid_hpf_.Reset();
    tone_treble_hpf_.Reset();
    plate_lpf_.Reset();
    presence_hpf_.Reset();
    output_hpf_.Reset();
    cathode_env_ = 0.0;
  }

  float Process(float x) {
    double s = static_cast<double>(x);
    s = input_hpf_.Process(s);
    s += bright_gain_ * bright_hpf_.Process(s);
    s = ApplyToneStackPre(s);
    s *= drive_lin_;

    const double dynamic_bias =
        spec_.nominal_bias + controls_.bias_trim -
        spec_.cathode_memory_amount * cathode_env_;

    s = ProcessNonlinear(s, dynamic_bias);
    s = plate_lpf_.Process(s);
    s = ApplyPresencePost(s);
    s = output_hpf_.Process(s);
    s *= level_lin_;

    const double abs_s = std::abs(s);
    cathode_env_ = cathode_env_alpha_ * cathode_env_ +
                   (1.0 - cathode_env_alpha_) * abs_s;
    return static_cast<float>(s);
  }

private:
  double ProcessNonlinear(double x, double bias) const {
    const double v = x + bias;
    double y = 0.0;

    if (v >= 0.0) {
      const double soft = std::tanh(spec_.positive_curve * v);
      const double grid = std::tanh(3.0 * std::max(0.0, v - 0.22));
      y = 0.82 * soft - 0.16 * grid;
    } else {
      const double n = -v;
      y = -spec_.asymmetry *
          std::tanh(spec_.negative_curve * n + 0.18 * n * n);
    }

    y += 0.025 * v * v * v;
    return 0.78 * y;
  }

  double ApplyToneStackPre(double x) {
    const double bass = tone_bass_lpf_.Process(x);
    const double mid_band = tone_mid_hpf_.Process(tone_mid_lpf_.Process(x));
    const double treble = tone_treble_hpf_.Process(x);

    double y = x;
    y += bass_mix_ * bass;
    y += mid_mix_ * mid_band;
    y += treble_mix_ * treble;

    const double tone_activity =
        std::abs(bass_mix_) + std::abs(mid_mix_) + std::abs(treble_mix_);
    const double tone_norm = 1.0 / (1.0 + 0.18 * tone_activity);
    return y * tone_norm;
  }

  double ApplyPresencePost(double x) {
    const double presence = presence_hpf_.Process(x);
    const double y = x + presence_mix_ * presence;
    const double tone_norm = 1.0 / (1.0 + 0.15 * std::abs(presence_mix_));
    return y * tone_norm;
  }

  void UpdateDerived() {
    input_hpf_.SetCutoff(sample_rate_hz_, spec_.input_hpf_hz);
    bright_hpf_.SetCutoff(sample_rate_hz_, spec_.bright_hpf_hz);
    tone_bass_lpf_.SetCutoff(sample_rate_hz_, 180.0);
    tone_mid_lpf_.SetCutoff(sample_rate_hz_, 1400.0);
    tone_mid_hpf_.SetCutoff(sample_rate_hz_, 350.0);
    tone_treble_hpf_.SetCutoff(sample_rate_hz_, 1800.0);
    plate_lpf_.SetCutoff(sample_rate_hz_, spec_.plate_lpf_hz);
    presence_hpf_.SetCutoff(sample_rate_hz_, 2800.0);
    output_hpf_.SetCutoff(sample_rate_hz_, spec_.output_hpf_hz);

    drive_lin_ = DbToLin(controls_.drive_db);
    level_lin_ = DbToLin(controls_.level_db);
    bright_gain_ = std::max(0.0, DbToLin(controls_.bright_db) - 1.0);
    bass_mix_ = ControlToMix(controls_.bass, 0.55);
    mid_mix_ = ControlToMix(controls_.mid, 0.45);
    treble_mix_ = ControlToMix(controls_.treble, 0.55);
    presence_mix_ = ControlToMix(controls_.presence, 0.35);

    const double tau_seconds = 0.020;
    cathode_env_alpha_ = std::exp(-1.0 / (sample_rate_hz_ * tau_seconds));
  }

  static double DbToLin(double db) {
    return std::pow(10.0, db / 20.0);
  }

  static double ControlToMix(double value, double max_mix) {
    const double normalized = std::clamp(value, 0.0, 10.0);
    return ((normalized - 5.0) / 5.0) * max_mix;
  }

  TubeStageSpec spec_;
  TubeStageControls controls_;

  double sample_rate_hz_ = 48000.0;
  double drive_lin_ = 1.0;
  double level_lin_ = 1.0;
  double bright_gain_ = 0.0;
  double bass_mix_ = 0.0;
  double mid_mix_ = 0.0;
  double treble_mix_ = 0.0;
  double presence_mix_ = 0.0;
  double cathode_env_ = 0.0;
  double cathode_env_alpha_ = 0.999;

  OnePoleHPF input_hpf_;
  OnePoleHPF bright_hpf_;
  OnePoleLPF tone_bass_lpf_;
  OnePoleLPF tone_mid_lpf_;
  OnePoleHPF tone_mid_hpf_;
  OnePoleHPF tone_treble_hpf_;
  OnePoleLPF plate_lpf_;
  OnePoleHPF presence_hpf_;
  OnePoleHPF output_hpf_;
};

inline TubeStageSpec FenderStage1Spec() {
  TubeStageSpec s;
  s.input_hpf_hz = 45.0;
  s.bright_hpf_hz = 2200.0;
  s.plate_lpf_hz = 6200.0;
  s.output_hpf_hz = 55.0;
  s.nominal_bias = 0.04;
  s.positive_curve = 1.45;
  s.negative_curve = 1.00;
  s.asymmetry = 0.90;
  s.cathode_memory_amount = 0.10;
  return s;
}

inline TubeStageSpec MarshallStage1Spec() {
  TubeStageSpec s;
  s.input_hpf_hz = 75.0;
  s.bright_hpf_hz = 1800.0;
  s.plate_lpf_hz = 4300.0;
  s.output_hpf_hz = 90.0;
  s.nominal_bias = 0.06;
  s.positive_curve = 1.75;
  s.negative_curve = 1.18;
  s.asymmetry = 0.96;
  s.cathode_memory_amount = 0.14;
  return s;
}

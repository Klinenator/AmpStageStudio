#pragma once

#include <algorithm>
#include <cmath>

#include "tube_stage.h"

struct CompressorControls {
  double sustain = 0.5;  // 0..1
  double attack = 0.5;   // 0..1
  double blend = 0.75;   // 0..1
  double level_db = 0.0;
};

class CompressorEffect {
public:
  void SetSampleRate(double sample_rate_hz) {
    sample_rate_hz_ = sample_rate_hz;
    UpdateDerived();
    Reset();
  }

  void SetControls(const CompressorControls& controls) {
    controls_.sustain = std::clamp(controls.sustain, 0.0, 1.0);
    controls_.attack = std::clamp(controls.attack, 0.0, 1.0);
    controls_.blend = std::clamp(controls.blend, 0.0, 1.0);
    controls_.level_db = controls.level_db;
    UpdateDerived();
  }

  void Reset() {
    sidechain_hpf_.Reset();
    tone_lpf_.Reset();
    envelope_ = 0.0;
  }

  float Process(float x) {
    const double dry = static_cast<double>(x);
    const double detector_input = std::abs(sidechain_hpf_.Process(dry));

    if (detector_input > envelope_) {
      envelope_ += attack_coeff_ * (detector_input - envelope_);
    } else {
      envelope_ += release_coeff_ * (detector_input - envelope_);
    }

    double gain = 1.0;
    if (envelope_ > threshold_) {
      const double over = envelope_ / threshold_;
      const double compressed = std::pow(over, -(ratio_ - 1.0) / ratio_);
      gain = std::max(0.12, compressed);
    }

    double wet = tone_lpf_.Process(dry * gain * makeup_gain_);
    const double mixed =
        ((1.0 - controls_.blend) * dry + controls_.blend * wet) * level_lin_;
    return static_cast<float>(mixed);
  }

private:
  static double DbToLin(double db) {
    return std::pow(10.0, db / 20.0);
  }

  static double TimeCoeff(double sample_rate_hz, double time_seconds) {
    return 1.0 - std::exp(-1.0 / std::max(1.0, sample_rate_hz * time_seconds));
  }

  void UpdateDerived() {
    sidechain_hpf_.SetCutoff(sample_rate_hz_, 90.0);
    tone_lpf_.SetCutoff(sample_rate_hz_, 2800.0 + controls_.attack * 3200.0);

    const double attack_ms = 2.0 + controls_.attack * 28.0;
    attack_coeff_ = TimeCoeff(sample_rate_hz_, attack_ms * 0.001);

    const double release_ms = 70.0 + controls_.sustain * 260.0;
    release_coeff_ = TimeCoeff(sample_rate_hz_, release_ms * 0.001);

    threshold_ = 0.38 - controls_.sustain * 0.28;
    ratio_ = 2.0 + controls_.sustain * 8.0;
    makeup_gain_ = DbToLin(1.0 + controls_.sustain * 7.0);
    level_lin_ = DbToLin(controls_.level_db);
  }

  CompressorControls controls_;
  double sample_rate_hz_ = 48000.0;
  double attack_coeff_ = 0.01;
  double release_coeff_ = 0.001;
  double threshold_ = 0.3;
  double ratio_ = 4.0;
  double makeup_gain_ = 1.0;
  double level_lin_ = 1.0;
  double envelope_ = 0.0;

  OnePoleHPF sidechain_hpf_;
  OnePoleLPF tone_lpf_;
};

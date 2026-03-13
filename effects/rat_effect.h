#pragma once

#include <algorithm>
#include <cmath>

#include "tube_stage.h"

struct RatControls {
  double distortion = 0.5;  // 0..1
  double filter = 0.5;      // 0..1, higher is darker like the real RAT
  double level_db = 0.0;
};

class RatEffect {
public:
  void SetSampleRate(double sample_rate_hz) {
    sample_rate_hz_ = sample_rate_hz;
    UpdateDerived();
    Reset();
  }

  void SetControls(const RatControls& controls) {
    controls_.distortion = std::clamp(controls.distortion, 0.0, 1.0);
    controls_.filter = std::clamp(controls.filter, 0.0, 1.0);
    controls_.level_db = controls.level_db;
    UpdateDerived();
  }

  void Reset() {
    input_hpf_.Reset();
    drive_hpf_.Reset();
    drive_lpf_.Reset();
    filter_lpf_.Reset();
    output_hpf_.Reset();
  }

  float Process(float x) {
    double s = static_cast<double>(x);

    // The RAT keeps more low end than a Tube Screamer, but still trims rumble.
    s = input_hpf_.Process(s);

    // A little pre-emphasis helps the clipped op-amp stay aggressive and focused.
    const double focused = s + pre_emphasis_gain_ * drive_hpf_.Process(s);
    s = drive_gain_ * focused;
    s = HardSiliconClip(s);

    // Clip smoothing before the classic RAT filter control.
    s = drive_lpf_.Process(s);
    s = filter_lpf_.Process(s);
    s = output_hpf_.Process(s);
    s *= level_lin_;

    return static_cast<float>(s);
  }

private:
  static double DbToLin(double db) {
    return std::pow(10.0, db / 20.0);
  }

  double HardSiliconClip(double x) const {
    constexpr double threshold = 0.72;
    if (x > threshold) {
      const double excess = x - threshold;
      return threshold + 0.08 * std::tanh(8.0 * excess);
    }
    if (x < -threshold) {
      const double excess = -threshold - x;
      return -threshold - 0.08 * std::tanh(8.0 * excess);
    }
    return x;
  }

  void UpdateDerived() {
    input_hpf_.SetCutoff(sample_rate_hz_, 55.0);
    drive_hpf_.SetCutoff(sample_rate_hz_, 180.0);
    drive_lpf_.SetCutoff(sample_rate_hz_, 5600.0);

    // RAT filter works backwards: higher knob settings roll off more top end.
    const double filter_cutoff =
        8500.0 - controls_.filter * (8500.0 - 900.0);
    filter_lpf_.SetCutoff(sample_rate_hz_, filter_cutoff);
    output_hpf_.SetCutoff(sample_rate_hz_, 45.0);

    drive_gain_ = 1.8 + controls_.distortion * 42.0;
    pre_emphasis_gain_ = 0.12 + controls_.distortion * 0.18;
    level_lin_ = DbToLin(controls_.level_db);
  }

  RatControls controls_;
  double sample_rate_hz_ = 48000.0;
  double drive_gain_ = 1.0;
  double pre_emphasis_gain_ = 0.0;
  double level_lin_ = 1.0;

  OnePoleHPF input_hpf_;
  OnePoleHPF drive_hpf_;
  OnePoleLPF drive_lpf_;
  OnePoleLPF filter_lpf_;
  OnePoleHPF output_hpf_;
};

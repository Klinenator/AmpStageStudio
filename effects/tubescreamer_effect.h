#pragma once

#include <algorithm>
#include <cmath>

#include "tube_stage.h"

struct TubeScreamerControls {
  double drive = 0.5;   // 0..1
  double tone = 0.5;    // 0..1
  double level_db = 0.0;
};

class TubeScreamerEffect {
public:
  void SetSampleRate(double sample_rate_hz) {
    sample_rate_hz_ = sample_rate_hz;
    UpdateDerived();
    Reset();
  }

  void SetControls(const TubeScreamerControls& controls) {
    controls_.drive = std::clamp(controls.drive, 0.0, 1.0);
    controls_.tone = std::clamp(controls.tone, 0.0, 1.0);
    controls_.level_db = controls.level_db;
    UpdateDerived();
  }

  void Reset() {
    input_hpf_.Reset();
    pre_emphasis_hpf_.Reset();
    clip_lpf_.Reset();
    tone_lpf_.Reset();
    output_hpf_.Reset();
  }

  float Process(float x) {
    double s = static_cast<double>(x);

    // Tube Screamer-style bass cut before clipping.
    s = input_hpf_.Process(s);
    s += pre_emphasis_gain_ * pre_emphasis_hpf_.Process(s);

    // Op-amp drive into silicon-style symmetrical clipping.
    s *= drive_gain_;
    s = SoftSiliconClip(s);

    // Tame the top after clipping.
    s = clip_lpf_.Process(s);

    // Tone control: lower setting is darker, higher setting is brighter.
    const double dark = tone_lpf_.Process(s);
    s = (1.0 - controls_.tone) * dark + controls_.tone * s;

    // Output coupling and level.
    s = output_hpf_.Process(s);
    s *= level_lin_;
    return static_cast<float>(s);
  }

private:
  static double DbToLin(double db) {
    return std::pow(10.0, db / 20.0);
  }

  double SoftSiliconClip(double x) const {
    // Tube Screamer clipping is tighter and more symmetrical than the Klon.
    constexpr double threshold = 0.48;
    const double sign = (x >= 0.0) ? 1.0 : -1.0;
    const double mag = std::abs(x);
    if (mag <= threshold) {
      return x;
    }

    const double excess = mag - threshold;
    const double compressed =
        threshold + 0.24 * std::tanh(4.2 * excess);
    return sign * compressed;
  }

  void UpdateDerived() {
    input_hpf_.SetCutoff(sample_rate_hz_, 720.0);
    pre_emphasis_hpf_.SetCutoff(sample_rate_hz_, 1100.0);
    clip_lpf_.SetCutoff(sample_rate_hz_, 3600.0);
    tone_lpf_.SetCutoff(sample_rate_hz_, 1200.0 + controls_.tone * 1600.0);
    output_hpf_.SetCutoff(sample_rate_hz_, 85.0);

    drive_gain_ = 2.0 + controls_.drive * 22.0;
    pre_emphasis_gain_ = 0.18 + controls_.drive * 0.35;
    level_lin_ = DbToLin(controls_.level_db);
  }

  TubeScreamerControls controls_;
  double sample_rate_hz_ = 48000.0;
  double drive_gain_ = 1.0;
  double pre_emphasis_gain_ = 0.0;
  double level_lin_ = 1.0;

  OnePoleHPF input_hpf_;
  OnePoleHPF pre_emphasis_hpf_;
  OnePoleLPF clip_lpf_;
  OnePoleLPF tone_lpf_;
  OnePoleHPF output_hpf_;
};

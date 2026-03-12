#pragma once

#include <algorithm>
#include <cmath>

#include "tube_stage.h"

struct KlonControls {
  double drive = 0.5;  // 0..1
  double tone = 0.5;   // 0..1
  double level_db = 0.0;
  double clean_blend = 0.45;  // 0..1
};

class KlonEffect {
public:
  void SetSampleRate(double sample_rate_hz) {
    sample_rate_hz_ = sample_rate_hz;
    UpdateDerived();
    Reset();
  }

  void SetControls(const KlonControls& controls) {
    controls_.drive = std::clamp(controls.drive, 0.0, 1.0);
    controls_.tone = std::clamp(controls.tone, 0.0, 1.0);
    controls_.level_db = controls.level_db;
    controls_.clean_blend = std::clamp(controls.clean_blend, 0.0, 1.0);
    UpdateDerived();
  }

  void Reset() {
    input_hpf_.Reset();
    clean_hpf_.Reset();
    dirty_hpf_.Reset();
    dirty_lpf_.Reset();
    tone_lpf_.Reset();
    output_hpf_.Reset();
  }

  float Process(float x) {
    double s = static_cast<double>(x);

    // Input coupling and bass tightening.
    s = input_hpf_.Process(s);

    // Klon-style clean path: mostly intact, just high-passed a little.
    const double clean = clean_hpf_.Process(s);

    // Dirty path: gain into diode-like clipping with a mid-focused band.
    double dirty = dirty_hpf_.Process(s);
    dirty *= dirty_gain_;
    dirty = SoftGermaniumClip(dirty);
    dirty = dirty_lpf_.Process(dirty);

    // Blend clean and clipped paths.
    double mixed =
        controls_.clean_blend * clean +
        (1.0 - controls_.clean_blend) * dirty;

    // Tone control: brighter as the tone knob goes up.
    mixed = tone_lpf_.Process(mixed);

    // Output coupling and level.
    mixed = output_hpf_.Process(mixed);
    mixed *= level_lin_;

    return static_cast<float>(mixed);
  }

private:
  static double DbToLin(double db) {
    return std::pow(10.0, db / 20.0);
  }

  double SoftGermaniumClip(double x) const {
    // Klon uses 1N34A germanium diodes; this is a practical approximation.
    constexpr double threshold = 0.28;
    const double sign = (x >= 0.0) ? 1.0 : -1.0;
    const double mag = std::abs(x);
    if (mag <= threshold) {
      return x;
    }

    const double excess = mag - threshold;
    const double compressed =
        threshold + 0.35 * std::tanh(3.2 * excess);
    return sign * compressed;
  }

  void UpdateDerived() {
    input_hpf_.SetCutoff(sample_rate_hz_, 82.0);
    clean_hpf_.SetCutoff(sample_rate_hz_, 100.0);
    dirty_hpf_.SetCutoff(sample_rate_hz_, 720.0);
    dirty_lpf_.SetCutoff(sample_rate_hz_, 4200.0);
    output_hpf_.SetCutoff(sample_rate_hz_, 65.0);

    // Tone sweeps the low-pass from darker to brighter.
    const double tone_cutoff =
        1800.0 + controls_.tone * (6500.0 - 1800.0);
    tone_lpf_.SetCutoff(sample_rate_hz_, tone_cutoff);

    // Drive is intentionally moderate; the Klon keeps a lot of pick definition.
    dirty_gain_ = 1.5 + controls_.drive * 7.5;
    level_lin_ = DbToLin(controls_.level_db);
  }

  KlonControls controls_;
  double sample_rate_hz_ = 48000.0;
  double dirty_gain_ = 1.0;
  double level_lin_ = 1.0;

  OnePoleHPF input_hpf_;
  OnePoleHPF clean_hpf_;
  OnePoleHPF dirty_hpf_;
  OnePoleLPF dirty_lpf_;
  OnePoleLPF tone_lpf_;
  OnePoleHPF output_hpf_;
};

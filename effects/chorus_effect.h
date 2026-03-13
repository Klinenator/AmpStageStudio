#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "tube_stage.h"

struct ChorusControls {
  double depth = 0.5;  // 0..1
  double tone = 0.5;   // 0..1
  double mix = 0.35;   // 0..1
  double level_db = 0.0;
};

class ChorusEffect {
public:
  void SetSampleRate(double sample_rate_hz) {
    sample_rate_hz_ = sample_rate_hz;
    UpdateDerived();
    Reset();
  }

  void SetControls(const ChorusControls& controls) {
    controls_.depth = std::clamp(controls.depth, 0.0, 1.0);
    controls_.tone = std::clamp(controls.tone, 0.0, 1.0);
    controls_.mix = std::clamp(controls.mix, 0.0, 1.0);
    controls_.level_db = controls.level_db;
    UpdateDerived();
  }

  void Reset() {
    input_hpf_.Reset();
    wet_lpf_.Reset();
    delay_.Reset();
    lfo_phase_ = 0.0;
  }

  float Process(float x) {
    const double dry = static_cast<double>(x);
    const double input = input_hpf_.Process(dry);

    const double lfo = 0.5 + 0.5 * std::sin(2.0 * kPi * lfo_phase_);
    const double delay_samples = base_delay_samples_ + depth_samples_ * lfo;
    const double wet = wet_lpf_.Process(delay_.Process(input, delay_samples));

    lfo_phase_ += lfo_rate_hz_ / sample_rate_hz_;
    if (lfo_phase_ >= 1.0) {
      lfo_phase_ -= std::floor(lfo_phase_);
    }

    const double mixed =
        ((1.0 - controls_.mix) * dry + controls_.mix * wet) * level_lin_;
    return static_cast<float>(mixed);
  }

private:
  class FractionalDelayLine {
  public:
    void Resize(int max_delay_samples) {
      const int size = std::max(4, max_delay_samples + 4);
      buffer_.assign(static_cast<std::size_t>(size), 0.0);
      write_index_ = 0;
    }

    void Reset() {
      std::fill(buffer_.begin(), buffer_.end(), 0.0);
      write_index_ = 0;
    }

    void Write(double x) {
      if (buffer_.empty()) {
        return;
      }
      buffer_[static_cast<std::size_t>(write_index_)] = x;
      write_index_ = (write_index_ + 1) % static_cast<int>(buffer_.size());
    }

    double Read(double delay_samples) const {
      if (buffer_.empty()) {
        return 0.0;
      }

      const double clamped_delay =
          std::clamp(delay_samples, 1.0, static_cast<double>(buffer_.size() - 3));
      double read_index = static_cast<double>(write_index_) - clamped_delay;
      while (read_index < 0.0) {
        read_index += static_cast<double>(buffer_.size());
      }
      while (read_index >= static_cast<double>(buffer_.size())) {
        read_index -= static_cast<double>(buffer_.size());
      }

      const int index0 = static_cast<int>(read_index);
      const int index1 = (index0 + 1) % static_cast<int>(buffer_.size());
      const double frac = read_index - static_cast<double>(index0);
      return buffer_[static_cast<std::size_t>(index0)] * (1.0 - frac) +
             buffer_[static_cast<std::size_t>(index1)] * frac;
    }

    double Process(double x, double delay_samples) {
      const double y = Read(delay_samples);
      Write(x);
      return y;
    }

  private:
    std::vector<double> buffer_;
    int write_index_ = 0;
  };

  static constexpr double kPi = 3.14159265358979323846;

  static double DbToLin(double db) {
    return std::pow(10.0, db / 20.0);
  }

  static int MillisecondsToSamples(double sample_rate_hz, double ms) {
    return std::max(1, static_cast<int>(std::lround(sample_rate_hz * ms * 0.001)));
  }

  void UpdateDerived() {
    input_hpf_.SetCutoff(sample_rate_hz_, 70.0);

    const double wet_cutoff = 1800.0 + controls_.tone * (7200.0 - 1800.0);
    wet_lpf_.SetCutoff(sample_rate_hz_, wet_cutoff);

    base_delay_samples_ = static_cast<double>(MillisecondsToSamples(sample_rate_hz_, 10.5));
    depth_samples_ = 2.0 + controls_.depth * 8.0;
    lfo_rate_hz_ = 0.16 + controls_.depth * 1.45;
    level_lin_ = DbToLin(controls_.level_db);

    delay_.Resize(MillisecondsToSamples(sample_rate_hz_, 28.0));
  }

  ChorusControls controls_;
  double sample_rate_hz_ = 48000.0;
  double base_delay_samples_ = 0.0;
  double depth_samples_ = 0.0;
  double lfo_rate_hz_ = 0.8;
  double lfo_phase_ = 0.0;
  double level_lin_ = 1.0;

  OnePoleHPF input_hpf_;
  OnePoleLPF wet_lpf_;
  FractionalDelayLine delay_;
};

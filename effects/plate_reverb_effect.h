#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include "tube_stage.h"

struct PlateReverbControls {
  double mix = 0.18;          // 0..1
  double predelay_ms = 12.0;  // 0..40
  double decay = 0.55;        // 0..1
  double damping = 0.45;      // 0..1
  double brightness = 0.58;   // 0..1
  double low_cut_hz = 180.0;  // 40..600
  double modulation = 0.12;   // 0..1
  double level_db = 0.0;
};

class PlateReverbEffect {
public:
  void SetSampleRate(double sample_rate_hz) {
    sample_rate_hz_ = sample_rate_hz;
    UpdateDerived();
    Reset();
  }

  void SetControls(const PlateReverbControls& controls) {
    controls_.mix = std::clamp(controls.mix, 0.0, 1.0);
    controls_.predelay_ms = std::clamp(controls.predelay_ms, 0.0, 40.0);
    controls_.decay = std::clamp(controls.decay, 0.0, 1.0);
    controls_.damping = std::clamp(controls.damping, 0.0, 1.0);
    controls_.brightness = std::clamp(controls.brightness, 0.0, 1.0);
    controls_.low_cut_hz = std::clamp(controls.low_cut_hz, 40.0, 600.0);
    controls_.modulation = std::clamp(controls.modulation, 0.0, 1.0);
    controls_.level_db = controls.level_db;
    UpdateDerived();
  }

  void Reset() {
    input_hpf_.Reset();
    wet_lpf_.Reset();
    predelay_.Reset();
    for (auto& diffuser : diffusers_) {
      diffuser.Reset();
    }
    for (auto& line : tank_lines_) {
      line.Reset();
    }
    for (auto& lpf : tank_damping_lpfs_) {
      lpf.Reset();
    }
    lfo_phase_ = {0.0, 0.19, 0.41, 0.67};
  }

  float Process(float x) {
    const double dry = static_cast<double>(x);
    double s = input_hpf_.Process(dry);
    s = predelay_.Process(s, predelay_samples_);

    for (auto& diffuser : diffusers_) {
      s = diffuser.Process(s);
    }

    std::array<double, 4> delayed{};
    for (std::size_t i = 0; i < tank_lines_.size(); ++i) {
      const double mod =
          modulation_depth_samples_ * std::sin(2.0 * kPi * lfo_phase_[i]);
      delayed[i] = tank_lines_[i].Read(delay_samples_[i] + mod);
      lfo_phase_[i] += lfo_rate_hz_[i] / sample_rate_hz_;
      if (lfo_phase_[i] >= 1.0) {
        lfo_phase_[i] -= std::floor(lfo_phase_[i]);
      }
    }

    const double sum =
        delayed[0] + delayed[1] + delayed[2] + delayed[3];
    const std::array<double, 4> input_injection{0.58, -0.47, 0.41, -0.53};
    for (std::size_t i = 0; i < tank_lines_.size(); ++i) {
      double feedback = 0.5 * sum - delayed[i];
      feedback = tank_damping_lpfs_[i].Process(feedback);
      const double write_sample = 0.28 * input_injection[i] * s + feedback_gain_ * feedback;
      tank_lines_[i].Write(write_sample);
    }

    double wet = 0.0;
    for (double tap : delayed) {
      wet += tap;
    }
    wet *= 0.25;
    wet = wet_lpf_.Process(wet);
    wet *= wet_level_lin_;

    const double mixed = (1.0 - controls_.mix) * dry + controls_.mix * wet;
    return static_cast<float>(mixed);
  }

private:
  class FractionalDelayLine {
  public:
    void Resize(int max_delay_samples) {
      const int min_size = std::max(4, max_delay_samples + 4);
      buffer_.assign(static_cast<std::size_t>(min_size), 0.0);
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

  class AllpassDelay {
  public:
    void SetSize(int delay_samples) {
      delay_samples_ = std::max(1, delay_samples);
      delay_.Resize(delay_samples_);
      delay_.Reset();
    }

    void SetFeedback(double feedback) {
      feedback_ = std::clamp(feedback, -0.95, 0.95);
    }

    void Reset() {
      delay_.Reset();
    }

    double Process(double x) {
      const double delayed = delay_.Read(static_cast<double>(delay_samples_));
      const double y = -feedback_ * x + delayed;
      delay_.Write(x + feedback_ * y);
      return y;
    }

  private:
    int delay_samples_ = 1;
    double feedback_ = 0.7;
    FractionalDelayLine delay_;
  };

  static constexpr double kPi = 3.14159265358979323846;

  static double DbToLin(double db) {
    return std::pow(10.0, db / 20.0);
  }

  static int ScaleSamples(int at_48k, double sample_rate_hz) {
    return std::max(1, static_cast<int>(std::lround(at_48k * sample_rate_hz / 48000.0)));
  }

  void UpdateDerived() {
    input_hpf_.SetCutoff(sample_rate_hz_, controls_.low_cut_hz);

    const int max_predelay = ScaleSamples(1920, sample_rate_hz_);
    predelay_.Resize(max_predelay);
    predelay_samples_ = controls_.predelay_ms * 0.001 * sample_rate_hz_;

    constexpr std::array<int, 4> kDiffuserSamplesAt48k{113, 163, 241, 347};
    constexpr std::array<double, 4> kDiffuserFeedback{0.70, 0.72, 0.68, 0.74};
    for (std::size_t i = 0; i < diffusers_.size(); ++i) {
      diffusers_[i].SetSize(ScaleSamples(kDiffuserSamplesAt48k[i], sample_rate_hz_));
      diffusers_[i].SetFeedback(kDiffuserFeedback[i]);
    }

    constexpr std::array<int, 4> kTankSamplesAt48k{1423, 1789, 2053, 2549};
    int max_delay_samples = 0;
    for (std::size_t i = 0; i < tank_lines_.size(); ++i) {
      delay_samples_[i] = static_cast<double>(ScaleSamples(kTankSamplesAt48k[i], sample_rate_hz_));
      max_delay_samples =
          std::max(max_delay_samples, static_cast<int>(std::ceil(delay_samples_[i] + 16.0)));
    }
    for (auto& line : tank_lines_) {
      line.Resize(max_delay_samples);
    }

    const double damping_hz = 7000.0 - controls_.damping * 5200.0;
    for (auto& lpf : tank_damping_lpfs_) {
      lpf.SetCutoff(sample_rate_hz_, damping_hz);
    }

    const double wet_lpf_hz = 3200.0 + controls_.brightness * 6000.0;
    wet_lpf_.SetCutoff(sample_rate_hz_, wet_lpf_hz);

    feedback_gain_ = 0.55 + controls_.decay * 0.37;
    wet_level_lin_ = DbToLin(controls_.level_db);
    modulation_depth_samples_ = 0.5 + controls_.modulation * 5.5;
    lfo_rate_hz_ = {0.11, 0.17, 0.23, 0.31};
  }

  double sample_rate_hz_ = 48000.0;
  double predelay_samples_ = 0.0;
  double feedback_gain_ = 0.75;
  double wet_level_lin_ = 1.0;
  double modulation_depth_samples_ = 1.0;
  PlateReverbControls controls_;

  OnePoleHPF input_hpf_;
  OnePoleLPF wet_lpf_;
  FractionalDelayLine predelay_;
  std::array<AllpassDelay, 4> diffusers_{};
  std::array<FractionalDelayLine, 4> tank_lines_{};
  std::array<OnePoleLPF, 4> tank_damping_lpfs_{};
  std::array<double, 4> delay_samples_{1423.0, 1789.0, 2053.0, 2549.0};
  std::array<double, 4> lfo_phase_{0.0, 0.19, 0.41, 0.67};
  std::array<double, 4> lfo_rate_hz_{0.11, 0.17, 0.23, 0.31};
};

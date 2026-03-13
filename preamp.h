#pragma once

#include <array>
#include <cmath>

#include "preamp_profile.h"

class MesaMarkInterstageToneStack {
public:
  void SetSampleRate(double sample_rate_hz) {
    sample_rate_hz_ = sample_rate_hz;
    UpdateDerived();
  }

  void SetControls(const TubeStageControls& controls) {
    controls_ = controls;
    UpdateDerived();
  }

  void Reset() {
    filter_.Reset();
  }

  float Process(float x) {
    return static_cast<float>(filter_.Process(static_cast<double>(x)));
  }

private:
  static double ControlToUnit(double value) {
    return std::clamp(value, 0.0, 10.0) / 10.0;
  }

  static double ApplyMesaLogBTaper(double position) {
    const double x = std::clamp(position, 0.0, 1.0);
    return x < 0.5 ? x * 0.2 : x * 1.8 - 0.8;
  }

  static std::array<double, 2> SplitPotValue(double value, double proportion) {
    const double clamped = std::clamp(proportion, 0.0, 1.0);
    const double upper = clamped * value;
    return {upper, value - upper};
  }

  static std::array<double, 4> BilinearTransformPoly(
      const std::array<double, 4>& analog,
      double sample_rate_hz) {
    const double k = 2.0 * sample_rate_hz;
    const double k2 = k * k;
    const double k3 = k2 * k;
    return {
        analog[0] + analog[1] * k + analog[2] * k2 + analog[3] * k3,
        3.0 * analog[0] + analog[1] * k - analog[2] * k2 - 3.0 * analog[3] * k3,
        3.0 * analog[0] - analog[1] * k - analog[2] * k2 + 3.0 * analog[3] * k3,
        analog[0] - analog[1] * k + analog[2] * k2 - analog[3] * k3,
    };
  }

  void UpdateDerived() {
    constexpr double kRin = 1300.0;
    constexpr double kRl = 1.0e6;
    constexpr double kRb = 1.0e6;
    constexpr double kRm = 25.0e3;
    constexpr double kRt = 250.0e3;
    constexpr double kR1 = 47.0e3;
    constexpr double kC1 = 500.0e-12;
    constexpr double kC2 = 22.0e-9;
    constexpr double kC3 = 22.0e-9;

    const auto [rt2, rt1] = SplitPotValue(kRt, ControlToUnit(controls_.treble));
    const auto [rm2, rm1] = SplitPotValue(kRm, ControlToUnit(controls_.mid));
    const auto [rb2, rb1] =
        SplitPotValue(kRb, ApplyMesaLogBTaper(ControlToUnit(controls_.bass)));

    const double t0 = rt1 * rt2;
    const double t1 = kRin + rt1;
    const double t2 = rm2 + rt2;
    const double t3 = rt1 + rt2;
    const double t4 = kRin + rm2;
    const double t5 = kR1 + t3;
    const double t6 = rb1 + rm1;
    const double t7 = kC2 * t6;
    const double t8 = kC3 * t7;
    const double t9 = kRin * rm2;
    const double t10 = kRl + t2;
    const double t11 = kRl + rt2;
    const double t12 = kRin + kRl;
    const double t13 = kRl * t4 + t9;
    const double t14 = t4 * (kRl + rt1) + t9;
    const double t15 = kRl + t1;
    const double t16 = rb1 * t15;
    const double t17 = kC2 + kC3;
    const double t18 = kR1 * t17;
    const double t19 = kC2 * t11;
    const double t20 = t2 + t6;
    const double t21 = kC1 * kRl;
    const double t22 = kC3 * rm2;
    const double t23 = t10 + t6;
    const double t24 = rm2 + t6;

    const std::array<double, 4> numerator = {
        0.0,
        kRl * (kC1 * rt2 + t22 + t24 * (kC1 + kC2)),
        kRl * (kC1 * (kC2 * t24 * t3 + t18 * t20) +
               t22 * (kC1 * (t3 + t6) + t7)),
        t21 * t8 * (kR1 * rt2 + rm2 * t5),
    };
    const std::array<double, 4> denominator = {
        t23,
        kC1 * rt1 * t23 + kRin * t23 * (kC1 + t17) + rm1 * t19 + t18 * t23 +
            t19 * (rb1 + rm2) + t20 * t21 + t22 * (t11 + t6),
        kC1 * (rt2 * (kC2 * (kRin * kRl + kRin * rt1 +
                              t15 * (rm1 + rm2) + t16) +
                       kC3 * t14) +
               t18 * (kRl * rm2 + kRl * t1 + rm1 * t15 + rm2 * rt1 +
                      rt2 * t15 + t16 + t9)) +
            kC1 * (kC2 * rt1 * (rb1 * t12 + rm1 * t12 + t13) +
                   kC3 * (rb1 * t14 + rm1 * t14 + rt1 * t13)) +
            t8 * (kR1 * t10 + t11 * t4 + t9),
        kC1 * t8 *
            (kR1 * t1 * t2 + kRin * (rm2 * t3 + t0) +
             kRl * (kR1 * t3 + t4 * t5) + rm2 * t0),
    };

    const std::array<double, 4> digital_numerator =
        BilinearTransformPoly(numerator, sample_rate_hz_);
    const std::array<double, 4> digital_denominator =
        BilinearTransformPoly(denominator, sample_rate_hz_);
    filter_.SetNormalized(
        digital_numerator[0], digital_numerator[1], digital_numerator[2],
        digital_numerator[3], digital_denominator[0], digital_denominator[1],
        digital_denominator[2], digital_denominator[3]);
  }

  double sample_rate_hz_ = 48000.0;
  TubeStageControls controls_;
  ThirdOrderIIR filter_;
};

class SingleStagePreamp {
public:
  void SetSampleRate(double sample_rate_hz) {
    stage_.SetSampleRate(sample_rate_hz);
  }

  void SetProfile(const PreampProfile& profile) {
    stage_.SetSpec(profile.spec);
  }

  void SetControls(const TubeStageControls& controls) {
    TubeStageControls stage_controls = controls;
    stage_controls.level_db = 0.0;
    stage_.SetControls(stage_controls);
    output_level_lin_ = DbToLin(controls.level_db);
  }

  void Reset() {
    stage_.Reset();
  }

  float Process(float x) {
    return static_cast<float>(output_level_lin_ * stage_.Process(x));
  }

private:
  static double DbToLin(double db) {
    return std::pow(10.0, db / 20.0);
  }

  TubeStage stage_;
  double output_level_lin_ = 1.0;
};

struct MultiStageStageVoicing {
  TubeStageSpec spec;
  double drive_scale = 0.0;
  double drive_offset_db = 0.0;
  double level_scale = 0.0;
  double level_offset_db = 0.0;
  double bright_scale = 0.0;
  double bright_offset_db = 0.0;
  double bias_scale = 0.0;
  double bias_offset = 0.0;
  bool apply_tone_stack = false;
  bool apply_presence = false;
};

struct MultiStageLinkVoicing {
  double highpass_hz = 0.0;
  double lowpass_hz = 0.0;
  double trim_db = 0.0;
  bool apply_mesa_mark_tone_stack = false;
};

struct MultiStageVoice {
  int stage_count = 0;
  double input_trim_db = 0.0;
  std::array<MultiStageStageVoicing, 4> stages{};
  std::array<MultiStageLinkVoicing, 4> links{};
};

class MultiStagePreamp {
public:
  void SetSampleRate(double sample_rate_hz) {
    sample_rate_hz_ = sample_rate_hz;
    for (auto& stage : stages_) {
      stage.SetSampleRate(sample_rate_hz_);
    }
    for (auto& tone_stack : link_tone_stacks_) {
      tone_stack.SetSampleRate(sample_rate_hz_);
    }
    UpdateDerived();
    Reset();
  }

  void SetProfile(const PreampProfile& profile) {
    profile_ = profile;
    voice_ = BuildVoice(profile_);
    UpdateDerived();
  }

  void SetControls(const TubeStageControls& controls) {
    controls_ = controls;
    UpdateDerived();
  }

  void Reset() {
    for (auto& stage : stages_) {
      stage.Reset();
    }
    for (auto& tone_stack : link_tone_stacks_) {
      tone_stack.Reset();
    }
    for (auto& hpf : link_hpfs_) {
      hpf.Reset();
    }
    for (auto& lpf : link_lpfs_) {
      lpf.Reset();
    }
  }

  float Process(float x) {
    double s = input_trim_lin_ * static_cast<double>(x);
    for (int i = 0; i < voice_.stage_count; ++i) {
      s = stages_[i].Process(static_cast<float>(s));
      if (link_tone_stack_enabled_[i]) {
        s = link_tone_stacks_[i].Process(static_cast<float>(s));
      }
      s *= link_trim_lin_[i];
      if (link_hpf_enabled_[i]) {
        s = link_hpfs_[i].Process(s);
      }
      if (link_lpf_enabled_[i]) {
        s = link_lpfs_[i].Process(s);
      }
    }
    return static_cast<float>(output_level_lin_ * s);
  }

private:
  static constexpr int kMaxStages = 4;

  static double DbToLin(double db) {
    return std::pow(10.0, db / 20.0);
  }

  static TubeStageSpec MakeStageSpec(const TubeStageSpec& base,
                                     double input_hpf_hz,
                                     double bright_hpf_hz,
                                     double plate_lpf_hz,
                                     double output_hpf_hz,
                                     double nominal_bias,
                                     double positive_curve,
                                     double negative_curve,
                                     double asymmetry,
                                     double cathode_memory_amount) {
    TubeStageSpec spec = base;
    spec.input_hpf_hz = input_hpf_hz;
    spec.bright_hpf_hz = bright_hpf_hz;
    spec.plate_lpf_hz = plate_lpf_hz;
    spec.output_hpf_hz = output_hpf_hz;
    spec.nominal_bias = nominal_bias;
    spec.positive_curve = positive_curve;
    spec.negative_curve = negative_curve;
    spec.asymmetry = asymmetry;
    spec.cathode_memory_amount = cathode_memory_amount;
    return spec;
  }

  static MultiStageVoice BuildMarkIICPlusVoice(const PreampProfile& profile) {
    MultiStageVoice voice;
    voice.stage_count = 4;
    voice.input_trim_db = 1.5;
    voice.stages[0] = {
        MakeStageSpec(profile.spec, 90.0, 2000.0, 5500.0, 120.0,
                      0.055, 1.68, 1.12, 0.95, 0.12),
        0.22, 1.5, 0.0, -1.0, 0.55, 0.0, 0.15, 0.0, false, false};
    voice.stages[1] = {
        MakeStageSpec(profile.spec, 130.0, 1850.0, 4700.0, 150.0,
                      0.070, 1.85, 1.20, 0.98, 0.15),
        0.28, 3.25, 0.0, -0.5, 0.25, 0.0, 0.35, 0.01, false, false};
    voice.stages[2] = {
        MakeStageSpec(profile.spec, 180.0, 1650.0, 3900.0, 170.0,
                      0.100, 2.10, 1.32, 1.02, 0.20),
        0.32, 2.75, 0.0, -2.0, 0.10, 0.0, 0.50, 0.02, false, false};
    TubeStageSpec final_stage_spec =
        MakeStageSpec(profile.spec, 95.0, 2100.0, 3500.0, 88.0, 0.080, 1.86,
                      1.22, 0.97, 0.16);
    voice.stages[3] = {
        final_stage_spec,
        0.18, 0.75, 1.0, 0.0, 0.15, 0.0, 0.20, 0.0, false, true};
    voice.links[0] = {82.0, 7600.0, 2.25, true};
    voice.links[1] = {145.0, 6300.0, -2.2};
    voice.links[2] = {180.0, 5200.0, -2.8};
    voice.links[3] = {0.0, 4600.0, 0.0};
    return voice;
  }

  static MultiStageVoice BuildSLO100Voice(const PreampProfile& profile) {
    MultiStageVoice voice;
    voice.stage_count = 4;
    voice.input_trim_db = 1.2;
    voice.stages[0] = {
        MakeStageSpec(profile.spec, 82.0, 1900.0, 5200.0, 105.0,
                      0.060, 1.72, 1.14, 0.96, 0.13),
        0.24, 1.0, 0.0, -0.8, 0.45, 0.0, 0.15, 0.0, false, false};
    voice.stages[1] = {
        MakeStageSpec(profile.spec, 115.0, 1750.0, 4500.0, 138.0,
                      0.078, 1.92, 1.24, 0.99, 0.16),
        0.29, 1.75, 0.0, -1.4, 0.18, 0.0, 0.30, 0.01, false, false};
    voice.stages[2] = {
        MakeStageSpec(profile.spec, 155.0, 1600.0, 3900.0, 155.0,
                      0.095, 2.04, 1.32, 1.01, 0.18),
        0.30, 2.2, 0.0, -1.8, 0.10, 0.0, 0.45, 0.015, false, false};
    voice.stages[3] = {
        MakeStageSpec(profile.spec, 90.0, 2050.0, 3600.0, 90.0,
                      0.082, 1.90, 1.24, 0.98, 0.17),
        0.17, 0.5, 1.0, 0.0, 0.12, 0.0, 0.18, 0.0, true, true};
    voice.links[0] = {95.0, 7300.0, -0.9};
    voice.links[1] = {130.0, 6100.0, -1.8};
    voice.links[2] = {165.0, 5200.0, -2.4};
    voice.links[3] = {0.0, 4300.0, 0.0};
    return voice;
  }

  static MultiStageVoice Build5150Voice(const PreampProfile& profile) {
    MultiStageVoice voice;
    voice.stage_count = 4;
    voice.input_trim_db = 2.0;
    voice.stages[0] = {
        MakeStageSpec(profile.spec, 95.0, 1750.0, 5000.0, 118.0,
                      0.065, 1.76, 1.18, 0.98, 0.14),
        0.22, 1.5, 0.0, -1.1, 0.60, 0.0, 0.18, 0.0, false, false};
    voice.stages[1] = {
        MakeStageSpec(profile.spec, 140.0, 1600.0, 4300.0, 160.0,
                      0.086, 2.00, 1.28, 1.00, 0.17),
        0.30, 2.25, 0.0, -1.7, 0.20, 0.0, 0.32, 0.01, false, false};
    voice.stages[2] = {
        MakeStageSpec(profile.spec, 205.0, 1450.0, 3600.0, 210.0,
                      0.115, 2.18, 1.45, 1.08, 0.22),
        0.33, 3.0, 0.0, -2.2, 0.08, 0.0, 0.55, 0.03, false, false};
    voice.stages[3] = {
        MakeStageSpec(profile.spec, 105.0, 1900.0, 3300.0, 95.0,
                      0.086, 1.94, 1.28, 1.00, 0.18),
        0.18, 0.5, 1.0, 0.0, 0.10, 0.0, 0.20, 0.0, true, true};
    voice.links[0] = {120.0, 7000.0, -1.2};
    voice.links[1] = {170.0, 5600.0, -2.1};
    voice.links[2] = {220.0, 4700.0, -2.9};
    voice.links[3] = {0.0, 4000.0, 0.0};
    return voice;
  }

  static MultiStageVoice BuildDualRectifierVoice(const PreampProfile& profile) {
    MultiStageVoice voice;
    voice.stage_count = 4;
    voice.input_trim_db = 1.0;
    voice.stages[0] = {
        MakeStageSpec(profile.spec, 78.0, 1650.0, 4800.0, 92.0,
                      0.060, 1.74, 1.16, 0.98, 0.14),
        0.24, 1.0, 0.0, -0.8, 0.35, 0.0, 0.14, 0.0, false, false};
    voice.stages[1] = {
        MakeStageSpec(profile.spec, 105.0, 1500.0, 4000.0, 125.0,
                      0.084, 1.96, 1.26, 1.00, 0.17),
        0.30, 2.0, 0.0, -1.4, 0.15, 0.0, 0.30, 0.01, false, false};
    voice.stages[2] = {
        MakeStageSpec(profile.spec, 145.0, 1400.0, 3400.0, 155.0,
                      0.104, 2.08, 1.34, 1.03, 0.21),
        0.29, 2.6, 0.0, -2.0, 0.08, 0.0, 0.42, 0.02, false, false};
    voice.stages[3] = {
        MakeStageSpec(profile.spec, 82.0, 1750.0, 3200.0, 78.0,
                      0.088, 1.90, 1.24, 0.99, 0.18),
        0.17, 0.4, 1.0, 0.0, 0.12, 0.0, 0.18, 0.0, true, true};
    voice.links[0] = {85.0, 6500.0, -0.8};
    voice.links[1] = {110.0, 5200.0, -1.8};
    voice.links[2] = {140.0, 4300.0, -2.6};
    voice.links[3] = {0.0, 3800.0, 0.0};
    return voice;
  }

  static MultiStageVoice BuildFallbackVoice(const PreampProfile& profile) {
    MultiStageVoice voice;
    voice.stage_count = 3;
    voice.stages[0] = {
        profile.spec, 0.28, 1.0, 0.0, -1.0, 0.35, 0.0, 0.20, 0.0, false, false};
    voice.stages[1] = {
        profile.spec, 0.34, 2.0, 0.0, -2.0, 0.10, 0.0, 0.35, 0.01, false, false};
    voice.stages[2] = {
        profile.spec, 0.18, 0.0, 1.0, 0.0, 0.10, 0.0, 0.15, 0.0, true, true};
    voice.links[0] = {110.0, 6500.0, -1.5};
    voice.links[1] = {160.0, 5200.0, -2.5};
    voice.links[2] = {0.0, 4200.0, 0.0};
    return voice;
  }

  static MultiStageVoice BuildVoice(const PreampProfile& profile) {
    if (profile.circuit == "mark_iic_plus") {
      return BuildMarkIICPlusVoice(profile);
    }
    if (profile.circuit == "slo_100") {
      return BuildSLO100Voice(profile);
    }
    if (profile.circuit == "5150") {
      return Build5150Voice(profile);
    }
    if (profile.circuit == "dual_rectifier") {
      return BuildDualRectifierVoice(profile);
    }
    return BuildFallbackVoice(profile);
  }

  TubeStageControls MakeStageControls(const MultiStageStageVoicing& voicing) const {
    TubeStageControls controls;
    controls.drive_db = voicing.drive_offset_db + controls_.drive_db * voicing.drive_scale;
    controls.level_db = voicing.level_offset_db;
    controls.bright_db =
        voicing.bright_offset_db + controls_.bright_db * voicing.bright_scale;
    controls.bias_trim = voicing.bias_offset + controls_.bias_trim * voicing.bias_scale;
    if (voicing.apply_tone_stack) {
      controls.bass = controls_.bass;
      controls.mid = controls_.mid;
      controls.treble = controls_.treble;
    }
    if (voicing.apply_presence) {
      controls.presence = controls_.presence;
    }
    return controls;
  }

  void UpdateDerived() {
    input_trim_lin_ = DbToLin(voice_.input_trim_db);
    output_level_lin_ = DbToLin(controls_.level_db);
    for (int i = 0; i < kMaxStages; ++i) {
      stages_[i].SetSpec(voice_.stages[i].spec);
      stages_[i].SetControls(MakeStageControls(voice_.stages[i]));
      link_tone_stack_enabled_[i] = voice_.links[i].apply_mesa_mark_tone_stack;
      if (link_tone_stack_enabled_[i]) {
        link_tone_stacks_[i].SetControls(controls_);
      }

      link_trim_lin_[i] = DbToLin(voice_.links[i].trim_db);
      link_hpf_enabled_[i] = voice_.links[i].highpass_hz > 0.0;
      link_lpf_enabled_[i] = voice_.links[i].lowpass_hz > 0.0;
      if (link_hpf_enabled_[i]) {
        link_hpfs_[i].SetCutoff(sample_rate_hz_, voice_.links[i].highpass_hz);
      }
      if (link_lpf_enabled_[i]) {
        link_lpfs_[i].SetCutoff(sample_rate_hz_, voice_.links[i].lowpass_hz);
      }
    }
  }

  double sample_rate_hz_ = 48000.0;
  double input_trim_lin_ = 1.0;
  double output_level_lin_ = 1.0;
  PreampProfile profile_;
  TubeStageControls controls_;
  MultiStageVoice voice_;
  std::array<TubeStage, kMaxStages> stages_{};
  std::array<MesaMarkInterstageToneStack, kMaxStages> link_tone_stacks_{};
  std::array<OnePoleHPF, kMaxStages> link_hpfs_{};
  std::array<OnePoleLPF, kMaxStages> link_lpfs_{};
  std::array<double, kMaxStages> link_trim_lin_{1.0, 1.0, 1.0, 1.0};
  std::array<bool, kMaxStages> link_tone_stack_enabled_{false, false, false, false};
  std::array<bool, kMaxStages> link_hpf_enabled_{false, false, false, false};
  std::array<bool, kMaxStages> link_lpf_enabled_{false, false, false, false};
};

class PreampProcessor {
public:
  void SetSampleRate(double sample_rate_hz) {
    single_stage_.SetSampleRate(sample_rate_hz);
    multi_stage_.SetSampleRate(sample_rate_hz);
  }

  void SetProfile(const PreampProfile& profile) {
    topology_ = profile.topology;
    single_stage_.SetProfile(profile);
    multi_stage_.SetProfile(profile);
  }

  void SetControls(const TubeStageControls& controls) {
    single_stage_.SetControls(controls);
    multi_stage_.SetControls(controls);
  }

  void Reset() {
    single_stage_.Reset();
    multi_stage_.Reset();
  }

  float Process(float x) {
    if (topology_ == PreampTopology::kMultiStage) {
      return multi_stage_.Process(x);
    }
    return single_stage_.Process(x);
  }

private:
  PreampTopology topology_ = PreampTopology::kSingleStage;
  SingleStagePreamp single_stage_;
  MultiStagePreamp multi_stage_;
};

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

enum class ToneStackMode {
  kClassicApprox,
  kMesaMarkPassive,
};

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
  ToneStackMode tone_stack_mode = ToneStackMode::kClassicApprox;
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

class Biquad {
public:
  void Reset(double value = 0.0) {
    x1_ = value;
    x2_ = value;
    y1_ = value;
    y2_ = value;
  }

  double Process(double x) {
    const double y = b0_ * x + b1_ * x1_ + b2_ * x2_ - a1_ * y1_ - a2_ * y2_;
    x2_ = x1_;
    x1_ = x;
    y2_ = y1_;
    y1_ = y;
    return y;
  }

  void SetPeakingEQ(double sample_rate_hz,
                    double frequency_hz,
                    double q,
                    double gain_db) {
    const double clamped_frequency =
        std::clamp(frequency_hz, 10.0, 0.45 * sample_rate_hz);
    const double clamped_q = std::max(0.1, q);
    const double a = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * kPi * clamped_frequency / sample_rate_hz;
    const double alpha = std::sin(w0) / (2.0 * clamped_q);
    const double cos_w0 = std::cos(w0);

    const double b0 = 1.0 + alpha * a;
    const double b1 = -2.0 * cos_w0;
    const double b2 = 1.0 - alpha * a;
    const double a0 = 1.0 + alpha / a;
    const double a1 = -2.0 * cos_w0;
    const double a2 = 1.0 - alpha / a;
    SetNormalized(b0, b1, b2, a0, a1, a2);
  }

  void SetLowShelf(double sample_rate_hz,
                   double frequency_hz,
                   double slope,
                   double gain_db) {
    const double clamped_frequency =
        std::clamp(frequency_hz, 10.0, 0.45 * sample_rate_hz);
    const double clamped_slope = std::max(0.1, slope);
    const double a = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * kPi * clamped_frequency / sample_rate_hz;
    const double cos_w0 = std::cos(w0);
    const double sin_w0 = std::sin(w0);
    const double alpha =
        sin_w0 / 2.0 *
        std::sqrt((a + 1.0 / a) * (1.0 / clamped_slope - 1.0) + 2.0);
    const double beta = 2.0 * std::sqrt(a) * alpha;

    const double b0 = a * ((a + 1.0) - (a - 1.0) * cos_w0 + beta);
    const double b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cos_w0);
    const double b2 = a * ((a + 1.0) - (a - 1.0) * cos_w0 - beta);
    const double a0 = (a + 1.0) + (a - 1.0) * cos_w0 + beta;
    const double a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cos_w0);
    const double a2 = (a + 1.0) + (a - 1.0) * cos_w0 - beta;
    SetNormalized(b0, b1, b2, a0, a1, a2);
  }

  void SetHighShelf(double sample_rate_hz,
                    double frequency_hz,
                    double slope,
                    double gain_db) {
    const double clamped_frequency =
        std::clamp(frequency_hz, 10.0, 0.45 * sample_rate_hz);
    const double clamped_slope = std::max(0.1, slope);
    const double a = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * kPi * clamped_frequency / sample_rate_hz;
    const double cos_w0 = std::cos(w0);
    const double sin_w0 = std::sin(w0);
    const double alpha =
        sin_w0 / 2.0 *
        std::sqrt((a + 1.0 / a) * (1.0 / clamped_slope - 1.0) + 2.0);
    const double beta = 2.0 * std::sqrt(a) * alpha;

    const double b0 = a * ((a + 1.0) + (a - 1.0) * cos_w0 + beta);
    const double b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cos_w0);
    const double b2 = a * ((a + 1.0) + (a - 1.0) * cos_w0 - beta);
    const double a0 = (a + 1.0) - (a - 1.0) * cos_w0 + beta;
    const double a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cos_w0);
    const double a2 = (a + 1.0) - (a - 1.0) * cos_w0 - beta;
    SetNormalized(b0, b1, b2, a0, a1, a2);
  }

private:
  static constexpr double kPi = 3.14159265358979323846;

  void SetNormalized(double b0,
                     double b1,
                     double b2,
                     double a0,
                     double a1,
                     double a2) {
    b0_ = b0 / a0;
    b1_ = b1 / a0;
    b2_ = b2 / a0;
    a1_ = a1 / a0;
    a2_ = a2 / a0;
  }

  double b0_ = 1.0;
  double b1_ = 0.0;
  double b2_ = 0.0;
  double a1_ = 0.0;
  double a2_ = 0.0;
  double x1_ = 0.0;
  double x2_ = 0.0;
  double y1_ = 0.0;
  double y2_ = 0.0;
};

class ThirdOrderIIR {
public:
  void Reset(double value = 0.0) {
    x1_ = value;
    x2_ = value;
    x3_ = value;
    y1_ = value;
    y2_ = value;
    y3_ = value;
  }

  double Process(double x) {
    const double y = b0_ * x + b1_ * x1_ + b2_ * x2_ + b3_ * x3_ -
                     a1_ * y1_ - a2_ * y2_ - a3_ * y3_;
    x3_ = x2_;
    x2_ = x1_;
    x1_ = x;
    y3_ = y2_;
    y2_ = y1_;
    y1_ = std::isfinite(y) ? y : 0.0;
    return y1_;
  }

  void SetIdentity() {
    b0_ = 1.0;
    b1_ = 0.0;
    b2_ = 0.0;
    b3_ = 0.0;
    a1_ = 0.0;
    a2_ = 0.0;
    a3_ = 0.0;
  }

  void SetNormalized(double b0,
                     double b1,
                     double b2,
                     double b3,
                     double a0,
                     double a1,
                     double a2,
                     double a3) {
    if (std::abs(a0) < 1.0e-18) {
      SetIdentity();
      return;
    }
    b0_ = b0 / a0;
    b1_ = b1 / a0;
    b2_ = b2 / a0;
    b3_ = b3 / a0;
    a1_ = a1 / a0;
    a2_ = a2 / a0;
    a3_ = a3 / a0;
  }

private:
  double b0_ = 1.0;
  double b1_ = 0.0;
  double b2_ = 0.0;
  double b3_ = 0.0;
  double a1_ = 0.0;
  double a2_ = 0.0;
  double a3_ = 0.0;
  double x1_ = 0.0;
  double x2_ = 0.0;
  double x3_ = 0.0;
  double y1_ = 0.0;
  double y2_ = 0.0;
  double y3_ = 0.0;
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
    tone_bass_shelf_.Reset();
    tone_fixed_mid_scoop_.Reset();
    tone_mid_peak_.Reset();
    tone_treble_shelf_.Reset();
    mesa_mark_tone_stack_.Reset();
    plate_lpf_.Reset();
    presence_shelf_.Reset();
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
    if (spec_.tone_stack_mode == ToneStackMode::kMesaMarkPassive) {
      return mesa_mark_tone_stack_.Process(x);
    }
    double y = tone_bass_shelf_.Process(x);
    y = tone_fixed_mid_scoop_.Process(y);
    y = tone_mid_peak_.Process(y);
    y = tone_treble_shelf_.Process(y);
    return y * tone_stack_loss_lin_;
  }

  double ApplyPresencePost(double x) {
    return presence_shelf_.Process(x);
  }

  void UpdateDerived() {
    input_hpf_.SetCutoff(sample_rate_hz_, spec_.input_hpf_hz);
    bright_hpf_.SetCutoff(sample_rate_hz_, spec_.bright_hpf_hz);
    if (spec_.tone_stack_mode == ToneStackMode::kMesaMarkPassive) {
      UpdateMesaMarkPassiveToneStack();
    } else {
      tone_bass_shelf_.SetLowShelf(
          sample_rate_hz_, 110.0, 0.70, ControlToDb(controls_.bass, 7.0));
      tone_fixed_mid_scoop_.SetPeakingEQ(sample_rate_hz_, 750.0, 0.75, -5.0);
      tone_mid_peak_.SetPeakingEQ(
          sample_rate_hz_, 750.0, 0.90, ControlToDb(controls_.mid, 9.0));
      tone_treble_shelf_.SetHighShelf(
          sample_rate_hz_, 2200.0, 0.75, ControlToDb(controls_.treble, 7.0));
    }
    plate_lpf_.SetCutoff(sample_rate_hz_, spec_.plate_lpf_hz);
    presence_shelf_.SetHighShelf(
        sample_rate_hz_, 3200.0, 0.80, ControlToDb(controls_.presence, 4.5));
    output_hpf_.SetCutoff(sample_rate_hz_, spec_.output_hpf_hz);

    drive_lin_ = DbToLin(controls_.drive_db);
    level_lin_ = DbToLin(controls_.level_db);
    bright_gain_ = std::max(0.0, DbToLin(controls_.bright_db) - 1.0);
    tone_stack_loss_lin_ = DbToLin(-4.8);

    const double tau_seconds = 0.020;
    cathode_env_alpha_ = std::exp(-1.0 / (sample_rate_hz_ * tau_seconds));
  }

  static double DbToLin(double db) {
    return std::pow(10.0, db / 20.0);
  }

  static double ControlToDb(double value, double max_db) {
    const double normalized = std::clamp(value, 0.0, 10.0);
    return ((normalized - 5.0) / 5.0) * max_db;
  }

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

  void UpdateMesaMarkPassiveToneStack() {
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
    mesa_mark_tone_stack_.SetNormalized(
        digital_numerator[0], digital_numerator[1], digital_numerator[2],
        digital_numerator[3], digital_denominator[0], digital_denominator[1],
        digital_denominator[2], digital_denominator[3]);
  }

  TubeStageSpec spec_;
  TubeStageControls controls_;

  double sample_rate_hz_ = 48000.0;
  double drive_lin_ = 1.0;
  double level_lin_ = 1.0;
  double bright_gain_ = 0.0;
  double tone_stack_loss_lin_ = 1.0;
  double cathode_env_ = 0.0;
  double cathode_env_alpha_ = 0.999;

  OnePoleHPF input_hpf_;
  OnePoleHPF bright_hpf_;
  Biquad tone_bass_shelf_;
  Biquad tone_fixed_mid_scoop_;
  Biquad tone_mid_peak_;
  Biquad tone_treble_shelf_;
  ThirdOrderIIR mesa_mark_tone_stack_;
  OnePoleLPF plate_lpf_;
  Biquad presence_shelf_;
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

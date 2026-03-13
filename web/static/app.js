const valueKeys = [
  "amp",
  "preamp",
  "power_tube_type",
  "drive_db",
  "level_db",
  "bright_db",
  "bias_trim",
  "bass",
  "mid",
  "treble",
  "presence",
  "power_drive_db",
  "power_level_db",
  "power_bias_trim",
  "compressor_sustain",
  "compressor_attack",
  "compressor_level_db",
  "compressor_blend",
  "klon_drive",
  "klon_tone",
  "klon_level_db",
  "klon_clean_blend",
  "tubescreamer_drive",
  "tubescreamer_tone",
  "tubescreamer_level_db",
  "rat_distortion",
  "rat_filter",
  "rat_level_db",
  "chorus_depth",
  "chorus_tone",
  "chorus_level_db",
  "chorus_mix",
  "plate_mix",
  "plate_brightness",
  "plate_level_db",
  "plate_decay",
];

const checkboxKeys = [
  "effect_compression_enabled",
  "effect_klon_enabled",
  "effect_tubescreamer_enabled",
  "effect_rat_enabled",
  "effect_chorus_enabled",
  "effect_plate_enabled",
];

const stateKeys = [...valueKeys, ...checkboxKeys];

const effectBlockIds = {
  effect_compression_enabled: "effect_compression_block",
  effect_klon_enabled: "effect_klon_block",
  effect_tubescreamer_enabled: "effect_tubescreamer_block",
  effect_rat_enabled: "effect_rat_block",
  effect_chorus_enabled: "effect_chorus_block",
  effect_plate_enabled: "effect_plate_block",
};

let saveTimer = null;
let audioBackend = "portaudio";
let currentControlSchema = {
  input_hpf_hz: 60.0,
};

function $(id) {
  return document.getElementById(id);
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function controlToDb(value, maxDb) {
  const normalized = clamp(Number(value || 5), 0, 10);
  return ((normalized - 5) / 5) * maxDb;
}

function onePoleCoefficients(sampleRate, cutoffHz) {
  const clamped = clamp(cutoffHz, 1, 0.45 * sampleRate);
  const a = Math.exp(-2 * Math.PI * clamped / sampleRate);
  return {a, b: 1 - a};
}

function complexAdd(a, b) {
  return {re: a.re + b.re, im: a.im + b.im};
}

function complexSub(a, b) {
  return {re: a.re - b.re, im: a.im - b.im};
}

function complexMul(a, b) {
  return {
    re: a.re * b.re - a.im * b.im,
    im: a.re * b.im + a.im * b.re,
  };
}

function complexScale(a, scalar) {
  return {re: a.re * scalar, im: a.im * scalar};
}

function complexMagnitude(a) {
  return Math.sqrt(a.re * a.re + a.im * a.im);
}

function onePoleLpfResponse(sampleRate, cutoffHz, frequencyHz) {
  const {a, b} = onePoleCoefficients(sampleRate, cutoffHz);
  const omega = 2 * Math.PI * frequencyHz / sampleRate;
  const zRe = Math.cos(omega);
  const zIm = -Math.sin(omega);
  const denom = {re: 1 - a * zRe, im: -a * zIm};
  const denomMagSq = denom.re * denom.re + denom.im * denom.im;
  return {
    re: b * denom.re / denomMagSq,
    im: -b * denom.im / denomMagSq,
  };
}

function onePoleHpfResponse(sampleRate, cutoffHz, frequencyHz) {
  return complexSub({re: 1, im: 0}, onePoleLpfResponse(sampleRate, cutoffHz, frequencyHz));
}

function evaluateTransfer(coeffs, frequencyHz) {
  const angular = 2 * Math.PI * frequencyHz;
  const sumPoly = (poly) => {
    let re = 0;
    let im = 0;
    let multiplier = 1;
    for (let i = 0; i < poly.length; i += 1) {
      if (i % 2 === 1) {
        im += multiplier * poly[i];
        multiplier *= -angular;
      } else {
        re += multiplier * poly[i];
        multiplier *= angular;
      }
    }
    return {re, im};
  };

  const num = sumPoly(coeffs[0]);
  const den = sumPoly(coeffs[1]);
  const denMagSq = den.re * den.re + den.im * den.im;
  return {
    re: (num.re * den.re + num.im * den.im) / denMagSq,
    im: (num.im * den.re - num.re * den.im) / denMagSq,
  };
}

function applyTaper(position, taper) {
  const x = clamp(position, 0, 1);
  if (taper === "LogA") {
    return x < 0.5 ? x * 0.6 : x * 1.4 - 0.4;
  }
  if (taper === "LogB") {
    return x < 0.5 ? x * 0.2 : x * 1.8 - 0.8;
  }
  if (taper === "LogC") {
    return x < 0.5 ? x * 1.4 : x * 0.6 + 0.4;
  }
  return x;
}

function splitPotValue(value, proportion) {
  const r2 = proportion * value;
  return [r2, value - r2];
}

function getReferenceTonestackDefinition() {
  const preamp = $("preamp")?.value || "";

  if (preamp === "mesa_boogie_mark_iic_plus") {
    return {
      name: "Mesa Mark passive reference",
      model: "bassman",
      components: {
        RIN: 1300,
        RL: 1e6,
        RB: 1e6,
        RM: 25e3,
        RT: 250e3,
        R1: 47e3,
        C1: 500e-12,
        C2: 22e-9,
        C3: 22e-9,
      },
      tapers: {RB: "LogB", RM: "Linear", RT: "Linear"},
    };
  }

  if (preamp === "vox_ac30_top_boost") {
    return {
      name: "Vox reference",
      model: "vox",
      components: {
        RIN: 717,
        RL: 600e3,
        RB: 1e6,
        RT: 1e6,
        R1: 100e3,
        R2: 10e3,
        C1: 47e-12,
        C2: 22e-9,
        C3: 22e-9,
      },
      tapers: {RB: "LogA", RT: "LogA"},
    };
  }

  if (preamp === "fender_bassman_5f6a") {
    return {
      name: "Bassman 5F6-A reference",
      model: "bassman",
      components: {
        RIN: 1300,
        RL: 1e6,
        RB: 1e6,
        RM: 25e3,
        RT: 250e3,
        R1: 56e3,
        C1: 250e-12,
        C2: 20e-9,
        C3: 20e-9,
      },
      tapers: {RB: "LogA", RM: "Linear", RT: "Linear"},
    };
  }

  if ([
    "marshall_jtm45",
    "marshall_plexi_1959",
    "marshall_jcm800",
    "soldano_slo_100",
    "peavey_5150",
    "mesa_dual_rectifier",
  ].includes(preamp)) {
    return {
      name: "Marshall reference",
      model: "bassman",
      components: {
        RIN: 1300,
        RL: 517e3,
        RB: 1e6,
        RM: 25e3,
        RT: 220e3,
        R1: 33e3,
        C1: 470e-12,
        C2: 22e-9,
        C3: 22e-9,
      },
      tapers: {RB: "LogB", RM: "Linear", RT: "Linear"},
    };
  }

  if ([
    "fender_deluxe_reverb",
    "fender_twin_reverb_ab763",
    "fender_princeton",
  ].includes(preamp)) {
    return {
      name: "Fender TMB reference",
      model: "fender_tmb",
      components: {
        RIN: 38e3,
        RL: 1e6,
        RB: 250e3,
        RM: 10e3,
        RT: 250e3,
        R1: 100e3,
        C1: 250e-12,
        C2: 100e-9,
        C3: 47e-9,
      },
      tapers: {RB: "LogA", RM: "Linear", RT: "LogA"},
    };
  }

  return null;
}

function getReferenceControlValue(controls, key) {
  return clamp(Number(controls[key] ?? 5), 0, 10) / 10;
}

function bassmanReferenceCoefficients(definition, controls) {
  const {components, tapers} = definition;
  const [RT2, RT1] = splitPotValue(components.RT, applyTaper(getReferenceControlValue(controls, "treble"), tapers.RT));
  const [RM2, RM1] = splitPotValue(components.RM, applyTaper(getReferenceControlValue(controls, "mid"), tapers.RM));
  const [RB2, RB1] = splitPotValue(components.RB, applyTaper(getReferenceControlValue(controls, "bass"), tapers.RB));
  const {RIN, R1, RL, C1, C2, C3} = components;

  const t0 = RT1 * RT2;
  const t1 = RIN + RT1;
  const t2 = RM2 + RT2;
  const t3 = RT1 + RT2;
  const t4 = RIN + RM2;
  const t5 = R1 + t3;
  const t6 = RB1 + RM1;
  const t7 = C2 * t6;
  const t8 = C3 * t7;
  const t9 = RIN * RM2;
  const t10 = RL + t2;
  const t11 = RL + RT2;
  const t12 = RIN + RL;
  const t13 = RL * t4 + t9;
  const t14 = t4 * (RL + RT1) + t9;
  const t15 = RL + t1;
  const t16 = RB1 * t15;
  const t17 = C2 + C3;
  const t18 = R1 * t17;
  const t19 = C2 * t11;
  const t20 = t2 + t6;
  const t21 = C1 * RL;
  const t22 = C3 * RM2;
  const t23 = t10 + t6;
  const t24 = RM2 + t6;

  const denAIm = C1 * t8 * (R1 * t1 * t2 + RIN * (RM2 * t3 + t0) + RL * (R1 * t3 + t4 * t5) + RM2 * t0);
  const denBRe = C1 * (RT2 * (C2 * (RIN * RL + RIN * RT1 + t15 * (RM1 + RM2) + t16) + C3 * t14) + t18 * (RL * RM2 + RL * t1 + RM1 * t15 + RM2 * RT1 + RT2 * t15 + t16 + t9))
    + C1 * (C2 * RT1 * (RB1 * t12 + RM1 * t12 + t13) + C3 * (RB1 * t14 + RM1 * t14 + RT1 * t13))
    + t8 * (R1 * t10 + t11 * t4 + t9);
  const denCIm = C1 * RT1 * t23 + RIN * t23 * (C1 + t17) + RM1 * t19 + t18 * t23 + t19 * (RB1 + RM2) + t20 * t21 + t22 * (t11 + t6);
  const denDRe = t23;
  const numAIm = t21 * t8 * (R1 * RT2 + RM2 * t5);
  const numBRe = RL * (C1 * (C2 * t24 * t3 + t18 * t20) + t22 * (C1 * (t3 + t6) + t7));
  const numCIm = RL * (C1 * RT2 + t22 + t24 * (C1 + C2));
  const numDRe = 0;

  return [
    [numDRe, numCIm, numBRe, numAIm],
    [denDRe, denCIm, denBRe, denAIm],
  ];
}

function fenderTmbReferenceCoefficients(definition, controls) {
  const {components, tapers} = definition;
  const [RT2, RT1] = splitPotValue(components.RT, applyTaper(getReferenceControlValue(controls, "treble"), tapers.RT));
  const RM = splitPotValue(components.RM, applyTaper(getReferenceControlValue(controls, "mid"), tapers.RM))[0];
  const RB = splitPotValue(components.RB, applyTaper(getReferenceControlValue(controls, "bass"), tapers.RB))[0];
  const {RIN, R1, RL, C1, C2, C3} = components;

  const t0 = RT1 * RT2;
  const t1 = RIN + RT1;
  const t2 = RM + RT2;
  const t3 = R1 * t2;
  const t4 = RT1 + RT2;
  const t5 = RIN + RM;
  const t6 = R1 + t4;
  const t7 = C2 * C3;
  const t8 = C1 * RB * t7;
  const t9 = C2 + C3;
  const t10 = RL + t2;
  const t11 = R1 * t10;
  const t12 = RIN + RL;
  const t13 = RIN * RL + RM * t12;
  const t14 = RT1 * t13 + RT2 * t5 + t11;
  const t15 = RL + t1;
  const t16 = R1 * t15;
  const t17 = RL + RT2;
  const t18 = C3 * RM;
  const t19 = RB + RM;
  const t20 = C2 * t19;
  const t21 = RB + t10;
  const t22 = RB + t2;
  const t23 = C1 + C2;

  const denAIm = t8 * (RIN * (RM * t4 + t0) + RL * (R1 * t4 + t5 * t6) + RM * t0 + t1 * t3);
  const denBRe = C1 * t9 * (RIN * (RT2 * (RL + RM) + t11) + RL * (RM * RT2 + t3) + RT1 * t14)
    + RB * (C1 * (C2 * (RT1 * t12 + RT2 * t15 + t16) + C3 * (RT1 * t5 + t13 + t16)) + t14 * t7);
  const denCIm = C1 * (RB * (RL + RT1) + RIN * t22 + RL * (t4 + t5) + RT1 * t2)
    + t17 * t20 + t18 * (RB + t17) + t21 * t9 * (R1 + RIN);
  const denDRe = t21;
  const numAIm = RL * t8 * (R1 * RT2 + RM * t6);
  const numBRe = RL * (C1 * (R1 * t22 * t9 + t4 * (t18 + t20)) + RB * t18 * t23);
  const numCIm = RL * (C1 * RT2 + t18 + t19 * t23);
  const numDRe = 0;

  return [
    [numDRe, numCIm, numBRe, numAIm],
    [denDRe, denCIm, denBRe, denAIm],
  ];
}

function voxReferenceCoefficients(definition, controls) {
  const {components, tapers} = definition;
  const [RT2, RT1] = splitPotValue(components.RT, applyTaper(getReferenceControlValue(controls, "treble"), tapers.RT));
  const [RB1, RB2] = splitPotValue(components.RB, applyTaper(getReferenceControlValue(controls, "bass"), tapers.RB));
  const {RIN, R1, R2, RL, C1, C2, C3} = components;

  const t0 = R2 * RB2;
  const t1 = RIN * t0;
  const t2 = RIN + RT1;
  const t3 = R2 + RB2;
  const t4 = RT2 * t3;
  const t5 = RIN * t3 + t0;
  const t6 = RT1 + RT2;
  const t7 = C2 * RB1;
  const t8 = C1 * C3 * t7;
  const t9 = C2 + C3;
  const t10 = RIN * RL;
  const t11 = RIN + RL;
  const t12 = RB2 * t11;
  const t13 = R2 * (t10 + t12);
  const t14 = R2 * RL;
  const t15 = R2 + RL;
  const t16 = RIN * (RL + RT1);
  const t17 = RB2 * t16;
  const t18 = RL + t2;
  const t19 = R1 * t3;
  const t20 = t18 * t19;
  const t21 = RB2 * t18 + t16;
  const t22 = RL + RT2;
  const t23 = C2 * RT1;
  const t24 = RB1 + RT2;
  const t25 = RL * (C1 * t24 + t7) + RT2 * t7;
  const t26 = RB1 + t22;
  const t27 = R2 * t26 + RB2 * (t15 + t24);
  const t28 = R1 * t9;
  const t29 = C1 + t9;
  const t30 = C1 + C2;

  const denAIm = t8 * (R1 * t2 * (t0 + t4) + RL * (R1 * (t0 + t3 * (RIN + t6)) + t5 * t6) + RT1 * (RT2 * t5 + t1) + RT2 * t1);
  const denBRe = C1 * t9 * (R1 * (RB2 * (t14 + t15 * t2) + t14 * t2) + RB2 * RT1 * t10 + RT1 * t13 + RT2 * (R2 * t21 + t17 + t20))
    + RB1 * (C1 * (C2 * t18 * t4 + C3 * t17 + R2 * (C3 * t21 + t11 * t23) + t12 * t23 + t20 * t9) + C2 * C3 * (R1 * (RB2 * t15 + t14) + R2 * RT2 * (R1 + RIN) + RB2 * (RIN * t22 + RT2 * (R1 + R2)) + t13));
  const denCIm = R2 * t25 + RB2 * (R2 * (C2 * RT2 + C3 * t26 + RL * t30) + t25) + t27 * t28 + t27 * (C1 * RT1 + RIN * t29);
  const denDRe = t0 + t26 * t3;
  const numAIm = RL * t8 * (RT2 * (t0 + t19) + t0 * (R1 + RT1));
  const numBRe = RL * (C1 * (C2 * t6 * (R2 * (RB1 + RB2) + RB1 * RB2) + t28 * (R2 * (RB2 + t24) + RB2 * t24)) + C3 * t0 * (C1 * (RB1 + t6) + t7));
  const numCIm = RL * (t0 * t29 + t3 * (C1 * RT2 + RB1 * t30));
  const numDRe = 0;

  return [
    [numDRe, numCIm, numBRe, numAIm],
    [denDRe, denCIm, denBRe, denAIm],
  ];
}

function referenceResponseDbAt(frequencyHz, controls) {
  const definition = getReferenceTonestackDefinition();
  if (!definition) {
    return null;
  }

  let coeffs = null;
  if (definition.model === "bassman") {
    coeffs = bassmanReferenceCoefficients(definition, controls);
  } else if (definition.model === "fender_tmb") {
    coeffs = fenderTmbReferenceCoefficients(definition, controls);
  } else if (definition.model === "vox") {
    coeffs = voxReferenceCoefficients(definition, controls);
  }

  if (!coeffs) {
    return null;
  }

  const h = evaluateTransfer(coeffs, frequencyHz);
  return 20 * Math.log10(Math.max(1e-6, complexMagnitude(h)));
}

function biquadResponse(coeffs, frequencyHz, sampleRate) {
  const omega = 2 * Math.PI * frequencyHz / sampleRate;
  const z1 = {re: Math.cos(omega), im: -Math.sin(omega)};
  const z2 = complexMul(z1, z1);
  const num = complexAdd(
    complexAdd({re: coeffs.b0, im: 0}, complexScale(z1, coeffs.b1)),
    complexScale(z2, coeffs.b2),
  );
  const den = complexAdd(
    complexAdd({re: 1, im: 0}, complexScale(z1, coeffs.a1)),
    complexScale(z2, coeffs.a2),
  );
  const denMagSq = den.re * den.re + den.im * den.im;
  return {
    re: (num.re * den.re + num.im * den.im) / denMagSq,
    im: (num.im * den.re - num.re * den.im) / denMagSq,
  };
}

function bilinearTransformOrder3(coeffs, sampleRate) {
  const k = 2 * sampleRate;
  const k2 = k * k;
  const k3 = k2 * k;
  const transformPoly = (poly) => ([
    poly[0] + poly[1] * k + poly[2] * k2 + poly[3] * k3,
    3 * poly[0] + poly[1] * k - poly[2] * k2 - 3 * poly[3] * k3,
    3 * poly[0] - poly[1] * k - poly[2] * k2 + 3 * poly[3] * k3,
    poly[0] - poly[1] * k + poly[2] * k2 - poly[3] * k3,
  ]);

  const num = transformPoly(coeffs[0]);
  const den = transformPoly(coeffs[1]);
  const a0 = Math.abs(den[0]) < 1e-18 ? 1 : den[0];
  return {
    b0: num[0] / a0,
    b1: num[1] / a0,
    b2: num[2] / a0,
    b3: num[3] / a0,
    a1: den[1] / a0,
    a2: den[2] / a0,
    a3: den[3] / a0,
  };
}

function order3Response(coeffs, frequencyHz, sampleRate) {
  const omega = 2 * Math.PI * frequencyHz / sampleRate;
  const z1 = {re: Math.cos(omega), im: -Math.sin(omega)};
  const z2 = complexMul(z1, z1);
  const z3 = complexMul(z2, z1);
  const num = complexAdd(
    complexAdd(
      complexAdd({re: coeffs.b0, im: 0}, complexScale(z1, coeffs.b1)),
      complexScale(z2, coeffs.b2),
    ),
    complexScale(z3, coeffs.b3),
  );
  const den = complexAdd(
    complexAdd(
      complexAdd({re: 1, im: 0}, complexScale(z1, coeffs.a1)),
      complexScale(z2, coeffs.a2),
    ),
    complexScale(z3, coeffs.a3),
  );
  const denMagSq = den.re * den.re + den.im * den.im;
  return {
    re: (num.re * den.re + num.im * den.im) / denMagSq,
    im: (num.im * den.re - num.re * den.im) / denMagSq,
  };
}

function normalizeBiquad(b0, b1, b2, a0, a1, a2) {
  return {
    b0: b0 / a0,
    b1: b1 / a0,
    b2: b2 / a0,
    a1: a1 / a0,
    a2: a2 / a0,
  };
}

function makePeakingEq(sampleRate, frequencyHz, q, gainDb) {
  const frequency = clamp(frequencyHz, 10, 0.45 * sampleRate);
  const qq = Math.max(0.1, q);
  const a = 10 ** (gainDb / 40);
  const w0 = 2 * Math.PI * frequency / sampleRate;
  const alpha = Math.sin(w0) / (2 * qq);
  const cosW0 = Math.cos(w0);
  return normalizeBiquad(
    1 + alpha * a,
    -2 * cosW0,
    1 - alpha * a,
    1 + alpha / a,
    -2 * cosW0,
    1 - alpha / a,
  );
}

function makeLowShelf(sampleRate, frequencyHz, slope, gainDb) {
  const frequency = clamp(frequencyHz, 10, 0.45 * sampleRate);
  const s = Math.max(0.1, slope);
  const a = 10 ** (gainDb / 40);
  const w0 = 2 * Math.PI * frequency / sampleRate;
  const cosW0 = Math.cos(w0);
  const sinW0 = Math.sin(w0);
  const alpha = sinW0 / 2 * Math.sqrt((a + 1 / a) * (1 / s - 1) + 2);
  const beta = 2 * Math.sqrt(a) * alpha;
  return normalizeBiquad(
    a * ((a + 1) - (a - 1) * cosW0 + beta),
    2 * a * ((a - 1) - (a + 1) * cosW0),
    a * ((a + 1) - (a - 1) * cosW0 - beta),
    (a + 1) + (a - 1) * cosW0 + beta,
    -2 * ((a - 1) + (a + 1) * cosW0),
    (a + 1) + (a - 1) * cosW0 - beta,
  );
}

function makeHighShelf(sampleRate, frequencyHz, slope, gainDb) {
  const frequency = clamp(frequencyHz, 10, 0.45 * sampleRate);
  const s = Math.max(0.1, slope);
  const a = 10 ** (gainDb / 40);
  const w0 = 2 * Math.PI * frequency / sampleRate;
  const cosW0 = Math.cos(w0);
  const sinW0 = Math.sin(w0);
  const alpha = sinW0 / 2 * Math.sqrt((a + 1 / a) * (1 / s - 1) + 2);
  const beta = 2 * Math.sqrt(a) * alpha;
  return normalizeBiquad(
    a * ((a + 1) + (a - 1) * cosW0 + beta),
    -2 * a * ((a - 1) + (a + 1) * cosW0),
    a * ((a + 1) + (a - 1) * cosW0 - beta),
    (a + 1) - (a - 1) * cosW0 + beta,
    2 * ((a - 1) - (a + 1) * cosW0),
    (a + 1) - (a - 1) * cosW0 - beta,
  );
}

function currentResponseDbAt(frequencyHz, controls) {
  const sampleRate = 48000;
  const preamp = $("preamp")?.value || "";
  const inputHpfHz = Number(currentControlSchema.input_hpf_hz || 60);
  const inputHpf = onePoleHpfResponse(sampleRate, inputHpfHz, frequencyHz);
  const presence = biquadResponse(
    makeHighShelf(sampleRate, 3200, 0.80, controlToDb(controls.presence, 4.5)),
    frequencyHz,
    sampleRate,
  );

  if (preamp === "mesa_boogie_mark_iic_plus") {
    const definition = getReferenceTonestackDefinition();
    if (definition) {
      let total = inputHpf;
      total = complexMul(
        total,
        order3Response(
          bilinearTransformOrder3(
            bassmanReferenceCoefficients(definition, controls),
            sampleRate,
          ),
          frequencyHz,
          sampleRate,
        ),
      );
      total = complexMul(total, presence);
      return 20 * Math.log10(Math.max(1e-6, complexMagnitude(total)));
    }
  }

  const bass = biquadResponse(
    makeLowShelf(sampleRate, 110, 0.70, controlToDb(controls.bass, 7.0)),
    frequencyHz,
    sampleRate,
  );
  const fixedScoop = biquadResponse(
    makePeakingEq(sampleRate, 750, 0.75, -5.0),
    frequencyHz,
    sampleRate,
  );
  const mid = biquadResponse(
    makePeakingEq(sampleRate, 750, 0.90, controlToDb(controls.mid, 9.0)),
    frequencyHz,
    sampleRate,
  );
  const treble = biquadResponse(
    makeHighShelf(sampleRate, 2200, 0.75, controlToDb(controls.treble, 7.0)),
    frequencyHz,
    sampleRate,
  );

  let total = inputHpf;
  total = complexMul(total, bass);
  total = complexMul(total, fixedScoop);
  total = complexMul(total, mid);
  total = complexMul(total, treble);
  total = complexMul(total, presence);
  total = complexScale(total, 10 ** (-4.8 / 20));
  return 20 * Math.log10(Math.max(1e-6, complexMagnitude(total)));
}

function buildToneResponseCurve(controls) {
  const points = [];
  const minFreq = 20;
  const maxFreq = 20000;
  const steps = 160;
  for (let i = 0; i < steps; i += 1) {
    const t = i / (steps - 1);
    const freq = minFreq * (maxFreq / minFreq) ** t;
    points.push({freq, db: currentResponseDbAt(freq, controls)});
  }
  return points;
}

function buildReferenceToneResponseCurve(controls) {
  const definition = getReferenceTonestackDefinition();
  if (!definition) {
    return null;
  }

  const points = [];
  const minFreq = 10;
  const maxFreq = 20000;
  const steps = 180;
  for (let i = 0; i < steps; i += 1) {
    const t = i / (steps - 1);
    const freq = minFreq * (maxFreq / minFreq) ** t;
    const db = referenceResponseDbAt(freq, controls);
    points.push({freq, db});
  }
  return {name: definition.name, points};
}

function valueOrDefault(id, fallback) {
  const el = $(id);
  return el ? Number(el.value) : fallback;
}

function drawTonePlot() {
  const svg = $("tone_plot");
  if (!svg) return;

  const width = 760;
  const height = 280;
  const margin = {top: 18, right: 18, bottom: 28, left: 44};
  const innerWidth = width - margin.left - margin.right;
  const innerHeight = height - margin.top - margin.bottom;
  const dbMin = -30;
  const dbMax = 12;
  const freqMin = 10;
  const freqMax = 20000;

  const currentCurve = buildToneResponseCurve({
    bass: valueOrDefault("bass", 5),
    mid: valueOrDefault("mid", 5),
    treble: valueOrDefault("treble", 5),
    presence: valueOrDefault("presence", 5),
  });
  const referenceDefinition = getReferenceTonestackDefinition();
  const referenceCurve = buildReferenceToneResponseCurve({
    bass: valueOrDefault("bass", 5),
    mid: valueOrDefault("mid", 5),
    treble: valueOrDefault("treble", 5),
  });
  const referenceNoonCurve = buildReferenceToneResponseCurve({
    bass: 5,
    mid: 5,
    treble: 5,
  });

  const xForFreq = (freq) => {
    const t = Math.log(freq / freqMin) / Math.log(freqMax / freqMin);
    return margin.left + t * innerWidth;
  };
  const yForDb = (db) => {
    const t = (clamp(db, dbMin, dbMax) - dbMin) / (dbMax - dbMin);
    return margin.top + (1 - t) * innerHeight;
  };
  const pathForCurve = (curve) => curve.map((point, index) => {
    const x = xForFreq(point.freq).toFixed(2);
    const y = yForDb(point.db).toFixed(2);
    return `${index === 0 ? "M" : "L"} ${x} ${y}`;
  }).join(" ");

  const freqLines = [10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
  const dbLines = [-30, -24, -18, -12, -6, 0, 6, 12];

  let markup = "";
  for (const db of dbLines) {
    const y = yForDb(db);
    markup += `<line x1="${margin.left}" y1="${y}" x2="${width - margin.right}" y2="${y}" stroke="rgba(31,23,19,0.14)" stroke-width="1" />`;
    markup += `<text x="${margin.left - 8}" y="${y + 4}" text-anchor="end" font-size="11" fill="rgba(31,23,19,0.72)">${db > 0 ? "+" : ""}${db} dB</text>`;
  }

  for (const freq of freqLines) {
    const x = xForFreq(freq);
    markup += `<line x1="${x}" y1="${margin.top}" x2="${x}" y2="${height - margin.bottom}" stroke="rgba(31,23,19,0.10)" stroke-width="1" />`;
    const label = freq >= 1000 ? `${freq / 1000}k` : `${freq}`;
    markup += `<text x="${x}" y="${height - 8}" text-anchor="middle" font-size="11" fill="rgba(31,23,19,0.72)">${label}</text>`;
  }

  markup += `<rect x="${margin.left}" y="${margin.top}" width="${innerWidth}" height="${innerHeight}" fill="none" stroke="rgba(31,23,19,0.26)" stroke-width="1.2" />`;
  if (referenceCurve) {
    markup += `<path d="${pathForCurve(referenceCurve.points)}" fill="none" stroke="#356e9f" stroke-width="2.5" />`;
  }
  if (referenceNoonCurve) {
    markup += `<path d="${pathForCurve(referenceNoonCurve.points)}" fill="none" stroke="rgba(31,23,19,0.45)" stroke-width="2" stroke-dasharray="5 5" />`;
  }
  markup += `<path d="${pathForCurve(currentCurve)}" fill="none" stroke="#c56c24" stroke-width="3" />`;

  const note = $("tone_plot_note");
  if (note) {
    const bassLabel = $("tone_bass_label")?.textContent || "Bass";
    const midLabel = $("tone_mid_label")?.textContent || "Mid";
    const trebleLabel = $("tone_treble_label")?.textContent || "Treble";
    const presenceLabel = $("tone_presence_label")?.textContent || "Presence";
    if (referenceDefinition) {
      note.textContent =
        `Orange shows AmpStageStudio's current DSP. Blue shows a ${referenceDefinition.name} curve using YATSC-derived component values at the same knob positions, with the dashed line at noon.`;
    } else {
      note.textContent =
        `Approximate response of AmpStageStudio's modeled ${bassLabel}/${midLabel}/${trebleLabel}/${presenceLabel} controls. No YATSC reference family is assigned for this preamp yet.`;
    }
  }

  svg.innerHTML = markup;
}

function parseBoolish(value) {
  return value === true || value === "1" || value === "true" || value === "on";
}

function hydrateLegacyEffectState(data) {
  const hydrated = {...data};
  const hasExplicitChain = checkboxKeys.some((key) => hydrated[key] !== undefined);
  if (hasExplicitChain) {
    return hydrated;
  }

  const legacyEffect = hydrated.effect || "none";
  const enableMap = {
    compression: "effect_compression_enabled",
    klon: "effect_klon_enabled",
    tubescreamer: "effect_tubescreamer_enabled",
    rat: "effect_rat_enabled",
    chorus: "effect_chorus_enabled",
    plate: "effect_plate_enabled",
  };
  const enabledKey = enableMap[legacyEffect];
  if (enabledKey) {
    hydrated[enabledKey] = "1";
  }

  if (hydrated.effect_drive !== undefined) {
    if (legacyEffect === "compression") hydrated.compressor_sustain = hydrated.effect_drive;
    if (legacyEffect === "klon") hydrated.klon_drive = hydrated.effect_drive;
    if (legacyEffect === "tubescreamer") hydrated.tubescreamer_drive = hydrated.effect_drive;
    if (legacyEffect === "rat") hydrated.rat_distortion = hydrated.effect_drive;
    if (legacyEffect === "chorus") hydrated.chorus_depth = hydrated.effect_drive;
    if (legacyEffect === "plate") hydrated.plate_mix = hydrated.effect_drive;
  }
  if (hydrated.effect_tone !== undefined) {
    if (legacyEffect === "compression") hydrated.compressor_attack = hydrated.effect_tone;
    if (legacyEffect === "klon") hydrated.klon_tone = hydrated.effect_tone;
    if (legacyEffect === "tubescreamer") hydrated.tubescreamer_tone = hydrated.effect_tone;
    if (legacyEffect === "rat") hydrated.rat_filter = hydrated.effect_tone;
    if (legacyEffect === "chorus") hydrated.chorus_tone = hydrated.effect_tone;
    if (legacyEffect === "plate") hydrated.plate_brightness = hydrated.effect_tone;
  }
  if (hydrated.effect_level_db !== undefined) {
    if (legacyEffect === "compression") hydrated.compressor_level_db = hydrated.effect_level_db;
    if (legacyEffect === "klon") hydrated.klon_level_db = hydrated.effect_level_db;
    if (legacyEffect === "tubescreamer") hydrated.tubescreamer_level_db = hydrated.effect_level_db;
    if (legacyEffect === "rat") hydrated.rat_level_db = hydrated.effect_level_db;
    if (legacyEffect === "chorus") hydrated.chorus_level_db = hydrated.effect_level_db;
    if (legacyEffect === "plate") hydrated.plate_level_db = hydrated.effect_level_db;
  }
  if (hydrated.effect_clean_blend !== undefined) {
    if (legacyEffect === "compression") hydrated.compressor_blend = hydrated.effect_clean_blend;
    if (legacyEffect === "klon") hydrated.klon_clean_blend = hydrated.effect_clean_blend;
    if (legacyEffect === "chorus") hydrated.chorus_mix = hydrated.effect_clean_blend;
    if (legacyEffect === "plate") hydrated.plate_decay = hydrated.effect_clean_blend;
  }
  return hydrated;
}

function refreshEffectBlocks() {
  for (const [toggleId, blockId] of Object.entries(effectBlockIds)) {
    const enabled = parseBoolish($(toggleId)?.checked ? "1" : "0");
    const block = $(blockId);
    if (!block) continue;
    block.classList.toggle("effect-block-disabled", !enabled);
    const controls = block.querySelectorAll("input[type='range']");
    for (const control of controls) {
      control.disabled = !enabled;
    }
  }
}

const slowFetchThresholdMs = 400;

async function fetchJson(url, options, label = url) {
  const start = performance.now();
  const response = await fetch(url, options);
  const elapsedMs = performance.now() - start;
  if (elapsedMs >= slowFetchThresholdMs) {
    console.warn(`[slow fetch] ${label} took ${Math.round(elapsedMs)} ms`);
  }
  if (!response.ok) throw new Error(`Request failed: ${response.status}`);
  return response.json();
}

function syncOutput(id, value) {
  const output = $(`${id}_value`);
  if (output) output.value = value;
}

async function refreshControlSchema() {
  const schema = await fetchJson("/api/control-schema", undefined, "control-schema");
  currentControlSchema = {
    ...currentControlSchema,
    ...schema,
  };
  $("preamp_panel_title").textContent = schema.panel_title || "Preamp";
  $("preamp_drive_label").textContent = schema.drive_label || "Drive";
  $("preamp_level_label").textContent = schema.level_label || "Level";
  $("preamp_bright_label").textContent = schema.bright_label || "Bright";
  $("preamp_bias_label").textContent = schema.bias_label || "Bias";
  const toneFields = [
    ["bass", schema.bass_label],
    ["mid", schema.mid_label],
    ["treble", schema.treble_label],
    ["presence", schema.presence_label],
  ];
  for (const [key, label] of toneFields) {
    const row = $(`tone_${key}_row`);
    const labelEl = $(`tone_${key}_label`);
    if (!row || !labelEl) continue;
    const visible = Boolean(label);
    row.hidden = !visible;
    if (visible) labelEl.textContent = label;
  }
  $("preamp_schema_note").textContent =
    schema.note || "These labels reflect the current modeled controls.";
  drawTonePlot();
}

function readStateFromInputs() {
  const payload = {};
  for (const key of valueKeys) {
    const el = $(key);
    if (!el) continue;
    payload[key] = el.value;
  }
  for (const key of checkboxKeys) {
    const el = $(key);
    if (!el) continue;
    payload[key] = el.checked ? "1" : "0";
  }
  const inputSelect = $("input_device_select");
  const outputSelect = $("output_device_select");
  if (audioBackend === "alsa") {
    payload.alsa_input = inputSelect ? inputSelect.value : "";
    payload.alsa_output = outputSelect ? outputSelect.value : "";
    payload.input_device = "";
    payload.output_device = "";
  } else {
    payload.input_device = inputSelect ? inputSelect.value : "";
    payload.output_device = outputSelect ? outputSelect.value : "";
    payload.alsa_input = "";
    payload.alsa_output = "";
  }
  return payload;
}

function applyState(data) {
  const hydrated = hydrateLegacyEffectState(data);
  for (const key of valueKeys) {
    const el = $(key);
    if (!el || hydrated[key] === undefined) continue;
    el.value = hydrated[key];
    syncOutput(key, hydrated[key]);
  }
  for (const key of checkboxKeys) {
    const el = $(key);
    if (!el || hydrated[key] === undefined) continue;
    el.checked = parseBoolish(hydrated[key]);
  }
  const inputSelect = $("input_device_select");
  const outputSelect = $("output_device_select");
  if (audioBackend === "alsa") {
    if (inputSelect && hydrated.alsa_input !== undefined) inputSelect.value = hydrated.alsa_input;
    if (outputSelect && hydrated.alsa_output !== undefined) outputSelect.value = hydrated.alsa_output;
  } else {
    if (inputSelect && hydrated.input_device !== undefined) inputSelect.value = hydrated.input_device;
    if (outputSelect && hydrated.output_device !== undefined) outputSelect.value = hydrated.output_device;
  }
  refreshEffectBlocks();
  drawTonePlot();
  $("status").textContent = `Connected to ${hydrated._control_file || "control file"}`;
}

async function saveState() {
  const payload = readStateFromInputs();
  const data = await fetchJson("/api/state", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify(payload),
  }, "save-state");
  applyState(data);
  await refreshControlSchema();
}

function scheduleSave() {
  if (saveTimer) clearTimeout(saveTimer);
  $("status").textContent = "Saving…";
  saveTimer = setTimeout(async () => {
    try {
      await saveState();
    } catch (error) {
      $("status").textContent = `Save failed: ${error.message}`;
    }
  }, 120);
}

async function init() {
  try {
    const [ampsData, preampsData, powerTubesData, audioDevicesData, stateData] = await Promise.all([
      fetchJson("/api/amps", undefined, "amps"),
      fetchJson("/api/preamps", undefined, "preamps"),
      fetchJson("/api/power-tubes", undefined, "power-tubes"),
      fetchJson("/api/audio-devices", undefined, "audio-devices"),
      fetchJson("/api/state", undefined, "state"),
    ]);

    $("amp").replaceChildren();
    $("preamp").replaceChildren();
    $("power_tube_type").replaceChildren();
    $("input_device_select").replaceChildren();
    $("output_device_select").replaceChildren();

    audioBackend = audioDevicesData.backend || "portaudio";
    $("input_device_label").textContent = audioBackend === "alsa" ? "ALSA Input" : "Input Device";
    $("output_device_label").textContent = audioBackend === "alsa" ? "ALSA Output" : "Output Device";

    for (const id of ["input_device_select", "output_device_select"]) {
      const option = document.createElement("option");
      option.value = "";
      option.textContent = "(use current CLI/default)";
      $(id).appendChild(option);
    }

    for (const amp of ampsData.amps) {
      const option = document.createElement("option");
      option.value = amp;
      option.textContent = amp;
      $("amp").appendChild(option);
    }

    for (const preamp of preampsData.preamps) {
      const option = document.createElement("option");
      option.value = preamp;
      option.textContent = preamp;
      $("preamp").appendChild(option);
    }

    for (const tube of powerTubesData.power_tubes) {
      const option = document.createElement("option");
      option.value = tube;
      option.textContent = tube;
      $("power_tube_type").appendChild(option);
    }

    for (const device of audioDevicesData.input_devices || []) {
      const option = document.createElement("option");
      option.value = device.value;
      option.textContent = device.label;
      $("input_device_select").appendChild(option);
    }

    for (const device of audioDevicesData.output_devices || []) {
      const option = document.createElement("option");
      option.value = device.value;
      option.textContent = device.label;
      $("output_device_select").appendChild(option);
    }

    applyState(stateData);
    await refreshControlSchema();
    refreshEffectBlocks();
    drawTonePlot();
    for (const key of stateKeys) {
      const el = $(key);
      if (!el) continue;
      el.addEventListener("input", () => {
        if (valueKeys.includes(key)) {
          syncOutput(key, el.value);
        }
        if (checkboxKeys.includes(key)) {
          refreshEffectBlocks();
        }
        if (["bass", "mid", "treble", "presence", "amp", "preamp"].includes(key)) {
          drawTonePlot();
        }
        scheduleSave();
      });
      el.addEventListener("change", () => {
        if (checkboxKeys.includes(key)) {
          refreshEffectBlocks();
        }
        if (["bass", "mid", "treble", "presence", "amp", "preamp"].includes(key)) {
          drawTonePlot();
        }
        scheduleSave();
      });
    }
    $("input_device_select").addEventListener("change", scheduleSave);
    $("output_device_select").addEventListener("change", scheduleSave);
  } catch (error) {
    $("status").textContent = `Failed to load UI: ${error.message}`;
  }
}

init();

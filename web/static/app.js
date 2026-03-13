const stateKeys = [
  "amp",
  "preamp",
  "power_tube_type",
  "effect",
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
  "effect_drive",
  "effect_tone",
  "effect_level_db",
  "effect_clean_blend",
];

let saveTimer = null;
let audioBackend = "portaudio";

function $(id) {
  return document.getElementById(id);
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function controlToMix(value, maxMix) {
  const normalized = clamp(Number(value || 5), 0, 10);
  return ((normalized - 5) / 5) * maxMix;
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

function responseDbAt(frequencyHz, controls) {
  const sampleRate = 48000;
  const bassMix = controlToMix(controls.bass, 0.55);
  const midMix = controlToMix(controls.mid, 0.45);
  const trebleMix = controlToMix(controls.treble, 0.55);
  const presenceMix = controlToMix(controls.presence, 0.35);

  const bass = onePoleLpfResponse(sampleRate, 180, frequencyHz);
  const mid = complexMul(
    onePoleHpfResponse(sampleRate, 350, frequencyHz),
    onePoleLpfResponse(sampleRate, 1400, frequencyHz),
  );
  const treble = onePoleHpfResponse(sampleRate, 1800, frequencyHz);
  const presence = onePoleHpfResponse(sampleRate, 2800, frequencyHz);

  let pre = {re: 1, im: 0};
  pre = complexAdd(pre, complexScale(bass, bassMix));
  pre = complexAdd(pre, complexScale(mid, midMix));
  pre = complexAdd(pre, complexScale(treble, trebleMix));
  const toneActivity = Math.abs(bassMix) + Math.abs(midMix) + Math.abs(trebleMix);
  pre = complexScale(pre, 1 / (1 + 0.18 * toneActivity));

  let post = complexAdd({re: 1, im: 0}, complexScale(presence, presenceMix));
  post = complexScale(post, 1 / (1 + 0.15 * Math.abs(presenceMix)));

  const total = complexMul(pre, post);
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
    points.push({freq, db: responseDbAt(freq, controls)});
  }
  return points;
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
  const dbMin = -18;
  const dbMax = 12;
  const freqMin = 20;
  const freqMax = 20000;

  const currentCurve = buildToneResponseCurve({
    bass: valueOrDefault("bass", 5),
    mid: valueOrDefault("mid", 5),
    treble: valueOrDefault("treble", 5),
    presence: valueOrDefault("presence", 5),
  });
  const referenceCurve = buildToneResponseCurve({
    bass: 5,
    mid: 5,
    treble: 5,
    presence: 5,
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

  const freqLines = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000];
  const dbLines = [-18, -12, -6, 0, 6, 12];

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
  markup += `<path d="${pathForCurve(referenceCurve)}" fill="none" stroke="rgba(31,23,19,0.45)" stroke-width="2" stroke-dasharray="5 5" />`;
  markup += `<path d="${pathForCurve(currentCurve)}" fill="none" stroke="#c56c24" stroke-width="3" />`;

  const note = $("tone_plot_note");
  if (note) {
    const bassLabel = $("tone_bass_label")?.textContent || "Bass";
    const midLabel = $("tone_mid_label")?.textContent || "Mid";
    const trebleLabel = $("tone_treble_label")?.textContent || "Treble";
    const presenceLabel = $("tone_presence_label")?.textContent || "Presence";
    note.textContent =
      `Approximate response of AmpStageStudio's modeled ${bassLabel}/${midLabel}/${trebleLabel}/${presenceLabel} controls. Useful for comparison, but not a schematic solver.`;
  }

  svg.innerHTML = markup;
}

function refreshEffectSchema() {
  const effect = $("effect")?.value || "none";
  const config = {
    none: {
      drive: "Drive",
      tone: "Tone",
      level: "Level",
      cleanBlend: "Clean Blend",
      showCleanBlend: false,
    },
    klon: {
      drive: "Drive",
      tone: "Tone",
      level: "Level",
      cleanBlend: "Clean Blend",
      showCleanBlend: true,
    },
    tubescreamer: {
      drive: "Drive",
      tone: "Tone",
      level: "Level",
      cleanBlend: "Clean Blend",
      showCleanBlend: false,
    },
    plate: {
      drive: "Mix",
      tone: "Brightness",
      level: "Level",
      cleanBlend: "Decay",
      showCleanBlend: true,
    },
  }[effect] || {
    drive: "Drive",
    tone: "Tone",
    level: "Level",
    cleanBlend: "Clean Blend",
    showCleanBlend: true,
  };

  $("effect_drive_label").textContent = config.drive;
  $("effect_tone_label").textContent = config.tone;
  $("effect_level_label").textContent = config.level;
  $("effect_clean_blend_label").textContent = config.cleanBlend;
  $("effect_clean_blend_row").hidden = !config.showCleanBlend;
}

async function fetchJson(url, options) {
  const response = await fetch(url, options);
  if (!response.ok) throw new Error(`Request failed: ${response.status}`);
  return response.json();
}

function syncOutput(id, value) {
  const output = $(`${id}_value`);
  if (output) output.value = value;
}

async function refreshControlSchema() {
  const schema = await fetchJson("/api/control-schema");
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
  for (const key of stateKeys) {
    const el = $(key);
    if (!el) continue;
    payload[key] = el.value;
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
  for (const key of stateKeys) {
    const el = $(key);
    if (!el || data[key] === undefined) continue;
    el.value = data[key];
    syncOutput(key, data[key]);
  }
  const inputSelect = $("input_device_select");
  const outputSelect = $("output_device_select");
  if (audioBackend === "alsa") {
    if (inputSelect && data.alsa_input !== undefined) inputSelect.value = data.alsa_input;
    if (outputSelect && data.alsa_output !== undefined) outputSelect.value = data.alsa_output;
  } else {
    if (inputSelect && data.input_device !== undefined) inputSelect.value = data.input_device;
    if (outputSelect && data.output_device !== undefined) outputSelect.value = data.output_device;
  }
  refreshEffectSchema();
  drawTonePlot();
  $("status").textContent = `Connected to ${data._control_file || "control file"}`;
}

async function saveState() {
  const payload = readStateFromInputs();
  const data = await fetchJson("/api/state", {
    method: "POST",
    headers: {"Content-Type": "application/json"},
    body: JSON.stringify(payload),
  });
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
      fetchJson("/api/amps"),
      fetchJson("/api/preamps"),
      fetchJson("/api/power-tubes"),
      fetchJson("/api/audio-devices"),
      fetchJson("/api/state"),
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
    refreshEffectSchema();
    drawTonePlot();
    for (const key of stateKeys) {
      const el = $(key);
      if (!el) continue;
      el.addEventListener("input", () => {
        syncOutput(key, el.value);
        if (key === "effect") refreshEffectSchema();
        if (["bass", "mid", "treble", "presence", "amp", "preamp"].includes(key)) {
          drawTonePlot();
        }
        scheduleSave();
      });
      el.addEventListener("change", () => {
        if (key === "effect") refreshEffectSchema();
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

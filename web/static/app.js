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
    for (const key of stateKeys) {
      const el = $(key);
      if (!el) continue;
      el.addEventListener("input", () => {
        syncOutput(key, el.value);
        scheduleSave();
      });
      el.addEventListener("change", scheduleSave);
    }
    $("input_device_select").addEventListener("change", scheduleSave);
    $("output_device_select").addEventListener("change", scheduleSave);
  } catch (error) {
    $("status").textContent = `Failed to load UI: ${error.message}`;
  }
}

init();

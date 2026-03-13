const stateKeys = [
  "amp",
  "preamp",
  "input_device",
  "output_device",
  "power_tube_type",
  "effect",
  "drive_db",
  "level_db",
  "bright_db",
  "bias_trim",
  "power_drive_db",
  "power_level_db",
  "power_bias_trim",
  "effect_drive",
  "effect_tone",
  "effect_level_db",
  "effect_clean_blend",
];

let saveTimer = null;

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

function readStateFromInputs() {
  const payload = {};
  for (const key of stateKeys) {
    const el = $(key);
    if (!el) continue;
    payload[key] = el.value;
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
    $("input_device").replaceChildren();
    $("output_device").replaceChildren();

    for (const id of ["input_device", "output_device"]) {
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

    for (const name of audioDevicesData.input_devices || []) {
      const option = document.createElement("option");
      option.value = name;
      option.textContent = name;
      $("input_device").appendChild(option);
    }

    for (const name of audioDevicesData.output_devices || []) {
      const option = document.createElement("option");
      option.value = name;
      option.textContent = name;
      $("output_device").appendChild(option);
    }

    applyState(stateData);
    for (const key of stateKeys) {
      const el = $(key);
      if (!el) continue;
      el.addEventListener("input", () => {
        syncOutput(key, el.value);
        scheduleSave();
      });
      el.addEventListener("change", scheduleSave);
    }
  } catch (error) {
    $("status").textContent = `Failed to load UI: ${error.message}`;
  }
}

init();

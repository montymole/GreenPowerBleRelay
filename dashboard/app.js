const API_PREFIX = "/api/v1";
const DEFAULT_RELAY_URL = "http://192.168.10.130";
const baseUrlInput = document.querySelector("#base-url");
const apiKeyInput = document.querySelector("#api-key");
const toast = document.querySelector("#toast");
let scanPollTimer = 0;
let toastTimer = 0;

baseUrlInput.value = localStorage.getItem("relayBaseUrl") || DEFAULT_RELAY_URL;
apiKeyInput.value = sessionStorage.getItem("relayApiKey") || "";

function relayBase() {
  const value = baseUrlInput.value.trim().replace(/\/+$/, "");
  if (!value) throw new Error("Enter the relay URL.");
  const parsed = new URL(value);
  if (!["http:", "https:"].includes(parsed.protocol)) {
    throw new Error("Relay URL must use http or https.");
  }
  return value;
}

function showToast(message, isError = false) {
  toast.textContent = message;
  toast.style.borderColor = isError ? "#88444a" : "";
  toast.classList.add("visible");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove("visible"), 3300);
}

async function request(path, { method = "GET", body, auth = false } = {}) {
  const headers = { Accept: "application/json" };
  if (body !== undefined) headers["Content-Type"] = "application/json";
  if (auth && apiKeyInput.value.trim()) headers["X-Api-Key"] = apiKeyInput.value.trim();
  const response = await fetch(`${relayBase()}${path}`, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
    cache: "no-store",
  });
  const raw = await response.text();
  let data;
  try { data = raw ? JSON.parse(raw) : null; } catch { data = raw; }
  return { status: response.status, ok: response.ok, data };
}

function format(value) {
  return typeof value === "string" ? value : JSON.stringify(value, null, 2);
}

function setOutput(selector, result) {
  const element = document.querySelector(selector);
  element.textContent = `HTTP ${result.status}\n${format(result.data)}`;
  element.classList.toggle("muted", result.ok);
}

function escapeHtml(value) {
  return String(value ?? "").replace(/[&<>"']/g, character => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  })[character]);
}

function addressPath(address) {
  return encodeURIComponent(String(address).replaceAll(":", "").toLowerCase());
}

function formatUptime(ms) {
  const seconds = Math.floor(Number(ms || 0) / 1000);
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const rest = seconds % 60;
  return days ? `${days}d ${hours}h ${minutes}m` : `${hours}h ${minutes}m ${rest}s`;
}

function showHealth(status, result) {
  const healthy = Boolean(status.healthy);
  const connection = document.querySelector("#connection-state");
  connection.textContent = `${result.status} · ${healthy ? "Healthy" : "Not ready"}`;
  connection.className = `pill ${healthy ? "good" : "bad"}`;
  document.querySelector("#health-state").textContent = healthy ? "Healthy" : "Not ready";
  document.querySelector("#health-code").textContent = `HTTP ${result.status} · ${status.deviceCount ?? 0} configured device(s)`;
  document.querySelector("#wifi-state").textContent = status.wifiConnected ? "Connected" : "Disconnected";
  document.querySelector("#wifi-detail").textContent = status.wifiStatus || "—";
  document.querySelector("#ble-state").textContent = status.bleReady ? "Ready" : status.bleConnected ? "Connected" : "Not connected";
  document.querySelector("#ble-detail").textContent = status.deviceCount ? `${status.deviceCount} configured device(s)` : "No configured devices";
  document.querySelector("#uptime").textContent = formatUptime(status.uptimeMs);
  document.querySelector("#last-error").textContent = `Last error: ${status.lastError || "none"}`;
}

async function loadHealth() {
  try {
    const result = await request(`${API_PREFIX}/health`);
    if (result.data && typeof result.data === "object") showHealth(result.data, result);
    else throw new Error(`Unexpected health response (HTTP ${result.status}).`);
  } catch (error) {
    document.querySelector("#connection-state").textContent = "Unreachable";
    document.querySelector("#connection-state").className = "pill bad";
    document.querySelector("#health-state").textContent = "Offline";
    document.querySelector("#health-code").textContent = error.message;
  }
}

async function loadDevices() {
  const container = document.querySelector("#device-list");
  try {
    const result = await request(`${API_PREFIX}/devices`);
    if (!result.ok) throw new Error(format(result.data));
    const devices = result.data.devices || [];
    if (!devices.length) {
      container.className = "empty";
      container.textContent = "No devices configured. Discover one or add an address below.";
      return;
    }
    container.className = "";
    container.innerHTML = devices.map(device => {
      const id = escapeHtml(device.id);
      const address = escapeHtml(device.address);
      const name = escapeHtml(device.name || device.protocol);
      const state = device.protocol === "victron"
        ? `${device.productType || "Victron"} · ${device.decoded ? "decoded" : device.keyConfigured ? "waiting for valid data" : "key required"} · RSSI ${device.rssi ?? "—"}`
        : `GreenPower · ${device.ready ? "ready" : device.connected ? "connected" : "disconnected"}`;
      return `<article class="device-row">
        <div class="device-main"><strong>${name} <span class="pill neutral">${escapeHtml(device.protocol)}</span></strong>
          <small>${address} · ${escapeHtml(state)}</small></div>
        <div class="device-actions">
          <button class="button secondary" data-action="telemetry" data-id="${id}">Telemetry</button>
          ${device.protocol === "greenpower" ? `<button class="button secondary" data-action="reconnect-device" data-id="${id}">Reconnect</button>` : ""}
          <button class="button secondary" data-action="rename" data-id="${id}" data-name="${name}">Rename</button>
          <button class="button danger" data-action="delete-device" data-id="${id}">Delete</button>
        </div>
      </article>`;
    }).join("");
  } catch (error) {
    container.className = "empty";
    container.textContent = `Could not load devices: ${error.message}`;
  }
}

async function loadDiscovery() {
  const result = await request(`${API_PREFIX}/ble/discovery`);
  if (!result.ok) throw new Error(format(result.data));
  const { scanning, devices = [], advertisementCount = 0 } = result.data;
  const state = document.querySelector("#scan-state");
  state.innerHTML = `<span class="pulse ${scanning ? "active" : ""}"></span><span>${scanning ? "BLE scan running" : "Scan idle"} · ${advertisementCount} advertisement callback(s) · ${devices.length} unique device(s)</span>`;
  const container = document.querySelector("#discovery-list");
  if (!devices.length) {
    container.className = "empty";
    container.textContent = scanning
      ? "Listening for all BLE advertisements, including devices that are not GreenPower or Victron…"
      : "No BLE advertisements received. Confirm Bluetooth is enabled on the relay and nearby devices are advertising.";
  } else {
    container.className = "";
    container.innerHTML = devices.map(device => {
      const name = escapeHtml(device.name || device.protocol);
      const address = escapeHtml(device.address);
      const type = device.protocol === "victron"
        ? ` · ${escapeHtml(device.productName || device.productType || "Victron")}`
        : "";
      const diagnostics = `manufacturer data ${escapeHtml(device.manufacturerDataLength ?? 0)} B · GreenPower service ${device.greenPowerService ? "yes" : "no"}`;
      const manufacturerHex = device.manufacturerDataHex
        ? ` · raw ${escapeHtml(device.manufacturerDataHex)}${device.manufacturerDataTruncated ? "…" : ""}`
        : "";
      const configureButton = device.protocol === "victron" || device.protocol === "greenpower"
        ? `<button class="button primary" data-action="add-discovered" data-address="${address}" data-name="${name}" data-protocol="${escapeHtml(device.protocol)}">Configure</button>`
        : "";
      return `<article class="device-row">
        <div class="device-main"><strong>${name} <span class="pill neutral">${escapeHtml(device.protocol)}</span></strong>
          <small>${address} · RSSI ${escapeHtml(device.rssi)} dBm${type} · ${diagnostics}${manufacturerHex}</small></div>
        <div class="device-actions">${configureButton}</div>
      </article>`;
    }).join("");
  }
  if (scanning) {
    clearTimeout(scanPollTimer);
    scanPollTimer = setTimeout(() => loadDiscovery().catch(error => showToast(error.message, true)), 1000);
  }
  return result;
}

async function loadLogs() {
  const output = document.querySelector("#logs-output");
  try {
    const result = await request(`${API_PREFIX}/logs?limit=40`);
    if (!result.ok) throw new Error(format(result.data));
    const logs = result.data.logs || [];
    output.textContent = logs.length
      ? logs.map(entry => `[+${formatUptime(entry.uptimeMs)}] ${entry.message}`).join("\n")
      : "No firmware log entries yet.";
    output.classList.toggle("muted", logs.length === 0);
    output.scrollTop = output.scrollHeight;
  } catch (error) {
    output.textContent = `Could not load firmware logs: ${error.message}`;
    output.classList.add("muted");
  }
}

async function refreshAll() {
  await Promise.all([loadHealth(), loadDevices(), loadLogs()]);
  try { await loadDiscovery(); } catch (error) { showToast(`Discovery status: ${error.message}`, true); }
}

document.querySelector("#save-settings").addEventListener("click", () => {
  try {
    localStorage.setItem("relayBaseUrl", relayBase());
    sessionStorage.setItem("relayApiKey", apiKeyInput.value.trim());
    showToast("Relay URL saved; API key kept for this browser tab.");
    refreshAll();
  } catch (error) { showToast(error.message, true); }
});

document.querySelector("#refresh-all").addEventListener("click", refreshAll);
document.querySelector("#refresh-devices").addEventListener("click", loadDevices);
document.querySelector("#refresh-logs").addEventListener("click", loadLogs);
document.querySelector("#scan-button").addEventListener("click", async event => {
  const button = event.currentTarget;
  button.disabled = true;
  try {
    const duration = document.querySelector("#scan-duration").value;
    const result = await request(
      `${API_PREFIX}/ble/discovery?durationSeconds=${encodeURIComponent(duration)}`,
      { method: "POST" },
    );
    if (result.status !== 202) throw new Error(format(result.data));
    showToast(`${duration}-second BLE discovery scan started.`);
    await loadDiscovery();
  } catch (error) { showToast(`Could not start scan: ${error.message}`, true); }
  finally { button.disabled = false; }
});

document.querySelector("#discovery-list").addEventListener("click", async event => {
  const button = event.target.closest('[data-action="add-discovered"]');
  if (!button) return;
  const form = document.querySelector("#add-device-form");
  form.elements.address.value = button.dataset.address;
  form.elements.name.value = button.dataset.name === button.dataset.protocol ? "" : button.dataset.name;
  form.elements.protocol.value = button.dataset.protocol;
  form.elements.key.value = "";
  document.querySelector(".victron-key-field").hidden = button.dataset.protocol !== "victron";
  form.scrollIntoView({ behavior: "smooth", block: "center" });
  form.elements.name.focus();
});

document.querySelector("#add-device-form").elements.protocol.addEventListener("change", event => {
  document.querySelector(".victron-key-field").hidden = event.target.value !== "victron";
});

document.querySelector("#add-device-form").addEventListener("submit", async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const body = {
    address: form.elements.address.value.trim(),
    protocol: form.elements.protocol.value,
  };
  if (form.elements.name.value.trim()) body.name = form.elements.name.value.trim();
  if (form.elements.key.value.trim()) body.key = form.elements.key.value.trim();
  try {
    const result = await request(`${API_PREFIX}/devices`, { method: "POST", body, auth: true });
    if (result.status !== 201) throw new Error(format(result.data));
    form.reset();
    document.querySelector(".victron-key-field").hidden = false;
    showToast("Device added.");
    await refreshAll();
  } catch (error) { showToast(`Could not add device: ${error.message}`, true); }
});

document.querySelector("#device-list").addEventListener("click", async event => {
  const button = event.target.closest("[data-action]");
  if (!button) return;
  const { action, id } = button.dataset;
  try {
    if (action === "telemetry") {
      const result = await request(`${API_PREFIX}/devices/${encodeURIComponent(id)}/telemetry`);
      setOutput("#live-output", result);
      document.querySelector("#live-output").scrollIntoView({ behavior: "smooth", block: "center" });
      return;
    }
    if (action === "reconnect-device") {
      const result = await request(`${API_PREFIX}/devices/${encodeURIComponent(id)}/reconnect`, { method: "POST" });
      if (!result.ok) throw new Error(format(result.data));
      showToast("GreenPower reconnect completed.");
    } else if (action === "rename") {
      const name = prompt("New device name:", button.dataset.name || "");
      if (name === null) return;
      const result = await request(`${API_PREFIX}/devices/${encodeURIComponent(id)}`, {
        method: "PUT", body: { name }, auth: true,
      });
      if (!result.ok) throw new Error(format(result.data));
      showToast("Device renamed.");
    } else if (action === "delete-device") {
      if (!confirm(`Remove device ${id}?`)) return;
      const result = await request(`${API_PREFIX}/devices/${encodeURIComponent(id)}`, { method: "DELETE", auth: true });
      if (!result.ok) throw new Error(format(result.data));
      showToast("Device removed.");
    }
    await refreshAll();
  } catch (error) { showToast(`Device action failed: ${error.message}`, true); }
});

document.querySelector("#load-live").addEventListener("click", async () => {
  try { setOutput("#live-output", await request(`${API_PREFIX}/live`)); }
  catch (error) { showToast(error.message, true); }
});

document.querySelector("#register-read-form").addEventListener("submit", async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const query = new URLSearchParams({ start: form.elements.start.value, count: form.elements.count.value });
  try { setOutput("#register-output", await request(`${API_PREFIX}/registers?${query}`)); }
  catch (error) { showToast(error.message, true); }
});

document.querySelector("#register-write-form").addEventListener("submit", async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const address = encodeURIComponent(form.elements.address.value.trim());
  try {
    const result = await request(`${API_PREFIX}/registers/${address}`, {
      method: "PUT", body: { value: Number(form.elements.value.value) }, auth: true,
    });
    setOutput("#register-output", result);
  } catch (error) { showToast(error.message, true); }
});

document.querySelector("#config-form").addEventListener("submit", async event => {
  event.preventDefault();
  const action = event.submitter?.value || "read";
  const kind = event.currentTarget.elements.kind.value;
  try {
    let body;
    if (action === "write") {
      const input = document.querySelector("#config-body").value.trim();
      if (!input) throw new Error("Enter a JSON body for the configuration write.");
      body = JSON.parse(input);
    }
    const result = await request(`${API_PREFIX}/config/${kind}`, {
      method: action === "write" ? "PUT" : "GET", body, auth: action === "write",
    });
    setOutput("#config-output", result);
  } catch (error) { showToast(`Configuration request failed: ${error.message}`, true); }
});

document.querySelector("#api-form").addEventListener("submit", async event => {
  event.preventDefault();
  const form = event.currentTarget;
  const path = form.elements.path.value.trim();
  if (!path.startsWith("/api/v1/")) {
    showToast("Use an API path beginning with /api/v1/.", true);
    return;
  }
  let body;
  const bodyText = document.querySelector("#api-body").value.trim();
  if (bodyText) {
    try { body = JSON.parse(bodyText); }
    catch (error) { showToast(`Invalid JSON body: ${error.message}`, true); return; }
  }
  const method = form.elements.method.value;
  try {
    const result = await request(path, { method, body, auth: method !== "GET" });
    setOutput("#api-output", result);
  } catch (error) { setOutput("#api-output", { status: "network error", ok: false, data: error.message }); }
});

refreshAll();
setInterval(loadHealth, 3000);
setInterval(loadDevices, 5000);
setInterval(loadLogs, 3000);

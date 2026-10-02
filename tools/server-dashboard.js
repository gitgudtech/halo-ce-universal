"use strict";

const state = {
  connected: false,
  serverRunning: false,
  phase: "Connecting",
  map: "",
  gameType: "",
  buildNumber: null,
  playerCount: 0,
  machineCount: 0,
  hostAddress: "",
  players: [],
  playerStats: {},
  teamScores: [],
  statsUpdatedAt: "",
  statsError: "",
  updateStatus: "Not checked",
  updateAvailable: false,
  updateInstallAvailable: false,
  updateInstallInProgress: false,
  latestBuild: 0,
  updateReleaseUrl: "",
  queue: [],
  updatedAt: "--:--:--",
  events: []
};
const mapDescriptions = {
  bloodgulch: "Wide-open valley | vehicle routes",
  hangemhigh: "Open sightlines | elevated routes",
  damnation: "Vertical arena | narrow routes",
  ratrace: "Enclosed paths | close combat",
  prisoner: "Multi-level interior | tight routes",
  chillout: "Tight corridors | vertical paths",
  carousel: "Compact arena | central routes",
  boardingaction: "Split platforms | long-range lanes",
  wizard: "Compact arena | mirrored sides",
  putput: "Open course | vehicle friendly",
  longest: "Linear lanes | fast rotations",
  sidewinder: "Large terrain | vehicle routes",
  beavercreek: "Twin bases | central cave"
};
let lastSnapshot = null;
let lastConnectionError = "";
let refreshInProgress = false;
let statsRefreshInProgress = false;
let updateCheckInProgress = false;
let statsRefreshTimer;
let toastTimer;
let currentInviteUrl = "";
const statsRefreshControl = document.getElementById("statsRefreshInterval");
const statsRefreshStorageKey = "readyup.statsRefreshInterval";
const statsRefreshIntervals = new Set(["0", "5", "15", "30", "60"]);

try {
  const savedInterval = localStorage.getItem(statsRefreshStorageKey);
  if (statsRefreshIntervals.has(savedInterval)) statsRefreshControl.value = savedInterval;
} catch {}

const escapeHtml = value => String(value).replace(/[&<>"']/g, character => ({
  "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;"
}[character]));

async function requestJson(path, options) {
  const response = await fetch(path, { cache: "no-store", ...options });
  const payload = await response.json();
  if (!response.ok) {
    throw new Error(payload.error || payload.response || `HTTP ${response.status}`);
  }
  return payload;
}

function notify(message) {
  const toast = document.getElementById("toast");
  toast.textContent = message;
  toast.classList.add("visible");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toast.classList.remove("visible"), 2600);
}

function logEvent(message, tone = "") {
  const time = new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", hour12: false });
  state.events.unshift({ time, text: escapeHtml(message), tone });
  state.events = state.events.slice(0, 12);
  renderEvents();
}

function renderEvents() {
  const list = document.getElementById("eventList");
  list.replaceChildren();
  for (const event of state.events) {
    const item = document.createElement("li");
    item.className = "event";
    const time = document.createElement("span");
    time.className = "event-time";
    time.textContent = event.time;
    const dot = document.createElement("span");
    dot.className = `event-dot ${event.tone || ""}`;
    const text = document.createElement("span");
    text.className = "event-text";
    text.innerHTML = event.text;
    item.append(time, dot, text);
    list.append(item);
  }
}

function renderPlayers() {
  const body = document.getElementById("playersBody");
  body.replaceChildren();
  state.players.forEach(player => {
    const row = document.createElement("tr");
    const playerCell = document.createElement("td");
    const identity = document.createElement("div");
    identity.className = "player-cell";
    const badge = document.createElement("span");
    badge.className = "player-badge";
    badge.textContent = player.name.slice(0, 2).toUpperCase();
    const name = document.createElement("span");
    name.textContent = player.name;
    identity.append(badge, name);
    if (player.host) {
      const host = document.createElement("span");
      host.className = "host-pill";
      host.textContent = "HOST";
      identity.append(host);
    }
    playerCell.append(identity);
    row.append(playerCell);

    const stats = state.playerStats[player.id] || {};
    for (const value of [player.team, stats.score, stats.kills, stats.assists, stats.deaths, player.machine, player.controller]) {
      const cell = document.createElement("td");
      cell.className = "mono";
      cell.textContent = Number.isInteger(value) ? String(value) : value ?? "--";
      row.append(cell);
    }

    const action = document.createElement("td");
    if (!player.host) {
      const kick = document.createElement("button");
      kick.className = "button small danger";
      kick.type = "button";
      kick.textContent = "Kick";
      kick.setAttribute("aria-label", `Kick ${player.name}`);
      kick.addEventListener("click", () => runCommand(`kick ${player.id}`, `Kick requested for ${player.name}`, "red"));
      action.append(kick);
    }
    row.append(action);
    body.append(row);
  });

  const unlistedPlayers = Math.max(0, state.playerCount - state.players.length);
  if (unlistedPlayers) {
    const row = document.createElement("tr");
    const cell = document.createElement("td");
    cell.colSpan = 9;
    cell.className = "empty";
    cell.textContent = `${unlistedPlayers} additional player${unlistedPlayers === 1 ? "" : "s"} reported; roster details unavailable`;
    row.append(cell);
    body.append(row);
  } else if (!state.players.length) {
    const row = document.createElement("tr");
    const cell = document.createElement("td");
    cell.colSpan = 9;
    cell.className = "empty";
    cell.textContent = state.connected ? "No players connected" : "Waiting for server connection";
    row.append(cell);
    body.append(row);
  }
  document.getElementById("playerCount").textContent = state.playerCount;
  document.getElementById("machineCount").textContent = state.machineCount;
  renderStatsSummary();
}

function renderStatsSummary() {
  const teamScores = document.getElementById("teamScores");
  teamScores.replaceChildren();
  state.teamScores.forEach(team => {
    const item = document.createElement("span");
    item.className = "team-score";
    const name = document.createElement("span");
    name.textContent = team.team;
    const score = document.createElement("strong");
    score.textContent = team.score;
    item.append(name, score);
    teamScores.append(item);
  });
  document.getElementById("statsUpdatedAt").textContent = state.statsError ||
    (state.statsUpdatedAt ? `STATS ${state.statsUpdatedAt}` : "STATS NOT LOADED");
}

function scheduleStatsRefresh() {
  clearTimeout(statsRefreshTimer);
  const intervalSeconds = Number(statsRefreshControl.value);
  if (intervalSeconds > 0) {
    statsRefreshTimer = setTimeout(refreshStats, intervalSeconds * 1000);
  }
}

async function refreshStats() {
  if (statsRefreshInProgress) return;
  clearTimeout(statsRefreshTimer);
  statsRefreshInProgress = true;
  try {
    const snapshot = await requestJson("/api/stats");
    state.playerStats = Object.fromEntries(snapshot.players.map(player => [player.id, player]));
    state.teamScores = snapshot.teams;
    state.statsUpdatedAt = snapshot.updatedAt;
    state.statsError = "";
  } catch {
    state.statsError = "STATS UNAVAILABLE";
  } finally {
    statsRefreshInProgress = false;
    renderPlayers();
    scheduleStatsRefresh();
  }
}

async function checkForUpdates() {
  if (updateCheckInProgress) return;
  updateCheckInProgress = true;
  state.updateStatus = "Checking fork releases";
  render();
  try {
    const update = await requestJson("/api/update-check");
    state.updateAvailable = update.updateAvailable;
    state.updateInstallAvailable = update.installEnabled === true;
    state.latestBuild = update.latestBuild;
    state.updateReleaseUrl = update.releaseUrl || "";
    if (update.updateAvailable) {
      const current = update.currentBuild > 0 ? `current ${update.currentBuild}` : "current build unknown";
      state.updateStatus = `Build ${update.latestBuild} available (${current})`;
      logEvent(`Update available | build ${update.latestBuild}`, "amber");
    } else {
      state.updateStatus = `Up to date | build ${update.currentBuild}`;
      logEvent(`Server is up to date | build ${update.currentBuild}`);
    }
  } catch (error) {
    state.updateAvailable = false;
    state.updateInstallAvailable = false;
    state.latestBuild = 0;
    state.updateReleaseUrl = "";
    state.updateStatus = "Update check failed";
    logEvent(`Update check failed | ${error.message}`, "red");
  } finally {
    updateCheckInProgress = false;
    render();
  }
}

async function installUpdate() {
  if (!state.updateAvailable || !state.updateInstallAvailable || state.updateInstallInProgress) return;
  const confirmed = window.confirm(
    `Install build ${state.latestBuild} and restart the server? ` +
    `${state.playerCount} connected player(s) will be disconnected.`
  );
  if (!confirmed) return;

  state.updateInstallInProgress = true;
  state.updateStatus = `Installing build ${state.latestBuild}`;
  render();
  try {
    const result = await requestJson("/api/update", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ confirm: true })
    });
    state.updateAvailable = false;
    state.updateStatus = `Build ${result.installedBuild} installed; server restarted`;
    state.buildNumber = result.installedBuild;
    logEvent(`Server updated to build ${result.installedBuild}`, "amber");
    notify(`Build ${result.installedBuild} installed`);
    await refreshSnapshot();
  } catch (error) {
    state.updateStatus = `Update failed | ${error.message}`;
    logEvent(`Update failed | ${error.message}`, "red");
    notify(`Update failed: ${error.message}`);
  } finally {
    state.updateInstallInProgress = false;
    render();
  }
}

async function showInvite() {
  const button = document.getElementById("showInvite");
  const status = document.getElementById("inviteStatus");
  button.disabled = true;
  status.textContent = "Loading invite";
  try {
    const result = await requestJson("/api/invite");
    currentInviteUrl = result.url;
    const input = document.getElementById("inviteUrl");
    input.value = currentInviteUrl;
    input.hidden = false;
    button.textContent = "Refresh invite";
    document.getElementById("copyInvite").hidden = false;
    status.textContent = "Works while this host is running";
  } catch (error) {
    status.textContent = error.message;
  } finally {
    button.disabled = false;
  }
}

async function copyInvite() {
  if (!currentInviteUrl) return;
  try {
    await navigator.clipboard.writeText(currentInviteUrl);
    notify("Invite URL copied");
  } catch {
    const input = document.getElementById("inviteUrl");
    input.focus();
    input.select();
    notify("Select and copy the invite URL");
  }
}

function renderQueue() {
  const list = document.getElementById("queueList");
  list.replaceChildren();
  document.getElementById("queueCount").textContent = `${state.queue.length} QUEUED`;
  state.queue.forEach((entry, index) => {
    const item = document.createElement("li");
    item.className = "queue-item";
    const number = document.createElement("span");
    number.className = "queue-number";
    number.textContent = String(index + 1).padStart(2, "0");
    const details = document.createElement("span");
    const map = document.createElement("span");
    map.className = "queue-map";
    map.textContent = entry.map;
    const mode = document.createElement("span");
    mode.className = "queue-mode";
    mode.textContent = entry.gameType;
    details.append(map, mode);
    const stateLabel = document.createElement("span");
    stateLabel.className = "mono";
    stateLabel.textContent = "QUEUED";
    item.append(number, details, stateLabel);
    list.append(item);
  });
  if (!state.queue.length) {
    const empty = document.createElement("li");
    empty.className = "empty";
    empty.textContent = "Map queue is empty";
    list.append(empty);
  }
  document.getElementById("advanceMap").disabled = !state.connected || !state.serverRunning || !state.queue.length;
}

function render() {
  const serverValue = document.getElementById("serverValue");
  const serverMark = document.getElementById("serverMark");
  serverValue.textContent = state.connected && state.serverRunning ? "Running" : state.connected ? "Offline" : "Unavailable";
  serverMark.style.background = state.connected && state.serverRunning ? "var(--green)" : "var(--red)";
  document.getElementById("serverNote").textContent = state.connected ? "Live via local SSH tunnel" : "SSH bridge unavailable";
  document.getElementById("phaseValue").textContent = state.phase;
  document.getElementById("phaseMark").style.background = state.phase === "In Match" ? "var(--green)" : "var(--amber)";
  document.getElementById("phaseNote").textContent = state.gameType || "Waiting for host";
  document.getElementById("mapStatus").textContent = state.phase.toUpperCase();
  document.getElementById("mapName").textContent = state.map || "--";
  document.getElementById("mapArtName").textContent = state.map || "--";
  document.getElementById("mapDescription").textContent = mapDescriptions[state.mapAlias] || "Multiplayer arena";
  document.getElementById("gameType").textContent = state.gameType || "--";
  document.getElementById("actionState").textContent = state.connected ? `${state.phase.toUpperCase()} | LIVE` : "DISCONNECTED";
  document.getElementById("connectionBadge").textContent = state.connected ? "LIVE VIA SSH TUNNEL" : "CONNECTING TO HOST";
  document.getElementById("connectionDetail").textContent = state.connected ? "127.0.0.1 bridge" : "Local bridge unavailable";
  document.getElementById("buildNumber").textContent = state.buildNumber > 0
    ? `Build ${state.buildNumber}`
    : state.connected && state.serverRunning ? "Unversioned" : "--";
  document.getElementById("uptime").textContent = state.updatedAt;
  document.getElementById("serverAddress").textContent = state.hostAddress || "--";
  document.getElementById("serverAddressDetail").textContent = state.hostAddress || "--";
  document.getElementById("serverAddressDetail").textContent = "192.168.0.88";
  document.getElementById("startMatch").disabled = !state.connected || !state.serverRunning || state.phase === "In Match";
  document.getElementById("endMatch").disabled = !state.connected || !state.serverRunning || state.phase !== "In Match";
  document.getElementById("restartMatch").disabled = !state.connected || !state.serverRunning || !["In Match", "Postgame"].includes(state.phase);
  document.getElementById("checkUpdates").disabled = !state.connected || !state.serverRunning || updateCheckInProgress;
  document.getElementById("checkUpdates").textContent = updateCheckInProgress ? "Checking..." : "Check for updates";
  document.getElementById("updateStatus").textContent = state.updateStatus;
  const releaseLink = document.getElementById("updateReleaseLink");
  releaseLink.hidden = !state.updateAvailable || !state.updateReleaseUrl;
  releaseLink.href = state.updateReleaseUrl || "#";
  const applyUpdateButton = document.getElementById("applyUpdate");
  applyUpdateButton.hidden = !state.updateAvailable || !state.updateInstallAvailable;
  applyUpdateButton.disabled = !state.connected || !state.serverRunning || state.updateInstallInProgress;
  applyUpdateButton.textContent = state.updateInstallInProgress ? "Updating..." : "Update & restart";
  document.getElementById("queueForm").querySelector("button").disabled = !state.connected || !state.serverRunning;
  renderPlayers();
  renderQueue();
  renderEvents();
}

async function refreshSnapshot() {
  if (refreshInProgress) return;
  refreshInProgress = true;
  try {
    const snapshot = await requestJson("/api/snapshot");
    const changedMap = lastSnapshot && (lastSnapshot.map !== snapshot.map || lastSnapshot.gameType !== snapshot.gameType);
    const changedPhase = lastSnapshot && lastSnapshot.phase !== snapshot.phase;
    const changedCounts = lastSnapshot && (lastSnapshot.playerCount !== snapshot.playerCount || lastSnapshot.machineCount !== snapshot.machineCount);
    state.connected = true;
    state.serverRunning = snapshot.serverRunning;
    state.phase = snapshot.phase;
    state.map = snapshot.map;
    state.mapAlias = snapshot.mapAlias;
    state.gameType = snapshot.gameType;
    state.buildNumber = snapshot.buildNumber;
    state.playerCount = snapshot.playerCount;
    state.machineCount = snapshot.machineCount;
    state.hostAddress = snapshot.hostAddress;
    state.players = snapshot.players;
    state.queue = snapshot.queue;
    state.updatedAt = snapshot.updatedAt;
    lastConnectionError = "";
    if (changedMap) logEvent(`Rotation changed to ${snapshot.map} | ${snapshot.gameType}`);
    if (changedPhase) logEvent(`Server entered ${snapshot.phase.toLowerCase()}`, "amber");
    if (changedCounts) logEvent(`Roster updated | ${snapshot.playerCount} players on ${snapshot.machineCount} machines`);
    lastSnapshot = snapshot;
  } catch (error) {
    state.connected = false;
    state.serverRunning = false;
    state.phase = "Offline";
    state.players = [];
    state.queue = [];
    if (lastConnectionError !== error.message) {
      logEvent(`Bridge disconnected | ${error.message}`, "red");
      lastConnectionError = error.message;
    }
  } finally {
    refreshInProgress = false;
    render();
  }
}

async function runCommand(command, successMessage, tone = "") {
  try {
    const payload = await requestJson("/api/command", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ command })
    });
    const responseLine = payload.response.split(/\r?\n/).find(line => /^(OK|ERR)\b/.test(line)) || payload.response.trim();
    if (!payload.ok) throw new Error(responseLine || "Server rejected command");
    logEvent(successMessage || responseLine, tone);
    notify(responseLine);
    await refreshSnapshot();
  } catch (error) {
    logEvent(error.message, "red");
    notify(error.message);
    await refreshSnapshot();
  }
}

document.getElementById("startMatch").addEventListener("click", () => runCommand("start", "Start requested"));
document.getElementById("endMatch").addEventListener("click", () => runCommand("end", "End requested", "amber"));
document.getElementById("restartMatch").addEventListener("click", () => runCommand("restart", "Restart requested", "amber"));
document.getElementById("refreshStatus").addEventListener("click", refreshSnapshot);
document.getElementById("checkUpdates").addEventListener("click", checkForUpdates);
document.getElementById("applyUpdate").addEventListener("click", installUpdate);
document.getElementById("showInvite").addEventListener("click", showInvite);
document.getElementById("copyInvite").addEventListener("click", copyInvite);
document.getElementById("refreshPlayers").addEventListener("click", refreshStats);
statsRefreshControl.addEventListener("change", () => {
  try {
    localStorage.setItem(statsRefreshStorageKey, statsRefreshControl.value);
  } catch {}
  if (Number(statsRefreshControl.value) === 0) scheduleStatsRefresh();
  else refreshStats();
});
document.getElementById("queueForm").addEventListener("submit", event => {
  event.preventDefault();
  const map = document.getElementById("mapSelect").value;
  const gameType = document.getElementById("typeSelect").value;
  runCommand(`queuemap ${map} ${gameType}`, `${map.replace(/([a-z])([A-Z])/g, "$1 $2")} queued`);
});
document.getElementById("advanceMap").addEventListener("click", () => runCommand("nextmap", "Map advance requested", "amber"));
document.getElementById("clearActivity").addEventListener("click", () => {
  state.events = [];
  renderEvents();
});

render();
refreshSnapshot();
if (Number(statsRefreshControl.value) > 0) refreshStats();
setInterval(refreshSnapshot, 3000);

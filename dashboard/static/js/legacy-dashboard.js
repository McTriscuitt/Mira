// The old dashboard, reworked once for the move to /legacy. It speaks the 3.0 commands
// and modes. It may fall behind later; the terminal is the maintained interface.

const CHICAGO = { timeZone: 'America/Chicago' };
const SLEEP_CONFIRM_MS = 3000;
const OUTCOME_POLL_MS = 1500;
const OUTCOME_GIVE_UP_MS = 5 * 60 * 1000;

const MODE_LABELS = {
  ACTIVE: 'Normal',
  WAKE: 'Waking up',
  HOLD: 'On hold',
  NAP: 'Napping',
  ASLEEP: 'Asleep',
};

let lastStatus = null;
let pending = null;       // { id, action } while waiting on the board
let message = '';         // the last command's result, under the state name
let sleepArmed = false;
let sleepTimer = null;

function byId(id) { return document.getElementById(id); }

// --- Status ---

async function fetchStatus() {
  const response = await fetch('/api/status/latest');
  if (response.ok) applyStatus(await response.json());
}

function updateBackdrop(lux) {
  const value = lux || 0;
  const letterOpacity = Math.min(1, Math.max(0, value / 2000)) * 0.14;
  const coneOpacity = Math.min(1, value / 1000) * 0.85;
  document.querySelectorAll('.bg-letter').forEach(el => { el.style.opacity = letterOpacity; });
  document.querySelector('.bg-cone').style.opacity = coneOpacity;
  const t = Math.min(1, value / 1000);
  const textV = Math.round(136 * (1 - t) + 40 * t);    // #888 to #282828
  const borderV = Math.round(42 * (1 - t) + 80 * t);   // #2a2a2a to #505050
  document.documentElement.style.setProperty('--btn-text', `rgb(${textV},${textV},${textV})`);
  document.documentElement.style.setProperty('--btn-border', `rgb(${borderV},${borderV},${borderV})`);
}

// Single render path for the 30 s poll and the status stream.
function applyStatus(data) {
  if (!data) return;
  lastStatus = data;
  const hasLevel = Boolean(data.ct);
  byId('val-lux').textContent = data.lux === null ? '—' : Math.round(data.lux);
  byId('val-bri').textContent = hasLevel ? `${data.bri.toFixed(1)}%` : '—';
  byId('val-ct').textContent = hasLevel ? data.ct : '—';
  byId('val-ts').textContent = new Date(data.timestamp).toLocaleTimeString('en-US', CHICAGO);
  renderProgress(data);
  renderLights(data);
  updateBackdrop(data.lux);
}

function renderProgress(data) {
  const wrap = byId('progress-slider-wrap');
  const slider = byId('progress-slider');
  const text = byId('progress-text');
  const paused = data.mode === 'HOLD' || data.mode === 'NAP';
  const waking = data.mode === 'WAKE';

  byId('progress-state-name').textContent = MODE_LABELS[data.mode] || data.mode;
  wrap.classList.toggle('hidden', !paused && !waking);
  byId('extend-btn').classList.toggle('hidden', !paused);
  byId('finish-btn').classList.toggle('hidden', !paused);

  if (paused) {
    const remaining = Math.ceil((data.remaining_s || 0) / 60);
    // The bar burns down left to right. It grows past an hour once extended.
    const max = Math.max(60, remaining);
    slider.max = max;
    slider.value = max - remaining;
    text.textContent = `${remaining} min remaining`;
  } else if (waking) {
    const total = data.wake_total || 30;
    slider.max = total;
    slider.value = data.wake_step || 0;
    text.textContent = `${data.wake_step || 0} / ${total} steps`;
  }

  byId('progress-detail').textContent = pending ? 'pending' : message;
}

function renderLights(data) {
  document.querySelectorAll('.light-chip').forEach(chip => {
    const name = chip.dataset.light;
    const out = Boolean(data.excluded && data.excluded[name]);
    const on = Boolean(data.lights_on && data.lights_on[name]);
    chip.classList.toggle('in-cycle', on);
    chip.classList.toggle('excluded', out && !on);
  });
}

// --- Log ---

async function fetchLog() {
  const response = await fetch('/api/log/recent?limit=10');
  if (!response.ok) return;
  const entries = await response.json();
  const list = byId('log-list');
  list.replaceChildren();
  entries.forEach(entry => {
    const row = document.createElement('div');
    row.className = 'log-entry';
    const time = document.createElement('span');
    time.className = 'log-time';
    time.textContent = new Date(entry.timestamp).toLocaleTimeString('en-US', CHICAGO);
    const text = document.createElement('span');
    text.textContent = entry.message;
    row.append(time, text);
    list.appendChild(row);
  });
}

// --- Commands ---

function refreshButtons() {
  document.querySelectorAll('.btn[data-action]').forEach(button => {
    button.classList.toggle('btn-active', Boolean(pending) && button.dataset.action === pending.action);
  });
  if (lastStatus) renderProgress(lastStatus);
}

function postJson(path, body) {
  return fetch(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': csrfToken() },
    body: JSON.stringify(body || {}),
  });
}

// An ack only means the board queued it, so ask the server what actually happened.
async function followOutcome(id) {
  const started = Date.now();
  while (pending && pending.id === id && Date.now() - started < OUTCOME_GIVE_UP_MS) {
    await new Promise(resolve => setTimeout(resolve, OUTCOME_POLL_MS));
    const response = await fetch(`/api/command/${id}`);
    if (!response.ok) break;
    const { outcome, note } = await response.json();
    if (outcome === 'waiting' || outcome === 'picked_up') continue;
    if (!pending || pending.id !== id) return;
    if (outcome === 'ignored') message = note || 'the board refused that';
    else if (outcome === 'expired') message = 'the board never picked that up';
    else if (outcome === 'unconfirmed') message = 'the board took it, but no change showed up';
    else message = '';
    break;
  }
  if (pending && pending.id === id) pending = null;
  refreshButtons();
  fetchStatus();
  fetchLog();
}

async function sendCommand(action, value) {
  message = '';
  const pressedAgain = pending && pending.action === action && value === undefined;
  if (pending) {
    const previous = pending;
    pending = null;
    await postJson(`/api/command/${previous.id}/cancel`);
    if (pressedAgain) {
      refreshButtons();
      return;
    }
  }
  const body = { action };
  if (value !== undefined) body.value = value;
  const response = await postJson('/api/command', body);
  if (!response.ok) {
    message = response.status === 401 ? 'your login has ended' : 'the server refused that';
    refreshButtons();
    return;
  }
  const { id } = await response.json();
  pending = { id, action };
  refreshButtons();
  followOutcome(id);
}

function disarmSleep() {
  sleepArmed = false;
  clearTimeout(sleepTimer);
  byId('sleep-btn').textContent = 'Sleep';
}

document.querySelectorAll('.btn[data-action]').forEach(button => {
  button.addEventListener('click', () => {
    if (button.dataset.action !== 'SLEEP') {
      disarmSleep();
      sendCommand(button.dataset.action);
    } else if (sleepArmed) {
      disarmSleep();
      sendCommand('SLEEP');
    } else {
      sleepArmed = true;
      button.textContent = 'confirm?';
      sleepTimer = setTimeout(disarmSleep, SLEEP_CONFIRM_MS);
    }
  });
});

byId('extend-btn').addEventListener('click', () => sendCommand('EXTEND', 10));
byId('finish-btn').addEventListener('click', () => sendCommand('RESUME'));

// --- Live updates ---

// The firmware posts a snapshot right after applying a command, so this paints almost
// instantly. The 30 s polls below are the fallback; EventSource reconnects on its own.
function connectStream() {
  if (!window.EventSource) return;
  const stream = new EventSource('/api/status/stream');
  stream.addEventListener('status', event => applyStatus(JSON.parse(event.data)));
  stream.addEventListener('log', fetchLog);
  stream.addEventListener('ended', () => { window.location.href = '/'; });
  window.addEventListener('pagehide', () => stream.close());
}

fetchStatus();
fetchLog();
connectStream();
setInterval(fetchStatus, 30000);
setInterval(fetchLog, 30000);

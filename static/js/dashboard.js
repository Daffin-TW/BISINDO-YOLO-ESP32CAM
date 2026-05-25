/**
 * BISINDO Dashboard  –  dashboard.js
 *
 * Responsibilities:
 *   1. Start / stop stream via POST /api/start and /api/stop
 *   2. Listen to Server-Sent Events from /events for live sidebar stats
 *   3. Update all UI elements (status, FPS, latency, active timer, detection)
 *   4. Manage the active-time clock independently on the client side
 */

'use strict';

// ─────────────────────────────────────────────────────────────────────────────
// DOM refs
// ─────────────────────────────────────────────────────────────────────────────
const $statusDot       = document.getElementById('statusDot');
const $statusText      = document.getElementById('statusText');
const $latency         = document.getElementById('latency');
const $fps             = document.getElementById('fps');
const $activeTime      = document.getElementById('activeTime');
const $battery         = document.getElementById('battery');
const $detectionBadge  = document.getElementById('detectionBadge');
const $detectionLabel  = document.getElementById('detectionLabel');
const $toggleBtn       = document.getElementById('toggleBtn');
const $controlHint     = document.getElementById('controlHint');
const $videoOverlay    = document.getElementById('videoOverlay');
const $subtitleBar     = document.getElementById('subtitleBar');
const $subtitleText    = document.getElementById('subtitleText');
const $hudFps          = document.getElementById('hudFps');
const $hudLatency      = document.getElementById('hudLatency');
const $topbarDot       = document.getElementById('topbarDot');
const $topbarStatus    = document.getElementById('topbarStatus');
const $topbarBadge     = $topbarDot.closest('.topbar-badge');

// ─────────────────────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────────────────────
let isStreaming   = false;
let isConnected   = false;
let startEpoch    = null;       // ms timestamp when stream started
let clockInterval = null;       // setInterval handle for active-time ticker
let sse           = null;       // EventSource handle

// ─────────────────────────────────────────────────────────────────────────────
// Utility
// ─────────────────────────────────────────────────────────────────────────────
function pad(n) { return String(n).padStart(2, '0'); }

function secondsToHMS(s) {
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  return `${pad(h)}:${pad(m)}:${pad(sec)}`;
}

// ─────────────────────────────────────────────────────────────────────────────
// UI update helpers
// ─────────────────────────────────────────────────────────────────────────────
function setConnected(connected) {
  isConnected = connected;

  $statusDot.classList.toggle('connected', connected);
  $statusText.textContent = connected ? 'Terhubung' : 'Tidak Terhubung';

  $topbarDot.classList.toggle('on', connected);
  $topbarStatus.textContent = connected ? 'Live' : 'Offline';
  $topbarBadge.classList.toggle('connected-badge', connected);
}

function updateDetection(label) {
  if (!label) return;
  $detectionLabel.textContent = label.toUpperCase();
  $subtitleText.textContent   = label;

  $detectionBadge.classList.remove('flash-in');
  void $detectionBadge.offsetWidth;  // force reflow to restart animation
  $detectionBadge.classList.add('active', 'flash-in');

  $subtitleBar.classList.add('active');
  $subtitleText.classList.remove('flash-in');
  void $subtitleText.offsetWidth;
  $subtitleText.classList.add('flash-in');
}

function resetStats() {
  $fps.textContent     = '--';
  $latency.textContent = '--';
  $hudFps.textContent  = '-- FPS';
  $hudLatency.textContent = '-- ms';
  $activeTime.textContent = '00:00:00';
}

// ─────────────────────────────────────────────────────────────────────────────
// Active-time clock (client-side)
// ─────────────────────────────────────────────────────────────────────────────
function startClock() {
  startEpoch = Date.now();
  clearInterval(clockInterval);
  clockInterval = setInterval(() => {
    const elapsed = Math.floor((Date.now() - startEpoch) / 1000);
    $activeTime.textContent = secondsToHMS(elapsed);
  }, 1000);
}

function stopClock() {
  clearInterval(clockInterval);
  clockInterval = null;
  $activeTime.textContent = '00:00:00';
}

// ─────────────────────────────────────────────────────────────────────────────
// Server-Sent Events
// ─────────────────────────────────────────────────────────────────────────────
function startSSE() {
  if (sse) { sse.close(); }
  sse = new EventSource('/events');

  // Initial full state dump sent on connect
  sse.addEventListener('init', (e) => {
    const data = JSON.parse(e.data);
    setConnected(data.connected || false);
    if (data.last_label) updateDetection(data.last_label);
  });

  // Per-frame stats
  sse.addEventListener('stats', (e) => {
    const data = JSON.parse(e.data);

    const fps = data.fps ?? '--';
    const lat = data.latency_ms ?? '--';

    $fps.textContent         = fps;
    $latency.textContent     = lat;
    $hudFps.textContent      = `${fps} FPS`;
    $hudLatency.textContent  = `${lat} ms`;

    setConnected(data.connected === true);

    if (data.last_label) updateDetection(data.last_label);
  });

  // Connection / streaming status changes
  sse.addEventListener('status', (e) => {
    const data = JSON.parse(e.data);
    setConnected(data.connected === true);
    if (data.streaming === false) {
      setStreamingUI(false);
    }
  });

  sse.onerror = () => {
    setConnected(false);
    // SSE will auto-reconnect; no manual action needed
  };
}

function stopSSE() {
  if (sse) { sse.close(); sse = null; }
}

// ─────────────────────────────────────────────────────────────────────────────
// Streaming toggle
// ─────────────────────────────────────────────────────────────────────────────
function setStreamingUI(streaming) {
  isStreaming = streaming;

  // Overlay
  $videoOverlay.classList.toggle('hidden', streaming);

  // Button
  if (streaming) {
    $toggleBtn.className = 'btn-stream btn-stream--stop';
    $toggleBtn.innerHTML = `
      <svg viewBox="0 0 24 24" fill="currentColor">
        <rect x="6" y="4" width="4" height="16"/><rect x="14" y="4" width="4" height="16"/>
      </svg>
      Hentikan Stream`;
    $controlHint.textContent = 'Stream & inferensi aktif';
  } else {
    $toggleBtn.className = 'btn-stream btn-stream--start';
    $toggleBtn.innerHTML = `
      <svg viewBox="0 0 24 24" fill="currentColor">
        <polygon points="5 3 19 12 5 21 5 3"/>
      </svg>
      Mulai Stream`;
    $controlHint.textContent = 'Tekan untuk memulai inferensi';
    $subtitleBar.classList.remove('active');
    $subtitleText.textContent = 'Menunggu deteksi…';
    $detectionBadge.classList.remove('active');
    $detectionLabel.textContent = '–';
    resetStats();
    stopClock();
    setConnected(false);
  }
}

async function toggleStream() {
  if (isStreaming) {
    // Stop
    setStreamingUI(false);
    stopSSE();
    await fetch('/api/stop', { method: 'POST' });
  } else {
    // Start
    setStreamingUI(true);
    startClock();
    startSSE();
    await fetch('/api/start', { method: 'POST' });
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────────────────
(async function init() {
  // Fetch current server state to sync UI after page reload
  try {
    const res  = await fetch('/api/status');
    const data = await res.json();
    if (data.streaming) {
      setStreamingUI(true);
      startClock();
      startSSE();
    }
    setConnected(data.connected || false);
    if (data.last_label) updateDetection(data.last_label);
  } catch (_) {
    /* server not reachable — stay in default state */
  }
})();

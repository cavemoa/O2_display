#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTP_Method.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ADS1X15.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_SCD30.h>

#include "secrets.h"

namespace {

// ---- Timing, buffering, and calibration constants ----

constexpr unsigned long kBlinkIntervalMs = 500;
constexpr unsigned long kInitialWifiTimeoutMs = 15000;
constexpr unsigned long kRetryWifiTimeoutMs = 5000;
constexpr unsigned long kWifiRetryIntervalMs = 10000;
constexpr unsigned long kTimeSyncRetryIntervalMs = 60000;
constexpr unsigned long kTimeSyncPollIntervalMs = 5000;

constexpr size_t kTftHistorySize = 60;
constexpr size_t kWebHistoryPoints = 60;
constexpr size_t kOxygenSourceHistorySize = 32;
constexpr size_t kHighO2CalibrationWindowSamples = 6;
constexpr adsGain_t kAdsGain = GAIN_SIXTEEN;
constexpr float kAirOxygenPartialPressure = 0.2099f;
constexpr float kMinHighO2CalibrationPercent = 21.0f;
constexpr float kMaxHighO2CalibrationPercent = 100.0f;
constexpr float kHighO2CalibrationMinRiseMv = 1.0f;
constexpr float kHighO2CalibrationStableP2pMv = 0.2f;
constexpr unsigned long kHighO2CalibrationMinDurationMs = 20000;
constexpr unsigned long kHighO2CalibrationRiseTimeoutMs = 120000;
constexpr unsigned long kHighO2CalibrationTimeoutMs = 300000;

constexpr uint16_t kDefaultMeasurementInterval = 2;
constexpr uint16_t kMinMeasurementInterval = 2;
constexpr uint16_t kMaxMeasurementInterval = 1800;

constexpr size_t kDefaultTftHistoryWindow = 24;

constexpr size_t kWebViewCount = 4;
constexpr uint16_t kDefaultWebViewMinutes = 5;
constexpr uint16_t kWebViewMinutesOptions[kWebViewCount] = {5, 15, 30, 60};
constexpr uint16_t kWebBucketSecondsOptions[kWebViewCount] = {5, 15, 30, 60};

constexpr int kScreenWidth = 135;
constexpr int kScreenHeight = 240;
constexpr int kHeaderHeight = 16;
constexpr int kTftRowCount = 4;
constexpr int kTftRowHeight = kScreenHeight / kTftRowCount;
constexpr int kPanelInset = 3;
constexpr uint16_t kGridColor = 0x4208;
constexpr char kHostname[] = "o2-display";
constexpr char kNzTimeZone[] = "NZST-12NZDT,M9.5.0/2,M4.1.0/3";
constexpr char kTimeZoneName[] = "Pacific/Auckland";
constexpr char kNtpServerPrimary[] = "pool.ntp.org";
constexpr char kNtpServerSecondary[] = "time.google.com";
constexpr char kNtpServerTertiary[] = "time.windows.com";
constexpr time_t kConfirmedEpochThreshold = 1704067200;

// Metric identifiers shared by the history and JSON code paths.
enum class MetricType {
  Co2,
  Humidity,
  Temperature,
  OxygenCell1Mv,
  OxygenCell2Mv,
  OxygenCell3Mv,
};

struct WebBucket {
  uint32_t bucket_index = 0;
  float co2_sum = 0.0f;
  float humidity_sum = 0.0f;
  float temperature_sum = 0.0f;
  float oxygen_cell1_mv_sum = 0.0f;
  float oxygen_cell2_mv_sum = 0.0f;
  float oxygen_cell3_mv_sum = 0.0f;
  uint16_t sample_count = 0;
};

// Synthetic channels let the app behave like the future three-cell design
// while only one physical oxygen cell is fitted during development.
struct SyntheticOxygenCell {
  float lag_seconds = 0.0f;
  float offset_mv = 0.0f;
};

// Recent raw oxygen values are retained only so synthetic cells can look back
// in time and apply a lag before their fixed offset is added.
struct OxygenSourceSample {
  uint32_t timestamp_seconds = 0;
  float oxygen_mv = 0.0f;
};

enum class HighO2CalibrationResult {
  Idle,
  Running,
  Success,
  Cancelled,
  Timeout,
  NoRise,
  InvalidReadings,
};

struct HighO2CalibrationWindowSample {
  float cell1_mv = 0.0f;
  float cell2_mv = 0.0f;
  float cell3_mv = 0.0f;
};

struct WebHistory {
  uint16_t span_minutes = 0;
  uint16_t bucket_seconds = 0;
  WebBucket buckets[kWebHistoryPoints];
  size_t head = 0;
  size_t count = 0;
};

struct HighO2CalibrationState {
  bool active = false;
  float gas_percent = 0.0f;
  float gas_fraction = 0.0f;
  unsigned long started_ms = 0;
  float start_mean_mv = 0.0f;
  float peak_mean_mv = 0.0f;
  bool observed_rise = false;
  HighO2CalibrationWindowSample window[kHighO2CalibrationWindowSamples];
  size_t window_head = 0;
  size_t window_count = 0;
  float last_window_p2p_mv = NAN;
  HighO2CalibrationResult last_result = HighO2CalibrationResult::Idle;
};

// Bucketed web histories are rendered from a display start bucket rather than
// from raw sample indices so they can fill left-to-right before they scroll.
uint32_t web_display_start_bucket(const WebHistory &history);

// ---- Embedded web UI ----

const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>O2 Display Dashboard</title>
  <style>
    :root {
      --bg: #09111b;
      --panel: #132133;
      --border: #2d4662;
      --text: #eff7ff;
      --muted: #99afc4;
      --co2: #35d8ff;
      --hum: #39d98a;
      --temp: #ffd166;
      --oxy1: #ff8c42;
      --oxy2: #ffb703;
      --oxy3: #ff4d6d;
      --active-text: #04111d;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: "Segoe UI", Tahoma, sans-serif;
      background: linear-gradient(180deg, #0a1420 0%, #081019 100%);
      color: var(--text);
    }
    main {
      max-width: 980px;
      margin: 0 auto;
      padding: 24px 16px 40px;
    }
    h1 {
      margin: 0 0 6px;
      font-size: 28px;
      letter-spacing: 0.03em;
    }
    .meta {
      color: var(--muted);
      margin-bottom: 18px;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
      gap: 14px;
      margin-bottom: 18px;
    }
    .controls {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
      gap: 12px;
      margin-bottom: 18px;
    }
    .card, .chart {
      background: rgba(19, 33, 51, 0.88);
      border: 1px solid var(--border);
      border-radius: 16px;
      padding: 14px 16px;
      box-shadow: 0 10px 30px rgba(0, 0, 0, 0.2);
    }
    .label {
      font-size: 13px;
      text-transform: uppercase;
      letter-spacing: 0.08em;
      color: var(--muted);
      margin-bottom: 8px;
    }
    .value {
      font-size: 34px;
      font-weight: 700;
      line-height: 1.1;
    }
    .value-number {
      display: inline-block;
    }
    .value-unit {
      display: inline-block;
      font-size: 0.5em;
      font-weight: 600;
      margin-left: 0.25em;
      vertical-align: baseline;
    }
    label {
      display: block;
      color: var(--muted);
      font-size: 13px;
      margin-bottom: 6px;
      text-transform: uppercase;
      letter-spacing: 0.08em;
    }
    input, select {
      width: 100%;
      border-radius: 10px;
      border: 1px solid var(--border);
      background: #0b1521;
      color: var(--text);
      padding: 10px 12px;
      font-size: 16px;
      margin-bottom: 12px;
    }
    button {
      border: 0;
      border-radius: 12px;
      background: linear-gradient(135deg, #1c91ff, #35d8ff);
      color: var(--active-text);
      font-weight: 700;
      padding: 12px 16px;
      cursor: pointer;
      font-size: 15px;
    }
    button:disabled {
      opacity: 0.5;
      cursor: not-allowed;
    }
    .actions {
      display: flex;
      gap: 10px;
      margin-top: 4px;
      flex-wrap: wrap;
    }
    .secondary {
      background: transparent;
      color: var(--text);
      border: 1px solid var(--border);
    }
    .hint {
      color: var(--muted);
      font-size: 13px;
      margin-top: 10px;
    }
    .accent-co2 { color: var(--co2); }
    .accent-hum { color: var(--hum); }
    .accent-temp { color: var(--temp); }
    .accent-oxy { color: var(--oxy1); }
    .accent-oxy-calibrated { color: var(--hum); }
    .charts {
      display: grid;
      gap: 14px;
    }
    .legend {
      display: flex;
      gap: 14px;
      flex-wrap: wrap;
      margin-top: 10px;
      color: var(--muted);
      font-size: 13px;
    }
    .legend span::before {
      content: '';
      display: inline-block;
      width: 10px;
      height: 10px;
      border-radius: 999px;
      margin-right: 8px;
      vertical-align: middle;
    }
    .legend .oxy1::before { background: var(--oxy1); }
    .legend .oxy2::before { background: var(--oxy2); }
    .legend .oxy3::before { background: var(--oxy3); }
    .chart-title {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 8px;
      color: var(--muted);
      font-size: 14px;
    }
    canvas {
      width: 100%;
      height: 120px;
      display: block;
      background: #0b1521;
      border-radius: 12px;
    }
    .status {
      margin-top: 14px;
      color: var(--muted);
      font-size: 14px;
    }
  </style>
</head>
<body>
  <main>
    <h1>O2 Display Dashboard</h1>
    <div class="meta" id="meta">Connecting...</div>

    <section class="grid">
      <div class="card">
        <div class="label">CO2</div>
        <div class="value accent-co2" id="co2Value"><span class="value-number">--</span><span class="value-unit">ppm</span></div>
      </div>
      <div class="card">
        <div class="label">Humidity</div>
        <div class="value accent-hum" id="humidityValue"><span class="value-number">--</span><span class="value-unit">%RH</span></div>
      </div>
      <div class="card">
        <div class="label">Temperature</div>
        <div class="value accent-temp" id="temperatureValue"><span class="value-number">--</span><span class="value-unit">C</span></div>
      </div>
      <div class="card">
        <div class="label">Oxygen Mean</div>
        <div class="value accent-oxy" id="oxygenValue"><span class="value-number">--</span><span class="value-unit">mV</span></div>
      </div>
    </section>

    <section class="controls">
      <div class="card">
        <div class="label">Sampling</div>
        <label for="measurementInterval">Sample rate in seconds</label>
        <input id="measurementInterval" type="number" min="2" max="1800" step="1" value="2">
        <div class="actions">
          <button id="applySettings" type="button" disabled>Apply sample rate</button>
          <button id="resetSettings" class="secondary" type="button" disabled>Revert</button>
        </div>
        <div class="hint" id="controlsHint">Live refresh will not overwrite edits in progress.</div>
      </div>
      <div class="card">
        <div class="label">Web History</div>
        <label for="viewMinutes">Time range</label>
        <select id="viewMinutes">
          <option value="5">5 min</option>
          <option value="15">15 min</option>
          <option value="30">30 min</option>
          <option value="60">60 min</option>
        </select>
        <div class="hint">This selection changes the web graphs immediately. The TFT display stays on its current raw history view.</div>
      </div>
      <div class="card">
        <div class="label">Oxygen Calibration</div>
        <button id="calibrateAir" type="button">Calibrate in Air</button>
        <label for="highO2Percent">High O2 gas (%)</label>
        <input id="highO2Percent" type="number" min="21" max="100" step="0.1" value="98.0">
        <div class="actions">
          <button id="beginHighO2Calibration" type="button">Begin calibration</button>
          <button id="cancelHighO2Calibration" class="secondary" type="button" disabled>Cancel</button>
        </div>
        <div class="hint" id="oxygenCalibrationHint">Before calibration the oxygen values are shown in raw mV. Air reference: 0.2099 ppO2.</div>
        <div class="hint" id="oxygenHighO2Hint">High O2 calibration waits for a clear rise and then a stable sliding window before applying scales.</div>
      </div>
    </section>

    <section class="charts">
      <div class="chart">
        <div class="chart-title"><span>Oxygen cell history</span><span id="oxygenRange">mV</span></div>
        <canvas id="oxygenChart" width="900" height="135"></canvas>
        <div class="legend">
          <span class="oxy1">Cell 1 Actual</span>
          <span class="oxy2">Cell 2 Synthetic</span>
          <span class="oxy3">Cell 3 Synthetic</span>
        </div>
      </div>
      <div class="chart">
        <div class="chart-title"><span>CO2 history</span><span id="co2Range">ppm</span></div>
        <canvas id="co2Chart" width="900" height="135"></canvas>
      </div>
      <div class="chart">
        <div class="chart-title"><span>Temperature history</span><span id="temperatureRange">C</span></div>
        <canvas id="temperatureChart" width="900" height="135"></canvas>
      </div>
      <div class="chart">
        <div class="chart-title"><span>Humidity history</span><span id="humidityRange">%RH</span></div>
        <canvas id="humidityChart" width="900" height="135"></canvas>
      </div>
    </section>

    <div class="status" id="status">Waiting for device data...</div>
  </main>

  <script>
    const supportedViews = [5, 15, 30, 60];
    const measurementInput = document.getElementById('measurementInterval');
    const applyButton = document.getElementById('applySettings');
    const resetButton = document.getElementById('resetSettings');
    const calibrateButton = document.getElementById('calibrateAir');
    const highO2PercentInput = document.getElementById('highO2Percent');
    const beginHighO2Button = document.getElementById('beginHighO2Calibration');
    const cancelHighO2Button = document.getElementById('cancelHighO2Calibration');
    const viewSelect = document.getElementById('viewMinutes');
    const controlsHint = document.getElementById('controlsHint');
    const oxygenCalibrationHint = document.getElementById('oxygenCalibrationHint');
    const oxygenHighO2Hint = document.getElementById('oxygenHighO2Hint');

    function loadSelectedView() {
      try {
        const stored = Number(window.localStorage.getItem('webViewMinutes'));
        return supportedViews.includes(stored) ? stored : 5;
      } catch (error) {
        return 5;
      }
    }

    const settingsState = {
      applying: false,
      calibrating: false,
      highO2Submitting: false,
      dirty: false,
      lastServer: null,
      selectedViewMinutes: loadSelectedView()
    };

    function saveSelectedView() {
      try {
        window.localStorage.setItem('webViewMinutes', String(settingsState.selectedViewMinutes));
      } catch (error) {
      }
    }

    function fmt(value, decimals, unit) {
      if (!Number.isFinite(value)) return `-- ${unit}`;
      return `${value.toFixed(decimals)} ${unit}`;
    }

    function formatCardMarkup(numberText, unit) {
      return `<span class="value-number">${numberText}</span><span class="value-unit">${unit}</span>`;
    }

    function formatFixedCardValue(value, decimals, unit) {
      if (!Number.isFinite(value)) {
        return formatCardMarkup('--', unit);
      }
      return formatCardMarkup(Number(value).toFixed(decimals), unit);
    }

    function formatOxygenSummaryValue(value, unit) {
      if (!Number.isFinite(value)) {
        return formatCardMarkup('--', unit);
      }

      const significantDigits = unit === 'ppO2' ? 3 : 4;
      return formatCardMarkup(Number(value).toPrecision(significantDigits), unit);
    }

    function controlsValid() {
      const measurementInterval = Number(measurementInput.value);
      return Number.isInteger(measurementInterval)
        && measurementInterval >= 2
        && measurementInterval <= 1800;
    }

    function highO2GasValid() {
      const gasPercent = Number(highO2PercentInput.value);
      return Number.isFinite(gasPercent)
        && gasPercent >= 21
        && gasPercent <= 100;
    }

    function syncDirtyState() {
      if (!settingsState.lastServer) {
        settingsState.dirty = false;
        return;
      }

      settingsState.dirty =
        Number(measurementInput.value) !== settingsState.lastServer.measurementInterval;
    }

    function updateControlsUi() {
      syncDirtyState();
      const valid = controlsValid();
      const highO2Valid = highO2GasValid();
      const highO2Active = Boolean(settingsState.lastServer && settingsState.lastServer.highO2CalActive);
      applyButton.disabled = settingsState.applying || !valid || !settingsState.dirty;
      resetButton.disabled = settingsState.applying || !settingsState.lastServer || !settingsState.dirty;
      calibrateButton.disabled = settingsState.calibrating || settingsState.highO2Submitting || highO2Active
        || !settingsState.lastServer || !settingsState.lastServer.adsReady;
      beginHighO2Button.disabled = settingsState.highO2Submitting || settingsState.calibrating || highO2Active
        || !highO2Valid || !settingsState.lastServer || !settingsState.lastServer.adsReady;
      cancelHighO2Button.disabled = settingsState.highO2Submitting || !highO2Active;
      viewSelect.value = String(settingsState.selectedViewMinutes);

      if (settingsState.applying) {
        controlsHint.textContent = 'Applying sample rate...';
      } else if (!valid) {
        controlsHint.textContent = 'Use 2-1800 seconds for the sample rate.';
      } else if (settingsState.dirty) {
        controlsHint.textContent = 'Unsaved sample-rate change. Live refresh will keep the page current but will not overwrite this field.';
      } else {
        controlsHint.textContent = 'Sample rate is synced with the device. Web history range buttons apply immediately.';
      }
    }

    function syncInputsFromServer(data) {
      settingsState.lastServer = {
        measurementInterval: Number(data.measurementInterval),
        adsReady: Boolean(data.adsReady),
        oxygenCalibrated: Boolean(data.oxygenCalibrated),
        highO2CalActive: Boolean(data.highO2CalActive)
      };
      const oxygenValue = document.getElementById('oxygenValue');
      oxygenValue.classList.toggle('accent-oxy-calibrated', Boolean(data.oxygenCalibrated));

      if (!settingsState.dirty && !settingsState.applying) {
        measurementInput.value = data.measurementInterval;
      }

      if (settingsState.calibrating) {
        oxygenCalibrationHint.textContent = 'Calibrating oxygen cells in air...';
      } else if (!data.adsReady) {
        oxygenCalibrationHint.textContent = 'ADS1115 is offline. Oxygen calibration is unavailable.';
      } else if (data.oxygenCalibrated) {
        oxygenCalibrationHint.textContent = 'Oxygen cells are calibrated to air at 0.2099 ppO2. Values now show partial pressure.';
      } else {
        oxygenCalibrationHint.textContent = 'Before calibration the oxygen values are shown in raw mV. Air reference: 0.2099 ppO2.';
      }

      if (data.highO2CalActive) {
        if (data.highO2CalObservedRise) {
          oxygenHighO2Hint.textContent =
            `High O2 calibration running at ${data.highO2CalTargetPercent.toFixed(1)}% O2. ` +
            `Peak ${data.highO2CalPeakMv.toFixed(3)} mV, stability window ${data.highO2CalWindowCount}/6, ` +
            `peak-to-peak ${Number.isFinite(data.highO2CalWindowP2pMv) ? data.highO2CalWindowP2pMv.toFixed(3) : '--'} mV, ` +
            `elapsed ${data.highO2CalElapsedSeconds}s.`;
        } else {
          oxygenHighO2Hint.textContent =
            `High O2 calibration running at ${data.highO2CalTargetPercent.toFixed(1)}% O2. ` +
            `Waiting for a clear rise above the starting ${data.highO2CalStartMv.toFixed(3)} mV baseline. ` +
            `Peak ${data.highO2CalPeakMv.toFixed(3)} mV, elapsed ${data.highO2CalElapsedSeconds}s.`;
        }
      } else {
        oxygenHighO2Hint.textContent = data.highO2CalMessage;
      }

      updateControlsUi();
    }

    function formatElapsed(seconds) {
      if (seconds <= 0) {
        return '0';
      }

      if (seconds < 60) {
        return `${seconds}s`;
      }

      const minutes = Math.floor(seconds / 60);
      const remainder = seconds % 60;
      if (remainder === 0) {
        return `${minutes}m`;
      }
      return `${minutes}m ${String(remainder).padStart(2, '0')}s`;
    }

    function formatAxisClockTime(epochSeconds, spanMinutes, timeZone) {
      try {
        const options = {
          timeZone: timeZone || 'Pacific/Auckland',
          hour: '2-digit',
          minute: '2-digit',
          hour12: false
        };
        if (spanMinutes <= 15) {
          options.second = '2-digit';
        }
        return new Intl.DateTimeFormat('en-NZ', options).format(new Date(epochSeconds * 1000));
      } catch (error) {
        return '';
      }
    }

    function drawTimeAxis(ctx, left, baseline, width, canvasHeight, axisOptions) {
      ctx.strokeStyle = '#395169';
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(left, baseline + 0.5);
      ctx.lineTo(left + width, baseline + 0.5);
      ctx.stroke();

      ctx.fillStyle = '#99afc4';
      ctx.font = '12px Segoe UI';
      ctx.textBaseline = 'alphabetic';

      const tickCount = 4;
      for (let i = 0; i <= tickCount; i += 1) {
        const ratio = i / tickCount;
        const x = left + ratio * width;
        ctx.beginPath();
        ctx.moveTo(x, baseline);
        ctx.lineTo(x, baseline + 6);
        ctx.stroke();

        const elapsedSeconds = Math.round(ratio * axisOptions.spanMinutes * 60);
        let label = formatElapsed(elapsedSeconds);
        if (axisOptions.timeConfirmed && Number.isFinite(axisOptions.axisStartEpochSeconds)) {
          const clockLabel = formatAxisClockTime(
            axisOptions.axisStartEpochSeconds + elapsedSeconds,
            axisOptions.spanMinutes,
            axisOptions.timeZone
          );
          if (clockLabel) {
            label = clockLabel;
          }
        }
        if (i === 0) {
          ctx.textAlign = 'left';
        } else if (i === tickCount) {
          ctx.textAlign = 'right';
        } else {
          ctx.textAlign = 'center';
        }
        ctx.fillText(label, x, canvasHeight - 8);
      }
    }

    function drawGraph(canvasId, rangeId, values, color, unit, options = {}) {
      const canvas = document.getElementById(canvasId);
      const ctx = canvas.getContext('2d');
      const w = canvas.width;
      const h = canvas.height;
      const showTimeAxis = Boolean(options.showTimeAxis);
      const axisOptions = {
        spanMinutes: Number(options.spanMinutes || 0),
        timeConfirmed: Boolean(options.timeConfirmed),
        axisStartEpochSeconds: Number(options.axisStartEpochSeconds),
        timeZone: options.timeZone || 'Pacific/Auckland'
      };
      const paddingLeft = 12;
      const paddingRight = 10;
      const paddingTop = 10;
      const paddingBottom = showTimeAxis ? 30 : 12;
      const plotWidth = w - paddingLeft - paddingRight;
      const plotHeight = h - paddingTop - paddingBottom;
      const plotBottom = paddingTop + plotHeight;

      ctx.clearRect(0, 0, w, h);

      ctx.strokeStyle = '#23384f';
      ctx.lineWidth = 1;
      for (let i = 0; i <= 3; i += 1) {
        const y = paddingTop + (plotHeight / 3) * i;
        ctx.beginPath();
        ctx.moveTo(paddingLeft, y);
        ctx.lineTo(paddingLeft + plotWidth, y);
        ctx.stroke();
      }

      const finiteValues = values.filter((value) => Number.isFinite(value));
      if (finiteValues.length === 0) {
        document.getElementById(rangeId).textContent = unit;
        ctx.fillStyle = '#99afc4';
        ctx.font = '18px Segoe UI';
        ctx.textAlign = 'left';
        ctx.fillText('Waiting for samples...', paddingLeft + 6, paddingTop + plotHeight / 2);
        if (showTimeAxis) {
          drawTimeAxis(ctx, paddingLeft, plotBottom, plotWidth, h, axisOptions);
        }
        return;
      }

      let min = Math.min(...finiteValues);
      let max = Math.max(...finiteValues);
      if (max - min < 0.5) {
        min -= 0.5;
        max += 0.5;
      }
      const pad = (max - min) * 0.15;
      min -= pad;
      max += pad;

      document.getElementById(rangeId).textContent = `${min.toFixed(1)} to ${max.toFixed(1)} ${unit}`;

      ctx.strokeStyle = color;
      ctx.lineWidth = 3;
      ctx.beginPath();
      let pathOpen = false;
      let lastPoint = null;

      values.forEach((value, index) => {
        const x = paddingLeft + (index / Math.max(values.length - 1, 1)) * plotWidth;
        if (!Number.isFinite(value)) {
          pathOpen = false;
          return;
        }

        const normalized = (value - min) / (max - min);
        const y = paddingTop + plotHeight - normalized * plotHeight;
        if (!pathOpen) {
          ctx.moveTo(x, y);
          pathOpen = true;
        } else {
          ctx.lineTo(x, y);
        }
        lastPoint = { x, y };
      });
      ctx.stroke();

      if (lastPoint) {
        ctx.fillStyle = color;
        ctx.beginPath();
        ctx.arc(lastPoint.x, lastPoint.y, 5, 0, Math.PI * 2);
        ctx.fill();
      }

      if (showTimeAxis) {
        drawTimeAxis(ctx, paddingLeft, plotBottom, plotWidth, h, axisOptions);
      }
    }

    function drawMultiGraph(canvasId, rangeId, seriesList, colors, unit, decimals) {
      const canvas = document.getElementById(canvasId);
      const ctx = canvas.getContext('2d');
      const w = canvas.width;
      const h = canvas.height;
      const paddingLeft = 12;
      const paddingRight = 10;
      const paddingTop = 10;
      const paddingBottom = 12;
      const plotWidth = w - paddingLeft - paddingRight;
      const plotHeight = h - paddingTop - paddingBottom;

      ctx.clearRect(0, 0, w, h);

      ctx.strokeStyle = '#23384f';
      ctx.lineWidth = 1;
      for (let i = 0; i <= 3; i += 1) {
        const y = paddingTop + (plotHeight / 3) * i;
        ctx.beginPath();
        ctx.moveTo(paddingLeft, y);
        ctx.lineTo(paddingLeft + plotWidth, y);
        ctx.stroke();
      }

      const finiteValues = seriesList.flat().filter((value) => Number.isFinite(value));
      if (finiteValues.length === 0) {
        document.getElementById(rangeId).textContent = unit;
        ctx.fillStyle = '#99afc4';
        ctx.font = '18px Segoe UI';
        ctx.textAlign = 'left';
        ctx.fillText('Waiting for samples...', paddingLeft + 6, paddingTop + plotHeight / 2);
        return;
      }

      let min = Math.min(...finiteValues);
      let max = Math.max(...finiteValues);
      if (max - min < 0.5) {
        min -= 0.5;
        max += 0.5;
      }
      const pad = (max - min) * 0.15;
      min -= pad;
      max += pad;

      document.getElementById(rangeId).textContent = `${min.toFixed(decimals)} to ${max.toFixed(decimals)} ${unit}`;

      seriesList.forEach((values, seriesIndex) => {
        ctx.strokeStyle = colors[seriesIndex];
        ctx.fillStyle = colors[seriesIndex];
        ctx.lineWidth = 3;
        ctx.beginPath();
        let pathOpen = false;
        let lastPoint = null;

        values.forEach((value, index) => {
          const x = paddingLeft + (index / Math.max(values.length - 1, 1)) * plotWidth;
          if (!Number.isFinite(value)) {
            pathOpen = false;
            return;
          }

          const normalized = (value - min) / (max - min);
          const y = paddingTop + plotHeight - normalized * plotHeight;
          if (!pathOpen) {
            ctx.moveTo(x, y);
            pathOpen = true;
          } else {
            ctx.lineTo(x, y);
          }
          lastPoint = { x, y };
        });
        ctx.stroke();

        if (lastPoint) {
          ctx.beginPath();
          ctx.arc(lastPoint.x, lastPoint.y, 4, 0, Math.PI * 2);
          ctx.fill();
        }
      });
    }

    async function refresh() {
      try {
        const response = await fetch(`/api/readings?view=${encodeURIComponent(settingsState.selectedViewMinutes)}`, { cache: 'no-store' });
        const data = await response.json();
        const timeModeLabel = data.timeConfirmed ? 'NZ local time' : 'uptime fallback';

        document.getElementById('meta').textContent =
          `${data.connected ? 'WiFi connected' : 'WiFi disconnected'} | ` +
          `${data.ip || 'no IP'} | samples ${data.sampleCount} | web view ${data.viewMinutes} min | ${timeModeLabel}`;
        document.getElementById('status').textContent =
          data.sensorReady
            ? `Sensor online. Measurement interval ${data.measurementInterval}s. ADS1115 ${data.adsReady ? 'online' : 'offline'}. Time source ${timeModeLabel}. Web view ${data.viewMinutes} min in ${data.bucketSeconds}s buckets with ${data.visiblePointCount}/${data.pointCount} populated points. TFT remains on ${data.tftHistoryWindow} raw samples. RSSI ${data.rssi} dBm.`
            : 'Sensor not ready.';

        syncInputsFromServer(data);

        document.getElementById('co2Value').innerHTML = formatFixedCardValue(data.co2, 0, 'ppm');
        document.getElementById('humidityValue').innerHTML = formatFixedCardValue(data.humidity, 1, '%RH');
        document.getElementById('temperatureValue').innerHTML = formatFixedCardValue(data.temperature, 1, 'C');
        document.getElementById('oxygenValue').innerHTML = data.adsReady
          ? formatOxygenSummaryValue(data.oxygenMean, data.oxygenUnit)
          : formatCardMarkup('--', data.oxygenUnit);

        drawGraph('co2Chart', 'co2Range', data.co2History, '#35d8ff', 'ppm');
        drawGraph('humidityChart', 'humidityRange', data.humidityHistory, '#39d98a', '%RH', {
          showTimeAxis: true,
          spanMinutes: data.viewMinutes,
          timeConfirmed: data.timeConfirmed,
          axisStartEpochSeconds: data.axisStartEpochSeconds,
          timeZone: data.timeZone
        });
        drawGraph('temperatureChart', 'temperatureRange', data.temperatureHistory, '#ffd166', 'C');
        drawMultiGraph(
          'oxygenChart',
          'oxygenRange',
          data.adsReady
            ? [data.oxygenCell1History, data.oxygenCell2History, data.oxygenCell3History]
            : [[], [], []],
          ['#ff8c42', '#ffb703', '#ff4d6d'],
          data.oxygenUnit,
          data.oxygenDecimals
        );
      } catch (error) {
        document.getElementById('status').textContent = `Fetch failed: ${error}`;
      }
    }

    async function applySettings() {
      if (!controlsValid()) {
        updateControlsUi();
        return;
      }

      const measurementInterval = measurementInput.value;
      settingsState.applying = true;
      updateControlsUi();
      try {
        const response = await fetch('/api/settings', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `measurementInterval=${encodeURIComponent(measurementInterval)}`
        });
        const data = await response.json();
        document.getElementById('status').textContent =
          response.ok
            ? `Sample rate applied. Measurement interval ${data.measurementInterval}s.`
            : `Settings update failed: ${data.error || 'unknown error'}`;
        if (response.ok) {
          settingsState.lastServer = {
            measurementInterval: Number(data.measurementInterval)
          };
          measurementInput.value = data.measurementInterval;
        }
        await refresh();
      } catch (error) {
        document.getElementById('status').textContent = `Settings update failed: ${error}`;
      } finally {
        settingsState.applying = false;
        updateControlsUi();
      }
    }

    function resetSettings() {
      if (!settingsState.lastServer) {
        return;
      }
      measurementInput.value = settingsState.lastServer.measurementInterval;
      updateControlsUi();
    }

    async function calibrateInAir() {
      settingsState.calibrating = true;
      updateControlsUi();
      oxygenCalibrationHint.textContent = 'Calibrating oxygen cells in air...';
      try {
        const response = await fetch('/api/calibrate-air', {
          method: 'POST'
        });
        const data = await response.json();
        document.getElementById('status').textContent =
          response.ok
            ? `Oxygen calibration applied at air reference ${data.referencePpo2}. Values now display ${data.oxygenUnit}.`
            : `Oxygen calibration failed: ${data.error || 'unknown error'}`;
        await refresh();
      } catch (error) {
        document.getElementById('status').textContent = `Oxygen calibration failed: ${error}`;
      } finally {
        settingsState.calibrating = false;
        updateControlsUi();
      }
    }

    async function beginHighO2Calibration() {
      if (!highO2GasValid()) {
        updateControlsUi();
        return;
      }

      settingsState.highO2Submitting = true;
      oxygenHighO2Hint.textContent = 'Starting high O2 calibration. Apply the calibration gas now if it is not already flowing.';
      updateControlsUi();
      try {
        const response = await fetch('/api/calibrate-high-o2/start', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: `gasPercent=${encodeURIComponent(highO2PercentInput.value)}`
        });
        const data = await response.json();
        document.getElementById('status').textContent =
          response.ok
            ? `High O2 calibration started at ${Number(data.highO2CalTargetPercent).toFixed(1)}% O2.`
            : `High O2 calibration failed to start: ${data.error || 'unknown error'}`;
        await refresh();
      } catch (error) {
        document.getElementById('status').textContent = `High O2 calibration failed to start: ${error}`;
      } finally {
        settingsState.highO2Submitting = false;
        updateControlsUi();
      }
    }

    async function cancelHighO2Calibration() {
      settingsState.highO2Submitting = true;
      updateControlsUi();
      try {
        const response = await fetch('/api/calibrate-high-o2/cancel', {
          method: 'POST'
        });
        const data = await response.json();
        document.getElementById('status').textContent =
          response.ok
            ? 'High O2 calibration cancelled.'
            : `High O2 calibration cancel failed: ${data.error || 'unknown error'}`;
        await refresh();
      } catch (error) {
        document.getElementById('status').textContent = `High O2 calibration cancel failed: ${error}`;
      } finally {
        settingsState.highO2Submitting = false;
        updateControlsUi();
      }
    }

    function selectView(viewMinutes) {
      if (!supportedViews.includes(viewMinutes) || viewMinutes === settingsState.selectedViewMinutes) {
        return;
      }
      settingsState.selectedViewMinutes = viewMinutes;
      saveSelectedView();
      updateControlsUi();
      refresh();
    }

    measurementInput.addEventListener('input', updateControlsUi);
    applyButton.addEventListener('click', applySettings);
    resetButton.addEventListener('click', resetSettings);
    calibrateButton.addEventListener('click', calibrateInAir);
    highO2PercentInput.addEventListener('input', updateControlsUi);
    beginHighO2Button.addEventListener('click', beginHighO2Calibration);
    cancelHighO2Button.addEventListener('click', cancelHighO2Calibration);
    viewSelect.addEventListener('change', () => {
      selectView(Number(viewSelect.value));
    });

    updateControlsUi();
    refresh();
    setInterval(refresh, 2500);
  </script>
</body>
</html>
)HTML";

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
Adafruit_SCD30 scd30;
Adafruit_ADS1115 ads;
WebServer server(80);

// ---- Runtime state ----

bool led_state = false;
bool sensor_ready = false;
bool ads_ready = false;
bool oxygen_calibrated = false;
bool web_server_started = false;
bool mdns_started = false;
bool time_sync_started = false;
bool time_confirmed = false;
unsigned long last_blink_ms = 0;
unsigned long last_wifi_retry_ms = 0;
unsigned long last_time_sync_attempt_ms = 0;
unsigned long last_time_sync_check_ms = 0;
unsigned long sample_count = 0;
uint16_t measurement_interval_seconds = kDefaultMeasurementInterval;
size_t tft_history_window = kDefaultTftHistoryWindow;
int64_t confirmed_epoch_offset_seconds = 0;
time_t last_confirmed_epoch_seconds = 0;

float co2_history[kTftHistorySize] = {0.0f};
float humidity_history[kTftHistorySize] = {0.0f};
float temperature_history[kTftHistorySize] = {0.0f};
size_t history_head = 0;
size_t history_count = 0;

WebHistory web_histories[kWebViewCount] = {};

float current_co2 = 0.0f;
float current_humidity = 0.0f;
float current_temperature = 0.0f;
float current_oxygen_cell1_mv = 0.0f;
float current_oxygen_cell2_mv = 0.0f;
float current_oxygen_cell3_mv = 0.0f;
float current_oxygen_mean_mv = 0.0f;
float oxygen_calibration_scale[3] = {1.0f, 1.0f, 1.0f};
SyntheticOxygenCell synthetic_oxygen_cells[2];
OxygenSourceSample oxygen_source_history[kOxygenSourceHistorySize] = {};
size_t oxygen_source_head = 0;
size_t oxygen_source_count = 0;
HighO2CalibrationState high_o2_calibration = {};

// ---- Oxygen display and synthetic-cell helpers ----

size_t displayed_history_count() {
  return min(history_count, tft_history_window);
}

void init_web_histories() {
  for (size_t i = 0; i < kWebViewCount; ++i) {
    web_histories[i].span_minutes = kWebViewMinutesOptions[i];
    web_histories[i].bucket_seconds = kWebBucketSecondsOptions[i];
    web_histories[i].head = 0;
    web_histories[i].count = 0;
  }
}

float random_offset_mv() {
  return static_cast<float>(random(-1000, 1001)) / 1000.0f;
}

float random_lag_seconds() {
  return static_cast<float>(random(2000, 5001)) / 1000.0f;
}

void init_synthetic_oxygen_cells() {
  for (size_t i = 0; i < 2; ++i) {
    synthetic_oxygen_cells[i].offset_mv = random_offset_mv();
    synthetic_oxygen_cells[i].lag_seconds = random_lag_seconds();
  }
}

// Logical indices walk the lag buffer from oldest to newest.
size_t oxygen_source_index(size_t logical_index) {
  const size_t oldest = (oxygen_source_head + kOxygenSourceHistorySize - oxygen_source_count) % kOxygenSourceHistorySize;
  return (oldest + logical_index) % kOxygenSourceHistorySize;
}

void append_oxygen_source_sample(uint32_t timestamp_seconds, float oxygen_mv) {
  oxygen_source_history[oxygen_source_head].timestamp_seconds = timestamp_seconds;
  oxygen_source_history[oxygen_source_head].oxygen_mv = oxygen_mv;
  oxygen_source_head = (oxygen_source_head + 1) % kOxygenSourceHistorySize;
  if (oxygen_source_count < kOxygenSourceHistorySize) {
    ++oxygen_source_count;
  }
}

bool lookup_lagged_oxygen(float lag_seconds, uint32_t now_seconds, float &out_oxygen_mv) {
  if (oxygen_source_count == 0) {
    return false;
  }

  const float target_time = static_cast<float>(now_seconds) - lag_seconds;
  OxygenSourceSample newer = oxygen_source_history[oxygen_source_index(oxygen_source_count - 1)];
  OxygenSourceSample older = oxygen_source_history[oxygen_source_index(0)];

  if (target_time >= static_cast<float>(newer.timestamp_seconds)) {
    out_oxygen_mv = newer.oxygen_mv;
    return true;
  }

  if (target_time <= static_cast<float>(older.timestamp_seconds)) {
    out_oxygen_mv = older.oxygen_mv;
    return true;
  }

  for (size_t i = oxygen_source_count - 1; i > 0; --i) {
    const OxygenSourceSample newer_sample = oxygen_source_history[oxygen_source_index(i)];
    const OxygenSourceSample older_sample = oxygen_source_history[oxygen_source_index(i - 1)];
    if (target_time > static_cast<float>(newer_sample.timestamp_seconds)) {
      continue;
    }
    if (target_time < static_cast<float>(older_sample.timestamp_seconds)) {
      continue;
    }

    const float time_span = static_cast<float>(newer_sample.timestamp_seconds - older_sample.timestamp_seconds);
    if (time_span <= 0.0f) {
      out_oxygen_mv = newer_sample.oxygen_mv;
      return true;
    }

    const float ratio = (target_time - static_cast<float>(older_sample.timestamp_seconds)) / time_span;
    out_oxygen_mv = older_sample.oxygen_mv + ((newer_sample.oxygen_mv - older_sample.oxygen_mv) * ratio);
    return true;
  }

  out_oxygen_mv = newer.oxygen_mv;
  return true;
}

// All operator-facing displays use calibrated values when calibration is active
// and raw millivolts otherwise.
float oxygen_display_value(float oxygen_mv, size_t cell_index) {
  if (!oxygen_calibrated || cell_index >= 3) {
    return oxygen_mv;
  }
  return oxygen_mv * oxygen_calibration_scale[cell_index];
}

// The UI exposes the mean of the three cells to match the planned hardware.
float oxygen_display_mean() {
  return (oxygen_display_value(current_oxygen_cell1_mv, 0)
      + oxygen_display_value(current_oxygen_cell2_mv, 1)
      + oxygen_display_value(current_oxygen_cell3_mv, 2))
      / 3.0f;
}

const char *oxygen_display_unit() {
  return oxygen_calibrated ? "ppO2" : "mV";
}

uint8_t oxygen_display_decimals() {
  return oxygen_calibrated ? 4 : 3;
}

// ---- High-O2 calibration state machine ----

const char *high_o2_calibration_result_code(HighO2CalibrationResult result) {
  switch (result) {
    case HighO2CalibrationResult::Idle:
      return "idle";
    case HighO2CalibrationResult::Running:
      return "running";
    case HighO2CalibrationResult::Success:
      return "success";
    case HighO2CalibrationResult::Cancelled:
      return "cancelled";
    case HighO2CalibrationResult::Timeout:
      return "timeout";
    case HighO2CalibrationResult::NoRise:
      return "no-rise";
    case HighO2CalibrationResult::InvalidReadings:
      return "invalid-readings";
  }

  return "idle";
}

const char *high_o2_calibration_result_message(HighO2CalibrationResult result) {
  switch (result) {
    case HighO2CalibrationResult::Idle:
      return "High O2 calibration is idle.";
    case HighO2CalibrationResult::Running:
      return "High O2 calibration is running.";
    case HighO2CalibrationResult::Success:
      return "High O2 calibration completed successfully.";
    case HighO2CalibrationResult::Cancelled:
      return "High O2 calibration was cancelled.";
    case HighO2CalibrationResult::Timeout:
      return "High O2 calibration timed out before the cells stabilised.";
    case HighO2CalibrationResult::NoRise:
      return "High O2 calibration did not see a meaningful voltage rise.";
    case HighO2CalibrationResult::InvalidReadings:
      return "High O2 calibration stopped because the oxygen readings became invalid.";
  }

  return "High O2 calibration is idle.";
}

void reset_high_o2_calibration_window() {
  high_o2_calibration.window_head = 0;
  high_o2_calibration.window_count = 0;
  high_o2_calibration.last_window_p2p_mv = NAN;
  for (size_t i = 0; i < kHighO2CalibrationWindowSamples; ++i) {
    high_o2_calibration.window[i] = {};
  }
}

// Marks the calibration finished but leaves the last result available to the
// web page so the operator can see how it ended.
void finish_high_o2_calibration(HighO2CalibrationResult result) {
  high_o2_calibration.active = false;
  high_o2_calibration.last_result = result;
}

void cancel_high_o2_calibration(HighO2CalibrationResult result) {
  finish_high_o2_calibration(result);
  Serial.print(F("High O2 calibration ended: "));
  Serial.println(high_o2_calibration_result_message(result));
}

void start_high_o2_calibration(float gas_percent, unsigned long now_ms) {
  high_o2_calibration.active = true;
  high_o2_calibration.gas_percent = gas_percent;
  high_o2_calibration.gas_fraction = gas_percent / 100.0f;
  high_o2_calibration.started_ms = now_ms;
  high_o2_calibration.start_mean_mv = current_oxygen_mean_mv;
  high_o2_calibration.peak_mean_mv = current_oxygen_mean_mv;
  high_o2_calibration.observed_rise = false;
  high_o2_calibration.last_result = HighO2CalibrationResult::Running;
  reset_high_o2_calibration_window();

  Serial.print(F("Starting high O2 calibration at "));
  Serial.print(gas_percent, 1);
  Serial.print(F("% O2. Start mean="));
  Serial.print(current_oxygen_mean_mv, 3);
  Serial.println(F(" mV"));
}

// Each accepted sensor sample contributes one point to the stability window.
void append_high_o2_calibration_window_sample(float cell1_mv, float cell2_mv, float cell3_mv) {
  high_o2_calibration.window[high_o2_calibration.window_head].cell1_mv = cell1_mv;
  high_o2_calibration.window[high_o2_calibration.window_head].cell2_mv = cell2_mv;
  high_o2_calibration.window[high_o2_calibration.window_head].cell3_mv = cell3_mv;
  high_o2_calibration.window_head = (high_o2_calibration.window_head + 1) % kHighO2CalibrationWindowSamples;
  if (high_o2_calibration.window_count < kHighO2CalibrationWindowSamples) {
    ++high_o2_calibration.window_count;
  }
}

size_t high_o2_calibration_window_index(size_t logical_index) {
  const size_t oldest = (high_o2_calibration.window_head + kHighO2CalibrationWindowSamples - high_o2_calibration.window_count)
      % kHighO2CalibrationWindowSamples;
  return (oldest + logical_index) % kHighO2CalibrationWindowSamples;
}

float high_o2_calibration_window_max_p2p() {
  if (high_o2_calibration.window_count == 0) {
    return NAN;
  }

  float cell1_min = high_o2_calibration.window[high_o2_calibration_window_index(0)].cell1_mv;
  float cell1_max = cell1_min;
  float cell2_min = high_o2_calibration.window[high_o2_calibration_window_index(0)].cell2_mv;
  float cell2_max = cell2_min;
  float cell3_min = high_o2_calibration.window[high_o2_calibration_window_index(0)].cell3_mv;
  float cell3_max = cell3_min;

  for (size_t i = 1; i < high_o2_calibration.window_count; ++i) {
    const HighO2CalibrationWindowSample &sample = high_o2_calibration.window[high_o2_calibration_window_index(i)];
    cell1_min = min(cell1_min, sample.cell1_mv);
    cell1_max = max(cell1_max, sample.cell1_mv);
    cell2_min = min(cell2_min, sample.cell2_mv);
    cell2_max = max(cell2_max, sample.cell2_mv);
    cell3_min = min(cell3_min, sample.cell3_mv);
    cell3_max = max(cell3_max, sample.cell3_mv);
  }

  return max(cell1_max - cell1_min, max(cell2_max - cell2_min, cell3_max - cell3_min));
}

void high_o2_calibration_window_average(float &cell1_mv, float &cell2_mv, float &cell3_mv) {
  cell1_mv = 0.0f;
  cell2_mv = 0.0f;
  cell3_mv = 0.0f;
  if (high_o2_calibration.window_count == 0) {
    return;
  }

  for (size_t i = 0; i < high_o2_calibration.window_count; ++i) {
    const HighO2CalibrationWindowSample &sample = high_o2_calibration.window[high_o2_calibration_window_index(i)];
    cell1_mv += sample.cell1_mv;
    cell2_mv += sample.cell2_mv;
    cell3_mv += sample.cell3_mv;
  }

  const float divisor = static_cast<float>(high_o2_calibration.window_count);
  cell1_mv /= divisor;
  cell2_mv /= divisor;
  cell3_mv /= divisor;
}

// Calibration only succeeds after a clear rise above baseline and a stable
// peak-to-peak window across all three cells.
void maybe_complete_high_o2_calibration(unsigned long now_ms) {
  if (!high_o2_calibration.active) {
    return;
  }

  if (!isfinite(current_oxygen_cell1_mv) || !isfinite(current_oxygen_cell2_mv) || !isfinite(current_oxygen_cell3_mv)
      || current_oxygen_cell1_mv <= 0.0f || current_oxygen_cell2_mv <= 0.0f || current_oxygen_cell3_mv <= 0.0f) {
    cancel_high_o2_calibration(HighO2CalibrationResult::InvalidReadings);
    return;
  }

  append_high_o2_calibration_window_sample(current_oxygen_cell1_mv, current_oxygen_cell2_mv, current_oxygen_cell3_mv);
  high_o2_calibration.peak_mean_mv = max(high_o2_calibration.peak_mean_mv, current_oxygen_mean_mv);
  high_o2_calibration.observed_rise =
      (high_o2_calibration.peak_mean_mv - high_o2_calibration.start_mean_mv) >= kHighO2CalibrationMinRiseMv;
  high_o2_calibration.last_window_p2p_mv = high_o2_calibration_window_max_p2p();

  const unsigned long elapsed_ms = now_ms - high_o2_calibration.started_ms;
  if (elapsed_ms >= kHighO2CalibrationTimeoutMs) {
    cancel_high_o2_calibration(HighO2CalibrationResult::Timeout);
    return;
  }

  if (!high_o2_calibration.observed_rise && elapsed_ms >= kHighO2CalibrationRiseTimeoutMs) {
    cancel_high_o2_calibration(HighO2CalibrationResult::NoRise);
    return;
  }

  if (elapsed_ms < kHighO2CalibrationMinDurationMs
      || high_o2_calibration.window_count < kHighO2CalibrationWindowSamples
      || !high_o2_calibration.observed_rise
      || !isfinite(high_o2_calibration.last_window_p2p_mv)
      || high_o2_calibration.last_window_p2p_mv > kHighO2CalibrationStableP2pMv) {
    return;
  }

  float stable_cell1_mv = 0.0f;
  float stable_cell2_mv = 0.0f;
  float stable_cell3_mv = 0.0f;
  high_o2_calibration_window_average(stable_cell1_mv, stable_cell2_mv, stable_cell3_mv);
  if (stable_cell1_mv <= 0.0f || stable_cell2_mv <= 0.0f || stable_cell3_mv <= 0.0f) {
    cancel_high_o2_calibration(HighO2CalibrationResult::InvalidReadings);
    return;
  }

  oxygen_calibration_scale[0] = high_o2_calibration.gas_fraction / stable_cell1_mv;
  oxygen_calibration_scale[1] = high_o2_calibration.gas_fraction / stable_cell2_mv;
  oxygen_calibration_scale[2] = high_o2_calibration.gas_fraction / stable_cell3_mv;
  oxygen_calibrated = true;
  finish_high_o2_calibration(HighO2CalibrationResult::Success);

  Serial.print(F("High O2 calibration applied at "));
  Serial.print(high_o2_calibration.gas_percent, 1);
  Serial.print(F("% O2. Stable cells=["));
  Serial.print(stable_cell1_mv, 3);
  Serial.print(F(", "));
  Serial.print(stable_cell2_mv, 3);
  Serial.print(F(", "));
  Serial.print(stable_cell3_mv, 3);
  Serial.print(F("] mV, p2p="));
  Serial.print(high_o2_calibration.last_window_p2p_mv, 3);
  Serial.print(F(" mV, scales=["));
  Serial.print(oxygen_calibration_scale[0], 6);
  Serial.print(F(", "));
  Serial.print(oxygen_calibration_scale[1], 6);
  Serial.print(F(", "));
  Serial.print(oxygen_calibration_scale[2], 6);
  Serial.println(F("]"));
}

// ---- TFT rendering ----

String format_tft_significant(float value, uint8_t significant_figures) {
  if (!isfinite(value)) {
    return String("--");
  }
  if (value == 0.0f) {
    return String("0");
  }

  const float abs_value = fabsf(value);
  const int exponent = static_cast<int>(floorf(log10f(abs_value)));
  int decimals = static_cast<int>(significant_figures) - exponent - 1;
  if (decimals < 0) {
    decimals = 0;
  }
  if (decimals > 6) {
    decimals = 6;
  }

  return String(value, static_cast<unsigned int>(decimals));
}

void enable_board_power() {
#if defined(TFT_BACKLITE)
  pinMode(TFT_BACKLITE, OUTPUT);
  digitalWrite(TFT_BACKLITE, HIGH);
#endif

#if defined(TFT_I2C_POWER)
  pinMode(TFT_I2C_POWER, OUTPUT);
  digitalWrite(TFT_I2C_POWER, HIGH);
#endif

#if defined(NEOPIXEL_POWER)
  pinMode(NEOPIXEL_POWER, OUTPUT);
  digitalWrite(NEOPIXEL_POWER, NEOPIXEL_POWER_ON);
#endif

#if defined(LED_BUILTIN)
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
#endif
}

void init_display() {
  SPI.begin(SCK, MISO, MOSI, TFT_CS);
  tft.init(135, 240);
  tft.setRotation(0);
  tft.fillScreen(ST77XX_BLACK);
}

void show_message(const char *title, const char *message, uint16_t color) {
  tft.fillScreen(ST77XX_BLACK);
  tft.fillRect(0, 0, kScreenWidth, kHeaderHeight, color);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(8, 4);
  tft.print(title);
  tft.setCursor(10, 34);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.print(message);
}

// ---- Raw TFT history and bucketed web history ----

size_t history_index(size_t logical_index) {
  const size_t oldest = (history_head + kTftHistorySize - history_count) % kTftHistorySize;
  return (oldest + logical_index) % kTftHistorySize;
}

size_t displayed_history_index(size_t logical_index) {
  const size_t visible_count = displayed_history_count();
  const size_t oldest = (history_head + kTftHistorySize - visible_count) % kTftHistorySize;
  return (oldest + logical_index) % kTftHistorySize;
}

void append_history(float co2, float humidity, float temperature) {
  co2_history[history_head] = co2;
  humidity_history[history_head] = humidity;
  temperature_history[history_head] = temperature;
  history_head = (history_head + 1) % kTftHistorySize;
  if (history_count < kTftHistorySize) {
    ++history_count;
  }
}

size_t web_history_index(const WebHistory &history, size_t logical_index) {
  const size_t oldest = (history.head + kWebHistoryPoints - history.count) % kWebHistoryPoints;
  return (oldest + logical_index) % kWebHistoryPoints;
}

size_t latest_web_history_index(const WebHistory &history) {
  return (history.head + kWebHistoryPoints - 1) % kWebHistoryPoints;
}

WebHistory *find_web_history(uint16_t view_minutes) {
  for (size_t i = 0; i < kWebViewCount; ++i) {
    if (web_histories[i].span_minutes == view_minutes) {
      return &web_histories[i];
    }
  }
  return nullptr;
}

// Raw samples are accumulated into fixed-duration buckets so the web page can
// show longer views without holding every point in memory.
void append_web_history(
    WebHistory &history,
    uint32_t now_seconds,
    float co2,
    float humidity,
    float temperature,
    float oxygen_cell1_mv,
    float oxygen_cell2_mv,
    float oxygen_cell3_mv) {
  const uint32_t bucket_index = now_seconds / history.bucket_seconds;
  bool create_bucket = history.count == 0;

  if (!create_bucket) {
    const WebBucket &latest = history.buckets[latest_web_history_index(history)];
    create_bucket = latest.bucket_index != bucket_index;
  }

  if (create_bucket) {
    WebBucket &bucket = history.buckets[history.head];
    bucket = {};
    bucket.bucket_index = bucket_index;
    history.head = (history.head + 1) % kWebHistoryPoints;
    if (history.count < kWebHistoryPoints) {
      ++history.count;
    }
  }

  WebBucket &active_bucket = history.buckets[latest_web_history_index(history)];
  active_bucket.co2_sum += co2;
  active_bucket.humidity_sum += humidity;
  active_bucket.temperature_sum += temperature;
  active_bucket.oxygen_cell1_mv_sum += oxygen_cell1_mv;
  active_bucket.oxygen_cell2_mv_sum += oxygen_cell2_mv;
  active_bucket.oxygen_cell3_mv_sum += oxygen_cell3_mv;
  ++active_bucket.sample_count;
}

void append_web_histories(
    float co2,
    float humidity,
    float temperature,
    float oxygen_cell1_mv,
    float oxygen_cell2_mv,
    float oxygen_cell3_mv,
    uint32_t now_seconds) {
  for (size_t i = 0; i < kWebViewCount; ++i) {
    append_web_history(
        web_histories[i],
        now_seconds,
        co2,
        humidity,
        temperature,
        oxygen_cell1_mv,
        oxygen_cell2_mv,
        oxygen_cell3_mv);
  }
}

bool lookup_bucket_average(const WebHistory &history, uint32_t target_bucket, MetricType metric, float &out_value) {
  for (size_t i = 0; i < history.count; ++i) {
    const WebBucket &bucket = history.buckets[web_history_index(history, i)];
    if (bucket.bucket_index != target_bucket || bucket.sample_count == 0) {
      continue;
    }

    switch (metric) {
      case MetricType::Co2:
        out_value = bucket.co2_sum / static_cast<float>(bucket.sample_count);
        return true;
      case MetricType::Humidity:
        out_value = bucket.humidity_sum / static_cast<float>(bucket.sample_count);
        return true;
      case MetricType::Temperature:
        out_value = bucket.temperature_sum / static_cast<float>(bucket.sample_count);
        return true;
      case MetricType::OxygenCell1Mv:
        out_value = oxygen_display_value(bucket.oxygen_cell1_mv_sum / static_cast<float>(bucket.sample_count), 0);
        return true;
      case MetricType::OxygenCell2Mv:
        out_value = oxygen_display_value(bucket.oxygen_cell2_mv_sum / static_cast<float>(bucket.sample_count), 1);
        return true;
      case MetricType::OxygenCell3Mv:
        out_value = oxygen_display_value(bucket.oxygen_cell3_mv_sum / static_cast<float>(bucket.sample_count), 2);
        return true;
    }
  }

  return false;
}

bool web_history_bucket_bounds(const WebHistory &history, uint32_t &oldest_bucket, uint32_t &latest_bucket) {
  if (history.count == 0) {
    return false;
  }

  oldest_bucket = history.buckets[web_history_index(history, 0)].bucket_index;
  latest_bucket = history.buckets[latest_web_history_index(history)].bucket_index;
  return true;
}

// At startup the graph fills from the left edge. Once enough buckets exist to
// cover the selected span, the visible window starts to scroll.
uint32_t web_display_start_bucket(const WebHistory &history) {
  uint32_t oldest_bucket = 0;
  uint32_t latest_bucket = 0;
  if (!web_history_bucket_bounds(history, oldest_bucket, latest_bucket)) {
    return 0;
  }

  const uint32_t window_span = static_cast<uint32_t>(kWebHistoryPoints - 1);
  if (latest_bucket > oldest_bucket + window_span) {
    return latest_bucket - window_span;
  }

  return oldest_bucket;
}

size_t visible_web_point_count(const WebHistory &history) {
  const uint32_t first_bucket = web_display_start_bucket(history);

  size_t visible_points = 0;
  for (size_t i = 0; i < kWebHistoryPoints; ++i) {
    float ignored_value = 0.0f;
    if (lookup_bucket_average(history, first_bucket + static_cast<uint32_t>(i), MetricType::Co2, ignored_value)) {
      ++visible_points;
    }
  }

  return visible_points;
}

void graph_range(const float *series, float fallback_min, float fallback_max, float &out_min, float &out_max) {
  const size_t visible_count = displayed_history_count();
  if (visible_count == 0) {
    out_min = fallback_min;
    out_max = fallback_max;
    return;
  }

  float min_value = series[displayed_history_index(0)];
  float max_value = min_value;
  for (size_t i = 1; i < visible_count; ++i) {
    const float value = series[displayed_history_index(i)];
    min_value = min(min_value, value);
    max_value = max(max_value, value);
  }

  if (max_value - min_value < 0.5f) {
    min_value -= 0.5f;
    max_value += 0.5f;
  }

  const float padding = (max_value - min_value) * 0.15f;
  out_min = min(min_value - padding, fallback_min);
  out_max = max(max_value + padding, fallback_max);
}

void draw_graph(int x, int y, int width, int height, uint16_t color, const float *series, float fallback_min, float fallback_max) {
  tft.drawRoundRect(x, y, width, height, 4, ST77XX_WHITE);
  tft.drawFastHLine(x + 4, y + height / 2, width - 8, kGridColor);

  const size_t visible_count = displayed_history_count();
  if (visible_count < 2) {
    return;
  }

  float range_min = 0.0f;
  float range_max = 0.0f;
  graph_range(series, fallback_min, fallback_max, range_min, range_max);

  const int inner_x = x + 3;
  const int inner_y = y + 3;
  const int inner_width = width - 6;
  const int inner_height = height - 6;

  int prev_x = inner_x;
  int prev_y = inner_y + inner_height / 2;

  for (size_t i = 0; i < visible_count; ++i) {
    const float value = series[displayed_history_index(i)];
    const float normalized = (value - range_min) / (range_max - range_min);
    const size_t sample_span = visible_count > 1 ? (visible_count - 1) : 1;
    const int px = inner_x + static_cast<int>((static_cast<float>(i) / static_cast<float>(sample_span)) * (inner_width - 1));
    const int py = inner_y + inner_height - 1 - static_cast<int>(normalized * (inner_height - 1));
    if (i > 0) {
      tft.drawLine(prev_x, prev_y, px, py, color);
    }
    prev_x = px;
    prev_y = py;
  }

  tft.fillCircle(prev_x, prev_y, 2, color);
}

void draw_tft_row(
    int row_index,
    const __FlashStringHelper *label,
    float value,
    const char *unit,
    uint16_t accent_color,
    uint16_t value_color) {
  const int section_y = row_index * kTftRowHeight;
  const int panel_y = section_y + kPanelInset;
  const int panel_height = kTftRowHeight - (2 * kPanelInset);
  const int panel_x = 4;
  const int panel_width = kScreenWidth - 8;
  const int value_area_top = panel_y + 16;
  const int value_area_height = panel_height - 16;
  const String value_text = format_tft_significant(value, 3);
  int16_t unit_x1 = 0;
  int16_t unit_y1 = 0;
  uint16_t unit_w = 0;
  uint16_t unit_h = 0;
  int16_t value_x1 = 0;
  int16_t value_y1 = 0;
  uint16_t value_w = 0;
  uint16_t value_h = 0;

  tft.drawRoundRect(panel_x, panel_y, panel_width, panel_height, 6, accent_color);
  tft.setTextColor(accent_color);
  tft.setTextSize(1);
  tft.setCursor(panel_x + 6, panel_y + 7);
  tft.print(label);

  tft.getTextBounds(unit, 0, 0, &unit_x1, &unit_y1, &unit_w, &unit_h);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(panel_x + panel_width - static_cast<int>(unit_w) - 7, panel_y + 7);
  tft.print(unit);

  tft.setTextSize(3);
  tft.getTextBounds(value_text, 0, 0, &value_x1, &value_y1, &value_w, &value_h);
  tft.setTextColor(value_color);
  tft.setCursor(
      panel_x + ((panel_width - static_cast<int>(value_w)) / 2) - value_x1,
      value_area_top + ((value_area_height - static_cast<int>(value_h)) / 2) - value_y1);
  tft.print(value_text);
}

void draw_dashboard() {
  tft.fillScreen(ST77XX_BLACK);

  draw_tft_row(0, F("Oxygen"), oxygen_display_mean(), oxygen_display_unit(), ST77XX_WHITE, oxygen_calibrated ? ST77XX_GREEN : ST77XX_RED);
  draw_tft_row(1, F("CO2"), current_co2, "ppm", ST77XX_CYAN, ST77XX_WHITE);
  draw_tft_row(2, F("Temp"), current_temperature, "C", ST77XX_YELLOW, ST77XX_WHITE);
  draw_tft_row(3, F("Humidity"), current_humidity, "%RH", ST77XX_GREEN, ST77XX_WHITE);
}

// ---- Time handling ----

String ip_address_string() {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("");
}

bool time_source_confirmed() {
  return time_confirmed && last_confirmed_epoch_seconds >= kConfirmedEpochThreshold;
}

int64_t epoch_seconds_for_uptime(uint32_t uptime_seconds) {
  return confirmed_epoch_offset_seconds + static_cast<int64_t>(uptime_seconds);
}

String format_local_time_string(time_t epoch_seconds) {
  struct tm time_info;
  if (!localtime_r(&epoch_seconds, &time_info)) {
    return String("");
  }

  char buffer[32] = {0};
  if (strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &time_info) == 0) {
    return String("");
  }

  return String(buffer);
}

// Once NTP provides a trustworthy epoch, uptime-derived sample timestamps can
// be translated into real New Zealand local time for the web graph axis.
void note_confirmed_time(unsigned long now_ms, time_t epoch_seconds) {
  confirmed_epoch_offset_seconds = static_cast<int64_t>(epoch_seconds) - static_cast<int64_t>(now_ms / 1000UL);
  last_confirmed_epoch_seconds = epoch_seconds;

  if (!time_confirmed) {
    Serial.print(F("Time synced from NTP ("));
    Serial.print(kTimeZoneName);
    Serial.print(F("): "));
    Serial.println(format_local_time_string(epoch_seconds));
  }

  time_confirmed = true;
}

void start_time_sync(unsigned long now_ms) {
  configTzTime(kNzTimeZone, kNtpServerPrimary, kNtpServerSecondary, kNtpServerTertiary);
  time_sync_started = true;
  last_time_sync_attempt_ms = now_ms;
  Serial.print(F("Starting NTP time sync for "));
  Serial.println(kTimeZoneName);
}

void ensure_time_source(unsigned long now_ms) {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!time_sync_started || (!time_source_confirmed() && now_ms - last_time_sync_attempt_ms >= kTimeSyncRetryIntervalMs)) {
    start_time_sync(now_ms);
  }

  if (now_ms - last_time_sync_check_ms < kTimeSyncPollIntervalMs) {
    return;
  }

  last_time_sync_check_ms = now_ms;
  const time_t epoch_now = time(nullptr);
  if (epoch_now >= kConfirmedEpochThreshold) {
    note_confirmed_time(now_ms, epoch_now);
  }
}

// ---- Web JSON and HTTP handlers ----

bool axis_start_epoch_seconds(const WebHistory &history, int64_t &out_epoch_seconds) {
  if (!time_source_confirmed()) {
    return false;
  }

  const uint32_t first_bucket = web_display_start_bucket(history);
  out_epoch_seconds = epoch_seconds_for_uptime(first_bucket * history.bucket_seconds);
  return true;
}

void append_json_web_series(String &json, const char *name, const WebHistory &history, MetricType metric, uint8_t decimals) {
  json += "\"";
  json += name;
  json += "\":[";

  const uint32_t first_bucket = web_display_start_bucket(history);

  for (size_t i = 0; i < kWebHistoryPoints; ++i) {
    if (i > 0) {
      json += ",";
    }

    float value = 0.0f;
    if (lookup_bucket_average(history, first_bucket + static_cast<uint32_t>(i), metric, value)) {
      json += String(value, static_cast<unsigned int>(decimals));
    } else {
      json += "null";
    }
  }

  json += "]";
}

String build_readings_json(uint16_t requested_view_minutes) {
  const WebHistory *selected_history = find_web_history(requested_view_minutes);
  if (selected_history == nullptr) {
    selected_history = find_web_history(kDefaultWebViewMinutes);
  }
  int64_t axis_start_epoch = 0;
  const bool has_confirmed_time = axis_start_epoch_seconds(*selected_history, axis_start_epoch);

  String json;
  json.reserve(7168);
  json += "{";
  json += "\"connected\":";
  json += (WiFi.status() == WL_CONNECTED) ? "true" : "false";
  json += ",\"sensorReady\":";
  json += sensor_ready ? "true" : "false";
  json += ",\"adsReady\":";
  json += ads_ready ? "true" : "false";
  json += ",\"oxygenCalibrated\":";
  json += oxygen_calibrated ? "true" : "false";
  json += ",\"ip\":\"";
  json += ip_address_string();
  json += "\"";
  json += ",\"rssi\":";
  json += String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  json += ",\"sampleCount\":";
  json += String(sample_count);
  json += ",\"measurementInterval\":";
  json += String(measurement_interval_seconds);
  json += ",\"viewMinutes\":";
  json += String(selected_history->span_minutes);
  json += ",\"bucketSeconds\":";
  json += String(selected_history->bucket_seconds);
  json += ",\"timeConfirmed\":";
  json += has_confirmed_time ? "true" : "false";
  json += ",\"timeMode\":\"";
  json += has_confirmed_time ? "nz-local" : "uptime";
  json += "\"";
  json += ",\"timeZone\":\"";
  json += kTimeZoneName;
  json += "\"";
  json += ",\"axisStartEpochSeconds\":";
  if (has_confirmed_time) {
    json += String(static_cast<long>(axis_start_epoch));
  } else {
    json += "null";
  }
  json += ",\"pointCount\":";
  json += String(kWebHistoryPoints);
  json += ",\"visiblePointCount\":";
  json += String(visible_web_point_count(*selected_history));
  json += ",\"tftHistoryWindow\":";
  json += String(tft_history_window);
  json += ",\"tftHistoryCount\":";
  json += String(displayed_history_count());
  json += ",\"co2\":";
  json += String(current_co2, 0);
  json += ",\"humidity\":";
  json += String(current_humidity, 1);
  json += ",\"temperature\":";
  json += String(current_temperature, 1);
  json += ",\"oxygenUnit\":\"";
  json += oxygen_display_unit();
  json += "\"";
  json += ",\"oxygenDecimals\":";
  json += String(oxygen_display_decimals());
  json += ",\"oxygenCell1\":";
  json += String(oxygen_display_value(current_oxygen_cell1_mv, 0), static_cast<unsigned int>(oxygen_display_decimals()));
  json += ",\"oxygenCell2\":";
  json += String(oxygen_display_value(current_oxygen_cell2_mv, 1), static_cast<unsigned int>(oxygen_display_decimals()));
  json += ",\"oxygenCell3\":";
  json += String(oxygen_display_value(current_oxygen_cell3_mv, 2), static_cast<unsigned int>(oxygen_display_decimals()));
  json += ",\"oxygenMean\":";
  json += String(oxygen_display_mean(), static_cast<unsigned int>(oxygen_display_decimals()));
  json += ",\"oxygenReferencePpo2\":";
  json += String(kAirOxygenPartialPressure, 4);
  json += ",\"highO2CalActive\":";
  json += high_o2_calibration.active ? "true" : "false";
  json += ",\"highO2CalTargetPercent\":";
  json += String(high_o2_calibration.gas_percent, 1);
  json += ",\"highO2CalObservedRise\":";
  json += high_o2_calibration.observed_rise ? "true" : "false";
  json += ",\"highO2CalWindowCount\":";
  json += String(high_o2_calibration.window_count);
  json += ",\"highO2CalWindowP2pMv\":";
  if (isfinite(high_o2_calibration.last_window_p2p_mv)) {
    json += String(high_o2_calibration.last_window_p2p_mv, 3);
  } else {
    json += "null";
  }
  json += ",\"highO2CalStartMv\":";
  json += String(high_o2_calibration.start_mean_mv, 3);
  json += ",\"highO2CalPeakMv\":";
  json += String(high_o2_calibration.peak_mean_mv, 3);
  json += ",\"highO2CalElapsedSeconds\":";
  json += String(high_o2_calibration.active ? (millis() - high_o2_calibration.started_ms) / 1000UL : 0);
  json += ",\"highO2CalResult\":\"";
  json += high_o2_calibration_result_code(high_o2_calibration.last_result);
  json += "\"";
  json += ",\"highO2CalMessage\":\"";
  json += high_o2_calibration_result_message(high_o2_calibration.last_result);
  json += "\"";
  json += ",\"views\":[5,15,30,60],";
  append_json_web_series(json, "co2History", *selected_history, MetricType::Co2, 0);
  json += ",";
  append_json_web_series(json, "humidityHistory", *selected_history, MetricType::Humidity, 1);
  json += ",";
  append_json_web_series(json, "temperatureHistory", *selected_history, MetricType::Temperature, 1);
  json += ",";
  append_json_web_series(json, "oxygenCell1History", *selected_history, MetricType::OxygenCell1Mv, 3);
  json += ",";
  append_json_web_series(json, "oxygenCell2History", *selected_history, MetricType::OxygenCell2Mv, 3);
  json += ",";
  append_json_web_series(json, "oxygenCell3History", *selected_history, MetricType::OxygenCell3Mv, 3);
  json += "}";
  return json;
}

String build_settings_response(bool ok, const char *error_message = nullptr) {
  String json;
  json.reserve(640);
  json += "{";
  json += "\"ok\":";
  json += ok ? "true" : "false";
  json += ",\"measurementInterval\":";
  json += String(measurement_interval_seconds);
  json += ",\"views\":[5,15,30,60]";
  json += ",\"oxygenCalibrated\":";
  json += oxygen_calibrated ? "true" : "false";
  json += ",\"oxygenUnit\":\"";
  json += oxygen_display_unit();
  json += "\"";
  json += ",\"referencePpo2\":";
  json += String(kAirOxygenPartialPressure, 4);
  json += ",\"highO2CalActive\":";
  json += high_o2_calibration.active ? "true" : "false";
  json += ",\"highO2CalTargetPercent\":";
  json += String(high_o2_calibration.gas_percent, 1);
  json += ",\"highO2CalObservedRise\":";
  json += high_o2_calibration.observed_rise ? "true" : "false";
  json += ",\"highO2CalWindowCount\":";
  json += String(high_o2_calibration.window_count);
  json += ",\"highO2CalWindowP2pMv\":";
  if (isfinite(high_o2_calibration.last_window_p2p_mv)) {
    json += String(high_o2_calibration.last_window_p2p_mv, 3);
  } else {
    json += "null";
  }
  json += ",\"highO2CalStartMv\":";
  json += String(high_o2_calibration.start_mean_mv, 3);
  json += ",\"highO2CalPeakMv\":";
  json += String(high_o2_calibration.peak_mean_mv, 3);
  json += ",\"highO2CalElapsedSeconds\":";
  json += String(high_o2_calibration.active ? (millis() - high_o2_calibration.started_ms) / 1000UL : 0);
  json += ",\"highO2CalResult\":\"";
  json += high_o2_calibration_result_code(high_o2_calibration.last_result);
  json += "\"";
  json += ",\"highO2CalMessage\":\"";
  json += high_o2_calibration_result_message(high_o2_calibration.last_result);
  json += "\"";
  if (error_message != nullptr) {
    json += ",\"error\":\"";
    json += error_message;
    json += "\"";
  }
  json += "}";
  return json;
}

// The API is intentionally small: one live readings endpoint, one settings
// endpoint, and explicit endpoints for the two oxygen calibration workflows.
void handle_root() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, PSTR("text/html"), kIndexHtml);
}

void handle_api_readings() {
  uint16_t requested_view_minutes = kDefaultWebViewMinutes;
  if (server.hasArg("view")) {
    const long candidate_view = server.arg("view").toInt();
    if (candidate_view > 0 && find_web_history(static_cast<uint16_t>(candidate_view)) != nullptr) {
      requested_view_minutes = static_cast<uint16_t>(candidate_view);
    }
  }

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", build_readings_json(requested_view_minutes));
}

void handle_api_settings() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", build_settings_response(false, "method not allowed"));
    return;
  }

  if (!server.hasArg("measurementInterval")) {
    server.send(400, "application/json", build_settings_response(false, "missing measurement interval"));
    return;
  }

  const long requested_interval = server.arg("measurementInterval").toInt();
  if (requested_interval < kMinMeasurementInterval || requested_interval > kMaxMeasurementInterval) {
    server.send(400, "application/json", build_settings_response(false, "invalid measurement interval"));
    return;
  }

  const uint16_t new_interval = static_cast<uint16_t>(requested_interval);
  if (!scd30.setMeasurementInterval(new_interval)) {
    server.send(500, "application/json", build_settings_response(false, "sensor rejected interval"));
    return;
  }

  measurement_interval_seconds = new_interval;
  draw_dashboard();

  Serial.print(F("Updated settings: interval="));
  Serial.print(measurement_interval_seconds);
  Serial.println(F("s"));

  server.send(200, "application/json", build_settings_response(true));
}

void handle_api_calibrate_air() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", build_settings_response(false, "method not allowed"));
    return;
  }

  if (high_o2_calibration.active) {
    server.send(400, "application/json", build_settings_response(false, "high o2 calibration running"));
    return;
  }

  if (!ads_ready) {
    server.send(400, "application/json", build_settings_response(false, "ads1115 offline"));
    return;
  }

  if (current_oxygen_cell1_mv <= 0.0f || current_oxygen_cell2_mv <= 0.0f || current_oxygen_cell3_mv <= 0.0f) {
    server.send(400, "application/json", build_settings_response(false, "oxygen readings unavailable"));
    return;
  }

  oxygen_calibration_scale[0] = kAirOxygenPartialPressure / current_oxygen_cell1_mv;
  oxygen_calibration_scale[1] = kAirOxygenPartialPressure / current_oxygen_cell2_mv;
  oxygen_calibration_scale[2] = kAirOxygenPartialPressure / current_oxygen_cell3_mv;
  oxygen_calibrated = true;

  Serial.print(F("Oxygen calibration applied in air at "));
  Serial.print(kAirOxygenPartialPressure, 4);
  Serial.print(F(" ppO2. Scales=["));
  Serial.print(oxygen_calibration_scale[0], 6);
  Serial.print(F(", "));
  Serial.print(oxygen_calibration_scale[1], 6);
  Serial.print(F(", "));
  Serial.print(oxygen_calibration_scale[2], 6);
  Serial.println(F("]"));

  server.send(200, "application/json", build_settings_response(true));
}

void handle_api_calibrate_high_o2_start() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", build_settings_response(false, "method not allowed"));
    return;
  }

  if (!ads_ready) {
    server.send(400, "application/json", build_settings_response(false, "ads1115 offline"));
    return;
  }

  if (high_o2_calibration.active) {
    server.send(400, "application/json", build_settings_response(false, "high o2 calibration already running"));
    return;
  }

  if (!server.hasArg("gasPercent")) {
    server.send(400, "application/json", build_settings_response(false, "missing gas percent"));
    return;
  }

  const float gas_percent = server.arg("gasPercent").toFloat();
  if (!isfinite(gas_percent) || gas_percent < kMinHighO2CalibrationPercent || gas_percent > kMaxHighO2CalibrationPercent) {
    server.send(400, "application/json", build_settings_response(false, "invalid gas percent"));
    return;
  }

  if (current_oxygen_cell1_mv <= 0.0f || current_oxygen_cell2_mv <= 0.0f || current_oxygen_cell3_mv <= 0.0f) {
    server.send(400, "application/json", build_settings_response(false, "oxygen readings unavailable"));
    return;
  }

  start_high_o2_calibration(gas_percent, millis());
  server.send(200, "application/json", build_settings_response(true));
}

void handle_api_calibrate_high_o2_cancel() {
  if (server.method() != HTTP_POST) {
    server.send(405, "application/json", build_settings_response(false, "method not allowed"));
    return;
  }

  if (!high_o2_calibration.active) {
    server.send(400, "application/json", build_settings_response(false, "high o2 calibration not running"));
    return;
  }

  cancel_high_o2_calibration(HighO2CalibrationResult::Cancelled);
  server.send(200, "application/json", build_settings_response(true));
}

void handle_health() {
  server.send(200, "text/plain", "ok");
}

void handle_not_found() {
  server.send(404, "text/plain", "Not found");
}

// ---- Wi-Fi, mDNS, and sensor bring-up ----

void maybe_start_mdns() {
  if (mdns_started) {
    return;
  }
  if (MDNS.begin(kHostname)) {
    mdns_started = true;
    Serial.print(F("mDNS started: http://"));
    Serial.print(kHostname);
    Serial.println(F(".local/"));
  } else {
    Serial.println(F("mDNS start failed"));
  }
}

void start_web_server() {
  if (web_server_started) {
    return;
  }

  server.on("/", HTTP_GET, handle_root);
  server.on("/api/readings", HTTP_GET, handle_api_readings);
  server.on("/api/settings", HTTP_POST, handle_api_settings);
  server.on("/api/calibrate-air", HTTP_POST, handle_api_calibrate_air);
  server.on("/api/calibrate-high-o2/start", HTTP_POST, handle_api_calibrate_high_o2_start);
  server.on("/api/calibrate-high-o2/cancel", HTTP_POST, handle_api_calibrate_high_o2_cancel);
  server.on("/health", HTTP_GET, handle_health);
  server.onNotFound(handle_not_found);
  server.begin();
  web_server_started = true;

  Serial.print(F("HTTP server started: http://"));
  Serial.print(ip_address_string());
  Serial.println(F("/"));
}

void announce_wifi() {
  Serial.print(F("Connected to WiFi SSID "));
  Serial.println(WIFI_SSID);
  Serial.print(F("IP address: "));
  Serial.println(ip_address_string());
  Serial.print(F("RSSI: "));
  Serial.print(WiFi.RSSI());
  Serial.println(F(" dBm"));
}

bool connect_wifi(unsigned long timeout_ms) {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(kHostname);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print(F("Connecting to WiFi"));
  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
    delay(250);
    Serial.print(F("."));
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.print(F("WiFi connect failed, status="));
    Serial.println(static_cast<int>(WiFi.status()));
    return false;
  }

  announce_wifi();
  start_time_sync(millis());
  ensure_time_source(millis());
  maybe_start_mdns();
  start_web_server();
  return true;
}

// Wi-Fi loss should not stop local sensing. The loop keeps running and retries
// connectivity in the background on a simple interval.
void ensure_wifi_connected(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  if (now - last_wifi_retry_ms < kWifiRetryIntervalMs) {
    return;
  }

  last_wifi_retry_ms = now;
  Serial.println(F("WiFi disconnected, retrying"));
  connect_wifi(kRetryWifiTimeoutMs);
}

void init_i2c_bus() {
  Wire.begin();
  Wire.setClock(100000);
}

// The SCD30 controls the main sampling cadence. If it is missing, the app can
// still host the web page but cannot produce live environmental samples.
bool init_sensor() {
  if (!scd30.begin()) {
    Serial.println(F("Failed to find SCD30 chip on I2C"));
    show_message("SCD30 status", "Sensor missing", ST77XX_RED);
    return false;
  }

  if (!scd30.setMeasurementInterval(measurement_interval_seconds)) {
    Serial.println(F("Failed to set SCD30 measurement interval"));
  }

  Serial.println(F("SCD30 Found"));
  Serial.print(F("Measurement Interval: "));
  Serial.print(measurement_interval_seconds);
  Serial.println(F(" seconds"));
  show_message("SCD30 status", "Waiting data", ST77XX_BLUE);
  return true;
}

bool init_ads1115() {
  if (!ads.begin()) {
    Serial.println(F("Failed to find ADS1115 on I2C"));
    return false;
  }

  ads.setGain(kAdsGain);
  Serial.println(F("ADS1115 Found"));
  return true;
}

}  // namespace

// ---- Arduino entry points ----

void setup() {
  Serial.begin(115200);
  const unsigned long serial_wait_start = millis();
  while (!Serial && millis() - serial_wait_start < 4000) {
    delay(10);
  }

  enable_board_power();
  init_display();
  init_web_histories();
  init_i2c_bus();
  randomSeed(static_cast<unsigned long>(micros()));
  init_synthetic_oxygen_cells();

  Serial.println();
  Serial.println(F("Adafruit Feather ESP32-S2 Reverse TFT + SCD30 + ADS1115 + WiFi"));
  Serial.print(F("Synthetic O2 cell 2: lag="));
  Serial.print(synthetic_oxygen_cells[0].lag_seconds, 3);
  Serial.print(F("s, offset="));
  Serial.print(synthetic_oxygen_cells[0].offset_mv, 3);
  Serial.println(F(" mV"));
  Serial.print(F("Synthetic O2 cell 3: lag="));
  Serial.print(synthetic_oxygen_cells[1].lag_seconds, 3);
  Serial.print(F("s, offset="));
  Serial.print(synthetic_oxygen_cells[1].offset_mv, 3);
  Serial.println(F(" mV"));

  sensor_ready = init_sensor();
  ads_ready = init_ads1115();
  last_wifi_retry_ms = millis();
  connect_wifi(kInitialWifiTimeoutMs);
}

void loop() {
  const unsigned long now = millis();

#if defined(LED_BUILTIN)
  if (now - last_blink_ms >= kBlinkIntervalMs) {
    last_blink_ms = now;
    led_state = !led_state;
    digitalWrite(LED_BUILTIN, led_state ? HIGH : LOW);
  }
#endif

  if (web_server_started) {
    server.handleClient();
  }

  ensure_wifi_connected(now);
  ensure_time_source(now);

  // The display and web server continue to run even if the SCD30 is offline.
  if (!sensor_ready) {
    delay(50);
    return;
  }

  if (!scd30.dataReady()) {
    delay(50);
    return;
  }

  if (!scd30.read()) {
    Serial.println(F("Error reading SCD30 data"));
    delay(200);
    return;
  }

  if (sample_count == 0 && scd30.CO2 <= 0.0f) {
    Serial.println(F("Ignoring initial zero CO2 sample"));
    delay(100);
    return;
  }

  current_co2 = scd30.CO2;
  current_humidity = scd30.relative_humidity;
  current_temperature = scd30.temperature;
  if (ads_ready) {
    const uint32_t now_seconds = static_cast<uint32_t>(now / 1000UL);
    const int16_t oxygen_counts = ads.readADC_SingleEnded(0);
    current_oxygen_cell1_mv = ads.computeVolts(oxygen_counts) * 1000.0f;
    append_oxygen_source_sample(now_seconds, current_oxygen_cell1_mv);

    // Cells 2 and 3 are synthetic variants of the real cell with fixed per-boot
    // lag and offset so the final multi-cell UX can be developed now.
    float lagged_oxygen_mv = current_oxygen_cell1_mv;
    if (!lookup_lagged_oxygen(synthetic_oxygen_cells[0].lag_seconds, now_seconds, lagged_oxygen_mv)) {
      lagged_oxygen_mv = current_oxygen_cell1_mv;
    }
    current_oxygen_cell2_mv = lagged_oxygen_mv + synthetic_oxygen_cells[0].offset_mv;

    lagged_oxygen_mv = current_oxygen_cell1_mv;
    if (!lookup_lagged_oxygen(synthetic_oxygen_cells[1].lag_seconds, now_seconds, lagged_oxygen_mv)) {
      lagged_oxygen_mv = current_oxygen_cell1_mv;
    }
    current_oxygen_cell3_mv = lagged_oxygen_mv + synthetic_oxygen_cells[1].offset_mv;

    current_oxygen_mean_mv = (current_oxygen_cell1_mv + current_oxygen_cell2_mv + current_oxygen_cell3_mv) / 3.0f;
  } else {
    current_oxygen_cell1_mv = 0.0f;
    current_oxygen_cell2_mv = 0.0f;
    current_oxygen_cell3_mv = 0.0f;
    current_oxygen_mean_mv = 0.0f;
  }

  // Calibration progresses on the same accepted sample cadence that drives the
  // TFT, web dashboard, and serial logs.
  maybe_complete_high_o2_calibration(now);

  append_history(current_co2, current_humidity, current_temperature);
  append_web_histories(
      current_co2,
      current_humidity,
      current_temperature,
      current_oxygen_cell1_mv,
      current_oxygen_cell2_mv,
      current_oxygen_cell3_mv,
      static_cast<uint32_t>(now / 1000UL));
  ++sample_count;
  draw_dashboard();

  Serial.print(F("Sample "));
  Serial.print(sample_count);
  Serial.print(F(": CO2="));
  Serial.print(current_co2, 0);
  Serial.print(F(" ppm, Humidity="));
  Serial.print(current_humidity, 1);
  Serial.print(F(" %RH, Temp="));
  Serial.print(current_temperature, 1);
  Serial.print(F(" C, IP="));
  Serial.print(ip_address_string());
  Serial.print(F(", O2Cells=["));
  Serial.print(current_oxygen_cell1_mv, 3);
  Serial.print(F(", "));
  Serial.print(current_oxygen_cell2_mv, 3);
  Serial.print(F(", "));
  Serial.print(current_oxygen_cell3_mv, 3);
  Serial.print(F("] mV, O2Mean="));
  Serial.print(current_oxygen_mean_mv, 3);
  Serial.print(F(" mV, TimeSource="));
  Serial.print(time_source_confirmed() ? F("NZ local") : F("uptime"));
  if (time_source_confirmed()) {
    Serial.print(F(", NZTime="));
    Serial.print(format_local_time_string(static_cast<time_t>(epoch_seconds_for_uptime(static_cast<uint32_t>(now / 1000UL)))));
  }
  Serial.println();
}

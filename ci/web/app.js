// Cerid CI web UI. Plain JavaScript, no build step, served by the coordinator (ADR-0135 §16, design in
// docs/design/ci-platform.md#the-web-ui). Information order on every page: status first, then what failed and why,
// then the logs. Live updates arrive by Server-Sent Events; every value from the API is escaped before it is shown.
'use strict';

// ------------------------------------------------------------------------------------------- pure helpers (tested)

const ESC = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
function esc(text) {
  return String(text === undefined || text === null ? '' : text).replace(/[&<>"']/g, (c) => ESC[c]);
}

const ANSI = /\x1b\[([0-9;]*)([A-Za-z])|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)/g;

function stripAnsi(text) {
  return String(text).replace(ANSI, '');
}

// ANSI SGR colours to HTML (0 reset, 1 bold, 22 normal, 30-37 and 90-97 foreground, 39 default); every other escape
// sequence is dropped. The text is escaped first.
function ansiToHtml(text) {
  let out = '';
  let bold = false;
  let fg = null;
  let last = 0;
  let match;
  const pattern = new RegExp(ANSI.source, 'g');
  const emit = (chunk) => {
    if (!chunk) {
      return;
    }
    const classes = [];
    if (bold) {
      classes.push('ansi-bold');
    }
    if (fg !== null) {
      classes.push('fg-' + fg);
    }
    out += classes.length ? '<span class="' + classes.join(' ') + '">' + esc(chunk) + '</span>' : esc(chunk);
  };
  while ((match = pattern.exec(text)) !== null) {
    emit(text.slice(last, match.index));
    last = pattern.lastIndex;
    if (match[2] !== 'm') {
      continue;
    }
    for (const part of (match[1] || '0').split(';')) {
      const code = parseInt(part || '0', 10);
      if (code === 0) {
        bold = false;
        fg = null;
      } else if (code === 1) {
        bold = true;
      } else if (code === 22) {
        bold = false;
      } else if (code === 39) {
        fg = null;
      } else if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97)) {
        fg = code;
      }
    }
  }
  emit(text.slice(last));
  return out;
}

// A line that reports an error: compiler and linker errors, failed tests, assertions, tracebacks, crashes. Counts of
// zero ("0 errors") and the agent's own progress lines are not errors.
const ERROR_PATTERNS = [/\berror\b\s*[:A-Z0-9(]/i, /\bfatal\b/i, /\bFAILED\b/, /^\s*FAIL\b/, /\bfailed\b/i,
  /Traceback \(most recent call last\)/, /\b\w*(Error|Exception):/, /\bpanic:/, /Segmentation fault/i,
  /Access violation/i, /\bAddressSanitizer\b|\bThreadSanitizer\b|\bUndefinedBehaviorSanitizer\b/, /\bLNK\d{4}\b/,
  /\b[CE]\d{4}\b:/];
const NOT_ERROR = [/\b0 (errors?|failures?|failed)\b/i, /\bno (errors?|failures?)\b/i, /^~~~ (?!agent error)/,
  /100% tests passed/, /--output-on-failure/];
function isErrorLine(line) {
  const text = stripAnsi(line);
  if (NOT_ERROR.some((p) => p.test(text))) {
    return false;
  }
  return ERROR_PATTERNS.some((p) => p.test(text));
}

// Add a chunk of live log text to `lines`. `open` says whether the last line is still unfinished (the previous
// chunk did not end with a newline); returns the same for the text now added.
function appendLog(lines, open, text) {
  if (!text) {
    return open;
  }
  const parts = String(text).replace(/\r\n/g, '\n').split('\n');
  const complete = parts[parts.length - 1] === '';
  if (complete) {
    parts.pop();
  }
  if (open && lines.length && parts.length) {
    lines[lines.length - 1] += parts.shift();
  }
  for (const part of parts) {
    lines.push(part);
  }
  return !complete;
}

function firstErrorLine(lines) {
  for (let i = 0; i < lines.length; i += 1) {
    if (isErrorLine(lines[i])) {
      return i;
    }
  }
  return -1;
}

// A small, safe Markdown subset for annotations: headings, bold, inline code, fenced code, lists and tables.
function markdown(text) {
  const lines = String(text).split('\n');
  const out = [];
  let i = 0;
  const inline = (s) => esc(s).replace(/`([^`]+)`/g, '<code>$1</code>').replace(/\*\*([^*]+)\*\*/g, '<b>$1</b>');
  while (i < lines.length) {
    const line = lines[i];
    if (line.startsWith('```')) {
      const body = [];
      i += 1;
      while (i < lines.length && !lines[i].startsWith('```')) {
        body.push(lines[i]);
        i += 1;
      }
      out.push('<pre>' + esc(body.join('\n')) + '</pre>');
      i += 1;
      continue;
    }
    const heading = /^(#{1,4})\s+(.*)$/.exec(line);
    if (heading) {
      const level = Math.min(heading[1].length + 2, 6);
      out.push('<h' + level + '>' + inline(heading[2]) + '</h' + level + '>');
    } else if (/^\s*\|/.test(line)) {
      const rows = [];
      while (i < lines.length && /^\s*\|/.test(lines[i])) {
        if (!/^\s*\|[\s:|-]+\|\s*$/.test(lines[i])) {
          rows.push(lines[i].trim().replace(/^\||\|$/g, '').split('|').map((c) => '<td>' + inline(c.trim()) + '</td>'));
        }
        i += 1;
      }
      out.push('<div class="table-wrap"><table>' + rows.map((r) => '<tr>' + r.join('') + '</tr>').join('') +
        '</table></div>');
      continue;
    } else if (/^\s*[-*]\s+/.test(line)) {
      const items = [];
      while (i < lines.length && /^\s*[-*]\s+/.test(lines[i])) {
        items.push('<li>' + inline(lines[i].replace(/^\s*[-*]\s+/, '')) + '</li>');
        i += 1;
      }
      out.push('<ul>' + items.join('') + '</ul>');
      continue;
    } else if (line.trim()) {
      out.push('<p>' + inline(line) + '</p>');
    }
    i += 1;
  }
  return out.join('');
}

// Layered layout of the step graph: a step's column is one past its deepest dependency.
function layout(jobs) {
  const byKey = {};
  for (const job of jobs) {
    byKey[job.key] = job;
  }
  const depth = {};
  const visit = (key, trail) => {
    if (depth[key] !== undefined) {
      return depth[key];
    }
    if (trail.has(key) || !byKey[key]) {
      return 0;
    }
    trail.add(key);
    let d = 0;
    for (const dep of byKey[key].depends_on || []) {
      d = Math.max(d, visit(dep.step, trail) + 1);
    }
    trail.delete(key);
    depth[key] = d;
    return d;
  };
  const columns = [];
  for (const job of jobs) {
    const d = visit(job.key, new Set());
    (columns[d] = columns[d] || []).push(job);
  }
  const place = {};
  columns.forEach((column, x) => column.forEach((job, y) => {
    place[job.key] = { x: x, y: y };
  }));
  return { columns: columns, place: place };
}

function cleanLabel(label) {
  return String(label || '').replace(/^(\s*:[a-z0-9_+-]+:\s*)+/i, '').trim() || String(label || '');
}

function durationText(start, end, now) {
  if (!start) {
    return '';
  }
  const seconds = Math.max(0, Math.round((end || now || Date.now() / 1000) - start));
  const m = Math.floor(seconds / 60);
  if (m >= 60) {
    return Math.floor(m / 60) + 'h ' + (m % 60) + 'm';
  }
  return m ? m + 'm ' + (seconds % 60) + 's' : seconds + 's';
}

function ago(stamp, now) {
  if (!stamp) {
    return '';
  }
  const seconds = Math.max(0, Math.round((now || Date.now() / 1000) - stamp));
  if (seconds < 60) {
    return 'just now';
  }
  const units = [[86400, 'd'], [3600, 'h'], [60, 'min']];
  for (const [size, name] of units) {
    if (seconds >= size) {
      return Math.floor(seconds / size) + ' ' + name + ' ago';
    }
  }
  return '';
}

// The QUAL matrix rows after the filters: {query, tier, state, feature, results}.
function matrixRows(data, filters) {
  const cells = {};
  for (const cell of data.cells) {
    cells[cell.device + '\u0000' + cell.feature] = cell;
  }
  const query = (filters.query || '').toLowerCase();
  const features = Object.keys(data.features).filter((f) => !filters.feature || f === filters.feature)
    .filter((f) => data.devices.some((d) => d.features.includes(f)) || data.cells.some((c) => c.feature === f));
  const rows = [];
  for (const device of data.devices) {
    if (filters.tier && device.tier !== filters.tier) {
      continue;
    }
    const text = [device.id, device.os, device.gpu, device.arch, device.kind].join(' ').toLowerCase();
    if (query && !text.includes(query)) {
      continue;
    }
    const row = features.map((f) => cells[device.id + '\u0000' + f] || null);
    if (filters.state && !row.some((c) => c && c.state === filters.state)) {
      continue;
    }
    if (filters.results && !row.some((c) => c && c.state !== 'waiting')) {
      continue;
    }
    rows.push({ device: device, cells: row });
  }
  return { features: features, rows: rows };
}

const STATUS = {
  passed: ['passed', 'Passed'], pass: ['passed', 'Pass'], failed: ['failed', 'Failed'], fail: ['failed', 'Fail'],
  running: ['running', 'Running'], assigned: ['running', 'Starting'], scheduled: ['scheduled', 'Waiting'],
  canceled: ['canceled', 'Canceled'], timed_out: ['failed', 'Timed out'], expired: ['failed', 'Expired'],
  cached: ['cached', 'Cached'], budget: ['budget', 'Budget'], soft: ['soft', 'Soft fail'],
  waiting: ['waiting', 'Waiting'], idle: ['passed', 'Idle'], busy: ['running', 'Busy'], lost: ['failed', 'Lost'],
  stopped: ['canceled', 'Stopped'], met: ['passed', 'Met'], missed: ['failed', 'Missed'],
  triage: ['triage', 'Needs triage'], untriaged: ['triage', 'Needs triage'], positive: ['passed', 'Approved'],
  negative: ['failed', 'Rejected'], pending: ['scheduled', 'Not compared yet'], error: ['failed', 'Unreadable'],
};
function statusParts(state, softFailed) {
  if (softFailed) {
    return { cls: 'soft', icon: 'soft', text: 'Soft fail' };
  }
  const [kind, text] = STATUS[state] || ['scheduled', String(state || 'unknown')];
  return { cls: kind, icon: kind, text: text };
}
function statusHtml(state, options) {
  const opts = options || {};
  const parts = statusParts(state, opts.soft);
  const live = parts.icon === 'running' ? ' class="spin"' : '';
  return '<span class="status st-' + esc(parts.cls) + (opts.large ? ' lg' : '') +
    '"><svg aria-hidden="true"' + live + '><use href="#i-' + parts.icon + '"/></svg>' + esc(parts.text) + '</span>';
}

// Diagnostic artifact kinds (ci/coordinator/diagnostics.py), most useful first; `restricted` ones are raw memory.
const DIAGNOSTICS = [['crash-bundle', 'Crash bundle'], ['crash-record', 'Crash record'],
  ['sanitizer-report', 'Sanitizer report'], ['gpu-validation', 'GPU validation'], ['flight-record', 'Flight record'],
  ['replay-record', 'Replay record'], ['perfetto-trace', 'Perfetto trace'], ['profile-capture', 'Profile capture'],
  ['crash-dump', 'Crash dump'], ['gpu-dump', 'GPU crash dump'], ['core-dump', 'Core dump'],
  ['summary', 'Diagnostics summary']];
const RESTRICTED = ['crash-dump', 'gpu-dump', 'core-dump'];

// The diagnostics of one job's newest attempt, grouped by kind in DIAGNOSTICS order: [{kind, title, items}].
function diagnosticGroups(artifacts, jobId) {
  const mine = (artifacts || []).filter((a) => a.job === jobId);
  const attempt = Math.max(0, ...mine.map((a) => a.attempt || 0));
  return DIAGNOSTICS.map(([kind, title]) => ({ kind: kind, title: title, restricted: RESTRICTED.includes(kind),
    items: mine.filter((a) => a.kind === kind && (a.attempt || 0) === attempt) })).filter((g) => g.items.length);
}

// A gold item's metrics against its tolerances: [{label, value, limit, ok}] (ok null when there is no limit).
function goldMetrics(item) {
  const m = ((item.comparison || {}).metrics) || {};
  const t = item.tolerance || {};
  if (m.size_mismatch) {
    return [{ label: 'Size', value: (m.test_size || []).join('x') + ' (approved ' + (m.reference_size || []).join('x') +
      ')', limit: 'the same size', ok: false }];
  }
  if (m.flip_mean === undefined) {
    return [];
  }
  const row = (label, value, limit, ok, digits, unit) => ({ label: label, value: Number(value).toFixed(digits) +
    (unit || ''), limit: limit, ok: ok });
  return [row('Mean FLIP', m.flip_mean, t.flip_mean !== undefined ? '\u2264 ' + t.flip_mean : '', t.flip_mean !==
      undefined ? m.flip_mean <= t.flip_mean : null, 4),
    row('Pixels over FLIP ' + (t.flip_pixel !== undefined ? t.flip_pixel : 0.1), m.flip_area * 100, t.flip_area !==
      undefined ? '\u2264 ' + (t.flip_area * 100).toFixed(2) + ' %' : '', t.flip_area !== undefined ? m.flip_area <=
      t.flip_area : null, 2, ' %'),
    row('SSIM', m.ssim, t.ssim_min !== undefined ? '\u2265 ' + t.ssim_min : '', t.ssim_min !== undefined ? m.ssim >=
      t.ssim_min : null, 4),
    row('Max FLIP', m.flip_max, '', null, 4),
    row('Pixels per degree', m.ppd, (m.scale > 1 ? 'compared downsampled 1/' + m.scale : 'compared at full size'),
      null, 1)];
}

const ZOOMS = [0, 1, 2, 4, 8, 16]; // 0 fits the pane
function zoomStep(current, direction) {
  const index = Math.max(0, ZOOMS.indexOf(current));
  return ZOOMS[Math.max(0, Math.min(ZOOMS.length - 1, index + direction))];
}

if (typeof module !== 'undefined') {
  module.exports = { esc: esc, stripAnsi: stripAnsi, ansiToHtml: ansiToHtml, isErrorLine: isErrorLine,
    firstErrorLine: firstErrorLine, appendLog: appendLog, cleanLabel: cleanLabel, markdown: markdown, layout: layout,
    durationText: durationText, ago: ago, matrixRows: matrixRows, statusParts: statusParts, statusHtml: statusHtml,
    diagnosticGroups: diagnosticGroups, goldMetrics: goldMetrics, zoomStep: zoomStep };
}

// ------------------------------------------------------------------------------------------- the application

if (typeof document !== 'undefined') {
  const view = document.getElementById('view');
  const state = { source: null, timer: null, cleanup: [], chord: null, selection: -1, lastRoute: '' };

  // Theme -----------------------------------------------------------------------------------------------------
  const applyTheme = (choice) => {
    if (choice === 'light' || choice === 'dark') {
      document.documentElement.dataset.theme = choice;
    } else {
      delete document.documentElement.dataset.theme;
    }
    document.querySelectorAll('[data-theme-choice]').forEach((b) => b.setAttribute('aria-pressed',
      String(b.dataset.themeChoice === (choice || 'auto'))));
  };
  let savedTheme = 'auto';
  try {
    savedTheme = localStorage.getItem('cerid-theme') || 'auto';
  } catch (error) {
    savedTheme = 'auto';
  }
  applyTheme(savedTheme);
  document.querySelectorAll('[data-theme-choice]').forEach((button) => button.addEventListener('click', () => {
    applyTheme(button.dataset.themeChoice);
    try {
      localStorage.setItem('cerid-theme', button.dataset.themeChoice);
    } catch (error) {
      // a private window: the choice lasts for this page only
    }
  }));

  // Plumbing --------------------------------------------------------------------------------------------------
  const api = async (path, options) => {
    const init = Object.assign({ headers: {}, credentials: 'same-origin' }, options || {});
    if (init.method && init.method !== 'GET') {
      init.headers['X-Cerid-Request'] = '1';
      init.headers['Content-Type'] = 'application/json';
    }
    const response = await fetch('/api/' + path, init);
    const body = await response.json().catch(() => ({}));
    if (!response.ok) {
      const error = new Error(body.error || response.statusText || 'request failed');
      error.status = response.status;
      throw error;
    }
    return body;
  };
  const post = (path, body) => api(path, { method: 'POST', body: JSON.stringify(body || {}) });
  const when = (t) => (t ? new Date(t * 1000).toISOString().replace('T', ' ').slice(0, 16) + ' UTC' : '');
  const announce = (text) => {
    document.getElementById('announce').textContent = text;
  };
  const toast = (text, bad) => {
    const node = document.createElement('div');
    node.className = 'toast' + (bad ? ' bad' : '');
    node.setAttribute('role', bad ? 'alert' : 'status');
    node.textContent = text;
    document.getElementById('toasts').appendChild(node);
    setTimeout(() => node.remove(), 6000);
  };
  const setLive = (on) => {
    const dot = document.getElementById('live');
    dot.dataset.live = on ? 'on' : 'off';
    dot.setAttribute('aria-label', on ? 'Live updates on' : 'Live updates off');
    dot.title = on ? 'Live updates on' : 'Live updates off';
  };
  const stopLive = () => {
    if (state.source) {
      state.source.close();
      state.source = null;
    }
    clearTimeout(state.timer);
    state.cleanup.forEach((fn) => fn());
    state.cleanup = [];
    setLive(false);
  };
  const live = (url, handlers) => {
    if (typeof EventSource === 'undefined') {
      return;
    }
    const source = new EventSource(url);
    state.source = source;
    source.onopen = () => setLive(true);
    source.onerror = () => setLive(false);
    for (const [name, fn] of Object.entries(handlers)) {
      source.addEventListener(name, (event) => fn(JSON.parse(event.data)));
    }
  };
  const loading = () => {
    view.innerHTML = '<div class="skeleton" aria-hidden="true"><i></i><i></i><i></i><i></i><i></i></div>' +
      '<p class="visually-hidden">Loading</p>';
  };
  const empty = (title, text, action) => '<div class="empty"><svg aria-hidden="true"><use href="#i-empty"/></svg>' +
    '<strong>' + esc(title) + '</strong><span>' + esc(text) + '</span>' + (action || '') + '</div>';
  const failed = (error) => {
    const hint = error.status === 401 ? 'Open the UI through `python ci/ops.py ui` (the IAP tunnel).' :
      error.status === 404 ? 'It may have been removed by retention, or the link is wrong.' :
        'Check that the coordinator is running: `python ci/ops.py health`.';
    view.innerHTML = '<div class="error-state" role="alert"><svg aria-hidden="true"><use href="#i-alert"/></svg>' +
      '<strong>' + esc(error.message || error) + '</strong><span>' + esc(hint) + '</span>' +
      '<button type="button" id="retry-load"><svg aria-hidden="true"><use href="#i-retry"/></svg>Try again</button></div>';
    document.getElementById('retry-load').addEventListener('click', route);
  };
  const confirmAction = (text) => window.confirm(text);

  // Lists: j/k selection ----------------------------------------------------------------------------------------
  const listItems = () => Array.from(view.querySelectorAll('[data-item]'));
  const select = (index) => {
    const items = listItems();
    if (!items.length) {
      return;
    }
    state.selection = Math.max(0, Math.min(items.length - 1, index));
    items.forEach((item, i) => item.setAttribute('aria-selected', String(i === state.selection)));
    items[state.selection].focus();
  };

  // Builds -------------------------------------------------------------------------------------------------------
  const buildRow = (b) => '<li><a class="item" data-item href="#/builds/' + b.number + '" aria-selected="false">' +
    '<span>' + statusHtml(b.state) + '</span><span><span class="title">' + esc(b.message || '(no message)') +
    '</span><span class="meta"><span class="num">#' + b.number + '</span><span>' + esc(b.repository) + '</span><span>' +
    esc(b.branch) + (b.pull_request ? ' · PR ' + esc(b.pull_request) : '') + '</span><span class="mono">' +
    esc((b.commit || '').slice(0, 10)) + '</span><span>' + esc(b.source) + '</span></span></span><span class="when">' +
    '<span>' + esc(ago(b.created)) + '</span><br><span class="num">' + esc(durationText(b.started_at, b.finished_at)) +
    '</span></span></a></li>';

  async function builds() {
    const data = await api('builds?limit=100');
    view.innerHTML = '<div class="page-head"><div class="grow"><h1>Builds</h1><p class="muted">Newest first; updates ' +
      'live. Polling builds every new head of the watched branches within a minute.</p></div><div class="row">' +
      '<label class="visually-hidden" for="filter">Filter builds</label><input id="filter" type="search" ' +
      'placeholder="Filter builds (/)" autocomplete="off"><label class="visually-hidden" for="state-filter">State' +
      '</label><select id="state-filter"><option value="">All states</option><option value="failed">Failed</option>' +
      '<option value="running">Running</option><option value="passed">Passed</option><option value="canceled">' +
      'Canceled</option></select><button type="button" class="primary" id="new"><svg aria-hidden="true"><use ' +
      'href="#i-plus"/></svg>New build</button></div></div><section class="card flush" aria-label="Builds"><ul ' +
      'class="list" id="build-list"></ul></section>';
    let current = data.builds;
    const paint = () => {
      const query = document.getElementById('filter').value.toLowerCase();
      const wanted = document.getElementById('state-filter').value;
      const rows = current.filter((b) => (!wanted || b.state === wanted || (wanted === 'running' &&
        b.state === 'scheduled')) && (!query || [b.message, b.branch, b.repository, b.commit, '#' + b.number]
        .join(' ').toLowerCase().includes(query)));
      document.getElementById('build-list').innerHTML = rows.map(buildRow).join('') || '<li>' + empty(
        current.length ? 'No build matches' : 'No builds yet', current.length ? 'Change the filter.' :
          'The poller starts one for the next push to main, or start one now.') + '</li>';
    };
    paint();
    document.getElementById('filter').addEventListener('input', paint);
    document.getElementById('state-filter').addEventListener('change', paint);
    document.getElementById('new').addEventListener('click', () => document.getElementById('new-build').showModal());
    live('/api/stream?limit=100', { builds: (payload) => {
      current = payload.builds;
      paint();
    } });
  }

  // Build --------------------------------------------------------------------------------------------------------
  const graphSvg = (jobs) => {
    const { columns, place } = layout(jobs);
    const w = 200;
    const h = 38;
    const gx = 56;
    const gy = 14;
    const height = Math.max(...columns.map((c) => c.length), 1) * (h + gy) + gy;
    const width = columns.length * (w + gx) + gx / 2;
    let edges = '';
    let nodes = '';
    for (const job of jobs) {
      const p = place[job.key];
      const x = gx / 4 + p.x * (w + gx);
      const y = gy + p.y * (h + gy);
      for (const dep of job.depends_on || []) {
        const q = place[dep.step];
        if (!q) {
          continue;
        }
        const x1 = gx / 4 + q.x * (w + gx) + w;
        const y1 = gy + q.y * (h + gy) + h / 2;
        edges += '<path class="edge' + (dep.allow_failure ? ' soft' : '') + '" d="M' + x1 + ',' + y1 + ' C' +
          (x1 + gx / 2) + ',' + y1 + ' ' + (x - gx / 2) + ',' + (y + h / 2) + ' ' + x + ',' + (y + h / 2) + '"/>';
      }
      const parts = statusParts(job.state, job.soft_failed);
      const name = cleanLabel(job.label);
      const label = name.length > 26 ? name.slice(0, 25) + '…' : name;
      nodes += '<a href="#/jobs/' + esc(job.id) + '" aria-label="' + esc(cleanLabel(job.label) + ': ' + parts.text) + '"><g ' +
        'class="node n-' + esc(job.soft_failed ? 'soft' : job.state) + '"><rect x="' + x + '" y="' + y + '" width="' +
        w + '" height="' + h + '"/><svg class="glyph" x="' + (x + 10) + '" y="' + (y + 11) + '" width="16" height="16" ' +
        'aria-hidden="true"><use href="#i-' + parts.icon + '"/></svg><text x="' + (x + 34) + '" y="' + (y + 23) + '">' +
        esc(label) + '</text></g></a>';
    }
    return '<svg width="' + width + '" height="' + height + '" role="group" aria-label="Step graph">' + edges + nodes +
      '</svg>';
  };

  const logLines = (text) => text.replace(/\r\n/g, '\n').split('\n');
  const renderLog = (lines, options) => {
    const first = firstErrorLine(lines);
    const needle = (options && options.search || '').toLowerCase();
    let html = '';
    lines.forEach((line, i) => {
      const plain = stripAnsi(line);
      const classes = ['ln'];
      if (plain.startsWith('~~~')) {
        classes.push('hdr');
      }
      if (isErrorLine(line)) {
        classes.push('err');
      }
      if (i === first) {
        classes.push('first-err');
      }
      if (needle && plain.toLowerCase().includes(needle)) {
        classes.push('hit');
      }
      const prefix = (options && options.prefix) || 'L';
      html += '<span class="' + classes.join(' ') + '" id="' + prefix + (i + 1) + '"><a href="#" data-line="' + (i + 1) +
        '" aria-hidden="true" tabindex="-1">' + (i + 1) + '</a><span>' + ansiToHtml(line) + '</span></span>';
    });
    return { html: html, first: first };
  };

  async function build(number) {
    const b = await api('builds/' + number);
    const render = (data, keepFailure) => {
      const running = !['passed', 'failed', 'canceled'].includes(data.state);
      const failedJobs = data.jobs.filter((j) => ['failed', 'timed_out', 'expired'].includes(j.state));
      document.title = '#' + data.number + ' ' + statusParts(data.state).text + ' · Cerid CI';
      const banner = '<section class="banner st-border-' + esc(data.state) + '" aria-labelledby="build-title">' +
        statusHtml(data.state, { large: true }) + '<div><h1 class="title" id="build-title">' + esc(data.message ||
        '(no message)') + '</h1><div class="meta"><span class="num">Build #' + data.number + '</span><span>' +
        esc(data.repository) + ' · ' + esc(data.branch) + '</span><span class="mono">' + esc(data.commit.slice(0, 12)) +
        '</span><span>' + esc(data.source) + (data.created_by ? ' by ' + esc(data.created_by) : '') + '</span><span>' +
        esc(when(data.created)) + '</span><span class="num">' + esc(durationText(data.started_at, data.finished_at)) +
        '</span>' + (data.trusted ? '' : '<span>untrusted: no secrets</span>') + '</div></div><div class="actions">' +
        (running ? '<button type="button" class="danger" id="cancel-build"><svg aria-hidden="true"><use href="#i-stop"/>' +
          '</svg>Cancel build</button>' : '') + (failedJobs.length && !running ? '<button type="button" ' +
          'id="retry-failed"><svg aria-hidden="true"><use href="#i-retry"/></svg>Retry failed steps</button>' : '') +
        '</div></section>';
      const failure = failedJobs.length ? '<section aria-labelledby="failed-title" class="stack"><h2 id="failed-title">' +
        'What failed</h2>' + failedJobs.map((j, i) => '<article class="failure" aria-label="' + esc(cleanLabel(j.label)) + '">' +
        '<header>' + statusHtml(j.state) + '<h3><a href="#/jobs/' + esc(j.id) + '">' + esc(cleanLabel(j.label)) + '</a></h3><span ' +
        'class="subtle">attempt ' + j.attempt + (j.exit_status !== null && j.exit_status !== undefined ? ' · exit ' +
          esc(j.exit_status) : '') + '</span></header><div class="reason">' +
        esc(j.reason || '') + '</div>' + diagnosticChips(data.artifacts, j.id) + (i === 0 ? '<div id="first-failure" ' +
        'data-job="' + esc(j.id) + '"></div>' : '') + '</article>').join('') + '</section>' : '';
      const steps = '<section class="card" aria-labelledby="steps-title"><header><h2 id="steps-title">Steps</h2>' +
        '<span class="subtle">' + data.jobs.length + ' steps</span></header><div class="graph">' + graphSvg(data.jobs) +
        '</div><div class="table-wrap"><table><thead><tr><th scope="col">Step</th><th scope="col">State</th>' +
        '<th scope="col">Where</th><th scope="col" class="num">Time</th><th scope="col">Reason</th><th scope="col">' +
        '<span class="visually-hidden">Actions</span></th></tr></thead><tbody>' + data.jobs.map((j) => '<tr><td><a ' +
        'href="#/jobs/' + esc(j.id) + '">' + esc(cleanLabel(j.label)) + '</a><div class="subtle mono">' + esc(j.key) + '</div></td>' +
        '<td>' + statusHtml(j.state, { soft: j.soft_failed }) + (j.attempt > 1 ? ' <span class="subtle">attempt ' +
          j.attempt + '</span>' : '') + '</td><td>' + esc((j.agents && j.agents.device) || (j.agents && j.agents.queue) ||
        '') + '<div class="subtle">' + esc(j.agent_name || j.machine || '') + '</div></td><td class="num">' +
        esc(durationText(j.started_at, j.finished_at)) + '</td><td>' + esc(j.waiting && j.waiting !== 'ready' ?
        j.waiting : j.reason || '') + '</td><td>' + (['failed', 'canceled', 'timed_out', 'expired', 'budget']
        .includes(j.state) ? '<button type="button" data-retry="' + esc(j.id) + '">Retry</button>' : '') +
        (['scheduled', 'assigned', 'running'].includes(j.state) ? '<button type="button" class="danger" data-cancel="' +
          esc(j.id) + '">Cancel</button>' : '') + '</td></tr>').join('') + '</tbody></table></div></section>';
      const notes = Object.entries(data.annotations || {});
      const extra = (notes.length ? '<section class="card" aria-labelledby="notes-title"><header><h2 id="notes-title">' +
        'Annotations</h2></header><div class="body stack">' + notes.map(([context, note]) => '<div><h3>' +
        statusHtml(note.style === 'error' ? 'failed' : note.style === 'success' ? 'passed' : note.style === 'warning' ?
          'soft' : 'scheduled', { soft: note.style === 'warning' }) + ' ' + esc(context) + '</h3>' + markdown(note.body) +
        '</div>').join('') + '</div></section>' : '') + ((data.artifacts || []).length ? '<section class="card" ' +
        'aria-labelledby="artifacts-title"><header><h2 id="artifacts-title">Artifacts</h2></header><div class="table-wrap">' +
        '<table><thead><tr><th scope="col">File</th><th scope="col">Kind</th><th scope="col">Step</th><th scope="col" ' +
        'class="num">Size</th></tr></thead><tbody>' + data.artifacts.map((a) => '<tr><td><a href="/api/jobs/' +
          esc(a.job) + '/artifacts?path=' + encodeURIComponent(a.path) + '"><svg aria-hidden="true" width="14" ' +
          'height="14"><use href="#i-down"/></svg> ' + esc(a.path) + '</a></td><td>' + esc(a.kind || 'file') + '</td><td>' +
          esc(a.key) + '</td><td class="num">' + a.size.toLocaleString() + ' B</td></tr>').join('') +
        '</tbody></table></div></section>' : '');
      const failureNode = keepFailure ? document.getElementById('first-failure') : null;
      view.innerHTML = '<div class="stack">' + banner + failure + steps + extra + '</div>';
      if (failureNode && document.getElementById('first-failure')) {
        document.getElementById('first-failure').replaceWith(failureNode);
      } else if (document.getElementById('first-failure')) {
        inlineFailure(document.getElementById('first-failure'));
      }
      wire(data);
    };
    const wire = (data) => {
      const cancel = document.getElementById('cancel-build');
      if (cancel) {
        cancel.addEventListener('click', async () => {
          if (confirmAction('Cancel build #' + data.number + '? Running steps are stopped.')) {
            await post('builds/' + data.number + '/cancel').then(() => toast('Cancel requested')).catch((e) =>
              toast(e.message, true));
          }
        });
      }
      const retry = document.getElementById('retry-failed');
      if (retry) {
        retry.addEventListener('click', async () => {
          for (const job of data.jobs.filter((j) => ['failed', 'timed_out', 'expired'].includes(j.state))) {
            await post('jobs/' + job.id + '/retry').catch((e) => toast(e.message, true));
          }
          toast('Failed steps scheduled again');
        });
      }
      view.querySelectorAll('[data-retry]').forEach((el) => el.addEventListener('click', () =>
        post('jobs/' + el.dataset.retry + '/retry').then(() => toast('Step scheduled again')).catch((e) =>
          toast(e.message, true))));
      view.querySelectorAll('[data-cancel]').forEach((el) => el.addEventListener('click', () => {
        if (confirmAction('Cancel this step?')) {
          post('jobs/' + el.dataset.cancel + '/cancel').then(() => toast('Cancel requested')).catch((e) =>
            toast(e.message, true));
        }
      }));
    };
    render(b, false);
    let lastState = b.state;
    let lastFailed = b.jobs.filter((j) => j.state === 'failed').map((j) => j.id).join();
    if (!['passed', 'failed', 'canceled'].includes(b.state)) {
      live('/api/stream?build=' + number, { build: (data) => {
        const failedNow = data.jobs.filter((j) => j.state === 'failed').map((j) => j.id).join();
        render(data, failedNow === lastFailed);
        if (data.state !== lastState) {
          announce('Build #' + data.number + ' is ' + statusParts(data.state).text.toLowerCase());
          lastState = data.state;
        }
        lastFailed = failedNow;
      } });
    }
  }

  // Links to a failed step's diagnostics, one chip per kind (counts when there are several files).
  const diagnosticChips = (artifacts, jobId) => {
    const groups = diagnosticGroups(artifacts, jobId);
    if (!groups.length) {
      return '';
    }
    return '<div class="chips" aria-label="Diagnostics">' + groups.map((g) => '<a class="chip' + (g.restricted ?
      ' restricted' : '') + '" href="' + (g.items.length === 1 ? '/api/jobs/' + esc(jobId) + '/artifacts?path=' +
      encodeURIComponent(g.items[0].path) : '#/jobs/' + esc(jobId)) + '"><svg aria-hidden="true"><use href="#i-down"/>' +
      '</svg>' + esc(g.title) + (g.items.length > 1 ? ' <span class="count">' + g.items.length + '</span>' : '') +
      '</a>').join('') + '<a class="chip" href="#/jobs/' + esc(jobId) + '">Reproduce</a></div>';
  };

  async function inlineFailure(node) {
    const jobId = node.dataset.job;
    try {
      const log = await api('jobs/' + jobId + '/log');
      const lines = logLines(log.text);
      const { html, first } = renderLog(lines, { prefix: 'F' });
      node.innerHTML = '<div class="log-tools"><button type="button" id="jump-error"' + (first < 0 ? ' disabled' : '') +
        '><svg aria-hidden="true"><use href="#i-jump"/></svg>Jump to first error</button><a class="button" href="#/jobs/' +
        esc(jobId) + '">Open full log</a><span class="subtle">' + (first < 0 ? 'No error line recognised; read the ' +
          'end of the log.' : 'First error on line ' + (first + 1)) + '</span></div><pre class="log wrap" tabindex="0" ' +
        'aria-label="Log of the first failed step">' + html + '</pre>';
      const box = node.querySelector('.log');
      const jump = () => {
        const target = first >= 0 ? node.querySelector('#F' + (first + 1)) : box.lastElementChild;
        if (target) {
          box.scrollTop = Math.max(0, target.offsetTop - box.clientHeight / 3);
        }
      };
      node.querySelector('#jump-error').addEventListener('click', jump);
      jump();
    } catch (error) {
      node.innerHTML = '<p class="reason">The log could not be loaded: ' + esc(error.message) + '</p>';
    }
  }

  // Job and its log ------------------------------------------------------------------------------------------------
  async function job(id) {
    const j = await api('jobs/' + id);
    document.title = cleanLabel(j.label) + ' · ' + statusParts(j.state).text + ' · Cerid CI';
    const params = new URLSearchParams(location.hash.split('?')[1] || '');
    const attempt = parseInt(params.get('attempt') || j.attempt, 10);
    const lines = [];
    let follow = true;
    let search = '';
    let hits = [];
    let hitIndex = -1;
    const options = [];
    for (let a = j.attempt; a >= 1; a -= 1) {
      options.push('<option value="' + a + '"' + (a === attempt ? ' selected' : '') + '>Attempt ' + a + '</option>');
    }
    const repro = j.repro ? '<details class="card"><summary class="body"><b>Reproduce this step</b> <span ' +
      'class="subtle">machine, toolchain and environment of attempt ' + esc(j.repro.attempt) + '</span></summary><div ' +
      'class="body stack"><pre class="mono">git checkout ' + esc(j.repro.commit) + '\n' + esc(j.command) + '</pre><dl ' +
      'class="kv"><dt>Machine</dt><dd>' + esc(j.repro.machine) + ' · ' + esc(j.repro.platform) + ' · ' +
      esc(j.repro.cpus) + ' CPUs</dd><dt>Image</dt><dd>' + esc(j.repro.image || 'not a cloud image') + '</dd>' +
      Object.entries(j.repro.tools || {}).map(([k, v]) => '<dt>' + esc(k) + '</dt><dd class="mono">' + esc(v) + '</dd>')
        .join('') + '</dl><details><summary>Environment (' + Object.keys(j.repro.env || {}).length + ' variables, ' +
      'secrets hidden)</summary><pre class="mono">' + esc(Object.entries(j.repro.env || {}).map(([k, v]) => k + '=' + v)
        .join('\n')) + '</pre></details></div></details>' : '';
    view.innerHTML = '<div class="stack"><section class="banner st-border-' + esc(j.state) + '" aria-labelledby=' +
      '"job-title"><span id="job-status">' + statusHtml(j.state, { large: true }) + '</span><div><h1 class="title" ' +
      'id="job-title">' + esc(cleanLabel(j.label)) + '</h1><div class="meta"><a href="#/builds/' + j.build + '">Build #' + j.build +
      '</a><span class="mono">' + esc(j.key) + '</span><span>' + esc(j.agent_name || j.machine || '') + '</span><span ' +
      'id="job-reason">' + esc(j.reason || '') + '</span></div></div><div class="actions"><label class="visually-hidden" ' +
      'for="attempt">Attempt</label><select id="attempt">' + options.join('') + '</select><a class="button" ' +
      'href="/api/jobs/' + esc(id) + '/log?raw=1&amp;attempt=' + attempt + '"><svg aria-hidden="true"><use ' +
      'href="#i-down"/></svg>Download</a>' + (['failed', 'canceled', 'timed_out', 'expired', 'budget'].includes(j.state) ?
        '<button type="button" id="retry-job"><svg aria-hidden="true"><use href="#i-retry"/></svg>Retry</button>' : '') +
      (['scheduled', 'assigned', 'running'].includes(j.state) ? '<button type="button" class="danger" id="cancel-job">' +
        '<svg aria-hidden="true"><use href="#i-stop"/></svg>Cancel</button>' : '') + '</div></section>' + repro +
      '<div id="diagnostics"></div>' +
      '<section class="card flush" aria-labelledby="log-title"><h2 class="visually-hidden" id="log-title">Log</h2>' +
      '<div class="log-tools"><label class="visually-hidden" for="log-search">Search the log</label><input ' +
      'id="log-search" type="search" placeholder="Search the log (/)" autocomplete="off"><span class="subtle num" ' +
      'id="hit-count" aria-live="polite"></span><button type="button" id="prev-hit" title="Previous match (N)">↑' +
      '<span class="visually-hidden">Previous match</span></button><button type="button" id="next-hit" title="Next ' +
      'match (n)">↓<span class="visually-hidden">Next match</span></button><button type="button" id="jump-error">' +
      '<svg aria-hidden="true"><use href="#i-jump"/></svg>First error (e)</button><button type="button" ' +
      'aria-pressed="true" id="wrap">Wrap</button><button type="button" aria-pressed="true" id="follow">Follow (f)' +
      '</button><span class="subtle" id="tail"></span></div><pre class="log wrap" id="log" tabindex="0" aria-label=' +
      '"Log"></pre></section></div>';
    const box = document.getElementById('log');
    const paint = () => {
      const { html } = renderLog(lines, { search: search });
      box.innerHTML = html || '<span class="ln"><a aria-hidden="true"></a><span class="subtle">No output yet.</span>' +
        '</span>';
      hits = search ? Array.from(box.querySelectorAll('.hit')) : [];
      document.getElementById('hit-count').textContent = search ? hits.length + ' matches' : '';
      if (follow && !search) {
        box.scrollTop = box.scrollHeight;
      }
    };
    let open = false;
    const appendText = (text) => {
      if (!text) {
        return;
      }
      open = appendLog(lines, open, text);
      paint();
    };
    const gotoHit = (step) => {
      if (!hits.length) {
        return;
      }
      hitIndex = (hitIndex + step + hits.length) % hits.length;
      follow = false;
      document.getElementById('follow').setAttribute('aria-pressed', 'false');
      box.scrollTop = Math.max(0, hits[hitIndex].offsetTop - box.clientHeight / 3);
      document.getElementById('hit-count').textContent = (hitIndex + 1) + ' of ' + hits.length;
    };
    const jumpError = () => {
      const first = firstErrorLine(lines);
      if (first < 0) {
        toast('No error line recognised in this log');
        return;
      }
      follow = false;
      document.getElementById('follow').setAttribute('aria-pressed', 'false');
      const target = document.getElementById('L' + (first + 1));
      box.scrollTop = Math.max(0, target.offsetTop - box.clientHeight / 3);
      announce('First error on line ' + (first + 1));
    };
    state.jumpError = jumpError;
    state.gotoHit = gotoHit;
    state.toggleFollow = () => document.getElementById('follow').click();
    document.getElementById('log-search').addEventListener('input', (event) => {
      search = event.target.value;
      hitIndex = -1;
      paint();
    });
    document.getElementById('next-hit').addEventListener('click', () => gotoHit(1));
    document.getElementById('prev-hit').addEventListener('click', () => gotoHit(-1));
    document.getElementById('jump-error').addEventListener('click', jumpError);
    document.getElementById('wrap').addEventListener('click', (event) => {
      const on = event.currentTarget.getAttribute('aria-pressed') !== 'true';
      event.currentTarget.setAttribute('aria-pressed', String(on));
      box.classList.toggle('wrap', on);
    });
    document.getElementById('follow').addEventListener('click', (event) => {
      follow = event.currentTarget.getAttribute('aria-pressed') !== 'true';
      event.currentTarget.setAttribute('aria-pressed', String(follow));
      if (follow) {
        box.scrollTop = box.scrollHeight;
      }
    });
    box.addEventListener('wheel', () => {
      if (follow && box.scrollTop + box.clientHeight < box.scrollHeight - 40) {
        follow = false;
        document.getElementById('follow').setAttribute('aria-pressed', 'false');
      }
    });
    document.getElementById('attempt').addEventListener('change', (event) => {
      location.hash = '#/jobs/' + id + '?attempt=' + event.target.value;
    });
    const retry = document.getElementById('retry-job');
    if (retry) {
      retry.addEventListener('click', () => post('jobs/' + id + '/retry').then(() => {
        toast('Step scheduled again');
        route();
      }).catch((e) => toast(e.message, true)));
    }
    const cancel = document.getElementById('cancel-job');
    if (cancel) {
      cancel.addEventListener('click', () => {
        if (confirmAction('Cancel this step?')) {
          post('jobs/' + id + '/cancel').then(() => toast('Cancel requested')).catch((e) => toast(e.message, true));
        }
      });
    }
    api('jobs/' + id + '/artifacts').then((listing) => {
      const groups = diagnosticGroups(listing.artifacts, id);
      const node = document.getElementById('diagnostics');
      if (!node || !groups.length) {
        return;
      }
      node.innerHTML = '<section class="card" aria-labelledby="diag-title"><header><h2 id="diag-title">Diagnostics' +
        '</h2><span class="subtle">collected when the step failed; secrets scrubbed, raw memory only where the ' +
        'device allows it</span></header><div class="body stack"><div class="table-wrap"><table><thead><tr><th ' +
        'scope="col">Kind</th><th scope="col">File</th><th scope="col" class="num">Size</th></tr></thead><tbody>' +
        [].concat(...groups.map((g) => g.items.map((a) => '<tr><td>' + esc(g.title) + (g.restricted ? ' <span ' +
          'class="subtle">(raw memory)</span>' : '') + '</td><td><a href="/api/jobs/' + esc(id) +
          '/artifacts?path=' + encodeURIComponent(a.path) + '"><svg aria-hidden="true" width="14" height="14"><use ' +
          'href="#i-down"/></svg> ' + esc(a.path.split('/').pop()) + '</a></td><td class="num">' +
          a.size.toLocaleString() + ' B</td></tr>'))).join('') + '</tbody></table></div><div class="command"><span>' +
        'Reproduce locally (downloads these, prints the exact replay for this commit, preset and CPU level):</span>' +
        '<pre class="mono">python ci/ops.py job reproduce ' + esc(id) + '</pre><button type="button" id="copy-repro">' +
        'Copy</button></div></div></section>';
      const summary = groups.find((g) => g.kind === 'summary');
      if (summary) {
        fetch('/api/jobs/' + id + '/artifacts?path=' + encodeURIComponent(summary.items[0].path), {
          credentials: 'same-origin' }).then((r) => r.json()).then((info) => {
          const withheld = (info.items || []).filter((i) => i.withheld);
          const lines = '<p>Failed phase <b>' + esc(info.phase || 'unknown') + '</b> at the CPU level <b>' +
            esc(info.cpu_level || 'native') + '</b>' + ((info.failed_tests || []).length ? '; failed tests: ' +
            esc(info.failed_tests.join(', ')) : '') + '.</p>' + (withheld.length ? '<p class="subtle">Kept on the ' +
            'machine, not uploaded: ' + esc(withheld.map((i) => i.kind + ' ' + String(i.source).split('/').pop() +
              ' (' + i.withheld + ')').join('; ')) + '</p>' : '');
          node.querySelector('.body').insertAdjacentHTML('afterbegin', lines);
        }).catch(() => {});
      }
      document.getElementById('copy-repro').addEventListener('click', () => {
        const text = 'python ci/ops.py job reproduce ' + id;
        (navigator.clipboard ? navigator.clipboard.writeText(text) : Promise.reject(new Error('no clipboard')))
          .then(() => toast('Copied')).catch(() => toast('Select the command and copy it', true));
      });
    }).catch(() => {});
    const first = await api('jobs/' + id + '/log?attempt=' + attempt);
    appendText(first.text);
    const tail = document.getElementById('tail');
    if (first.live && attempt === j.attempt) {
      tail.textContent = 'live';
      live('/api/stream?job=' + id + '&attempt=' + attempt + '&from=' + first.next, {
        log: (payload) => appendText(payload.text),
        job: (status) => {
          document.getElementById('job-status').innerHTML = statusHtml(status.state, { large: true });
          document.getElementById('job-reason').textContent = status.reason || '';
        },
        end: (status) => {
          tail.textContent = 'finished: ' + statusParts(status.state).text.toLowerCase();
          announce('The step finished: ' + statusParts(status.state).text);
          stopLive();
        },
      });
    } else {
      tail.textContent = 'finished';
      if (['failed', 'timed_out', 'expired'].includes(j.state) && firstErrorLine(lines) >= 0) {
        jumpError();
      }
    }
  }

  // QUAL matrix ------------------------------------------------------------------------------------------------------
  const CELL = { pass: ['passed', 'Pass'], fail: ['failed', 'Fail'], triage: ['triage', 'Needs triage'],
    cached: ['cached', 'Cached'], waiting: ['waiting', 'Waiting'], budget: ['budget', 'Budget'] };
  async function qual() {
    const data = await api('qual');
    const counts = {};
    for (const cell of data.cells) {
      counts[cell.state] = (counts[cell.state] || 0) + 1;
    }
    const featureOptions = Object.entries(data.features).map(([k, v]) => '<option value="' + esc(k) + '">' +
      esc(v.title) + '</option>').join('');
    view.innerHTML = '<div class="stack"><div class="page-head"><div class="grow"><h1>QUAL matrix</h1><p ' +
      'class="muted">Every device cell against every feature, from the result store. Cached means the same input key ' +
      'passed before. Budget, waiting and needs-triage cells are never passes.</p></div></div><div class="tiles">' +
      Object.entries(CELL).map(([key, [icon, text]]) => '<div class="tile"><div class="value num">' + (counts[key] || 0) +
        '</div><div class="label">' + statusHtml(key === 'pass' ? 'passed' : key === 'fail' ? 'failed' : key) + ' cells' +
        '</div></div>').join('') + '</div><div class="filters" role="search" aria-label="Filter the matrix"><label ' +
      'class="field">Device<input id="q" type="search" placeholder="id, OS, GPU (/)" autocomplete="off"></label><label ' +
      'class="field">Tier<select id="tier"><option value="">All</option><option>A</option><option>B</option><option>' +
      'C</option></select></label><label class="field">State<select id="state"><option value="">Any</option>' +
      '<option value="fail">Has a failure</option><option value="triage">Has images to triage</option><option ' +
      'value="pass">Has a pass</option><option value="waiting">' +
      'Has a waiting cell</option><option value="budget">Has a budget cell</option></select></label><label ' +
      'class="field">Feature<select id="feature"><option value="">All features</option>' + featureOptions +
      '</select></label><label class="row"><input type="checkbox" id="results"> Only devices with results</label>' +
      '</div><div class="legend" aria-label="Legend">' + Object.keys(CELL).map((k) => statusHtml(k === 'pass' ? 'passed' :
      k === 'fail' ? 'failed' : k)).join('') + '<span class="subtle">— no cell: the feature does not apply</span></div>' +
      '<div class="matrix-wrap" id="matrix" tabindex="0" aria-label="Device by feature matrix"></div></div>';
    const filters = () => ({ query: document.getElementById('q').value, tier: document.getElementById('tier').value,
      state: document.getElementById('state').value, feature: document.getElementById('feature').value,
      results: document.getElementById('results').checked });
    const paint = () => {
      const { features, rows } = matrixRows(data, filters());
      if (!rows.length) {
        document.getElementById('matrix').innerHTML = empty('No device matches', 'Change or clear the filters.');
        return;
      }
      document.getElementById('matrix').innerHTML = '<table class="matrix"><thead><tr><th scope="col" class="dev">' +
        'Device</th>' + features.map((f) => '<th scope="col" title="' + esc((data.features[f].rows || []).join(', ')) +
          '">' + esc(data.features[f].title) + '</th>').join('') + '</tr></thead><tbody>' + rows.map((row) => '<tr><th ' +
        'scope="row" class="dev">' + esc(row.device.id) + '<small>' + esc(row.device.os + ' · ' + row.device.arch +
          ' · ' + row.device.gpu + ' · tier ' + row.device.tier) + '</small></th>' + row.cells.map((cell, i) => {
        if (!cell) {
          return '<td><span class="cell none" aria-label="' + esc(row.device.id + ', ' + data.features[features[i]]
            .title + ': not applicable') + '"><svg aria-hidden="true"><use href="#i-none"/></svg></span></td>';
        }
        const [icon, text] = CELL[cell.state] || ['scheduled', cell.state];
        return '<td><button type="button" class="cell c-' + esc(cell.state) + '" data-cell="' +
          esc(JSON.stringify([row.device.id, features[i]])) + '" aria-label="' + esc(row.device.id + ', ' + data.features[features[i]].title + ': ' + text +
          (cell.reason ? ', ' + cell.reason : '')) + '"><svg aria-hidden="true"><use href="#i-' + icon + '"/></svg>' +
          esc(text) + '</button></td>';
      }).join('') + '</tr>').join('') + '</tbody></table>';
    };
    paint();
    ['q', 'tier', 'state', 'feature', 'results'].forEach((name) => document.getElementById(name).addEventListener(
      name === 'q' ? 'input' : 'change', paint));
    document.getElementById('matrix').addEventListener('click', (event) => {
      const button = event.target.closest('[data-cell]');
      if (!button) {
        return;
      }
      const [deviceId, feature] = JSON.parse(button.dataset.cell);
      const cell = data.cells.find((c) => c.device === deviceId && c.feature === feature);
      const device = data.devices.find((d) => d.id === deviceId);
      openDrawer(device.id + ' · ' + data.features[feature].title, '<div class="stack">' + statusHtml(
        (CELL[cell.state] || ['scheduled'])[0]) + '<dl class="kv"><dt>Reason</dt><dd>' + esc(cell.reason || '—') + '</dd><dt>Build</dt><dd>' +
        (cell.build ? '<a href="#/builds/' + esc(cell.build) + '">#' + esc(cell.build) + '</a>' : '—') + '</dd><dt>Log' +
        '</dt><dd>' + (cell.log ? '<a href="' + esc(cell.log) + '">open</a>' : '—') + '</dd>' + ((cell.diagnostics ||
          []).length ? '<dt>Diagnostics</dt><dd>' + (cell.job ? '<a href="#/jobs/' + esc(cell.job) + '">' : '') +
          esc(cell.diagnostics.map((k) => (DIAGNOSTICS.find((d) => d[0] === k) || [k, k])[1]).join(', ')) +
          (cell.job ? '</a>' : '') + (cell.phase ? ' <span class="subtle">(phase ' + esc(cell.phase) + ')</span>' : '') +
          '</dd>' : '') + (cell.gold ? '<dt>Gold images</dt><dd><a href="#/gold">' + esc(Object.entries(cell.gold)
          .map(([k, v]) => v + ' ' + statusParts(k === 'pass' ? 'passed' : k).text.toLowerCase()).join(', ')) +
          '</a></dd>' : '') + '<dt>Roadmap rows</dt><dd>' +
        esc((data.features[feature].rows || []).join(', ')) + '</dd><dt>Device</dt><dd>' + esc(device.kind + ', ' +
          device.os + ', ' + device.arch + ', ' + device.gpu + ', executor ' + device.executor + ', ' + device.status) +
        '</dd></dl></div>');
    });
  }

  const openDrawer = (title, html) => {
    document.getElementById('drawer-title').textContent = title;
    document.getElementById('drawer-body').innerHTML = html;
    document.getElementById('drawer').showModal();
  };

  // Spend -------------------------------------------------------------------------------------------------------------
  async function spend() {
    const s = await api('costs');
    const ratio = s.cap_usd ? s.spent_usd / s.cap_usd : 0;
    const forecastRatio = s.cap_usd ? s.forecast_usd / s.cap_usd : 0;
    const level = ratio >= 1 || s.forecast_over_cap ? 'over' : ratio >= 0.8 ? 'warn' : '';
    const max = Math.max(s.cap_usd, ...s.months.map((m) => m.usd), 1);
    const chart = '<svg class="bars" viewBox="0 -18 360 168" width="100%" height="168" role="img" aria-label="Spend of ' +
      'the last six months against the ' + s.cap_usd + ' USD cap">' + s.months.map((m, i) => {
      const h = Math.round(110 * m.usd / max);
      return '<rect class="month" x="' + (16 + i * 56) + '" y="' + (120 - h) + '" width="36" height="' + h + '" rx="3">' +
        '<title>' + esc(m.month + ': ' + m.usd.toFixed(2) + ' USD') + '</title></rect><text x="' + (34 + i * 56) +
        '" y="138" text-anchor="middle">' + esc(m.month.slice(5)) + '</text>';
    }).join('') + '<line class="cap" x1="8" x2="352" y1="' + (120 - Math.round(110 * s.cap_usd / max)) + '" y2="' +
      (120 - Math.round(110 * s.cap_usd / max)) + '"/><text x="352" y="' + (114 - Math.round(110 * s.cap_usd / max)) +
      '" text-anchor="end">cap ' + s.cap_usd + ' USD</text></svg>';
    view.innerHTML = '<div class="stack"><div class="page-head"><div class="grow"><h1>Spend, ' + esc(s.month) + '</h1>' +
      '<p class="muted">Recorded machine time from the ledger; the forecast extends this month\'s pace. Paid launches ' +
      'past the cap end as budget; the GCP budget stop is the second bound.</p></div></div><div class="tiles"><div ' +
      'class="tile"><div class="value num">' + s.spent_usd.toFixed(2) + ' <span class="subtle">of ' + s.cap_usd +
      ' USD</span></div><div class="meter" role="meter" aria-valuemin="0" aria-valuemax="' + s.cap_usd +
      '" aria-valuenow="' + s.spent_usd + '" aria-label="Spend against the cap"><i id="fill" class="' + level + '"></i>' +
      '<b id="forecast-mark" title="forecast"></b></div><div class="label">' + Math.round(ratio * 100) + ' % of the cap ' +
      'spent</div></div><div class="tile"><div class="value num">' + s.forecast_usd.toFixed(2) + ' USD</div><div ' +
      'class="label">' + (s.forecast_over_cap ? statusHtml('failed') + ' forecast over the cap' : 'forecast for the ' +
        'month') + '</div></div><div class="tile"><div class="value num">' + s.running_bound_usd.toFixed(2) + ' USD</div>' +
      '<div class="label">worst case of the ' + s.running + ' machines running now</div></div><div class="tile"><div ' +
      'class="value">' + (s.gcp_stop ? 'In force' : 'Not reached') + '</div><div class="label">GCP budget stop</div>' +
      '</div></div><div class="grid two"><section class="card"><header><h2>Last six months</h2></header><div ' +
      'class="body">' + chart + '</div></section><section class="card flush"><header><h2 id="by-title">Breakdown</h2>' +
      '<div class="segmented" role="group" aria-label="Group by"><button type="button" aria-pressed="true" data-by="lane">' +
      'Lane</button><button type="button" aria-pressed="false" data-by="device">Device</button><button type="button" ' +
      'aria-pressed="false" data-by="build">Build</button></div></header><div class="table-wrap" id="breakdown"></div>' +
      '</section></div></div>';
    document.getElementById('fill').style.width = Math.min(ratio * 100, 100).toFixed(1) + '%';
    document.getElementById('forecast-mark').style.left = Math.min(forecastRatio * 100, 100).toFixed(1) + '%';
    const table = (by) => {
      const rows = s['by_' + by];
      const field = by === 'lane' ? 'key' : by;
      document.getElementById('breakdown').innerHTML = rows.length ? '<table><thead><tr><th scope="col">' + esc(by) +
        '</th><th scope="col" class="num">Machines</th><th scope="col" class="num">Hours</th><th scope="col" ' +
        'class="num">USD</th></tr></thead><tbody>' + rows.map((r) => '<tr><td>' + (by === 'build' ? '<a href="#/builds/' +
          esc(r.build) + '">#' + esc(r.build) + '</a>' : esc(r[field])) + '</td><td class="num">' + r.machines + '</td>' +
          '<td class="num">' + r.hours.toFixed(2) + '</td><td class="num">' + r.usd.toFixed(2) + '</td></tr>').join('') +
        '</tbody></table>' : empty('Nothing spent yet', 'No paid machine ran this month.');
    };
    table('lane');
    view.querySelectorAll('[data-by]').forEach((button) => button.addEventListener('click', () => {
      view.querySelectorAll('[data-by]').forEach((b) => b.setAttribute('aria-pressed', String(b === button)));
      table(button.dataset.by);
    }));
  }

  // Test quality -----------------------------------------------------------------------------------------------------
  const spark = (history) => {
    if (!history.length) {
      return '';
    }
    const max = Math.max(...history.map((h) => h.seconds || 0), 0.001);
    const points = history.map((h, i) => (4 + i * 8) + ',' + (22 - 18 * (h.seconds || 0) / max)).join(' ');
    return '<svg class="spark" width="' + (8 + history.length * 8) + '" height="30" role="img" aria-label="' +
      history.length + ' runs, durations and outcomes"><polyline points="' + points + '"/>' + history.map((h, i) =>
      '<rect class="' + (h.outcome === 'pass' ? 'p' : 'f') + '" x="' + (2 + i * 8) + '" y="25" width="5" height="4"/>')
      .join('') + '</svg>';
  };
  async function quality() {
    const q = await api('quality');
    const active = q.quarantine.filter((r) => r.active && r.expires > q.now);
    const failing = [].concat(...q.lanes.map((l) => l.failing.map((t) => Object.assign({ lane: l.lane }, t))));
    const flaky = [].concat(...q.lanes.map((l) => l.flaky.map((t) => Object.assign({ lane: l.lane }, t))));
    view.innerHTML = '<div class="stack"><div class="page-head"><div class="grow"><h1>Test quality</h1><p class="muted">' +
      'Per-test history from every lane run. A test is flaky only when its identical key both passed and failed; it is ' +
      'then quarantined for a bounded time, still runs, and its failures stay visible here and in its build.</p></div>' +
      '</div><div class="tiles"><div class="tile"><div class="value num">' + failing.length + '</div><div class="label">' +
      'failing now</div></div><div class="tile"><div class="value num">' + flaky.length + '</div><div class="label">' +
      'flaky</div></div><div class="tile"><div class="value num">' + active.length + '</div><div class="label">' +
      'quarantined</div></div><div class="tile"><div class="value num">' + q.bisections.filter((b) => ['narrowing',
      'rerunning'].includes(b.state)).length + '</div><div class="label">bisections running</div></div></div>' +
      section('Failing now', ['Lane', 'Test', 'Fails', 'Median'], failing.map((t) => [esc(t.lane), '<a href="#" ' +
        'data-history="' + esc(JSON.stringify([t.lane, t.test])) + '">' + esc(t.test) + '</a>', t.fail, t.median_s.toFixed(2) +
        ' s']), 'No test fails in its lane\'s newest run.') +
      section('Quarantined', ['Lane', 'Test', 'Until', 'Issue', ''], q.quarantine.map((r) => [esc(r.lane), esc(r.test),
        esc(r.expires_text) + (r.active && r.expires > q.now ? '' : ' (ended)'), esc(r.issue), r.active &&
          r.expires > q.now ? '<button type="button" data-release="' + esc(JSON.stringify([r.lane, r.test])) + '">Release' +
          '</button>' : '']), 'Nothing is quarantined.') +
      section('First bad commit', ['Lane', 'Test', 'State', 'Good', 'Bad', 'First bad', 'Re-runs'], q.bisections.map((b) =>
        [esc(b.lane), esc(b.tests.join(', ')), esc(b.state), '<span class="mono">' + esc((b.good.commit || '').slice(0, 10)) +
          '</span>', '<span class="mono">' + esc((b.bad.commit || '').slice(0, 10)) + '</span>', '<span class="mono">' +
          esc((b.first_bad || '').slice(0, 12)) + '</span>', b.reruns.length]), 'No bisection: no test newly failed.') +
      section('Flaky tests', ['Lane', 'Test', 'Runs', 'Passed', 'Failed'], flaky.map((t) => [esc(t.lane), '<a href="#" ' +
        'data-history="' + esc(JSON.stringify([t.lane, t.test])) + '">' + esc(t.test) + '</a>', t.runs, t.pass, t.fail]),
      'No flaky test.') + q.issues.map((i) => '<section class="card"><header><h2>' + esc(i.id) + '</h2>' +
        statusHtml(i.state === 'open' ? 'failed' : 'passed') + '</header><div class="body">' + esc(i.text) + '</div>' +
        '</section>').join('') + '</div>';
    view.querySelectorAll('[data-release]').forEach((button) => button.addEventListener('click', async () => {
      const [lane, test] = JSON.parse(button.dataset.release);
      if (confirmAction('Release the quarantine of ' + test + '? Its failures fail ' + lane + ' again.')) {
        await post('quality/release', { lane: lane, test: test }).then(() => toast('Quarantine released')).catch((e) =>
          toast(e.message, true));
        route();
      }
    }));
    view.querySelectorAll('[data-history]').forEach((link) => link.addEventListener('click', async (event) => {
      event.preventDefault();
      const [lane, test] = JSON.parse(link.dataset.history);
      const data = await api('quality/test?lane=' + encodeURIComponent(lane) + '&test=' + encodeURIComponent(test));
      openDrawer(test, '<p class="subtle">' + esc(lane) + '</p>' + spark(data.history) + '<div class="table-wrap">' +
        '<table><thead><tr><th scope="col">Build</th><th scope="col">Commit</th><th scope="col">Outcome</th><th ' +
        'scope="col" class="num">Seconds</th></tr></thead><tbody>' + data.history.slice().reverse().map((h) => '<tr><td>' +
          '<a href="#/builds/' + esc(h.build) + '">#' + esc(h.build) + '</a></td><td class="mono">' + esc((h.commit || '')
          .slice(0, 10)) + '</td><td>' + statusHtml(h.outcome) + '</td><td class="num">' + esc(h.seconds) + '</td></tr>')
        .join('') + '</tbody></table></div>');
    }));
  }

  const section = (title, headers, rows, emptyText) => '<section class="card flush"><header><h2>' + esc(title) +
    '</h2><span class="subtle">' + rows.length + '</span></header>' + (rows.length ? '<div class="table-wrap"><table>' +
    '<thead><tr>' + headers.map((h) => '<th scope="col">' + (h ? esc(h) : '<span class="visually-hidden">Actions</span>') +
      '</th>').join('') + '</tr></thead><tbody>' + rows.map((r) => '<tr>' + r.map((c) => '<td>' + c + '</td>').join('') +
      '</tr>').join('') + '</tbody></table></div>' : '<div class="body subtle">' + esc(emptyText) + '</div>') +
    '</section>';

  // Gold images: triage ------------------------------------------------------------------------------------------------
  async function goldPage() {
    const data = await api('gold');
    const params = new URLSearchParams(location.hash.split('?')[1] || '');
    let items = data.items;
    let current = Math.max(0, items.findIndex((i) => i.digest === params.get('digest')));
    let zoom = 0;
    let blink = null;
    const reduced = window.matchMedia && window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    view.innerHTML = '<div class="stack"><div class="page-head"><div class="grow"><h1>Gold images</h1><p ' +
      'class="muted">Rendered images that match no approved baseline. Software renderers are compared exactly, real ' +
      'GPUs and phones perceptually (LDR-FLIP and SSIM against each test\'s declared tolerances). Until a person ' +
      'approves or rejects an image its QUAL cell reads needs triage, never pass.</p></div></div><div class="tiles">' +
      '<div class="tile"><div class="value num">' + items.length + '</div><div class="label">' + statusHtml('triage') +
      '</div></div><div class="tile"><div class="value num">' + data.pending + '</div><div class="label">results not ' +
      'yet compared</div></div><div class="tile"><div class="value num">' + data.findings.length + '</div><div ' +
      'class="label">recent rejections</div></div></div><div class="filters" role="search" aria-label="Filter the ' +
      'images"><label class="field">Test or device<input id="gold-filter" type="search" placeholder="Filter (/)" ' +
      'autocomplete="off"></label></div><div class="gold"><section class="card flush queue" aria-labelledby=' +
      '"queue-title"><header><h2 id="queue-title">To triage</h2><span class="subtle" id="queue-count"></span></header>' +
      '<ul class="list" id="gold-queue"></ul></section><section class="detail" id="gold-detail" aria-live="polite">' +
      '</section></div>' + (data.findings.length ? section('Rejected images (findings)', ['When', 'Test', 'By',
        'Finding'], data.findings.map((f) => [esc(when(f.at)), esc(f.test), esc(f.by), esc(f.text)]), '') : '') +
      '</div>';
    const queue = document.getElementById('gold-queue');
    const detail = document.getElementById('gold-detail');
    const shown = () => {
      const filter = document.getElementById('gold-filter').value.toLowerCase();
      return items.filter((i) => !filter || (i.test + ' ' + i.occurrences.map((o) => o.device).join(' '))
        .toLowerCase().includes(filter));
    };
    const stopBlink = () => {
      clearInterval(blink);
      blink = null;
    };
    state.cleanup.push(stopBlink);
    const imageUrl = (digest) => '/api/gold/image/' + digest;
    const pane = (title, url, alt, empty) => '<div class="pane' + (zoom === 0 ? ' fit' : '') + '"><header>' +
      esc(title) + '</header><div class="view" tabindex="0" aria-label="' + esc(title) + '">' + (url ? '<img src="' +
      esc(url) + '" alt="' + esc(alt) + '" data-zoomable>' : '<div class="none">' + esc(empty) + '</div>') +
      '</div></div>';
    const applyZoom = () => {
      detail.querySelectorAll('.pane').forEach((p) => p.classList.toggle('fit', zoom === 0));
      detail.querySelectorAll('img[data-zoomable]').forEach((img) => {
        const set = () => {
          img.style.width = zoom === 0 ? '' : (img.naturalWidth * zoom) + 'px';
        };
        if (img.complete) {
          set();
        } else {
          img.addEventListener('load', set, { once: true });
        }
      });
      const label = document.getElementById('zoom-level');
      if (label) {
        label.textContent = zoom === 0 ? 'Fit' : zoom + 'x';
      }
    };
    const syncScroll = () => {
      const views = Array.from(detail.querySelectorAll('.pane .view'));
      views.forEach((v) => v.addEventListener('scroll', () => {
        views.forEach((other) => {
          if (other !== v) {
            other.scrollLeft = v.scrollLeft;
            other.scrollTop = v.scrollTop;
          }
        });
      }));
    };
    const paintQueue = () => {
      const list = shown();
      document.getElementById('queue-count').textContent = list.length + ' of ' + items.length;
      queue.innerHTML = list.length ? list.map((i) => '<li><a class="item" data-item href="#/gold?digest=' +
        esc(i.digest) + '" data-digest="' + esc(i.digest) + '" aria-selected="' + String(items[current] === i) + '">' +
        '<span><span class="title">' + esc(i.test) + '</span><span class="meta"><span>' + esc(i.mode) + '</span><span>' +
        i.occurrences.length + ' run' + (i.occurrences.length === 1 ? '' : 's') + '</span><span>' +
        esc(Array.from(new Set(i.occurrences.map((o) => o.device))).join(', ')) + '</span></span></span><span>' +
        statusHtml(i.state) + '</span></a></li>').join('') : '<li>' + empty('Nothing to triage', 'Every rendered ' +
        'image matches an approved baseline or is within its tolerance.') + '</li>';
      queue.querySelectorAll('[data-digest]').forEach((a) => a.addEventListener('click', (event) => {
        event.preventDefault();
        current = items.findIndex((i) => i.digest === a.dataset.digest);
        paintQueue();
        paintDetail();
      }));
    };
    const paintDetail = () => {
      stopBlink();
      const item = items[current];
      if (!item) {
        detail.innerHTML = '';
        return;
      }
      const comparison = item.comparison || {};
      const reference = comparison.against;
      const metrics = comparison.metrics || {};
      const diff = reference && metrics.diff && !metrics.size_mismatch ? '/api/gold/diff/' + reference + '/' +
        item.digest + '/' + metrics.diff : null;
      const rows = goldMetrics(item);
      const group = Object.entries(item.group || {}).filter(([k]) => k !== 'test').map(([k, v]) => k + ' ' + v)
        .join(' · ');
      detail.innerHTML = '<div class="card"><header><h2 class="grow">' + esc(item.test) + '</h2>' + statusHtml(
        item.state) + '</header><div class="body stack"><p class="subtle">' + esc(group || 'one baseline set for ' +
        'every device') + ' · ' + esc(item.mode === 'exact' ? 'exact comparison (a software renderer)' :
        'perceptual comparison at ' + (item.ppd || 67) + ' pixels per degree') + ' · ' + esc((item.size || [])
        .join('x')) + '</p><div class="row" role="toolbar" aria-label="View"><button type="button" id="blink" ' +
        'aria-pressed="false"' + (reference ? '' : ' disabled') + ' title="Alternate the approved and the new image ' +
        '(b)">' + (reduced ? 'Flip (b)' :
        'Blink (b)') + '</button><button type="button" id="zoom-out" title="Zoom out (-)">−<span ' +
        'class="visually-hidden">Zoom out</span></button><span class="subtle num" id="zoom-level" aria-live="polite">' +
        'Fit</span><button type="button" id="zoom-in" title="Zoom in (+)">+<span class="visually-hidden">Zoom in' +
        '</span></button><button type="button" id="zoom-fit" title="Fit (0)">Fit</button></div><div class="panes" ' +
        'id="panes">' + pane('Approved', reference ? imageUrl(reference) : null, 'The approved image of ' + item.test,
        'No approved image yet: approving makes this the first baseline.') + pane('New', imageUrl(item.digest),
        'The new image of ' + item.test, '') + pane('Difference (FLIP)', diff, 'The FLIP difference map, brighter ' +
        'is more visible', metrics.size_mismatch ? 'The sizes differ.' : reference ? 'No difference map.' :
        'Nothing to compare with.') + '</div>' + (rows.length ? '<div class="table-wrap"><table class="metrics">' +
        '<thead><tr><th scope="col">Metric</th><th scope="col" class="num">Value</th><th scope="col">Tolerance</th>' +
        '</tr></thead><tbody>' + rows.map((r) => '<tr><td>' + esc(r.label) + '</td><td class="num ' + (r.ok ===
          null ? '' : r.ok ? 'ok' : 'bad') + '">' + esc(r.value) + (r.ok === null ? '' : r.ok ? ' (within)' :
          ' (over)') + '</td><td>' + esc(r.limit) + '</td></tr>').join('') + '</tbody></table></div>' : '') +
        '<details><summary>Where it was rendered (' + item.occurrences.length + ')</summary><div class="table-wrap">' +
        '<table><thead><tr><th scope="col">Build</th><th scope="col">Device</th><th scope="col">API</th><th ' +
        'scope="col">CPU level</th><th scope="col">Commit</th></tr></thead><tbody>' + item.occurrences.map((o) =>
        '<tr><td><a href="#/builds/' + esc(o.build) + '">#' + esc(o.build) + '</a></td><td>' + esc(o.device) +
          '</td><td>' + esc(((o.keys || {}).api || '') + ' ' + ((o.keys || {}).backend || '')) + '</td><td>' +
          esc((o.keys || {}).cpu_level || '') + '</td><td class="mono">' + esc((o.commit || '').slice(0, 10)) +
          '</td></tr>').join('') + '</tbody></table></div></details><div class="triage-actions"><label ' +
        'class="visually-hidden" for="triage-note">Note</label><input id="triage-note" placeholder="Note (why), ' +
        'kept in the audit log" autocomplete="off"><button type="button" class="approve" id="approve">' +
        '<svg aria-hidden="true"><use href="#i-passed"/></svg>Approve (a)</button><button type="button" ' +
        'class="danger" id="reject"><svg aria-hidden="true"><use href="#i-failed"/></svg>Reject (r)</button></div>' +
        '<p class="subtle">Approve makes this image a baseline of its grouping; Reject marks it wrong and files a ' +
        'finding. Both are audited.</p></div></div>';
      applyZoom();
      syncScroll();
      document.getElementById('zoom-in').addEventListener('click', () => {
        zoom = zoomStep(zoom, 1);
        applyZoom();
      });
      document.getElementById('zoom-out').addEventListener('click', () => {
        zoom = zoomStep(zoom, -1);
        applyZoom();
      });
      document.getElementById('zoom-fit').addEventListener('click', () => {
        zoom = 0;
        applyZoom();
      });
      document.getElementById('blink').addEventListener('click', toggleBlink);
      document.getElementById('approve').addEventListener('click', () => decide('positive'));
      document.getElementById('reject').addEventListener('click', () => decide('negative'));
    };
    const toggleBlink = () => {
      const item = items[current];
      const button = document.getElementById('blink');
      const reference = item && (item.comparison || {}).against;
      if (!item || !reference) {
        toast('There is no approved image to compare with');
        return;
      }
      const panes = document.getElementById('panes');
      const showing = panes.dataset.showing === 'new' ? 'reference' : 'new';
      if (!panes.classList.contains('blink')) {
        panes.classList.add('blink');
        panes.innerHTML = pane('Approved', imageUrl(reference), 'The approved image of ' + item.test, '');
        panes.dataset.showing = 'reference';
        applyZoom();
        button.setAttribute('aria-pressed', 'true');
        if (!reduced) {
          blink = setInterval(() => swap(panes, item, reference), 600);
        }
        return;
      }
      if (reduced && button.getAttribute('aria-pressed') === 'true' && showing) {
        swap(panes, item, reference);
        return;
      }
      button.setAttribute('aria-pressed', 'false');
      paintDetail();
    };
    const swap = (panes, item, reference) => {
      const next = panes.dataset.showing === 'new' ? 'reference' : 'new';
      const img = panes.querySelector('img');
      const title = panes.querySelector('.pane > header');
      if (!img || !title) {
        return;
      }
      img.src = imageUrl(next === 'new' ? item.digest : reference);
      img.alt = (next === 'new' ? 'The new image of ' : 'The approved image of ') + item.test;
      title.textContent = next === 'new' ? 'New' : 'Approved';
      panes.dataset.showing = next;
    };
    const decide = async (label) => {
      const item = items[current];
      if (!item) {
        return;
      }
      const note = (document.getElementById('triage-note') || {}).value || '';
      if (label === 'negative' && !confirmAction('Reject this image of ' + item.test + '? Its cells fail and a ' +
          'finding is filed.')) {
        return;
      }
      try {
        const result = await post('gold/triage', { grouping: item.grouping, digest: item.digest, expected:
          item.label, label: label, note: note });
        toast((label === 'positive' ? 'Approved: ' : 'Rejected: ') + item.test + (result.finding ? ' (finding ' +
          result.finding.id + ')' : ''));
        announce((label === 'positive' ? 'Approved ' : 'Rejected ') + item.test);
        items = items.filter((i) => i !== item);
        current = Math.min(current, items.length - 1);
        paintQueue();
        paintDetail();
      } catch (error) {
        toast(error.message, true);
        if (error.status === 409) {
          route();
        }
      }
    };
    document.getElementById('gold-filter').addEventListener('input', () => {
      paintQueue();
    });
    state.keys = {
      a: () => decide('positive'), r: () => decide('negative'), b: toggleBlink,
      '+': () => document.getElementById('zoom-in') && document.getElementById('zoom-in').click(),
      '=': () => document.getElementById('zoom-in') && document.getElementById('zoom-in').click(),
      '-': () => document.getElementById('zoom-out') && document.getElementById('zoom-out').click(),
      0: () => document.getElementById('zoom-fit') && document.getElementById('zoom-fit').click(),
    };
    state.onSelect = (index) => {
      const list = shown();
      if (list[index]) {
        current = items.indexOf(list[index]);
        paintQueue();
        paintDetail();
        const node = queue.querySelectorAll('[data-item]')[index];
        if (node) {
          node.focus();
        }
      }
    };
    paintQueue();
    paintDetail();
  }

  // Agents ------------------------------------------------------------------------------------------------------------
  async function agents() {
    const [a, q] = await Promise.all([api('agents'), api('queues')]);
    view.innerHTML = '<div class="stack"><div class="page-head"><div class="grow"><h1>Agents and queues</h1><p ' +
      'class="muted">Owned machines and the launcher poll their queues; cloud machines are one-job agents and show ' +
      'under the step they ran.</p></div></div>' + section('Queues', ['Queue', 'Agents', 'Waiting', 'Starting', 'Running',
      'Oldest wait'], q.queues.map((x) => [esc(x.queue), x.agents, x.scheduled, x.assigned, x.running,
      esc(durationText(0, x.oldest_wait_s))]), 'No active work.') + section('Agents', ['Name', 'Status', 'Queue', 'Tags',
      'Platform', 'Version', 'Last seen', ''], a.agents.map((x) => [esc(x.name), statusHtml(x.status), esc(x.queue),
      '<span class="mono">' + esc(Object.entries(x.tags || {}).map(([k, v]) => k + '=' + v).join(' ')) + '</span>',
      esc(x.os + ' ' + x.arch), esc(x.version), esc(ago(x.last_seen)), x.status !== 'stopped' ? '<button type="button" ' +
        'class="danger" data-stop="' + esc(x.id) + '">Stop</button>' : '']), 'No agent registered: create a token with ' +
      'python ci/ops.py token create.') + '</div>';
    view.querySelectorAll('[data-stop]').forEach((el) => el.addEventListener('click', async () => {
      if (confirmAction('Stop this agent? It takes no new job until it registers again.')) {
        await post('agents/' + el.dataset.stop + '/stop').then(() => toast('Agent stopped')).catch((e) =>
          toast(e.message, true));
        route();
      }
    }));
    state.timer = setTimeout(route, 15000);
  }

  // Operations: SLOs and audit log -------------------------------------------------------------------------------------
  async function operations() {
    const [slo, audit] = await Promise.all([api('slo'), api('audit?days=14')]);
    const availability = slo.availability === null ? 'No tick recorded yet' : (slo.availability * 100).toFixed(3) +
      ' % (objective ' + (slo.availability_objective * 100).toFixed(1) + ' %)';
    view.innerHTML = '<div class="stack"><div class="page-head"><div class="grow"><h1>Operations</h1><p class="muted">' +
      'Service-level objectives over ' + slo.window_days + ' days and the audit log of every manual and automatic ' +
      'decision.</p></div></div><div class="tiles"><div class="tile"><div class="value">' + statusHtml(
        slo.availability_met ? 'met' : 'missed') + '</div><div class="label">Coordinator availability: ' +
      esc(availability) + '</div></div></div>' + section('Queue time', ['Queue', 'Samples', 'p50', 'p95', 'Target',
      'Within target', 'Objective'], slo.queues.map((x) => [esc(x.queue), x.samples, esc(x.p50_s) + ' s', esc(x.p95_s) +
      ' s', esc(x.target_s) + ' s', (x.within * 100).toFixed(1) + ' %', statusHtml(x.met ? 'met' : 'missed')]),
      'No assignment measured yet.') + section('Audit log', ['When', 'Who', 'Action', 'Target', 'Detail'],
      audit.entries.map((e) => [esc(when(e.at)), esc(e.who), esc(e.action), esc(e.target), esc(e.detail)]),
      'No audited action in 14 days.') + '</div>';
  }

  // Router and keyboard --------------------------------------------------------------------------------------------------
  const ROUTES = { builds: builds, qual: qual, quality: quality, gold: goldPage, spend: spend, agents: agents,
    operations: operations };
  async function route() {
    stopLive();
    const hash = location.hash.replace(/^#/, '').split('?')[0] || '/builds';
    const parts = hash.split('/').filter(Boolean);
    const section = parts[0] === 'jobs' ? 'builds' : parts[0];
    document.querySelectorAll('[data-nav]').forEach((a) => {
      if (a.dataset.nav === section) {
        a.setAttribute('aria-current', 'page');
      } else {
        a.removeAttribute('aria-current');
      }
    });
    state.selection = -1;
    state.jumpError = null;
    state.keys = null;
    state.onSelect = null;
    const changed = hash !== state.lastRoute;
    if (changed) {
      loading();
    }
    try {
      if (parts[0] === 'builds' && parts[1]) {
        await build(parts[1]);
      } else if (parts[0] === 'jobs' && parts[1]) {
        await job(parts[1]);
      } else {
        await (ROUTES[parts[0]] || builds)();
      }
      if (!parts[1]) {
        document.title = (document.querySelector('h1') || {}).textContent + ' · Cerid CI';
      }
      if (changed) {
        view.focus({ preventScroll: true });
      }
      state.lastRoute = hash;
    } catch (error) {
      failed(error);
    }
  }

  const GO = { b: '#/builds', m: '#/qual', i: '#/gold', t: '#/quality', s: '#/spend', a: '#/agents',
    o: '#/operations' };
  document.addEventListener('keydown', (event) => {
    const target = event.target;
    const typing = target && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.tagName ===
      'SELECT' || target.isContentEditable);
    if (event.metaKey || event.ctrlKey || event.altKey) {
      return;
    }
    if (typing) {
      if (event.key === 'Escape') {
        target.blur();
      }
      return;
    }
    if (document.querySelector('dialog[open]')) {
      return;
    }
    if (state.chord === 'g') {
      state.chord = null;
      if (GO[event.key]) {
        event.preventDefault();
        location.hash = GO[event.key];
      }
      return;
    }
    const key = event.key;
    if (key === 'g') {
      state.chord = 'g';
      setTimeout(() => {
        state.chord = null;
      }, 1200);
    } else if (key === '?') {
      document.getElementById('help').showModal();
    } else if (key === '/') {
      const search = view.querySelector('input[type="search"]');
      if (search) {
        event.preventDefault();
        search.focus();
      }
    } else if (key === 'j' || key === 'k') {
      select(state.selection + (key === 'j' ? 1 : -1));
      if (state.onSelect) {
        state.onSelect(state.selection);
      }
    } else if (state.keys && state.keys[key]) {
      event.preventDefault();
      state.keys[key]();
    } else if (key === 'e' && state.jumpError) {
      state.jumpError();
    } else if ((key === 'n' || key === 'N') && state.gotoHit) {
      state.gotoHit(key === 'n' ? 1 : -1);
    } else if (key === 'f' && state.toggleFollow) {
      state.toggleFollow();
    }
  });

  document.querySelectorAll('dialog').forEach((dialog) => {
    dialog.addEventListener('click', (event) => {
      if (event.target === dialog || event.target.closest('[data-close]')) {
        dialog.close();
      }
    });
  });
  document.getElementById('help-open').addEventListener('click', () => document.getElementById('help').showModal());
  document.getElementById('new-build-form').addEventListener('submit', async (event) => {
    const form = new FormData(event.target);
    const env = {};
    if (form.get('tier')) {
      env.CERID_CI_TIER = form.get('tier');
    }
    if (form.get('fresh')) {
      env.CERID_CI_FRESH = '1';
    }
    try {
      const made = await post('builds', { repository: form.get('repository'), branch: form.get('branch'),
        commit: form.get('commit'), env: env });
      toast('Build #' + made.build + ' scheduled');
      location.hash = '#/builds/' + made.build;
    } catch (error) {
      toast(error.message, true);
    }
  });

  window.addEventListener('hashchange', route);
  api('me').then((me) => {
    document.getElementById('who').textContent = me.user + ' · ' + me.version;
  }).catch(() => {});
  route();
}

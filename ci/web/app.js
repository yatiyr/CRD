// The Cerid CI web UI: plain JavaScript, no build step, served by the coordinator behind Cloudflare Access or IAP.
// Pages: builds, a build (step graph, jobs, annotations, artifacts), a job's log (live tail, ANSI colours, search,
// download), agents and queues, spend against the cap, and the QUAL dashboard. Every value from the API is escaped.
'use strict';

const ESC = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
function esc(text) {
  return String(text === undefined || text === null ? '' : text).replace(/[&<>"']/g, (c) => ESC[c]);
}

// ANSI SGR colours to HTML: 0 reset, 1 bold, 22 normal, 30-37 and 90-97 foreground, 39 default. Every other escape
// sequence (cursor moves, erase, OSC titles) is dropped. The text itself is escaped first.
function ansiToHtml(text) {
  let out = '';
  let bold = false;
  let fg = null;
  const pattern = /\x1b\[([0-9;]*)([A-Za-z])|\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)/g;
  let last = 0;
  let match;
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
    const codes = (match[1] || '0').split(';').map((c) => parseInt(c || '0', 10));
    for (const code of codes) {
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
      out.push('<table>' + rows.map((r) => '<tr>' + r.join('') + '</tr>').join('') + '</table>');
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

function durationText(start, end) {
  if (!start) {
    return '';
  }
  const seconds = Math.max(0, Math.round((end || Date.now() / 1000) - start));
  const m = Math.floor(seconds / 60);
  return m >= 60 ? Math.floor(m / 60) + 'h ' + (m % 60) + 'm' : m + 'm ' + (seconds % 60) + 's';
}

function stateBadge(job) {
  const soft = job.soft_failed ? ' (soft)' : '';
  return '<span class="badge s-' + esc(job.soft_failed ? 'soft' : job.state) + '">' + esc(job.state + soft) + '</span>';
}

if (typeof module !== 'undefined') {
  module.exports = { esc: esc, ansiToHtml: ansiToHtml, markdown: markdown, layout: layout, durationText: durationText };
}

if (typeof document !== 'undefined') {
  const view = document.getElementById('view');
  let timer = null;

  const api = async (path, options) => {
    const init = Object.assign({ headers: {}, credentials: 'same-origin' }, options || {});
    if (init.method && init.method !== 'GET') {
      init.headers['X-Cerid-Request'] = '1';
      init.headers['Content-Type'] = 'application/json';
    }
    const response = await fetch('/api/' + path, init);
    const body = await response.json().catch(() => ({}));
    if (!response.ok) {
      throw new Error(body.error || response.statusText);
    }
    return body;
  };
  const post = (path, body) => api(path, { method: 'POST', body: JSON.stringify(body || {}) });
  const when = (t) => (t ? new Date(t * 1000).toISOString().replace('T', ' ').slice(0, 19) + 'Z' : '');
  const fail = (error) => {
    view.innerHTML = '<p class="error">' + esc(error.message || error) + '</p>';
  };
  const later = (fn, ms) => {
    clearTimeout(timer);
    timer = setTimeout(fn, ms);
  };

  async function builds() {
    const data = await api('builds?limit=100');
    const rows = data.builds.map((b) => '<tr><td><a href="#/builds/' + b.number + '">#' + b.number + '</a></td><td>' +
      stateBadge(b) + '</td><td>' + esc(b.repository) + '</td><td>' + esc(b.branch) +
      (b.pull_request ? ' (PR ' + esc(b.pull_request) + ')' : '') + '</td><td class="mono">' +
      esc((b.commit || '').slice(0, 12)) + '</td><td>' + esc(b.message) + '</td><td>' + esc(b.source) + '</td><td>' +
      esc(when(b.created)) + '</td><td>' + esc(durationText(b.started_at, b.finished_at)) + '</td></tr>');
    view.innerHTML = '<h1>Builds</h1><div class="panel"><form id="new" class="row">' +
      '<input name="repository" placeholder="repository" value="cerid" required>' +
      '<input name="branch" placeholder="branch" value="main" required>' +
      '<input name="commit" placeholder="commit (default: branch head)" class="grow">' +
      '<select name="tier"><option value="">tier: automatic</option><option>change</option><option>complete</option>' +
      '</select><label><input type="checkbox" name="fresh"> ignore the cache</label>' +
      '<button>New build</button></form></div><div class="panel scroll"><table><thead><tr><th>Build</th><th>State</th>' +
      '<th>Repository</th><th>Branch</th><th>Commit</th><th>Message</th><th>Source</th><th>Created</th><th>Time</th>' +
      '</tr></thead><tbody>' + (rows.join('') || '<tr><td colspan="9" class="muted">No builds yet.</td></tr>') +
      '</tbody></table></div>';
    document.getElementById('new').addEventListener('submit', async (event) => {
      event.preventDefault();
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
        location.hash = '#/builds/' + made.build;
      } catch (error) {
        alert(error.message);
      }
    });
    later(route, 10000);
  }

  function graph(jobs, selected) {
    const { columns, place } = layout(jobs);
    const w = 190;
    const h = 34;
    const gx = 50;
    const gy = 12;
    const height = Math.max(...columns.map((c) => c.length), 1) * (h + gy) + gy;
    const width = columns.length * (w + gx) + gx;
    let edges = '';
    let nodes = '';
    for (const job of jobs) {
      const p = place[job.key];
      const x = gx / 2 + p.x * (w + gx);
      const y = gy + p.y * (h + gy);
      for (const dep of job.depends_on || []) {
        const q = place[dep.step];
        if (!q) {
          continue;
        }
        const x1 = gx / 2 + q.x * (w + gx) + w;
        const y1 = gy + q.y * (h + gy) + h / 2;
        edges += '<path class="edge" d="M' + x1 + ',' + y1 + ' C' + (x1 + gx / 2) + ',' + y1 + ' ' + (x - gx / 2) +
          ',' + (y + h / 2) + ' ' + x + ',' + (y + h / 2) + '"' + (dep.allow_failure ? ' stroke-dasharray="4 3"' : '') +
          '/>';
      }
      const colour = { passed: 'var(--pass)', failed: 'var(--fail)', timed_out: 'var(--fail)', expired: 'var(--fail)',
        running: 'var(--run)', assigned: 'var(--run)', cached: 'var(--cache)', budget: 'var(--budget)',
        canceled: '#555' }[job.state] || 'var(--wait)';
      nodes += '<a href="#/jobs/' + esc(job.id) + '"><g class="node' + (job.id === selected ? ' sel' : '') +
        '"><rect x="' + x + '" y="' + y + '" width="' + w + '" height="' + h + '"/><circle class="dot" cx="' + (x + 12) +
        '" cy="' + (y + h / 2) + '" r="5" fill="' + colour + '"/><text x="' + (x + 24) + '" y="' + (y + h / 2 + 4) + '">' +
        esc(job.label.length > 26 ? job.label.slice(0, 25) + '…' : job.label) + '</text><title>' + esc(job.key + ': ' +
        job.state + (job.reason ? ' — ' + job.reason : '')) + '</title></g></a>';
    }
    return '<div class="graph panel"><svg width="' + width + '" height="' + height + '" role="img" aria-label="step graph">' +
      edges + nodes + '</svg></div>';
  }

  async function build(number) {
    const b = await api('builds/' + number);
    const live = !['passed', 'failed', 'canceled'].includes(b.state);
    const jobs = b.jobs.map((j) => '<tr><td><a href="#/jobs/' + esc(j.id) + '">' + esc(j.label) + '</a><div class="muted mono">' +
      esc(j.key) + '</div></td><td>' + stateBadge(j) + (j.attempt > 1 ? ' <span class="muted">attempt ' + j.attempt +
      '</span>' : '') + '</td><td>' + esc(j.agents && j.agents.queue) + (j.agents && j.agents.device ? ' · ' +
      esc(j.agents.device) : '') + '</td><td>' + esc(j.agent_name || j.machine || j.agent || '') + '</td><td>' +
      esc(durationText(j.started_at, j.finished_at)) + '</td><td>' + esc(j.waiting && j.waiting !== 'ready' ? j.waiting :
        j.reason || '') + '</td><td>' + (['failed', 'canceled', 'timed_out', 'expired', 'budget'].includes(j.state) ?
        '<button data-retry="' + esc(j.id) + '">Retry</button>' : '') + (['scheduled', 'assigned', 'running'].includes(j.state) ?
        ' <button class="danger" data-cancel="' + esc(j.id) + '">Cancel</button>' : '') + '</td></tr>');
    const notes = Object.entries(b.annotations || {}).map(([context, note]) => '<div class="note ' + esc(note.style) +
      '"><div class="muted">' + esc(context) + '</div>' + markdown(note.body) + '</div>');
    const artifacts = (b.artifacts || []).map((a) => '<li><a href="/api/jobs/' + esc(a.job) + '/artifacts?path=' +
      encodeURIComponent(a.path) + '">' + esc(a.path) + '</a> <span class="muted">' + esc(a.key) + ', ' + a.size +
      ' bytes</span></li>');
    const meta = Object.entries(b.meta || {}).map(([k, v]) => '<tr><td class="mono">' + esc(k) + '</td><td class="mono">' +
      esc(v.length > 200 ? v.slice(0, 200) + '…' : v) + '</td></tr>');
    view.innerHTML = '<div class="row"><h1 class="grow">Build #' + b.number + ' ' + stateBadge(b) + '</h1>' +
      (live ? '<button class="danger" id="cancel">Cancel build</button>' : '') + '</div><div class="panel">' +
      '<div><b>' + esc(b.message || '(no message)') + '</b></div><div class="muted">' + esc(b.repository) + ' · ' +
      esc(b.branch) + (b.pull_request ? ' · PR ' + esc(b.pull_request) + ' into ' + esc(b.base_branch) : '') +
      ' · <span class="mono">' + esc(b.commit) + '</span> · ' + esc(b.source) + (b.created_by ? ' by ' +
      esc(b.created_by) : '') + ' · ' + esc(when(b.created)) + ' · ' + esc(durationText(b.started_at, b.finished_at)) +
      (b.trusted ? '' : ' · untrusted (no secrets)') + '</div></div>' + graph(b.jobs) +
      '<div class="panel scroll"><table><thead><tr><th>Step</th><th>State</th><th>Queue</th><th>Agent</th><th>Time</th>' +
      '<th>Reason</th><th></th></tr></thead><tbody>' + jobs.join('') + '</tbody></table></div>' +
      (notes.length ? '<h2>Annotations</h2>' + notes.join('') : '') +
      (artifacts.length ? '<h2>Artifacts</h2><div class="panel"><ul>' + artifacts.join('') + '</ul></div>' : '') +
      (meta.length ? '<h2>Meta-data</h2><div class="panel"><table>' + meta.join('') + '</table></div>' : '');
    const cancel = document.getElementById('cancel');
    if (cancel) {
      cancel.addEventListener('click', async () => {
        if (confirm('Cancel build #' + b.number + '?')) {
          await post('builds/' + b.number + '/cancel').catch((e) => alert(e.message));
          route();
        }
      });
    }
    view.querySelectorAll('[data-retry]').forEach((el) => el.addEventListener('click', async () => {
      await post('jobs/' + el.dataset.retry + '/retry').catch((e) => alert(e.message));
      route();
    }));
    view.querySelectorAll('[data-cancel]').forEach((el) => el.addEventListener('click', async () => {
      await post('jobs/' + el.dataset.cancel + '/cancel').catch((e) => alert(e.message));
      route();
    }));
    if (live) {
      later(route, 3000);
    }
  }

  async function job(id) {
    const j = await api('jobs/' + id);
    const params = new URLSearchParams(location.hash.split('?')[1] || '');
    const attempt = parseInt(params.get('attempt') || j.attempt, 10);
    const attempts = [];
    for (let a = 1; a <= j.attempt; a += 1) {
      attempts.push('<option value="' + a + '"' + (a === attempt ? ' selected' : '') + '>attempt ' + a + '</option>');
    }
    view.innerHTML = '<div class="row"><h1 class="grow"><a href="#/builds/' + j.build + '">#' + j.build + '</a> · ' +
      esc(j.label) + ' ' + stateBadge(j) + '</h1><select id="attempt">' + attempts.join('') + '</select>' +
      '<input id="search" placeholder="search the log"><a href="/api/jobs/' + esc(id) + '/log?raw=1&attempt=' + attempt +
      '"><button>Download</button></a>' + (['scheduled', 'assigned', 'running'].includes(j.state) ?
        '<button class="danger" id="cancel">Cancel</button>' : '') + '</div><div class="muted">' + esc(j.reason || '') +
      '</div><pre class="log" id="log"></pre><p class="muted" id="tail"></p>';
    const logEl = document.getElementById('log');
    const tail = document.getElementById('tail');
    const search = document.getElementById('search');
    let text = '';
    let next = 0;
    let stick = true;
    logEl.addEventListener('scroll', () => {
      stick = logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 20;
    });
    const paint = () => {
      const needle = search.value.toLowerCase();
      const lines = text.split('\n');
      let html = '';
      lines.forEach((line, n) => {
        if (needle && !line.toLowerCase().includes(needle)) {
          return;
        }
        let rendered = ansiToHtml(line);
        if (needle) {
          rendered = '<mark>' + rendered + '</mark>';
        }
        html += '<span class="ln' + (line.startsWith('~~~') ? ' hdr' : '') + '" id="L' + (n + 1) + '">' + rendered + '</span>';
      });
      logEl.innerHTML = html;
      if (stick && !needle) {
        logEl.scrollTop = logEl.scrollHeight;
      }
    };
    search.addEventListener('input', paint);
    document.getElementById('attempt').addEventListener('change', (event) => {
      location.hash = '#/jobs/' + id + '?attempt=' + event.target.value;
    });
    const cancel = document.getElementById('cancel');
    if (cancel) {
      cancel.addEventListener('click', async () => {
        await post('jobs/' + id + '/cancel').catch((e) => alert(e.message));
      });
    }
    const pull = async () => {
      if (!document.body.contains(logEl)) {
        return;
      }
      const chunk = await api('jobs/' + id + '/log?attempt=' + attempt + '&from=' + next).catch(fail);
      if (!chunk) {
        return;
      }
      if (chunk.text) {
        text += chunk.text;
        paint();
      }
      next = chunk.next;
      tail.textContent = chunk.live ? 'live: ' + chunk.state + '…' : 'finished: ' + chunk.state;
      if (chunk.live) {
        later(pull, 1500);
      }
    };
    pull();
  }

  async function agents() {
    const [a, q] = await Promise.all([api('agents'), api('queues')]);
    view.innerHTML = '<h1>Agents and queues</h1><h2>Queues</h2><div class="panel scroll"><table><thead><tr><th>Queue</th>' +
      '<th>Agents</th><th>Scheduled</th><th>Assigned</th><th>Running</th><th>Oldest wait</th></tr></thead><tbody>' +
      q.queues.map((x) => '<tr><td>' + esc(x.queue) + '</td><td>' + x.agents + '</td><td>' + x.scheduled + '</td><td>' +
        x.assigned + '</td><td>' + x.running + '</td><td>' + esc(durationText(Date.now() / 1000 - x.oldest_wait_s)) +
        '</td></tr>').join('') + '</tbody></table></div><h2>Agents</h2><div class="panel scroll"><table><thead><tr>' +
      '<th>Name</th><th>Status</th><th>Queue</th><th>Tags</th><th>OS / arch</th><th>Version</th><th>Last seen</th><th></th>' +
      '</tr></thead><tbody>' + a.agents.map((x) => '<tr><td>' + esc(x.name) + '</td><td><span class="badge s-' +
        esc({ idle: 'passed', busy: 'running', lost: 'failed', stopped: 'canceled' }[x.status]) + '">' + esc(x.status) +
        '</span>' + (x.job ? ' <a href="#/jobs/' + esc(x.job) + '">job</a>' : '') + '</td><td>' + esc(x.queue) +
        '</td><td class="mono">' + esc(Object.entries(x.tags || {}).map(([k, v]) => k + '=' + v).join(' ')) + '</td><td>' +
        esc(x.os + ' ' + x.arch) + '</td><td>' + esc(x.version) + '</td><td>' + esc(when(x.last_seen)) + '</td><td>' +
        (x.status !== 'stopped' ? '<button class="danger" data-stop="' + esc(x.id) + '">Stop</button>' : '') +
        '</td></tr>').join('') + '</tbody></table></div><p class="muted">Cloud machines are one-job agents: they appear ' +
      'as the agent of their job on the build page and are listed on the Spend page while they run.</p>';
    view.querySelectorAll('[data-stop]').forEach((el) => el.addEventListener('click', async () => {
      if (confirm('Stop this agent? It finishes no new jobs until it registers again.')) {
        await post('agents/' + el.dataset.stop + '/stop').catch((e) => alert(e.message));
        route();
      }
    }));
    later(route, 10000);
  }

  async function spend() {
    const s = await api('spend');
    const ratio = s.cap_usd ? Math.min(s.spent_usd / s.cap_usd, 1) : 0;
    view.innerHTML = '<h1>Spend, ' + esc(s.month) + '</h1><div class="tiles"><div class="tile"><b>' + s.spent_usd.toFixed(2) +
      ' / ' + s.cap_usd.toFixed(0) + ' USD</b><span class="muted">recorded machine time this month</span>' +
      '<div class="meter' + (ratio > 0.9 ? ' hot' : '') + '"><i id="fill"></i></div></div><div class="tile"><b>' +
      s.running_bound_usd.toFixed(2) + ' USD</b><span class="muted">worst case of the ' + s.running.length +
      ' machines running now</span></div><div class="tile"><b>' + (s.gcp_stop ? 'IN FORCE' : 'no') +
      '</b><span class="muted">GCP budget stop' + (s.gcp_notification ? ' (last notice ' +
        esc(s.gcp_notification.costAmount) + ' of ' + esc(s.gcp_notification.budgetAmount) + ')' : '') +
      '</span></div></div><h2>By device</h2><div class="panel scroll"><table><thead><tr><th>Device</th><th>Machines</th>' +
      '<th>Hours</th><th>USD</th></tr></thead><tbody>' + s.by_device.map((d) => '<tr><td>' + esc(d.device) + '</td><td>' +
        d.machines + '</td><td>' + d.hours.toFixed(2) + '</td><td>' + d.usd.toFixed(2) + '</td></tr>').join('') +
      '</tbody></table></div><h2>Running machines</h2><div class="panel scroll"><table><thead><tr><th>Machine</th>' +
      '<th>Device</th><th>Step</th><th>Since</th><th>Rate</th></tr></thead><tbody>' + s.running.map((r) => '<tr><td class="mono">' +
        esc(r.machine) + '</td><td>' + esc(r.device) + '</td><td><a href="#/jobs/' + esc(r.job) + '">' + esc(r.key) +
        '</a></td><td>' + esc(r.started) + '</td><td>' + esc(r.rate) + ' USD/h</td></tr>').join('') + '</tbody></table></div>';
    document.getElementById('fill').style.width = (ratio * 100).toFixed(1) + '%';
    later(route, 30000);
  }

  function qual() {
    view.innerHTML = '<h1>QUAL dashboard</h1><p class="muted">Devices × features from the result store: cached means the ' +
      'same input key passed before; budget and waiting cells are never passes.</p><iframe class="qual" src="/qual" ' +
      'title="QUAL dashboard"></iframe>';
  }

  async function route() {
    clearTimeout(timer);
    const hash = location.hash.replace(/^#/, '').split('?')[0] || '/builds';
    const parts = hash.split('/').filter(Boolean);
    document.querySelectorAll('[data-nav]').forEach((a) => a.classList.toggle('active', a.dataset.nav === parts[0] ||
      (parts[0] === 'jobs' && a.dataset.nav === 'builds')));
    try {
      if (parts[0] === 'builds' && parts[1]) {
        await build(parts[1]);
      } else if (parts[0] === 'jobs' && parts[1]) {
        await job(parts[1]);
      } else if (parts[0] === 'agents') {
        await agents();
      } else if (parts[0] === 'spend') {
        await spend();
      } else if (parts[0] === 'qual') {
        qual();
      } else {
        await builds();
      }
    } catch (error) {
      fail(error);
    }
  }

  window.addEventListener('hashchange', route);
  api('me').then((me) => {
    document.getElementById('user').textContent = me.user + ' · coordinator ' + me.version;
  }).catch(() => {});
  route();
}

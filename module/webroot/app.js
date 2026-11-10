/* ============================================================================
   羽量调校 · WebUI 逻辑  (v1.1)
   ---------------------------------------------------------------------------
   三条通道，按优先级探测：
     1. exec   —— 管理器的 ksu.exec（KernelSU / Magisk+MMRL / APatch）
     2. http   —— daemon 自带的 127.0.0.1:7891（BakaSU / KSU-Next 会禁掉 exec）
     3. 页面本身也可以直接从 http://127.0.0.1:7891/ 打开，此时走同源 fetch
   任何一条都不通时，页面不再空转，而是给出明确的排查提示。
   ========================================================================== */
'use strict';

const MODID = 'feather_tune';
const CTL   = `/data/adb/modules/${MODID}/bin/ctl.sh`;
const HTTP_BASE = (location.protocol === 'http:') ? '' : 'http://127.0.0.1:7891';
const DEMO  = new URLSearchParams(location.search).get('mock') === '1';

/* 演示数据（?mock=1 或完全无通道时给页面一个可看的样子） */
const MOCK_STATE = {
  mode: 'auto', profile: 'daily', screen: '1', fg: 'com.tencent.mm', game: '0',
  battery: '78', temp: '33', cap_perf: '2611200', cap_prime: '2854400',
  max_perf: '3628800', max_prime: '4608000', gov_perf: 'schedutil',
  gov_prime: 'schedutil', hmbird: '1', hmbird_live: '0', hmbird_tuned: '0',
  threads: '0', boost_killed: '3', intensity: '50',
  runner: 'running', model: 'OnePlus 15 (PLK110)', android: '17', sdk: '37',
  cfg_enable: '1', cfg_mode: 'auto', cfg_intensity: '50', cfg_auto_game: '1',
  cfg_thread_opt: '1', cfg_screen_off: '1', cfg_kill_boost: '1',
  cfg_io: '1', cfg_gpu: '1', cfg_launch_boost: '1', games: '148'
};
const MOCK_GAMES = ['com.miHoYo.Yuanshen', 'com.miHoYo.hkrpg', 'com.tencent.tmgp.sgame',
  'com.kurogame.mingchao', 'com.hypergryph.arknights', 'com.mojang.minecraftpe'];
const MOCK_RULES = `*  RenderThread|UnityGfx|RHIThread|rhi.thread|VulkanThread  prime
*  GameThread|UnityMain|main  prime
*  AudioTrack|AudioOut_\\d|AAudio|oboe  low
*  Loading|AsyncLoading|TaskGraphThread|FileIO  perf
*  Network|Socket|NetThread  low`;

let TRANSPORT = '';        // 'exec' | 'http' | 'demo'
let probing = false;

/* ------------------------------------------------------------------ 通道 1 */
function ksuExecOnce(cmd, variant) {
  return new Promise((resolve) => {
    let done = false;
    const t = setTimeout(() => { if (!done) { done = true; resolve(null); } }, 2500);
    const cb = (errno, stdout /*, stderr */) => {
      if (done) return;
      done = true;
      clearTimeout(t);
      resolve(typeof stdout === 'string' ? stdout : '');
    };
    try {
      if (!window.ksu || typeof window.ksu.exec !== 'function') { clearTimeout(t); return resolve(null); }
      if (variant === 3) window.ksu.exec(`sh ${CTL} ${cmd}`, '{}', cb);
      else window.ksu.exec(`sh ${CTL} ${cmd}`, cb);
    } catch (e) {
      if (!done) { done = true; clearTimeout(t); resolve(null); }
    }
  });
}

async function execApi(cmd) {
  if (!window.ksu || typeof window.ksu.exec !== 'function') return null;
  let out = await ksuExecOnce(cmd, 3);
  if (out === null) out = await ksuExecOnce(cmd, 2);
  return out;
}

/* ------------------------------------------------------------------ 通道 2 */
async function httpGet(path, ms = 1600) {
  try {
    const ac = new AbortController();
    const timer = setTimeout(() => ac.abort(), ms);
    const r = await fetch(HTTP_BASE + path, { signal: ac.signal, cache: 'no-store' });
    clearTimeout(timer);
    if (!r.ok) return null;
    return await r.text();
  } catch (e) { return null; }
}

function httpMap(cmd) {
  const q = encodeURIComponent;
  if (cmd === 'status') return '/status';
  if (cmd === 'doctor') return '/doctor';
  if (cmd === 'apply')  return '/apply';
  if (cmd === 'restore') return '/restore';
  if (cmd === 'pkg list') return '/games';
  if (cmd === 'apps') return '/apps';
  if (cmd.startsWith('log')) return '/log?n=' + (cmd.split(/\s+/)[1] || 60);
  if (cmd.startsWith('pkg add ')) return '/games/add?p=' + q(cmd.slice(8).trim());
  if (cmd.startsWith('pkg del ')) return '/games/del?p=' + q(cmd.slice(8).trim());
  if (cmd.startsWith('set ')) {
    const [, k, v] = cmd.split(/\s+/);
    return `/set?k=${q(k)}&v=${q(v || '')}`;
  }
  if (cmd.startsWith('mode ')) return '/mode?m=' + q(cmd.split(/\s+/)[1] || '');
  return null;
}

/* ------------------------------------------------------------------ 统一  */
async function api(cmd) {
  if (TRANSPORT === 'demo') return null;
  if (TRANSPORT === 'exec') return execApi(cmd);
  if (TRANSPORT === 'http') {
    const m = httpMap(cmd);
    if (!m) return null;
    return httpGet(m);
  }
  return null;
}

let probePromise = null;

function probe() {
  if (probePromise) return probePromise;   // 并发调用共享同一次探测
  probePromise = (async () => {
    // 已经是 http 同源（页面由 daemon 提供），直接用它
    if (location.protocol === 'http:') {
      const v = await httpGet('/version');
      if (v && /v\d/.test(v)) { TRANSPORT = 'http'; return TRANSPORT; }
    }
    const e = await execApi('version');
    if (e && /v\d/.test(e)) { TRANSPORT = 'exec'; return TRANSPORT; }
    const h = await httpGet('/version');
    if (h && /v\d/.test(h)) { TRANSPORT = 'http'; return TRANSPORT; }
    TRANSPORT = DEMO ? 'demo' : '';
    return TRANSPORT;
  })();
  return probePromise;
}

/* -------------------------------------------------------------------- 工具 */
function parseKV(txt) {
  const out = {};
  if (!txt) return out;
  txt.split('\n').forEach((line) => {
    const i = line.indexOf('=');
    if (i > 0) out[line.slice(0, i).trim()] = line.slice(i + 1).trim();
  });
  return out;
}
const $  = (s) => document.querySelector(s);
const $$ = (s) => Array.from(document.querySelectorAll(s));
const fmtMHz = (khz) => {
  const n = parseInt(khz, 10);
  if (!n) return '—';
  return n >= 1000000 ? (n / 1000000).toFixed(2) + 'G' : Math.round(n / 1000) + 'M';
};
const PROFILE_CN = { daily: '均衡', battery: '省电', game: '游戏', perf: '满速', sleep: '熄屏' };
const TRANSPORT_CN = { exec: '内核接口', http: '本机端口', demo: '演示', '': '未连接' };

let state = {};

/* -------------------------------------------------------------------- 主题 */
function applyTheme(mode) {
  document.documentElement.setAttribute('data-theme', mode);
  try { localStorage.setItem('ft-theme', mode); } catch (e) {}
}
$('#themeBtn').addEventListener('click', () => {
  const cur = document.documentElement.getAttribute('data-theme') || 'auto';
  applyTheme(cur === 'auto' ? 'light' : cur === 'light' ? 'dark' : 'auto');
  toast(cur === 'auto' ? '浅色' : cur === 'light' ? '深色' : '跟随系统');
});

function toast(msg) {
  if (TRANSPORT === 'exec' && window.ksu && window.ksu.toast) { window.ksu.toast(msg); return; }
  const el = document.createElement('div');
  el.textContent = msg;
  el.style.cssText = 'position:fixed;left:50%;bottom:100px;transform:translateX(-50%);' +
    'background:rgba(28,28,30,.92);color:#fff;padding:9px 16px;border-radius:12px;' +
    'font-size:13px;z-index:99;pointer-events:none;transition:opacity .3s';
  document.body.appendChild(el);
  setTimeout(() => { el.style.opacity = '0'; }, 1400);
  setTimeout(() => el.remove(), 1800);
}

/* ---------------------------------------------------------------- 状态刷新 */
function showBlocker(on, msg) {
  const b = $('#blocker');
  if (!b) return;
  b.classList.toggle('hidden', !on);
  if (on) {
    $('#blockerMsg').textContent = msg || '未检测到可用的命令通道。';
    $('#statusline').textContent = '连接失败';
  }
}

async function refresh() {
  if (TRANSPORT === 'demo') { state = { ...MOCK_STATE }; render(); return; }
  if (!TRANSPORT) {
    const t = await probe();
    if (!t) {
      showBlocker(true, 'WebUI 拿不到执行通道：管理器的 shell 接口被禁用，且本机端口 7891 不通（守护进程没启动？）。');
      return;
    }
    showBlocker(false);
  }
  const txt = await api('status');
  if (txt === null || txt === '') {
    if (TRANSPORT === 'exec') {   // exec 失败时再试一次 http
      const h = await httpGet('/status');
      if (h) { TRANSPORT = 'http'; state = parseKV(h); render(); return; }
    }
    showBlocker(true, '命令已发出但没有返回内容。若管理器提示“高级 API 未授权”，请到 设置 → 超级用户 里允许 WebUI 调用 shell。');
    return;
  }
  showBlocker(false);
  state = parseKV(txt);
  render();
}

function render() {
  const p = state.profile || state.mode || 'daily';
  $('#profileName').textContent = PROFILE_CN[p] || p;

  const running = (state.runner || 'running') === 'running';
  const pill = $('#runPill');
  pill.classList.toggle('off', !running);
  pill.querySelector('span').textContent = running ? '运行中' : '已停止';

  const model = state.model || 'OnePlus 15';
  const andr = state.android || '';
  $('#subtitle').textContent = `${model}${andr ? ' · Android ' + andr : ''}` +
    (state.hmbird === '1' ? ' · 风驰内核' : '');
  const sl = $('#statusline');
  if (sl) sl.textContent = `${PROFILE_CN[p] || p} · ${TRANSPORT_CN[TRANSPORT] || TRANSPORT}`;

  $('#capPerf').textContent  = fmtMHz(state.cap_perf);
  $('#capPrime').textContent = fmtMHz(state.cap_prime);
  const gp = state.gov_prime || '—', gf = state.gov_perf || '';
  $('#govText').textContent = gp === gf ? gp : `${gf}/${gp}`;

  $('#mBattery').textContent = state.battery ? state.battery + '%' : '—';
  $('#mTemp').textContent    = state.temp ? state.temp + '°C' : '—';
  const fg = state.fg || '';
  let fgShort = '';
  if (fg) {
    fgShort = fg.split('.').slice(-2).join('.');
    if (fgShort.length > 11) fgShort = fgShort.slice(-11);
  }
  $('#mFg').textContent = fg ? fgShort : (state.game === '1' ? '游戏中' : '—');

  $('#stRunner').textContent = running ? '运行中' : '已停止';
  $('#stRunner').classList.toggle('on', running);
  const hm = state.hmbird === '1';
  $('#stHm').textContent = hm ? (state.hmbird_live === '1' ? '已接管' : '已就绪') : '未检测到';
  $('#stHm').classList.toggle('on', hm);
  const bk = parseInt(state.boost_killed || '0', 10);
  $('#stBoost').textContent = bk > 0 ? bk + ' 项' : '无节点';
  $('#stBoost').classList.toggle('on', bk > 0);
  const tc = $('#stTransport');
  if (tc) {
    tc.textContent = TRANSPORT_CN[TRANSPORT] || '未连接';
    tc.classList.toggle('on', !!TRANSPORT && TRANSPORT !== 'demo');
  }

  const map = {
    auto_game: state.cfg_auto_game, thread_opt: state.cfg_thread_opt,
    screen_off_saver: state.cfg_screen_off, kill_boost: state.cfg_kill_boost,
    launch_boost: state.cfg_launch_boost, io_tune: state.cfg_io, gpu_tune: state.cfg_gpu
  };
  $$('.switch input').forEach((el) => {
    const k = el.dataset.key;
    if (k in map && map[k] !== undefined) el.checked = map[k] === '1';
  });

  const mode = state.cfg_mode || 'auto';
  $$('#modeSeg button').forEach((b) => b.classList.toggle('on', b.dataset.mode === mode));

  const iv = parseInt(state.cfg_intensity || state.intensity || '50', 10);
  const slider = $('#intensity');
  if (document.activeElement !== slider) slider.value = iv;
  $('#intVal').textContent = slider.value;
  slider.style.setProperty('--p', slider.value + '%');
  renderSceneTable(parseInt(slider.value, 10), p);
}

/* 场景上限表：与 daemon 侧 profile_pct() 同一套公式 */
const SCENE_BASE = [
  { key: 'sleep',   name: '熄屏', perf: 30,  prime: 25 },
  { key: 'battery', name: '省电', perf: 50,  prime: 45 },
  { key: 'daily',   name: '日常', perf: 72,  prime: 62 },
  { key: 'game',    name: '游戏', perf: 100, prime: 100 }
];
function scenePct(base, intensity) {
  const d = Math.trunc((50 - intensity) * 2 / 5);
  return Math.max(15, Math.min(100, base + d));
}
function renderSceneTable(intensity, curProfile) {
  const box = $('#sceneRows');
  if (!box) return;
  box.innerHTML = SCENE_BASE.map((s) => {
    const fixed = s.key === 'game';
    const pf = fixed ? '满频' : scenePct(s.perf, intensity) + '%';
    const pr = fixed ? '满频' : scenePct(s.prime, intensity) + '%';
    const on = (curProfile === s.key) ? ' cur' : '';
    return `<div class="tr${on}"><span>${s.name}</span><span>${pf}</span><span>${pr}</span></div>`;
  }).join('');
}

/* ------------------------------------------------------------------- 开关 */
$$('.switch input').forEach((el) => {
  el.addEventListener('change', async () => {
    const key = el.dataset.key;
    const val = el.checked ? '1' : '0';
    toast(el.checked ? '已开启' : '已关闭');
    const r = await api(`set ${key} ${val}`);
    if (r === null || /^err=/m.test(r || '')) {
      el.checked = !el.checked;
      toast('设置失败');
    }
    setTimeout(refresh, 600);
  });
});

/* --------------------------------------------------------------- 场景分段 */
$$('#modeSeg button').forEach((b) => {
  b.addEventListener('click', async () => {
    $$('#modeSeg button').forEach((x) => x.classList.remove('on'));
    b.classList.add('on');
    const m = b.dataset.mode;
    await api(`mode ${m}`);
    toast('场景：' + (PROFILE_CN[m] || m));
    setTimeout(refresh, 500);
  });
});

/* ----------------------------------------------------------------- 强度条 */
const intEl = $('#intensity');
intEl.addEventListener('input', () => {
  $('#intVal').textContent = intEl.value;
  intEl.style.setProperty('--p', intEl.value + '%');
  renderSceneTable(parseInt(intEl.value, 10), state.profile || 'daily');
});
intEl.addEventListener('change', async () => {
  await api(`set intensity ${intEl.value}`);
  toast('省电强度 ' + intEl.value);
  setTimeout(refresh, 500);
});

/* ----------------------------------------------------------------- 标签页 */
function showTab(name) {
  const btn = $$('.tabbtn').find((b) => b.dataset.tab === name);
  if (!btn) return;
  $$('.tabbtn').forEach((x) => x.classList.remove('on'));
  btn.classList.add('on');
  $$('.tab').forEach((t) => t.classList.add('hidden'));
  const p = $('#tab-' + name);
  if (p) p.classList.remove('hidden');
  if (name === 'game') kickGames();
}

/* 游戏页依赖通道，早期打开时先等探测完成 */
async function kickGames() {
  if (!TRANSPORT && !DEMO) {
    const t = await probe();
    if (!t) { $('#gameCount').textContent = '通道未连接'; return; }
  }
  await loadGames();
}
$$('.tabbtn').forEach((b) => {
  b.addEventListener('click', () => {
    showTab(b.dataset.tab);
    window.scrollTo({ top: 0, behavior: 'smooth' });
  });
});

/* --------------------------------------------------------------------- 游戏 */
let gamesCache = [];
let appsCache = null;
let pickMode = false;

function renderRules(text) {
  const rows = (text || '').split('\n')
    .map((l) => l.replace(/#.*$/, '').trim())
    .filter(Boolean)
    .map((l) => l.split(/\s+/))
    .filter((a) => a.length >= 3)
    .map((a) => ({ re: a[1], tgt: a[2] }));
  if (!rows.length) { $('#rulesView').innerHTML = '<div class="empty">（空）</div>'; return; }
  $('#rulesView').innerHTML = rows.map((r) => {
    const hot = (r.tgt === 'prime' || r.tgt === 'prime1');
    return `<div class="rule-row"><code>${r.re.replace(/</g, '&lt;')}</code>` +
           `<span class="tag${hot ? ' t-game' : ''}">${r.tgt}</span></div>`;
  }).join('');
}

async function readRules() {
  if (TRANSPORT === 'exec') {
    const r = await execApi(`cat /data/adb/modules/${MODID}/etc/rules.conf`).catch(() => null);
    return r || '';
  }
  const r = await httpGet('/rules');
  return r || '';
}

async function loadGames() {
  if (TRANSPORT === 'demo') {
    gamesCache = MOCK_GAMES.slice();
    renderRules(MOCK_RULES);
  } else {
    const r = await api('pkg list');
    gamesCache = (r || '').split('\n').map((s) => s.trim()).filter(Boolean);
    renderRules(await readRules());
  }
  $('#gameCount').textContent = `${gamesCache.length} 个应用`;
  renderGames();
}

function renderGames() {
  const q = ($('#gameSearch').value || '').toLowerCase();
  const list = pickMode ? (appsCache || []).filter((p) => !gamesCache.includes(p)) : gamesCache;
  const shown = list.filter((p) => p.toLowerCase().includes(q));

  const box = $('#gameList');
  if (!shown.length) {
    box.innerHTML = `<div class="empty">${pickMode ? '没有可添加的应用' : '名单为空，点右上角添加'}</div>`;
    return;
  }
  box.innerHTML = shown.map((p) => `
    <div class="row">
      <div class="row-main"><span class="row-title">${p}</span></div>
      ${pickMode
        ? `<button class="add" data-add="${p}" aria-label="添加"><svg viewBox="0 0 24 24"><path d="M12 5v14M5 12h14"/></svg></button>`
        : `<button class="del" data-del="${p}" aria-label="删除"><svg viewBox="0 0 24 24"><path d="M6 6l12 12M18 6L6 18"/></svg></button>`}
    </div>`).join('');

  box.querySelectorAll('[data-del]').forEach((btn) => {
    btn.addEventListener('click', async () => {
      const pkg = btn.dataset.del;
      await api(`pkg del ${pkg}`);
      gamesCache = gamesCache.filter((x) => x !== pkg);
      if (appsCache) appsCache.push(pkg);
      renderGames();
      $('#gameCount').textContent = `${gamesCache.length} 个应用`;
      toast('已移出名单');
    });
  });
  box.querySelectorAll('[data-add]').forEach((btn) => {
    btn.addEventListener('click', async () => {
      const pkg = btn.dataset.add;
      await api(`pkg add ${pkg}`);
      gamesCache.push(pkg);
      renderGames();
      $('#gameCount').textContent = `${gamesCache.length} 个应用`;
      toast('已加入名单');
    });
  });
}
$('#gameSearch').addEventListener('input', renderGames);

$('#addGameBtn').addEventListener('click', async () => {
  if (!pickMode) {
    if (TRANSPORT === 'demo') {
      appsCache = ['com.tencent.mm', 'com.taobao.taobao', 'com.netease.cloudmusic',
        'com.tencent.tmgp.pubgmhd', 'com.bilibili.app.in'];
    } else {
      const r = await api('apps');
      appsCache = (r || '').split('\n').map((s) => s.trim()).filter(Boolean);
      if (!appsCache.length) { toast('读不到应用列表（通道受限）'); }
    }
    pickMode = true;
    $('#addGameBtn').textContent = '完成';
    $('#gameSearchWrap').classList.remove('hidden');
  } else {
    pickMode = false;
    $('#addGameBtn').textContent = '添加应用';
  }
  renderGames();
});

/* -------------------------------------------------------------------- 设置 */
async function showOutput(title, txt) {
  const box = $('#outputBox');
  box.hidden = false;
  $('#output').textContent = `# ${title}\n` + txt;
  box.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
}

$('#btnDoctor').addEventListener('click', async () => {
  showOutput('读取中…', '…');
  const r = await api('doctor');
  showOutput('环境诊断', r || '（通道不可用）');
});

$('#btnLog').addEventListener('click', async () => {
  showOutput('读取中…', '…');
  const r = await api('log 60');
  showOutput('最近 60 行', r || '（通道不可用）');
});

$('#btnRestart').addEventListener('click', async () => {
  if (TRANSPORT === 'http') { toast('端口模式下不支持重启，请用命令行'); return; }
  await api('restart');
  toast('守护进程已重启');
  setTimeout(refresh, 1500);
});

$('#btnRestore').addEventListener('click', async () => {
  await api('restore');
  toast('已还原频率与参数');
  setTimeout(refresh, 800);
});

$('#btnRetry')?.addEventListener('click', async () => {
  TRANSPORT = '';
  showBlocker(false);
  await refresh();
  if (TRANSPORT && TRANSPORT !== 'demo') toast('已连接：' + (TRANSPORT_CN[TRANSPORT] || TRANSPORT));
});

/* -------------------------------------------------------------------- 启动 */
(async function boot() {
  try {
    const t = localStorage.getItem('ft-theme');
    if (t) applyTheme(t);
  } catch (e) {}
  const qs = new URLSearchParams(location.search);
  if (qs.get('theme')) applyTheme(qs.get('theme'));

  await refresh();

  const tab = location.hash.replace('#', '') || qs.get('tab');
  if (tab) showTab(tab);

  if (!DEMO) setInterval(() => { if (TRANSPORT) refresh(); }, 3000);
})();


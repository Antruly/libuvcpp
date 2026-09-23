#!/usr/bin/env node
/**
 * 压测驱动器：对 uvcpp_bench_server 铺 N 个 autocannon 进程，报合计 RPS、
 * 各生成器的 CPU，以及**服务端自己占了多少个核**。
 *
 * 为什么要铺多个生成器进程：单个 autocannon 是单线程 JS，它自己就会先到顶。
 * 用单进程量出来的数只是"生成器的上限"，拿它判断服务端性能是错的 —— 实测本机上
 * 单进程卡在 ~36k，而那时服务端也已经占满一个核，两个都到顶，读数没有分辨力。
 *
 * 为什么必须报服务端占核数：**这是唯一能分辨"谁到顶"的量**。
 *    服务端 ≈ 工作循环数  ⇒ 服务端是瓶颈，这个 RPS 是服务端的能力；
 *    服务端低于它且生成器满 ⇒ 这个数只是生成器的上限，任何"变快了"都是假的。
 * 少了这一列的读数不构成结论。
 *
 * --loops N 是给多循环用的（对应靶场的同名开关）：到顶的门槛是 **0.9 × 工作循环数**，
 * 而工作循环数 = N−1（N>1 时），**不是 N**。`--loops N` 是「1 条接受者 + N−1 条工作
 * 循环」，0 号那条接受者**不承载任何连接**（负载期 `/stats` 的 per_loop[0] 恒为 0）。
 * 所以 N=4 的服务端最多只能占 ~3 个核 —— 拿 0.9×4 = 3.6 去判，**永远**判"没到顶"，
 * 把真实能力印成"没量到"（本机 D 臂实测 318~325%，正是 3 条工作循环的天花板）。
 * N=1 时没有接受者/工作之分，那条循环自己干全部活，门槛就是 0.9。
 *
 * 采样**按 pid 锚定，不按进程名**：按名字取会和同名残留进程混在一起，而那一项正好是
 * 判据的分子 ⇒ 假"到顶"。这条是 `doc/benchmark.md` 里记着的坑（第 5 节），
 * 本仓为此吃过一次 19.52 MB/min 的假泄漏。没给 --pid 时按名字查，**匹配数 ≠ 1 直接退 3**。
 *
 * 依赖：先在 `bench/` 里跑 `npm install`（声明在同目录 package.json，钉精确 8.0.0；
 * 本仓不 vendor 它）。
 *
 * 用法:
 *   node bench/driver.js --url http://127.0.0.1:8080/json --proc uvcpp_bench_server \
 *        [--pid 1234] [--loops 1] [--nproc 4] [--duration 15] [--connections 25]
 *        [--pipelining 1]
 */

'use strict';
const path = require('path');
const { spawn, execFileSync } = require('child_process');

function arg(name, dflt) {
  const i = process.argv.indexOf('--' + name);
  return i >= 0 && process.argv[i + 1] !== undefined ? process.argv[i + 1] : dflt;
}

const url = arg('url', 'http://127.0.0.1:8080/json');
const procName = arg('proc', 'uvcpp_bench_server');
const wantPid = arg('pid', '');
const loops = Number(arg('loops', 1));
const nproc = Number(arg('nproc', 4));
const duration = Number(arg('duration', 15));
const connections = Number(arg('connections', 25));
const pipelining = Number(arg('pipelining', 1));
const warmup = Number(arg('warmup', 2));

function ps(script) {
  return execFileSync('powershell', ['-NoProfile', '-Command', script],
                      { encoding: 'utf8' }).trim();
}

/**
 * 定出**唯一**一个被测进程的 pid。给不到唯一答案就退 3 —— 这条是前提，不是判据：
 * 按名字求和会把残留进程算进分子，那个数就不再是这个服务端的。
 */
function resolvePid() {
  if (wantPid !== '') {
    const n = Number(wantPid);
    if (!Number.isInteger(n) || n <= 0) { console.error('--pid 不是正整数：' + wantPid); process.exit(3); }
    return { pid: n, how: '命令行指定' };
  }
  if (process.platform !== 'win32') {
    // 非 Windows：按名字取，多于一个同样退 3。
    const out = execFileSync('pgrep', ['-x', procName], { encoding: 'utf8' }).trim();
    const ids = out.split('\n').map((s) => parseInt(s, 10)).filter(Number.isFinite);
    if (ids.length !== 1) { reportPidTrouble(ids); }
    return { pid: ids[0], how: '按进程名，唯一匹配' };
  }
  let ids = [];
  try {
    const out = ps(`Get-Process -Name ${procName} -ErrorAction SilentlyContinue | ` +
                   `Select-Object -ExpandProperty Id`);
    ids = out.split(/\s+/).map((s) => parseInt(s, 10)).filter(Number.isFinite);
  } catch (e) { ids = []; }
  if (ids.length !== 1) { reportPidTrouble(ids); }
  return { pid: ids[0], how: '按进程名，唯一匹配' };
}

function reportPidTrouble(ids) {
  console.log('判定         : **不可判** —— 被测进程不唯一，占核数这一项没有意义');
  if (ids.length === 0) {
    console.log(`  「${procName}」一个都没找到 —— 服务端起没起来？`);
  } else {
    console.log(`  「${procName}」找到 ${ids.length} 个（pid ${ids.join(', ')}）—— ` +
                `残留的那个会被算进分子，读数虚高。先清干净，或用 --pid 指名。`);
  }
  process.exit(3);
}

/**
 * 负载期**每秒**采一次被测进程吃掉的核数，边跑边打。
 *
 * 为什么不用 `(cpu_after - cpu_before) / 墙钟`：那个分母里混着 node 启动、钉核、
 * autocannon 预热，本机实测能把一个 91.5% 占核的服务端报成 **76.3%**。在 N=4 那档
 * 这个系统性低报更致命：真值 3.8 核会被报成 ~3.2，而门槛是 3.6 ⇒ 假"没到顶"。
 *
 * 顺带：`$pid` 在 PowerShell 里是自动变量（本进程 pid），**不能用**这个名字。
 */
function startSampler(pid, maxSeconds) {
  const script = [
    `$thePid=${pid}`,
    `$deadline=(Get-Date).AddSeconds(${maxSeconds})`,
    `$prev=$null; $idle=0; $sawLoad=$false`,
    `while((Get-Date) -lt $deadline){`,
    `  $p=Get-Process -Id $thePid -ErrorAction SilentlyContinue`,
    `  if($null -eq $p){ Write-Output 'GONE'; break }`,
    `  $cur=$p.CPU`,
    `  if($null -ne $prev){`,
    `    $pct=($cur-$prev)*100.0`,
    `    '{0:N1}' -f $pct`,
    `    if($pct -ge 5.0){ $sawLoad=$true; $idle=0 } else { if($sawLoad){ $idle++ } }`,
    `  }`,
    `  $prev=$cur`,
    `  if($sawLoad -and $idle -ge 3){ break }`,
    `  Start-Sleep -Milliseconds 1000`,
    `}`,
  ].join('\n');

  const p = spawn('powershell', ['-NoProfile', '-Command', script],
                  { stdio: ['ignore', 'pipe', 'ignore'] });
  const samples = [];
  let buf = '';
  p.stdout.on('data', (d) => {
    buf += d;
    const lines = buf.split('\n');
    buf = lines.pop();
    for (const ln of lines) {
      // 千位/小数点跟着本机区域走，中文区可能是逗号；`{0:N1}` 也可能给出 "1,234.5"。
      const v = parseFloat(ln.trim().replace(',', '.'));
      if (Number.isFinite(v)) samples.push(v);
    }
  });
  return { samples, proc: p };
}

function oneGen() {
  return new Promise((resolve) => {
    const p = spawn(process.execPath, [path.join(__dirname, 'gen.js'),
      '--url', url, '--duration', String(duration),
      '--connections', String(connections), '--pipelining', String(pipelining),
      '--warmup', String(warmup)], { stdio: ['ignore', 'pipe', 'ignore'] });
    let out = '';
    p.stdout.on('data', (d) => { out += d; });
    p.on('close', () => {
      try { resolve(JSON.parse(out.trim().split('\n').pop())); }
      catch (e) { resolve({ rps: 0, cpu_pct: 0, errors: -1 }); }
    });
  });
}

function median(xs) {
  const s = [...xs].sort((a, b) => a - b);
  const m = s.length >> 1;
  return s.length % 2 ? s[m] : (s[m - 1] + s[m]) / 2;
}

(async () => {
  const who = resolvePid();
  // 上限只是兜底：正常由「负载过后连续 3 秒空载」收尾。
  const sampler = startSampler(who.pid, duration + warmup + 45);

  const t0 = Date.now();
  const results = await Promise.all(Array.from({ length: nproc }, oneGen));
  const wall = (Date.now() - t0) / 1000;

  // 等采样器自己按"连续空载"收尾（这样每个样本都落在整秒边界上），再兜底杀掉。
  // 先看它是不是**已经**退出了 —— 否则在已退出的进程上等 close 会白等满 12 秒。
  await new Promise((resolve) => {
    if (sampler.proc.exitCode !== null || sampler.proc.signalCode !== null) { resolve(); return; }
    const t = setTimeout(() => { try { sampler.proc.kill(); } catch (e) {} resolve(); }, 12000);
    sampler.proc.on('close', () => { clearTimeout(t); resolve(); });
  });

  const rps = results.reduce((s, r) => s + r.rps, 0);
  const errors = results.reduce((s, r) => s + r.errors, 0);
  const genMax = Math.max(...results.map((r) => r.cpu_pct));

  // 只取"负载期"的样本（≥5%），与 `_bench/measure.sh` 同一条口径：中位数代表
  // **负载期间它占了几个核**，而不把启动/收尾的空档摊进来。
  const loaded = sampler.samples.filter((s) => s >= 5.0);
  const srvCores = loaded.length ? median(loaded) / 100 : null;

  console.log('被测进程     : ' + procName + ' (pid ' + who.pid + '，' + who.how + ')');
  console.log('循环档位     : --loops ' + loops);
  console.log('生成器       : ' + nproc + ' 进程 × ' + connections +
              ' 连接, 流水线 ' + pipelining + ', ' + duration + ' s');
  console.log('合计 RPS     : ' + rps);
  console.log('错误         : ' + errors);
  console.log('生成器峰值   : ' + genMax.toFixed(1) + '% 个核');
  console.log('服务端占核   : ' +
    (srvCores === null ? '没采到负载样本（服务端全程 <5% 个核？）'
                       : '中位 ' + srvCores.toFixed(2) + ' 个核（采到 ' + loaded.length +
                         ' 秒负载，共 ' + sampler.samples.length + ' 个样本）'));

  if (srvCores === null) {
    console.log('判定         : **不可判** —— 没采到服务端的负载样本，这个 RPS 说明不了是谁到顶');
    process.exit(3);
  }
  // 工作循环数：n>1 时是 n−1（0 号是接受者，不承载连接）；n==1 就是 1。
  const workerLoops = loops > 1 ? loops - 1 : 1;
  const threshold = 0.9 * workerLoops;
  const serverBound = srvCores >= threshold;
  console.log('判定         : ' + (serverBound
    ? '服务端到顶（门槛 ' + threshold.toFixed(2) + ' 核；这个 RPS 是服务端的能力）'
    : '服务端未到顶（只有 ' + srvCores.toFixed(2) + ' / 门槛 ' + threshold.toFixed(2) +
      ' 核）⇒ 这个数只是生成器的上限，**不能**当服务端性能'));
  console.log('JSON         : ' + JSON.stringify({
    url, proc: procName, pid: who.pid, loops, nproc, connections, pipelining, duration,
    rps_total: rps, errors_total: errors, gen_cores_max: +genMax.toFixed(1),
    server_cores: srvCores === null ? null : +srvCores.toFixed(2),
    worker_loops: workerLoops,
    server_cores_per_worker: srvCores === null ? null : +(srvCores / workerLoops).toFixed(2),
    server_bound: serverBound, threshold: +threshold.toFixed(2),
    per_proc: results.map((r) => ({ rps: r.rps, cpu_pct: r.cpu_pct })),
  }));

  // 退出码沿用本仓的三值约定：0 判过（服务端到顶）/ 3 前提不满足（没判）。
  process.exit(serverBound ? 0 : 3);
})().catch((e) => { console.error(e); process.exit(1); });

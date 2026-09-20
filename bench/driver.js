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
 *    服务端 ≈ 1 核        ⇒ 服务端是瓶颈，这个 RPS 是服务端的能力；
 *    服务端 < 1 核且生成器满 ⇒ 这个数只是生成器的上限，任何"变快了"都是假的。
 * 少了这一列的读数不构成结论。
 *
 * 依赖：`npm install autocannon`（本仓不 vendor 它）。
 *
 * 用法:
 *   node bench/driver.js --url http://127.0.0.1:8080/json --proc uvcpp_bench_server \
 *        [--nproc 4] [--duration 15] [--connections 25] [--pipelining 1]
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
const nproc = Number(arg('nproc', 4));
const duration = Number(arg('duration', 15));
const connections = Number(arg('connections', 25));
const pipelining = Number(arg('pipelining', 1));
const warmup = Number(arg('warmup', 2));

/**
 * 取被测进程累计消耗的 CPU 秒。Windows 走 PowerShell，其余平台走 /proc。
 * 取不到就返回 null —— **不假装是 0**，否则"量不到"会被读成"没占 CPU"。
 */
function serverCpu() {
  try {
    if (process.platform === 'win32') {
      const out = execFileSync('powershell', ['-NoProfile', '-Command',
        `(Get-Process ${procName} -ErrorAction SilentlyContinue | ` +
        `Measure-Object -Property CPU -Sum).Sum`], { encoding: 'utf8' });
      const v = parseFloat(String(out).trim());
      return Number.isFinite(v) ? v : null;
    }
    const out = execFileSync('ps', ['-o', 'times=', '-C', procName], { encoding: 'utf8' });
    const secs = String(out).trim().split('\n')
      .map((l) => parseFloat(l.trim())).filter(Number.isFinite);
    return secs.length ? secs.reduce((a, b) => a + b, 0) : null;
  } catch (e) {
    return null;
  }
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

(async () => {
  const c0 = serverCpu();
  const t0 = Date.now();
  const results = await Promise.all(Array.from({ length: nproc }, oneGen));
  const wall = (Date.now() - t0) / 1000;
  const c1 = serverCpu();

  const rps = results.reduce((s, r) => s + r.rps, 0);
  const errors = results.reduce((s, r) => s + r.errors, 0);
  const genMax = Math.max(...results.map((r) => r.cpu_pct));

  const srvCores = (c0 !== null && c1 !== null) ? (c1 - c0) / wall : null;

  console.log('被测进程     : ' + procName);
  console.log('生成器       : ' + nproc + ' 进程 × ' + connections +
              ' 连接, 流水线 ' + pipelining + ', ' + duration + ' s');
  console.log('合计 RPS     : ' + rps);
  console.log('错误         : ' + errors);
  console.log('生成器峰值   : ' + genMax.toFixed(1) + '% 个核');
  console.log('服务端占用   : ' +
    (srvCores === null ? '量不到（进程名不对？）'
                       : srvCores.toFixed(2) + ' 个核'));

  if (srvCores === null) {
    console.log('判定         : **不可判** —— 没量到服务端 CPU，这个 RPS 说明不了是谁到顶');
    process.exit(3);
  }
  const serverBound = srvCores >= 0.9;
  console.log('判定         : ' + (serverBound
    ? '服务端到顶（这个 RPS 是服务端的能力）'
    : '服务端未到顶 ⇒ 这个数只是生成器的上限，**不能**当服务端性能'));
  console.log('JSON         : ' + JSON.stringify({
    url, nproc, connections, pipelining, duration, rps_total: rps,
    errors_total: errors, gen_cores_max: +genMax.toFixed(1),
    server_cores: srvCores === null ? null : +srvCores.toFixed(2),
    server_bound: serverBound,
    per_proc: results.map((r) => ({ rps: r.rps, cpu_pct: r.cpu_pct })),
  }));

  // 退出码沿用本仓的三值约定：0 判过（服务端到顶）/ 3 前提不满足（没判）。
  process.exit(serverBound ? 0 : 3);
})().catch((e) => { console.error(e); process.exit(1); });

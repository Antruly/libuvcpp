// 单个生成器进程：跑一轮 autocannon，打印一行 JSON。
// 被 run_multi.js 拉起 N 份，用来判断"服务端到顶"还是"生成器到顶"。
//
// 用法: node gen.js --url ... --duration 10 --connections 10 --pipelining 1
const autocannon = require('autocannon');

function arg(name, dflt) {
  const i = process.argv.indexOf('--' + name);
  return i >= 0 && process.argv[i + 1] !== undefined ? process.argv[i + 1] : dflt;
}

const url = arg('url', 'http://127.0.0.1:8080/hello');
const duration = Number(arg('duration', 10));
const connections = Number(arg('connections', 10));
const pipelining = Number(arg('pipelining', 1));
const warmup = Number(arg('warmup', 2));

function run(seconds) {
  return new Promise((resolve, reject) => {
    const inst = autocannon(
      { url, connections, pipelining, duration: seconds, timeout: 10 },
      (err, res) => (err ? reject(err) : resolve(res))
    );
    autocannon.track(inst, { renderProgressBar: false, renderResultsTable: false });
  });
}

(async () => {
  await run(warmup); // 丢弃：把 JIT 与连接池拉起来
  const c0 = process.cpuUsage();
  const w0 = Date.now();
  const res = await run(duration);
  const wall = (Date.now() - w0) / 1000;
  const c = process.cpuUsage(c0);
  const cpuPct = (((c.user + c.system) / 1e6) / wall) * 100;
  console.log(JSON.stringify({
    rps: Math.round(res.requests.average),
    cpu_pct: +cpuPct.toFixed(1),
    errors: res.errors + res.timeouts + res.non2xx,
    p99: (res.latency.percentiles && res.latency.percentiles['99%']) || res.latency.p99,
  }));
})().catch((e) => {
  console.error(JSON.stringify({ rps: 0, cpu_pct: 0, errors: -1, err: String(e) }));
  process.exit(1);
});

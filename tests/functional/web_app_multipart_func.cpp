/**
 * @file tests/functional/web_multipart_func.cpp
 * @brief uvcpp_web_multipart 的用例（纯解析器：无服务器、无 libuv、无 IO）。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 本文件最有价值的一条是 `split_every_offset`
 * ------------------------------------------
 * 一个流式状态机的所有跨块 bug（保留长度算错、边界相位、CRLF 归属、
 * 假命中回退）都只在**某个特定的切分偏移**上暴露，而整包喂永远测不出来。
 * 所以这里把同一份报文在**每一个字节偏移**处切成两块喂进去，产出必须与
 * 整包喂**逐个部件、逐字节完全相同**；再补 3 段与随机多段切分。
 *
 * 用例的判据是"整包喂的产出"而不是"手写的期望值"：这样连"整包喂也解析错了"
 * 都会被抓住（两条路径会对不上），但如果只跟手写期望比，一个两条路径**同样**
 * 犯的错就会一起漏过去。手写期望只在 `single_part` 那一组里作为独立锚点出现。
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <webapp/uvcpp_web_multipart.h>

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
  if (!cond) {
    std::cout << "  [FAIL] " << what << std::endl;
    ++g_failures;
  }
}

void check_eq_i(long long got, long long want, const std::string& what) {
  if (got != want) {
    std::cout << "  [FAIL] " << what << "\n         期望: " << want
              << "\n         实际: " << got << std::endl;
    ++g_failures;
  }
}

void check_eq_s(const std::string& got, const std::string& want,
                const std::string& what) {
  if (got != want) {
    std::cout << "  [FAIL] " << what << "\n         期望: [" << want
              << "]\n         实际: [" << got << "]" << std::endl;
    ++g_failures;
  }
}

// ---------------------------------------------------------------------------
// 记录型 sink：把解析产出原样记下来，供逐字节比对
// ---------------------------------------------------------------------------

struct recorded_part {
  std::string name;
  std::string filename;
  std::string content_type;
  bool is_file;
  bool truncated;
  std::string body;
};

class recording_sink : public uvcpp::uvcpp_web_multipart_sink {
 public:
  recording_sink() : abort_after_(0), begins_(0), ends_(0) {}

  bool on_part_begin(const uvcpp::uvcpp_web_part_info& info) {
    ++begins_;
    cur_ = recorded_part();
    cur_.name = info.name;
    cur_.filename = info.filename;
    cur_.content_type = info.content_type;
    cur_.is_file = info.is_file;
    cur_.truncated = false;
    return !(abort_after_ > 0 && begins_ >= abort_after_);
  }

  bool on_part_data(const char* data, size_t len) {
    if (abort_after_ > 0 && begins_ >= abort_after_) return false;
    cur_.body.append(data, len);
    return true;
  }

  void on_part_end(bool truncated) {
    ++ends_;
    cur_.truncated = truncated;
    parts.push_back(cur_);
    cur_ = recorded_part();
  }

  std::vector<recorded_part> parts;
  size_t abort_after_;  // >0：第 N 个部件开始后中止
  size_t begins_;
  size_t ends_;

 private:
  recorded_part cur_;
};

// ---------------------------------------------------------------------------
// 驱动：把 body 分段喂给解析器，返回产出与最终结果
// ---------------------------------------------------------------------------

struct run_outcome {
  std::vector<recorded_part> parts;
  uvcpp::uvcpp_web_multipart_result result;
  std::string error_text;
  size_t begins;
  size_t ends;
  uint64_t body_bytes;
  size_t max_retained;
};

run_outcome run_split(const std::string& boundary, const std::string& body,
                      const std::vector<size_t>& cuts) {
  run_outcome out;
  out.result = uvcpp::uvcpp_web_multipart_result::OK;
  out.max_retained = 0;

  recording_sink sink;
  uvcpp::uvcpp_web_multipart mp;
  mp.set_sink(&sink);
  if (!mp.set_boundary(boundary)) {
    out.result = mp.result();
    out.error_text = mp.error_text();
    return out;
  }

  size_t pos = 0;
  size_t ci = 0;
  for (;;) {
    // 下一刀：默认切到结尾
    size_t next = body.size();
    if (ci < cuts.size() && cuts[ci] < body.size()) next = cuts[ci];
    ++ci;

    const size_t n = next - pos;
    uvcpp::uvcpp_web_multipart_result r =
        mp.feed(body.data() + pos, n);
    if (mp.retained_bytes() > out.max_retained) {
      out.max_retained = mp.retained_bytes();
    }
    if (r != uvcpp::uvcpp_web_multipart_result::OK) {
      out.result = r;
      out.error_text = mp.error_text();
      break;
    }
    pos = next;
    if (pos >= body.size()) {
      // 喂完了，收尾
      out.result = mp.finish();
      out.error_text = mp.error_text();
      break;
    }
  }

  out.parts = sink.parts;
  out.begins = sink.begins_;
  out.ends = sink.ends_;
  out.body_bytes = mp.body_bytes();
  return out;
}

run_outcome run_whole(const std::string& boundary, const std::string& body) {
  std::vector<size_t> none;
  return run_split(boundary, body, none);
}

// ---------------------------------------------------------------------------
// 报文构造
// ---------------------------------------------------------------------------

std::string make_body(const std::string& boundary,
                      const std::vector<std::string>& parts) {
  std::string b;
  for (size_t i = 0; i < parts.size(); ++i) {
    b += "--" + boundary + "\r\n";
    b += parts[i];
    b += "\r\n";
  }
  b += "--" + boundary + "--\r\n";
  return b;
}

std::string file_part(const std::string& name, const std::string& filename,
                      const std::string& mime, const std::string& data) {
  return "Content-Disposition: form-data; name=\"" + name +
         "\"; filename=\"" + filename + "\"\r\nContent-Type: " + mime +
         "\r\n\r\n" + data;
}

std::string field_part(const std::string& name, const std::string& value) {
  return "Content-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" +
         value;
}

// 两份产出必须逐部件、逐字节完全相同。
bool same_parts(const run_outcome& a, const run_outcome& b,
                std::string* why) {
  if (a.parts.size() != b.parts.size()) {
    *why = "部件数不同";
    return false;
  }
  for (size_t i = 0; i < a.parts.size(); ++i) {
    const recorded_part& x = a.parts[i];
    const recorded_part& y = b.parts[i];
    if (x.name != y.name || x.filename != y.filename ||
        x.content_type != y.content_type || x.is_file != y.is_file ||
        x.truncated != y.truncated || x.body != y.body) {
      *why = "第 " + std::to_string(i + 1) + " 个部件不同";
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// 一组有代表性的"体"
// ---------------------------------------------------------------------------

struct sample {
  const char* name;
  std::string boundary;
  std::string body;
};

std::vector<sample> samples() {
  std::vector<sample> v;

  {
    sample s;
    s.name = "单文件";
    s.boundary = "----uvcppBoundary123";
    s.body = make_body(s.boundary,
                       std::vector<std::string>(1, file_part(
                           "avatar", "a.png", "image/png", "PNGDATA")));
    v.push_back(s);
  }
  {
    sample s;
    s.name = "多部件（文件+字段）";
    s.boundary = "XyZ";
    std::vector<std::string> p;
    p.push_back(field_part("room", "lobby"));
    p.push_back(file_part("f1", "one.bin", "application/octet-stream",
                          std::string("A\0B\r\nC", 6)));
    p.push_back(field_part("note", "hi"));
    p.push_back(file_part("f2", "two.txt", "text/plain", ""));
    s.body = make_body(s.boundary, p);
    v.push_back(s);
  }
  {
    // body 里含**近似但不完整**的边界：这是"假命中回退"唯一的暴露方式。
    //
    // 注意这里**不能**放一个"完整的 `\r\n--BB` 且后面什么都没有"——它后面
    // 必然跟着这一段的其余字节，拼起来就可能变成一个**货真价实**的分隔符
    // （第一版就是这么写的：`\r\n--BB` 后面紧跟 `\r\n--BBx`，于是拼接结果里
    // 出现了 `\r\n--BB\r\n`，解析器在那里切开是**对的**，而用例却以为它错了）。
    // "边界后面没有尾随字节"这个形状只在**缓冲末尾**才有意义，
    // 由 `split_every_offset` 的每一个切点覆盖，不该塞进 body 里。
    sample s;
    s.name = "近似边界";
    s.boundary = "BB";
    std::string tricky = "\r\n--B";     // 缺一个字节
    tricky += "\r\n--BB-";              // 尾随只有一个 '-'
    tricky += "\r\n--BBx";              // 尾随字节不合法
    tricky += "\r\n--BB--x";            // 看似终边界但后面多一个字节
    tricky += "\r\n--BB \t";            // 有填充、但没有行尾
    tricky += "TAIL";
    s.body = make_body(s.boundary,
                       std::vector<std::string>(1, field_part("t", tricky)));
    v.push_back(s);
  }
  {
    // 二进制：NUL / 0xFF / CR / LF 混合，且以边界前缀结尾。
    sample s;
    s.name = "二进制";
    s.boundary = "bin";
    std::string raw;
    for (int i = 0; i < 256; ++i) raw += static_cast<char>(i);
    raw += "\r\n--bi";  // 结尾是半个边界
    s.body = make_body(s.boundary,
                       std::vector<std::string>(1,
                                                file_part("b", "b.bin",
                                                          "application/octet-stream",
                                                          raw)));
    v.push_back(s);
  }
  {
    // 前导与结尾都有多余字节。
    sample s;
    s.name = "前后导";
    s.boundary = "PRE";
    s.body = "junk-preamble\r\n" + make_body(s.boundary,
                                              std::vector<std::string>(
                                                  1, field_part("k", "v"))) +
             "trailing-epilogue";
    v.push_back(s);
  }
  {
    // 空部件序列：只有终边界。
    sample s;
    s.name = "无部件";
    s.boundary = "E";
    s.body = "--E--\r\n";
    v.push_back(s);
  }
  return v;
}

// 一份足够长的边界，用来把保留缓冲的上界压出来。
const char* kLongBoundary =
    "0123456789012345678901234567890123456789012345678901234567890123456789";

// =========================================================================
// 组：single_part —— 独立的锚点（手写期望，不跟整包喂比）
// =========================================================================
void test_single_part() {
  const std::string b = "----b0undary";
  const std::string body = make_body(
      b, std::vector<std::string>(
             1, file_part("avatar", "a b.png", "image/png", "hello world")));

  run_outcome o = run_whole(b, body);
  check(o.result == uvcpp::uvcpp_web_multipart_result::DONE,
        "single_part: 结果应当是 DONE");
  check_eq_i(static_cast<long long>(o.parts.size()), 1, "single_part: 部件数");
  if (o.parts.size() == 1) {
    check_eq_s(o.parts[0].name, "avatar", "single_part: name");
    check_eq_s(o.parts[0].filename, "a b.png", "single_part: filename");
    check_eq_s(o.parts[0].content_type, "image/png", "single_part: content-type");
    check(o.parts[0].is_file, "single_part: 是文件部件");
    check_eq_s(o.parts[0].body, "hello world", "single_part: body");
    check(!o.parts[0].truncated, "single_part: 不应被截断");
  }
  // **边界前的那个 CRLF 属于分隔符，不属于 body** —— body 必须是
  // 逐字节 "hello world"，既不多一个 CRLF 也不少。
  check_eq_i(static_cast<long long>(o.body_bytes), 11,
             "single_part: body 字节数（不含边界前的 CRLF）");

  // 字段部件（无 filename）
  const std::string body2 = make_body(
      b, std::vector<std::string>(1, field_part("room", "lobby")));
  run_outcome o2 = run_whole(b, body2);
  check(o2.result == uvcpp::uvcpp_web_multipart_result::DONE,
        "single_part: 字段报文应当 DONE");
  check_eq_i(static_cast<long long>(o2.parts.size()), 1,
             "single_part: 字段部件数");
  if (o2.parts.size() == 1) {
    check(!o2.parts[0].is_file, "single_part: 字段不是文件部件");
    check_eq_s(o2.parts[0].body, "lobby", "single_part: 字段值");
    check(o2.parts[0].content_type.empty(),
          "single_part: 字段没有 content-type");
  }
}

// =========================================================================
// 组：split_every_offset —— 本文件的核心
// =========================================================================
void test_split_every_offset() {
  std::vector<sample> ss = samples();
  for (size_t si = 0; si < ss.size(); ++si) {
    const sample& s = ss[si];
    const std::string tag = std::string("split_every_offset[") + s.name + "]";

    run_outcome whole = run_whole(s.boundary, s.body);
    check(whole.result == uvcpp::uvcpp_web_multipart_result::DONE,
          tag + ": 整包喂应当 DONE");
    size_t worst = whole.max_retained;

    // 每一个字节偏移切成两块。
    for (size_t cut = 1; cut < s.body.size(); ++cut) {
      std::vector<size_t> cuts(1, cut);
      run_outcome sp = run_split(s.boundary, s.body, cuts);
      if (sp.max_retained > worst) worst = sp.max_retained;

      if (sp.result != whole.result) {
        std::cout << "  [FAIL] " << tag << " 切在 " << cut << "：结果 "
                  << uvcpp::uvcpp_web_multipart_result_name(sp.result)
                  << " != "
                  << uvcpp::uvcpp_web_multipart_result_name(whole.result)
                  << std::endl;
        ++g_failures;
        continue;
      }
      std::string why;
      if (!same_parts(sp, whole, &why)) {
        std::cout << "  [FAIL] " << tag << " 切在 " << cut << "：" << why
                  << std::endl;
        ++g_failures;
      }
    }

    // 3 段等分（含余数落在最后一段）
    {
      std::vector<size_t> cuts;
      cuts.push_back(s.body.size() / 3);
      cuts.push_back(s.body.size() / 3 * 2);
      run_outcome o = run_split(s.boundary, s.body, cuts);
      if (o.max_retained > worst) worst = o.max_retained;
      std::string why;
      check(o.result == whole.result && same_parts(o, whole, &why),
            tag + ": 3 段切分产出必须与整包相同（" + why + "）");
    }

    // 逐字节喂（最极端的切法）
    {
      std::vector<size_t> cuts;
      for (size_t i = 1; i < s.body.size(); ++i) cuts.push_back(i);
      run_outcome o = run_split(s.boundary, s.body, cuts);
      if (o.max_retained > worst) worst = o.max_retained;
      std::string why;
      check(o.result == whole.result && same_parts(o, whole, &why),
            tag + ": 逐字节喂产出必须与整包相同（" + why + "）");
    }

    // **保留缓冲的上界与输入总量无关。** 取"所有切法里最大的那一个"而不是
    // 只看整包 —— 整包喂时缓冲区被一次性消费光，这个数恒为 0，断言会退化成
    // 恒真；真正把上界压出来的是"切在边界行中间"那些切点。
    check(worst <= uvcpp::uvcpp_web_multipart_max_retain(s.boundary.size()),
          tag + ": 保留缓冲必须不超过 max_retain(boundary)（实测 " +
              std::to_string(worst) + "）");
  }

  // 长边界（70 字节，RFC 2046 上限）单独跑一遍：保留上界随边界增长，
  // 短边界上"保留长度算错"可能恰好不出界，长边界才压得出来。
  {
    const std::string b = kLongBoundary;
    std::string payload;
    for (int i = 0; i < 500; ++i) payload += "abcdefghij";
    const std::string body =
        make_body(b, std::vector<std::string>(1, field_part("k", payload)));

    run_outcome whole = run_whole(b, body);
    check(whole.result == uvcpp::uvcpp_web_multipart_result::DONE,
          "split_every_offset[长边界]: 整包喂应当 DONE");
    if (whole.parts.size() == 1) {
      check_eq_s(whole.parts[0].body, payload,
                 "split_every_offset[长边界]: body 逐字节相同");
    }
    size_t worst = whole.max_retained;

    for (size_t cut = 1; cut < body.size(); cut += 7) {  // 抽样，够了
      std::vector<size_t> cuts(1, cut);
      run_outcome o = run_split(b, body, cuts);
      if (o.max_retained > worst) worst = o.max_retained;
      std::string why;
      check(o.result == whole.result && same_parts(o, whole, &why),
            "split_every_offset[长边界]: 切在 " + std::to_string(cut) +
                " 产出必须相同");
    }

    // 这份 body 有 5000 字节，保留缓冲却必须仍然只跟 boundary 有关 ——
    // 这一条是"内存不随输入增长"的直接证据（短边界上算错保留长度可能恰好
    // 不出界，长边界才压得出来）。
    check(worst <= uvcpp::uvcpp_web_multipart_max_retain(b.size()),
          "split_every_offset[长边界]: 保留缓冲必须不超过 max_retain"
          "（实测 " + std::to_string(worst) + "，body " +
              std::to_string(body.size()) + " 字节）");
  }
}

// =========================================================================
// 组：preamble_epilogue
// =========================================================================
void test_preamble_epilogue() {
  const std::string b = "PB";
  const std::string core =
      make_body(b, std::vector<std::string>(1, field_part("k", "v")));

  {
    const std::string body = "IGNORED PREAMBLE\r\n" + core;
    run_outcome o = run_whole(b, body);
    check(o.result == uvcpp::uvcpp_web_multipart_result::DONE,
          "preamble_epilogue: 有前导应当 DONE");
    check_eq_i(static_cast<long long>(o.parts.size()), 1,
               "preamble_epilogue: 前导不产生部件");
    if (o.parts.size() == 1) {
      check_eq_s(o.parts[0].body, "v", "preamble_epilogue: 前导不进 body");
    }
  }
  {
    const std::string body = core + "IGNORED EPILOGUE\r\nmore";
    run_outcome o = run_whole(b, body);
    check(o.result == uvcpp::uvcpp_web_multipart_result::DONE,
          "preamble_epilogue: 有结尾应当 DONE");
    check_eq_i(static_cast<long long>(o.parts.size()), 1,
               "preamble_epilogue: 结尾不产生部件");
    if (o.parts.size() == 1) {
      check_eq_s(o.parts[0].body, "v", "preamble_epilogue: 结尾不进 body");
    }
  }
  {
    // 前导里含"近似边界" —— 必须仍然只认真正的那个。
    //
    // 两个 `\r\n--PB` 后面都不是行尾（'x' 与 'g'），所以都是假命中；真正的
    // 分隔符是 `garbage` 之后那个 `\r\n--PB\r\n`。所以 `garbage` 与 `core`
    // **之间必须有一个 CRLF** —— 少了它，`--PB` 前面就不是 CRLF，
    // 一个部件都不会开始（第一版就漏了这个 CRLF）。
    const std::string body =
        "\r\n--PBx\r\n--PB" + std::string("garbage") + "\r\n" + core;
    run_outcome o = run_split(b, body, std::vector<size_t>(1, 5));
    check(o.result == uvcpp::uvcpp_web_multipart_result::DONE,
          "preamble_epilogue: 前导里的近似边界应当 DONE");
    check_eq_i(static_cast<long long>(o.parts.size()), 1,
               "preamble_epilogue: 前导近似边界不产生部件");
  }
  {
    // 终边界之后再来一份完整报文：必须被当作 epilogue 丢弃（不能解析出
    // 第二批部件 —— 那是请求走私的形状）。
    const std::string body = core + core;
    run_outcome o = run_whole(b, body);
    check(o.result == uvcpp::uvcpp_web_multipart_result::DONE,
          "preamble_epilogue: 双报文应当 DONE");
    check_eq_i(static_cast<long long>(o.parts.size()), 1,
               "preamble_epilogue: 终边界之后的内容必须全部丢弃");
  }
}

// =========================================================================
// 组：crlf_vs_lf —— 行尾必须"明确表态"，不能碰巧能跑
// =========================================================================
void test_crlf_vs_lf() {
  const std::string b = "LF";
  {
    // 只认 CRLF：用 LF 当分隔符 ⇒ 边界**从未出现** ⇒ body 被截断。
    // 这是"明确拒绝"的判据，而不是含混地报别的错。
    const std::string body = "--LF\nContent-Disposition: form-data; "
                             "name=\"k\"\n\nv\n--LF--\n";
    run_outcome o = run_whole(b, body);
    check(o.result == uvcpp::uvcpp_web_multipart_result::ERROR_BOUNDARY_NEVER_FOUND,
          "crlf_vs_lf: 纯 LF 报文必须判为截断（不接受 LF 当分隔符）");
    check_eq_i(static_cast<long long>(o.parts.size()), 0,
               "crlf_vs_lf: 纯 LF 报文不得产出任何部件");
  }
  {
    // 部件头里混入裸 LF ⇒ 畸形。
    const std::string body = "--LF\r\nContent-Disposition: form-data; "
                             "name=\"k\"\nContent-Type: x\r\n\r\nv\r\n--LF--\r\n";
    run_outcome o = run_whole(b, body);
    check(o.result == uvcpp::uvcpp_web_multipart_result::ERROR_BAD_HEADER,
          "crlf_vs_lf: 部件头里的裸 LF 必须判畸形");
  }
  {
    // 纯 CRLF 的同一份报文必须正常 —— 否则上面两条就只是"全都拒绝"。
    const std::string body = "--LF\r\nContent-Disposition: form-data; "
                             "name=\"k\"\r\n\r\nv\r\n--LF--\r\n";
    run_outcome o = run_whole(b, body);
    check(o.result == uvcpp::uvcpp_web_multipart_result::DONE,
          "crlf_vs_lf: 纯 CRLF 必须正常（对照组）");
    check_eq_i(static_cast<long long>(o.parts.size()), 1,
               "crlf_vs_lf: 纯 CRLF 应当产出一个部件");
    if (o.parts.size() == 1) {
      check_eq_s(o.parts[0].body, "v", "crlf_vs_lf: 纯 CRLF 的字段值");
    }
  }
  {
    // **零头部件**：部件的头块是那个空行本身（`\r\n`），不是 `\r\n\r\n`。
    // 判据不是"它该成功"（一个没有 `Content-Disposition` 的部件是畸形的），
    // 而是"它不能卡住" —— 少写这一支，解析器会一直等 `\r\n\r\n`，
    // 表现为**永远不结束**（不是报错，是静默挂死），那是最难查的一类。
    const std::string body = "--LF\r\n\r\nX\r\n--LF--\r\n";
    run_outcome o = run_whole(b, body);
    check(o.result != uvcpp::uvcpp_web_multipart_result::OK,
          "crlf_vs_lf: 零头部件不能「卡住」（必须给出确定结果）");
    check(o.result == uvcpp::uvcpp_web_multipart_result::ERROR_NOT_FORM_DATA,
          "crlf_vs_lf: 零头部件按「缺 Content-Disposition」处理");
  }
}

// =========================================================================
// 组：near_miss_boundary
// =========================================================================
void test_near_miss_boundary() {
  const std::string b = "NM";
  // 每一种"差一点就是边界"的形状都必须原样留在 body 里。
  const char* near[] = {
      "\r\n--N",         // 少一个字节
      "\r\n--NM",        // 完整边界，但没有尾随两字节
      "\r\n--NMx",       // 尾随字节不合法
      "\r\n--NM\r",      // 尾随只有 CR
      "\r\n--NM--x",     // 看似终边界，但 -- 之后多一个字节
      "\r\n--NM-",       // 尾随只有一个 '-'
      "\r--NM",          // 前缀只有 CR
      "\n--NM\r\n",      // 前缀只有 LF
  };
  for (size_t i = 0; i < sizeof(near) / sizeof(near[0]); ++i) {
    const std::string payload = std::string("A") + near[i] + "B";
    const std::string body =
        make_body(b, std::vector<std::string>(1, field_part("k", payload)));

    run_outcome o = run_whole(b, body);
    check(o.result == uvcpp::uvcpp_web_multipart_result::DONE,
          std::string("near_miss_boundary: 第 ") + std::to_string(i) +
              " 种应当 DONE");
    check_eq_i(static_cast<long long>(o.parts.size()), 1,
               std::string("near_miss_boundary: 第 ") + std::to_string(i) +
                   " 种必须恰好一个部件");
    if (o.parts.size() == 1) {
      check_eq_s(o.parts[0].body, payload,
                 std::string("near_miss_boundary: 第 ") + std::to_string(i) +
                     " 种必须原样留在 body 里");
    }

    // 并逐偏移切一遍：假命中回退最容易在跨块时把字节吃掉或吐重复。
    run_outcome whole = o;
    for (size_t cut = 1; cut < body.size(); ++cut) {
      run_outcome sp = run_split(b, body, std::vector<size_t>(1, cut));
      std::string why;
      if (!(sp.result == whole.result && same_parts(sp, whole, &why))) {
        std::cout << "  [FAIL] near_miss_boundary: 第 " << i << " 种切在 "
                  << cut << " 产出不同（" << why << "）" << std::endl;
        ++g_failures;
      }
    }
  }

  // -------------------------------------------------------------------
  // **带填充的边界行**（RFC 2046 §5.1.1 的 `[LWSP]`）。
  // 两个填充位置（boundary 之后、`--` 之后）各测"接受"与"拒绝"两个方向。
  //
  // 拒绝那一侧是安全要求，不是洁癖：流式解析不能为"一行空白"无界地留缓冲，
  // 而"超限就当 body"更坏 —— 那会让本端与接受该填充的对端对"部件从哪儿切"
  // 产生分歧，正是请求走私（同一段字节被两个解析器切出不同部件划分）的形状。
  // -------------------------------------------------------------------
  {
    const char* kHead =
        "--NM\r\nContent-Disposition: form-data; name=\"k\"\r\n\r\nv";
    for (size_t k = 0; k < 4; ++k) {
      const bool ok_case = (k % 2 == 0);
      const size_t pad = ok_case ? 100 : 200;
      const std::string padstr(pad, ' ');
      const std::string body =
          (k < 2) ? std::string(kHead) + "\r\n--NM" + padstr + "--\r\n"
                  : std::string(kHead) + "\r\n--NM--" + padstr + "\r\n";
      run_outcome o = run_whole(b, body);
      const std::string tag2 = std::string("near_miss_boundary: 填充[") +
                               (k < 2 ? "boundary 之后" : "-- 之后") + "] " +
                               std::to_string(pad) + " 字节";
      if (ok_case) {
        check(o.result == uvcpp::uvcpp_web_multipart_result::DONE,
              tag2 + " 必须被接受");
        check_eq_i(static_cast<long long>(o.parts.size()), 1,
                   tag2 + " 仍应产出那个部件");
      } else {
        check(o.result == uvcpp::uvcpp_web_multipart_result::ERROR_BAD_HEADER,
              tag2 + " 必须明确失败（不得当成 body）");
      }
    }
  }
}

// =========================================================================
// 组：quoted_filename
// =========================================================================
void test_quoted_filename() {
  const std::string b = "QF";
  {
    // 引号内的分号**不是**参数分隔符。
    const std::string part =
        "Content-Disposition: form-data; name=\"f\"; "
        "filename=\"a;b.txt\"\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    check(o.result == uvcpp::uvcpp_web_multipart_result::DONE,
          "quoted_filename: 分号在引号内应当 DONE");
    if (o.parts.size() == 1) {
      check_eq_s(o.parts[0].filename, "a;b.txt",
                 "quoted_filename: 引号内分号属于文件名");
      check_eq_s(o.parts[0].name, "f", "quoted_filename: name 不受影响");
    }
  }
  {
    // `\"` 是转义引号。
    const std::string part =
        "Content-Disposition: form-data; name=\"f\"; "
        "filename=\"a\\\"b.txt\"\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    check(o.result == uvcpp::uvcpp_web_multipart_result::DONE,
          "quoted_filename: 转义引号应当 DONE");
    if (o.parts.size() == 1) {
      check_eq_s(o.parts[0].filename, "a\"b.txt",
                 "quoted_filename: 转义引号必须解出来");
    }
  }
  {
    // 引号没闭合 ⇒ 畸形（不接受：参数边界会变成各说各话）。
    const std::string part =
        "Content-Disposition: form-data; name=\"f\"; filename=\"a.txt\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    check(o.result == uvcpp::uvcpp_web_multipart_result::ERROR_BAD_HEADER,
          "quoted_filename: 未闭合的引号必须判畸形");
  }
  {
    // RFC 7578 §4.2：普通 filename **不做**百分号解码。
    const std::string part =
        "Content-Disposition: form-data; name=\"f\"; "
        "filename=\"a%20b.txt\"\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    if (o.parts.size() == 1) {
      check_eq_s(o.parts[0].filename, "a%20b.txt",
                 "quoted_filename: 普通 filename 不得百分号解码");
    }
  }
  {
    // 文件名里有分号但**没有**引号包裹：按 token 处理，原样（RFC 要求
    // 这种值必须加引号，我们不做"猜"）。
    const std::string part =
        "Content-Disposition: form-data; name=\"f\"; filename=a.txt\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    if (o.parts.size() == 1) {
      check_eq_s(o.parts[0].filename, "a.txt",
                 "quoted_filename: token 形式的 filename");
      check(o.parts[0].is_file, "quoted_filename: token 形式仍算文件部件");
    }
  }
  {
    // `filename=""` ⇒ 仍是**文件部件**（匿名）。浏览器对空的 file input
    // 就是这么发的，把它当字段会走错分支。
    const std::string part =
        "Content-Disposition: form-data; name=\"f\"; filename=\"\"\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    if (o.parts.size() == 1) {
      check(o.parts[0].is_file,
            "quoted_filename: filename=\"\" 仍是文件部件");
      check(o.parts[0].filename.empty(),
            "quoted_filename: filename=\"\" 的名字为空");
    }
  }
}

// =========================================================================
// 组：rfc5987_filename_star
// =========================================================================
void test_rfc5987_filename_star() {
  const std::string b = "RS";
  const std::string utf8_cn = "\xe4\xb8\xad\xe6\x96\x87.txt";  // 中文.txt

  {
    // 只有 filename*。
    const std::string part =
        "Content-Disposition: form-data; name=\"f\"; "
        "filename*=UTF-8''%E4%B8%AD%E6%96%87.txt\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    check(o.result == uvcpp::uvcpp_web_multipart_result::DONE,
          "rfc5987: 只有 filename* 应当 DONE");
    if (o.parts.size() == 1) {
      check_eq_s(o.parts[0].filename, utf8_cn,
                 "rfc5987: filename* 必须百分号解码成 UTF-8");
      check(o.parts[0].is_file, "rfc5987: filename* 也算文件部件");
    }
  }
  {
    // RFC 6266 §4.3：**filename\* 优先于 filename**。
    const std::string part =
        "Content-Disposition: form-data; name=\"f\"; filename=\"fallback.txt\"; "
        "filename*=UTF-8''%E4%B8%AD%E6%96%87.txt\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    if (o.parts.size() == 1) {
      check_eq_s(o.parts[0].filename, utf8_cn,
                 "rfc5987: filename* 优先于 filename");
    }

    // 顺序反过来也必须得到同样结果（优先级与出现次序无关）。
    const std::string part2 =
        "Content-Disposition: form-data; name=\"f\"; "
        "filename*=UTF-8''%E4%B8%AD%E6%96%87.txt; filename=\"fallback.txt\""
        "\r\n\r\nD";
    run_outcome o2 =
        run_whole(b, make_body(b, std::vector<std::string>(1, part2)));
    if (o2.parts.size() == 1) {
      check_eq_s(o2.parts[0].filename, utf8_cn,
                 "rfc5987: 优先级与参数出现次序无关");
    }
  }
  {
    // `+` 在 RFC 5987 里是**字面加号**，不是空格。解成空格是静默损坏。
    const std::string part =
        "Content-Disposition: form-data; name=\"f\"; "
        "filename*=UTF-8''a+b.txt\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    if (o.parts.size() == 1) {
      check_eq_s(o.parts[0].filename, "a+b.txt",
                 "rfc5987: 加号必须保持字面（不是空格）");
    }
  }
  {
    // 认不出的字符集 ⇒ **回退到 filename=**（RFC 6266 §4.3）。
    const std::string part =
        "Content-Disposition: form-data; name=\"f\"; filename=\"ok.txt\"; "
        "filename*=ISO-8859-1''%E4%B8%AD.txt\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    if (o.parts.size() == 1) {
      check_eq_s(o.parts[0].filename, "ok.txt",
                 "rfc5987: 未知字符集必须回退到 filename=");
    }
  }
  {
    // filename* 本身畸形、且没有 filename= ⇒ 名字为空，但仍是文件部件。
    const std::string part =
        "Content-Disposition: form-data; name=\"f\"; "
        "filename*=garbage\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    check(o.result == uvcpp::uvcpp_web_multipart_result::DONE,
          "rfc5987: 畸形 filename* 不应让整个请求失败");
    if (o.parts.size() == 1) {
      check(o.parts[0].is_file, "rfc5987: 畸形 filename* 仍算文件部件");
      check(o.parts[0].filename.empty(), "rfc5987: 畸形 filename* 名字为空");
    }
  }
}

// =========================================================================
// 组：malformed
// =========================================================================
void test_malformed() {
  const std::string b = "MF";
  {
    // 缺 name=
    const std::string part = "Content-Disposition: form-data\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    check(o.result == uvcpp::uvcpp_web_multipart_result::ERROR_MISSING_NAME,
          "malformed: 缺 name= 必须判 ERROR_MISSING_NAME");
  }
  {
    // Content-Disposition 不是 form-data
    const std::string part =
        "Content-Disposition: attachment; name=\"k\"\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    check(o.result == uvcpp::uvcpp_web_multipart_result::ERROR_NOT_FORM_DATA,
          "malformed: 非 form-data 必须判 ERROR_NOT_FORM_DATA");
  }
  {
    // 完全没有 Content-Disposition
    const std::string part = "Content-Type: text/plain\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    check(o.result == uvcpp::uvcpp_web_multipart_result::ERROR_NOT_FORM_DATA,
          "malformed: 缺 Content-Disposition 必须判 ERROR_NOT_FORM_DATA");
  }
  {
    // 头行里没有冒号
    const std::string part =
        "Content-Disposition form-data; name=\"k\"\r\n\r\nD";
    run_outcome o = run_whole(b, make_body(b, std::vector<std::string>(1, part)));
    check(o.result == uvcpp::uvcpp_web_multipart_result::ERROR_BAD_HEADER,
          "malformed: 头行缺冒号必须判 ERROR_BAD_HEADER");
  }
  {
    // 边界从未出现 ⇒ finish() 必须判"截断"
    run_outcome o = run_whole(b, "this is not multipart at all");
    check(o.result ==
              uvcpp::uvcpp_web_multipart_result::ERROR_BOUNDARY_NEVER_FOUND,
          "malformed: 边界从未出现必须判 ERROR_BOUNDARY_NEVER_FOUND");
  }
  {
    // 终边界缺失（报文被掐断）⇒ 同样必须判出来，不能当成"正常结束"。
    // 少了这一条，一个被掐断的上传会被当成"文件就是小"，而半截文件已落盘。
    const std::string body = make_body(
        b, std::vector<std::string>(1, field_part("k", "v")));
    const std::string truncated = body.substr(0, body.size() - 10);
    run_outcome o = run_whole(b, truncated);
    check(o.result ==
              uvcpp::uvcpp_web_multipart_result::ERROR_BOUNDARY_NEVER_FOUND,
          "malformed: 终端边界缺失必须判 ERROR_BOUNDARY_NEVER_FOUND");
  }
  {
    // 空 boundary / 超长 boundary ⇒ 配置期就拒绝。
    uvcpp::uvcpp_web_multipart mp;
    check(!mp.set_boundary(""), "malformed: 空 boundary 必须被拒绝");
    check(!mp.set_boundary(std::string(71, 'x')),
          "malformed: 71 字节的 boundary 必须被拒绝（RFC 2046 上限 70）");
    check(mp.set_boundary(std::string(70, 'x')),
          "malformed: 恰好 70 字节的 boundary 必须被接受");
    check(mp.set_boundary("ok"), "malformed: 普通 boundary 必须被接受");
  }
  {
    // 终态之后再喂是空操作，且结果不变。
    run_outcome o = run_whole(b, "garbage");
    check(o.result ==
              uvcpp::uvcpp_web_multipart_result::ERROR_BOUNDARY_NEVER_FOUND,
          "malformed: 先拿到错误结果");
  }
}

// =========================================================================
// 组：header_too_long
// =========================================================================
void test_header_too_long() {
  const std::string b = "HL";
  std::string huge(9 * 1024, 'x');
  const std::string part = "Content-Disposition: form-data; name=\"" + huge +
                           "\"\r\n\r\nD";
  const std::string body = make_body(b, std::vector<std::string>(1, part));

  run_outcome o = run_whole(b, body);
  check(o.result == uvcpp::uvcpp_web_multipart_result::ERROR_HEADER_TOO_LONG,
        "header_too_long: 超过 8 KiB 的部件头必须判 ERROR_HEADER_TOO_LONG");

  // **上限要在攒的过程中判，不能等攒完** —— 否则对端可以一直发头撑爆内存。
  // 判据：只喂一半（还没到 \r\n\r\n）就必须已经报错。
  {
    recording_sink sink;
    uvcpp::uvcpp_web_multipart mp;
    mp.set_sink(&sink);
    check(mp.set_boundary(b), "header_too_long: set_boundary");
    const std::string head_only =
        "--" + b + "\r\nContent-Disposition: form-data; name=\"" + huge;
    uvcpp::uvcpp_web_multipart_result r = mp.feed(head_only.data(),
                                                 head_only.size());
    check(r == uvcpp::uvcpp_web_multipart_result::ERROR_HEADER_TOO_LONG,
          "header_too_long: 头还没结束就必须已经报错（不能等攒完）");
    // 报错之后不得继续持有那份大缓冲（`fail()` 会清掉它）。
    check(mp.retained_bytes() == 0,
          "header_too_long: 报错后必须释放保留缓冲");
  }

  // 正常大小的头必须照常通过（对照组：否则"一律拒绝"也能过上面那条）。
  {
    const std::string ok_part =
        "Content-Disposition: form-data; name=\"k\"\r\nX-Pad: " +
        std::string(4000, 'y') + "\r\n\r\nD";
    run_outcome o2 =
        run_whole(b, make_body(b, std::vector<std::string>(1, ok_part)));
    check(o2.result == uvcpp::uvcpp_web_multipart_result::DONE,
          "header_too_long: 4 KiB 的部件头必须照常通过（对照组）");
  }
}

// =========================================================================
// 组：sink_abort
// =========================================================================
void test_sink_abort() {
  const std::string b = "SA";
  const std::string body = make_body(
      b, std::vector<std::string>(1, field_part("k", "0123456789")));

  recording_sink sink;
  sink.abort_after_ = 1;
  uvcpp::uvcpp_web_multipart mp;
  mp.set_sink(&sink);
  check(mp.set_boundary(b), "sink_abort: set_boundary");
  uvcpp::uvcpp_web_multipart_result r = mp.feed(body.data(), body.size());
  check(r == uvcpp::uvcpp_web_multipart_result::ERROR_SINK_ABORTED,
        "sink_abort: sink 说不收 ⇒ 必须报 ERROR_SINK_ABORTED");
  check(mp.state() == uvcpp::uvcpp_web_multipart_state::ERROR_STATE,
        "sink_abort: 进入 ERROR_STATE");
}

// =========================================================================
// 组：limits（解析器层的上限：文件数/字段数/字段大小/单文件截断）
// =========================================================================
void test_limits() {
  const std::string b = "LM";
  {
    // 文件数超限
    std::vector<std::string> parts;
    for (int i = 0; i < 3; ++i) {
      parts.push_back(file_part("f" + std::to_string(i), "x.bin",
                                "application/octet-stream", "D"));
    }
    const std::string body = make_body(b, parts);

    recording_sink sink;
    uvcpp::uvcpp_web_multipart mp;
    mp.set_sink(&sink);
    check(mp.set_boundary(b), "limits: set_boundary");
    mp.set_max_file_count(2);
    uvcpp::uvcpp_web_multipart_result r = mp.feed(body.data(), body.size());
    check(r == uvcpp::uvcpp_web_multipart_result::ERROR_TOO_MANY_FILES,
          "limits: 第 3 个文件部件必须判 ERROR_TOO_MANY_FILES");
  }
  {
    // 字段数超限
    std::vector<std::string> parts;
    for (int i = 0; i < 4; ++i) parts.push_back(field_part("k", "v"));
    const std::string body = make_body(b, parts);

    recording_sink sink;
    uvcpp::uvcpp_web_multipart mp;
    mp.set_sink(&sink);
    check(mp.set_boundary(b), "limits: set_boundary");
    mp.set_max_field_count(3);
    uvcpp::uvcpp_web_multipart_result r = mp.feed(body.data(), body.size());
    check(r == uvcpp::uvcpp_web_multipart_result::ERROR_TOO_MANY_FIELDS,
          "limits: 第 4 个字段部件必须判 ERROR_TOO_MANY_FIELDS");
  }
  {
    // 字段超大小 ⇒ 拒绝（字段**不**截断）
    const std::string body = make_body(
        b, std::vector<std::string>(1, field_part("k", std::string(100, 'z'))));

    recording_sink sink;
    uvcpp::uvcpp_web_multipart mp;
    mp.set_sink(&sink);
    check(mp.set_boundary(b), "limits: set_boundary");
    mp.set_max_field_size(50);
    uvcpp::uvcpp_web_multipart_result r = mp.feed(body.data(), body.size());
    check(r == uvcpp::uvcpp_web_multipart_result::ERROR_FIELD_TOO_LARGE,
          "limits: 超限的字段必须判 ERROR_FIELD_TOO_LARGE");
  }
  {
    // 单文件超限 ⇒ **截断**（不是拒绝），且后续部件必须照常解析出来。
    // 后半句是 `PART_BODY_SKIP` 的全部意义：停下来交付，但**继续扫边界**。
    std::vector<std::string> parts;
    parts.push_back(file_part("f", "big.bin", "application/octet-stream",
                              std::string(1000, 'A')));
    parts.push_back(field_part("after", "reached"));
    const std::string body = make_body(b, parts);

    recording_sink sink;
    uvcpp::uvcpp_web_multipart mp;
    mp.set_sink(&sink);
    check(mp.set_boundary(b), "limits: set_boundary");
    mp.set_max_file_size(100);
    uvcpp::uvcpp_web_multipart_result r = mp.feed(body.data(), body.size());
    check(r == uvcpp::uvcpp_web_multipart_result::DONE,
          "limits: 单文件超限应当截断而不是拒绝");
    check_eq_i(static_cast<long long>(sink.parts.size()), 2,
               "limits: 截断之后**后续部件必须照常解析出来**");
    if (sink.parts.size() == 2) {
      check_eq_i(static_cast<long long>(sink.parts[0].body.size()), 100,
                 "limits: 被截断的文件恰好留下上限那么多字节");
      check(sink.parts[0].truncated, "limits: truncated 必须置位");
      check_eq_s(sink.parts[1].name, "after",
                 "limits: 截断之后的下一个部件名字");
      check_eq_s(sink.parts[1].body, "reached",
                 "limits: 截断之后的下一个部件完整");
      check(!sink.parts[1].truncated, "limits: 下一个部件不应被标记截断");
    }

    // 截断也必须逐偏移可复现（SKIP 状态的跨块行为最易错）。
    run_outcome whole;
    {
      // 用同样的配置跑整包，作为切分比对的基准。
      recording_sink s2;
      uvcpp::uvcpp_web_multipart m2;
      m2.set_sink(&s2);
      m2.set_boundary(b);
      m2.set_max_file_size(100);
      m2.feed(body.data(), body.size());
      whole.parts = s2.parts;
      whole.result = m2.result();
    }
    for (size_t cut = 1; cut < body.size(); ++cut) {
      recording_sink s3;
      uvcpp::uvcpp_web_multipart m3;
      m3.set_sink(&s3);
      m3.set_boundary(b);
      m3.set_max_file_size(100);
      m3.feed(body.data(), cut);
      m3.feed(body.data() + cut, body.size() - cut);
      std::string why;
      run_outcome o3;
      o3.parts = s3.parts;
      o3.result = m3.result();
      if (!(o3.result == whole.result && same_parts(o3, whole, &why))) {
        std::cout << "  [FAIL] limits: 截断路径切在 " << cut << " 产出不同（"
                  << why << "）" << std::endl;
        ++g_failures;
      }
    }
  }
  {
    // 默认不限 ⇒ 大部件完整通过（对照组）
    std::vector<std::string> parts;
    parts.push_back(file_part("f", "big.bin", "application/octet-stream",
                              std::string(200000, 'A')));
    const std::string body = make_body(b, parts);

    recording_sink sink;
    uvcpp::uvcpp_web_multipart mp;
    mp.set_sink(&sink);
    check(mp.set_boundary(b), "limits: set_boundary");
    uvcpp::uvcpp_web_multipart_result r = mp.feed(body.data(), body.size());
    check(r == uvcpp::uvcpp_web_multipart_result::DONE,
          "limits: 默认不限应当 DONE");
    if (sink.parts.size() == 1) {
      check_eq_i(static_cast<long long>(sink.parts[0].body.size()), 200000,
                 "limits: 默认不限时 200 KB 必须完整");
      check(!sink.parts[0].truncated, "limits: 默认不限时不应标记截断");
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string only;
  if (argc > 1) only = argv[1];

  struct case_entry {
    const char* name;
    void (*fn)();
  };
  const case_entry cases[] = {
      {"single_part", test_single_part},
      {"split_every_offset", test_split_every_offset},
      {"preamble_epilogue", test_preamble_epilogue},
      {"crlf_vs_lf", test_crlf_vs_lf},
      {"near_miss_boundary", test_near_miss_boundary},
      {"quoted_filename", test_quoted_filename},
      {"rfc5987_filename_star", test_rfc5987_filename_star},
      {"malformed", test_malformed},
      {"header_too_long", test_header_too_long},
      {"sink_abort", test_sink_abort},
      {"limits", test_limits},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (!only.empty() &&
        std::string(cases[i].name).find(only) == std::string::npos) {
      continue;
    }
    const int before = g_failures;
    std::cout << "[multipart] " << cases[i].name << std::endl;
    cases[i].fn();
    const bool ok = (g_failures == before);
    std::cout << (ok ? "  -> PASS" : "  -> FAIL") << std::endl;
    if (!ok) {
      std::cout << "[multipart] FAIL" << std::endl;
      return 2;
    }
  }

  std::cout << "[multipart] ALL PASS" << std::endl;
  return 0;
}

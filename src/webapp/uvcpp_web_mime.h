/**
 * @file src/webapp/uvcpp_web_mime.h
 * @brief 可配置的 MIME 表：在内置表之上增删改。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 为什么要有独立的类型
 * --------------------
 * `web_mime_type()`（`uvcpp_web_util.h`）只认内置表，返回的是**静态字符串**。
 * 它适合"猜个大概"，但静态文件服务有两个它办不到的事：
 *
 * 1. **加类型**。内置表覆盖了常见类型（含 `wasm`/`webp`/`avif`/`mjs`），
 *    但 `.jsx`、`.tsx`、`.yaml`、`.toml`、`.mdx`、`.heic` 这些没有，一律落到
 *    `application/octet-stream`。浏览器拿到这个类型会**下载**而不是渲染。
 * 2. **改类型**。这是最容易踩的一个：内置表里 **`.ts` 是 `video/mp2t`**
 *    （MPEG 传输流），因为那是 `.ts` 在多媒体语境下的标准含义。可是前端项目
 *    里的 `.ts` 是 TypeScript —— 不覆盖它，`main.ts` 会被当成视频送出去。
 *    同类还有 `.map`（内置给了 `application/json`，有人要 `application/octet-stream`
 *    来阻止 devtools 解析）。这些是**部署决策**，框架不该替使用者钉死。
 *
 * 所以本类是「内置表 + 使用者覆盖」的两层结构：构造时把内置表灌进去，
 * 之后 `set()` 只改被点名的那几条。这样**不知道的扩展名仍然有合理默认**，
 * 而不是"用了自定义表就只剩我填的那几条"。
 *
 * 关于大小写
 * ----------
 * 扩展名一律小写化后再查（`web_to_lower`），所以 `set(".PNG", ...)` 与
 * `set(".png", ...)` 等价 —— 但存的时候统一**不带点**，这是最容易写错的地方：
 * `set("png")` 和 `set(".png")` 都接受。
 *
 * 线程约定
 * --------
 * 与路由表相同：**注册期配置、运行期只读**。构造/`set()` 请在 `start()`
 * 之前做完。运行期并发只读是安全的（内部是 `std::map`，没有惰性写入）。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_MIME_H
#define SRC_WEBAPP_UVCPP_WEB_MIME_H

#include <map>
#include <string>

#include <uvcpp/uvcpp_export.h>

namespace uvcpp {

/**
 * @brief 扩展名 → MIME 类型 的表。
 *
 * @code
 *   uvcpp_web_mime_map mime;                    // 已经带内置表
 *   mime.set("ts",  "text/typescript");         // 盖掉内置的 video/mp2t
 *   mime.set(".tsx", "text/typescript");        // 带不带点都行
 *   mime.set_from_string("yaml=text/yaml;toml=text/toml");
 *   const char* t = mime.lookup("/src/main.ts");     // "text/typescript"
 * @endcode
 */
class UVCPP_API uvcpp_web_mime_map {
 public:
  /**
   * @brief 构造。
   * @param with_builtin 是否预置内置表（默认 true）。
   *
   * 传 false 会得到一张**空表** —— 只有确实想完全接管类型判定时才这么用；
   * 空表对未知扩展名返回 `application/octet-stream`，与内置表的兜底一致。
   */
  explicit uvcpp_web_mime_map(bool with_builtin = true);

  /**
   * @brief 设置一个扩展名的类型（覆盖已有）。
   *
   * @param ext  扩展名，**带不带前导点都行**（`"png"` / `".png"` 等价），
   *             大小写不敏感。空字符串或只有点的输入会被忽略。
   * @param mime 类型；传空字符串等价于 `remove()`。
   * @return 是否真的改了（被忽略的输入返回 false）。
   */
  bool set(const std::string& ext, const std::string& mime);

  /**
   * @brief 批量设置，格式 `"ext=mime;ext=mime;..."`，也接受 `,` 作分隔符。
   *
   * 从配置串里读类型时省一次手写循环。空白会被裁掉，空项跳过。
   * @return 成功设置的条数。
   */
  size_t set_from_string(const std::string& spec);

  /** @brief 删掉一条（此后回落到 `application/octet-stream`）。 */
  bool remove(const std::string& ext);

  /** @brief 清空整张表（含内置项）。 */
  void clear();

  /** @brief 恢复成只有内置表的初始状态。 */
  void reset_to_builtin();

  /**
   * @brief 按**路径**查类型（取最后一个点之后的部分当扩展名）。
   *
   * @return 静态字符串，不需要释放。查不到返回
   *         `"application/octet-stream"`，这个兜底恒定成立。
   *
   * @note 取的是**最后一个**点：`archive.tar.gz` 看的是 `gz`，
   *       `lib..min.js` 看的是 `js`（名字里有连续的点不影响判定）。
   */
  const char* lookup(const std::string& path) const;

  /** @brief 按**扩展名**查类型（不传路径时用）。 */
  const char* lookup_ext(const std::string& ext) const;

  /** @brief 这个类型是不是文本（可压缩、该补 `charset`）。 */
  static bool is_text(const std::string& mime);

  /** @brief 表里现有多少条。 */
  size_t size() const { return table_.size(); }

  /** @brief 内置表条目数（`reset_to_builtin()` 之后的 `size()`）。 */
  static size_t builtin_size();

  /**
   * @brief 进程级共享的默认表。
   *
   * 静态服务没显式给表时用它。**不在启动路径上就别改它** —— 它是全局的，
   * 改了会影响所有用默认表的实例。
   */
  static uvcpp_web_mime_map& default_map();

 private:
  /// 归一化扩展名：去前导点、小写化。返回 false 表示这个输入无效。
  static bool normalize_ext(const std::string& ext, std::string& out);

  std::map<std::string, std::string> table_;
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_MIME_H

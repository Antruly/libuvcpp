/**
 * @file src/webapp/uvcpp_web_upload.h
 * @brief multipart 上传的**落盘 sink**：边收边异步写盘，全程不把文件攒进内存。
 * @author zhuweiye
 * @version 1.0.0
 *
 * 它解决的问题
 * ------------
 * 3a 把「流式收一个 body」打通了，但收到的字节**没有终点** —— handler 只能在
 * 内存里自己攒，而 `uvcpp_buf::append_data` 是 O(n²)，真攒一个大文件就是自杀。
 * 本模块给每个文件部件一个终点：收到一块就交给 `uv_fs_write` 写出去，**一个
 * 字节都不在内存里留**（除了一块正在写的、有界的前缀）。
 *
 * 为什么是 sink 而不是解析器的一部分
 * --------------------------------
 * `uvcpp_web_multipart` 是**纯**状态机（无 IO、无 libuv），所以它能被"每个字节
 * 偏移切一刀、产出必须完全相同"那种测试直接钉死。落盘是它的第一个真实消费者，
 * 但绝不能因此把它拖进 IO 里 —— 本类实现它的 sink 接口，两者分居两侧。
 *
 * 载体的选择（与本计划里那个最重要的取舍对应）
 * ------------------------------------------
 * 走**事件循环上直接提交异步 `uv_fs_*`**，而不是仓库既有的「`uvcpp_work` worker
 * 里跑同步 `uv_fs_*`」。理由：上传写盘的本质是「一串有序的小写」，而异步 fs 的
 * 形状正好是「一次一个、完成回调里发下一个」；`uvcpp_fs` 的**单笔在途守卫**
 * 在这里不是限制，而是恰好合拍的模型。反面代价是 `uvcpp_work` 严格一次性
 * （`queue_work` 在 `loop != nullptr` 时抛异常），每写一块就要新建一个对象加
 * 一套 retire/drain 回收流水 —— 纯粹是为迁就它的不可复用性付出的复杂度。
 *
 * 这个载体的可行性在写本文件之前已经单独证过（`fs_async_write_func.cpp`），
 * 而且**顺带修掉了 `uvcpp_fs` 一个真实缺陷**：36 处内联完成回调原先直接调
 * 成员 `std::function`，而回调里链下一笔会给那个成员赋值、析构掉正在执行的
 * 闭包（连同它的按值捕获）—— 也就是"自己还没跑完就被释放"。现在全部改成
 * 先 `swap` 到栈上的本地副本再调。本文件**逐个回调都依赖这个契约**。
 *
 * 生命周期：自持 + 由 `uvcpp_fs` 的 swap 打破引用环
 * ------------------------------------------------
 * 会话对象由 `shared_ptr` 持有，且**每一个在途 fs 操作的完成回调都按值捕获
 * 一份 `shared_from_this()`**。于是「最后一次写完成时框架早就把会话丢了」
 * 不成问题 —— 会话自己活到 I/O 跑完。
 *
 * 这会形成一个引用环（会话 → `fs_` → 闭包 → 会话），但它**自己会断**：
 * `uvcpp_fs::callback_*` 在调用之前先把闭包 `swap` 到栈上的本地副本，成员随之
 * 变空，环就断在那一刻。之后本地副本析构 → 最后一份 `shared_ptr` 释放 → 会话
 * 析构。**析构确实发生在 `uvcpp_fs` 的某个完成回调栈上**（闭包销毁的那个
 * 右括号处），而 `callback_*` 在调完回调之后**什么都不做**，所以这是安全的；
 * 但它是"靠契约成立"而不是"显然成立"，故记在这里，改 `uvcpp_fs` 回调形状的人
 * 必须知道这件事。
 *
 * 所有权交接点
 * ------------
 * 正常结束（`notify_parse_done(true)` 且每个文件的 fsync/close 都跑完）之后，
 * 落盘的文件**归调用方**，框架不再碰 —— 本类只负责把路径交出去。
 * 失败/中止（`notify_parse_done(false)`、解析错误、对端断开）时，本类把**本次
 * 创建的所有文件删掉**，`result()` 清空。
 *
 * 背压（步骤 5）
 * --------------
 * 单槽双缓冲（`pending` / `incoming`）本来就是"同一时刻只有一个写在途"，
 * 但**光有槽不叫背压** —— 槽满了之后新到的字节总得有个去处，没有的话就是
 * 无界增长。背压是两件事：
 *
 * 1. **把压力退回去**：只要"还有字节没写出去"（有 fs 操作在途、或任何 job 的
 *    缓冲非空、或正在等名额），就调 `flow_.pause()` 让流层停止读；条件消失时
 *    调 `flow_.resume()`。判定收在 `pump()` 的唯一出口 `sync_flow()` 里 ——
 *    它的输入只有"当前状态"，与"刚才发生了什么"无关，所以不会出现
 *    "忘了恢复"那种安静的吞吐归零。
 * 2. **一个确定的兜底**：`pause()` 不是瞬时的，对端也可以无视背压继续灌，
 *    所以 `incoming` 有上限（`set_max_pending_bytes()`，默认 1 MiB，0 = 不限）。
 *    越过上限 ⇒ `abort(413)` 掐断。
 *
 * 另外**每次 fs 操作**都向 `uvcpp_web_work_limit` 取一个名额（`set_work_limit()`）：
 * 拿不到就 `pause()` + 注册唤醒。会话不长期占名额，因为它大部分时间在等网络。
 *
 * 上限由**谁**判（步骤 6 之后，这里只留一句话，细节见 `uvcpp_web_app.h` 里
 * 那一组 setter 的表格）：**会话本身一条上限都不判**。总长在框架的
 * `wire_upload()` 里按解析器的字节计数判（判在喂之前），其余五条转给解析器。
 * 会话这里唯一的上限是 `set_max_pending_bytes()` —— 那是**内存里的在途缓冲**，
 * 与"这次上传总共多大"无关，别混。
 *
 * 落盘名与文件名安全（步骤 7）
 * ---------------------------
 * **客户端给的名字永远不参与落盘**。`original_filename()` 是元数据（清洗过、
 * 可以安全展示/写库），落盘名由本模块自己生成：
 *
 * ```
 * <upload_dir_real>/<16 位随机十六进制>[.<白名单过滤后的扩展名>]
 * ```
 *
 * 三道防线，各挡不同的东西，缺一不可：
 *
 * 1. **`O_EXCL`**：`open` 用的是 `O_WRONLY | O_CREAT | O_EXCL`（**没有
 *    `O_TRUNC`** —— 有了 `O_EXCL` 就不会命中已存在的文件，`O_TRUNC` 是多余的）。
 *    它挡的是"预先在目标路径上种一个文件或符号链接"：没有 `O_EXCL` 时
 *    `O_CREAT` 会顺着一个已存在的符号链接写出去，`O_EXCL` 让 `open` 直接
 *    拿到 `UV_EEXIST`。撞上就**重摇名字**（`kOpenRetries` 次），仍失败则按
 *    open 失败收场。
 * 2. **叶子名校验**：生成器返回的东西必须是**不含分隔符的叶子**。这是
 *    `set_name_generator()` 的契约，会话**每次都查**（见下）。
 * 3. **包含判断**：拼出的最终路径再过一次 `web_is_within_root(upload_dir_real_,
 *    path)`。生成名里没有分隔符，所以它**逻辑上永真** —— 但把它写成判断，
 *    不变式才是显式的、可测的，而不是"读代码推出来的"。
 *
 * `upload_dir_real_` 是构造时解析的（或调用方传入的）真实路径，它同时是第 3
 * 道的基准。**解析失败不致命**：退回用原始 `upload_dir_` 当基准，此时
 * `join_path(upload_dir_, leaf)` 按字面前缀必然满足包含关系，真正的守卫是第 2 道。
 *
 * @warning 会话必须先于 `uvcpp_loop` 析构。析构函数会用那个 loop 做兜底收尾
 *          （同步 `close`/`unlink`），loop 没了就是 use-after-free。
 */

#pragma once
#ifndef SRC_WEBAPP_UVCPP_WEB_UPLOAD_H
#define SRC_WEBAPP_UVCPP_WEB_UPLOAD_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <uvcpp/uvcpp_export.h>
#include <webapp/uvcpp_web_multipart.h>

namespace uvcpp {

// 前向声明：本头**刻意不拉 libuv**。它会被 `uvcpp_web_request.h` 间接包含，
// 而后者是每个用户 handler 都要 include 的头 —— 为了一个只在 .cpp 里用到的
// `uvcpp_fs` 把 `<uv.h>` 摊给所有人是不划算的。`unique_ptr<uvcpp_fs>` 配合
// 在 .cpp 里定义的析构函数即可。
class uvcpp_fs;
class uvcpp_loop;
class uvcpp_web_upload;
// 同上：`set_work_limit()` 只用到指针，不必把 `uvcpp_web_work_limit.h` 拉进来。
class uvcpp_web_work_limit;

// =========================================================================
// 元数据类型
// =========================================================================

/** @brief 一个落盘文件部件的元数据。 */
class UVCPP_API uvcpp_web_upload_file {
 public:
  uvcpp_web_upload_file();

  /**
   * @brief 框架生成的**实际落盘路径**（`upload_dir` 的真实路径 + 随机名）。
   *
   * 它一定是**绝对路径**：基数是 `upload_dir` 解析出来的真实路径，而不是调用方
   * 配置时写的那串字符。理由是包含判断（见 `.cpp` 的 `resolve_leaf()`）要求
   * "拼出来的路径"与"判断的基准"是同一个基准；副产品是调用方拿到 `path()` 之后
   * 无论当前工作目录是什么都能直接 `rename`。
   *
   * 回调返回之后这个文件就归调用方了：想搬到最终位置就 `rename`，想读完删掉
   * 也行 —— 框架不会再碰它。
   */
  const std::string& path() const { return path_; }

  /** @brief 表单字段名（`Content-Disposition: form-data; name="..."`）。 */
  const std::string& field_name() const { return field_name_; }

  /**
   * @brief 客户端自己给的文件名，**已清洗**（`web_sanitize_filename()` 的产物）。
   *
   * 清洗做的是：`/` 与 `\` 都当分隔符取叶子、去控制字符、去掉结尾的 `.`/空格、
   * 空/`.`/`..` 退化成 `"file"`、保留设备名追加 `_`、截断保持 UTF-8 码点完整。
   * 所以它**可以安全地展示、写日志、写进数据库**。
   *
   * @warning **仍然不能拿它拼路径。** 它只保证"可安全展示"，不保证"适合落盘"
   *          —— 落盘名由框架自己生成（见 `path()`），与客户端给的名字**完全
   *          无关**。本函数的存在不是为了让客户端决定路径，这一点是本模块最
   *          重要的性质之一：清洗永远在"展示什么"这一侧，路径那一侧唯一的
   *          输入是框架自己的随机名。
   */
  const std::string& original_filename() const { return original_filename_; }

  /** @brief 该部件的 `Content-Type` 原始值（可空）。 */
  const std::string& content_type() const { return content_type_; }

  /**
   * @brief 真正**写进磁盘**的字节数。
   *
   * 语义是"落盘了多少"而不是"解析器交来多少"：短写会循环写完，所以正常结束
   * 时两者相等；而一旦写入失败，这个数如实反映盘上的实际长度（会话本身也会
   * 报失败，那份失败的文件会被删掉）。
   */
  uint64_t size() const { return size_; }

  /**
   * @brief 该部件是否因 `max_file_size` 被**截断**。
   *
   * 只有配了 `set_max_file_size()`（默认 0 = 不限）才可能为真。截断与其余几条
   * 上限**不同**：它不失败、不回 413，文件照样留给 handler（`size()` 是截断后的
   * 真实长度），由 handler 按业务判断"要不要收这半截"。所以这条上限是**用户
   * 显式要求的**，默认配置下永远不会触发。
   */
  bool truncated() const { return truncated_; }

 private:
  friend class uvcpp_web_upload;

  std::string path_;
  std::string field_name_;
  std::string original_filename_;
  std::string content_type_;
  uint64_t size_;
  bool truncated_;
};

/** @brief 一个普通字段部件（没有 `filename=`）的值。 */
class UVCPP_API uvcpp_web_upload_field {
 public:
  uvcpp_web_upload_field();

  const std::string& name() const { return name_; }
  const std::string& value() const { return value_; }

 private:
  friend class uvcpp_web_upload;

  std::string name_;
  std::string value_;
};

/** @brief 一次上传的产出：落盘的文件 + 内存里的字段。 */
class UVCPP_API uvcpp_web_upload_result {
 public:
  uvcpp_web_upload_result();

  /** @brief 按报文里的**出现顺序**排列（= 按落盘完成的顺序）。 */
  const std::vector<uvcpp_web_upload_file>& files() const { return files_; }
  const std::vector<uvcpp_web_upload_field>& fields() const { return fields_; }

  /** @return 第一个 `field_name() == field` 的文件；没有则 nullptr。 */
  const uvcpp_web_upload_file* file(const std::string& field) const;

  /** @return 第一个 `name() == name` 的字段；没有则 nullptr。 */
  const std::string* field(const std::string& name) const;

  void clear();

 private:
  friend class uvcpp_web_upload;

  std::vector<uvcpp_web_upload_file> files_;
  std::vector<uvcpp_web_upload_field> fields_;
};

// =========================================================================
// 会话
// =========================================================================

/**
 * @brief 会话与"流"之间的**流控接口**（由接线方注入）。
 *
 * 为什么是一组回调而不是直接持有 `uvcpp_web_stream*`：本模块要能被**脱开
 * 服务器**单独测（`web_app_upload_func.cpp` 就是这么喂字节的），而
 * `uvcpp_web_stream` 只在真连接上存在。三个回调都由接线方绑到流的同名方法上，
 * 于是"背压"这件事在本模块里只是一次函数调用 —— 测的时候塞一个计数器进去，
 * 就能确凿地观测到"pause 真的被调用了"。
 *
 * 三个回调**都可以为空**：
 * - `pause`/`resume` 为空 ⇒ 没有背压，`incoming` 只靠 `set_max_pending_bytes()`
 *   兜底（脱开服务器的用法，或者调用方自己就是消费者）；
 * - `abort` 为空 ⇒ 越界时就地按失败收场，不对外发响应。
 *
 * @warning 三个回调都在 **loop 线程、且在本会话的某个栈上**被同步调用。
 *          `abort` 尤其要注意：它会把控制流带回本会话（流层的 `on_abort` →
 *          `notify_parse_done(false)` → `fail()`），所以**调用之后不得再碰
 *          任何成员**。
 */
struct uvcpp_web_upload_flow {
  /// 请求流层**停止读**（把压力退回给对端）。不会立刻生效：当前已经在解析器
  /// 缓冲里的字节还会继续交付完 —— `set_max_pending_bytes()` 就是为那个窗口准备的。
  ::std::function<void()> pause;
  /// 请求流层**恢复读**。与 `pause()` 成对，只在跳变时下发（不是每块一次）。
  ::std::function<void()> resume;
  /// 回一个 HTTP 状态码并**掐断连接**（剩下的 body 没人要了）。
  ::std::function<void(int)> abort;
};

/**
 * @brief 一次上传的落盘会话：实现 `uvcpp_web_multipart_sink`。
 *
 * 用法（接线在步骤 4 的 `upload_route()` 里，这里只是形状）：
 *
 * ```cpp
 * std::shared_ptr<uvcpp_web_upload> up =
 *     uvcpp_web_upload::create(loop, "./uploads");
 * up->set_done_callback([up](const uvcpp_web_upload_result& r, bool ok,
 *                            const std::string& err) { ... });
 * multipart.set_sink(up.get());
 * // 每收到一块 body：multipart.feed(data, len);
 * // body 收完：multipart.finish(); up->notify_parse_done(finish_ok);
 * ```
 */
class UVCPP_API uvcpp_web_upload
    : public uvcpp_web_multipart_sink,
      public std::enable_shared_from_this<uvcpp_web_upload> {
 public:
  /**
   * @brief 完成回调：`(结果, 是否成功, 失败原因)`。
   *
   * **恰好被调用一次**（成功或失败都调）。成功时 `result()` 里的文件归调用方；
   * 失败时 `result` 是空的，本次创建的文件已经全部删掉。
   *
   * 回调在 loop 线程上跑，且**在本类的某个异步完成回调栈里** —— 回调里可以
   * 直接放弃自己那份 `shared_ptr`（会话会活到栈退完）。
   */
  typedef std::function<void(const uvcpp_web_upload_result&, bool,
                             const std::string&)>
      done_callback;

  /**
   * @brief 建一个会话。
   * @param loop       事件循环（必须比会话活得久）。
   * @param upload_dir 落盘目录。**必须在调用前已存在** —— 不存在时 `open`
   *                   会失败，整体按 500 收场（配置错，不是请求错）。
   * @return 目录为空时返回 `nullptr`；其余失败**不在创建时**暴露（`open` 的
   *         失败经完成回调上报，所以创建成功不等于目录可用）。
   *
   * 本重载会在**构造时解析一次** `upload_dir` 的真实路径，用作落盘路径包含
   * 判断的基准。那是一次**有界**的元数据操作（与静态服务启动时解析文档根
   * 同一量级，不是密集 IO），但它是同步的、跑在 loop 线程上 —— 所以生产
   * 路径应当用下面的三重载，把解析提前到配置期，每请求零开销。
   */
  static std::shared_ptr<uvcpp_web_upload> create(uvcpp_loop* loop,
                                                  const std::string& upload_dir);

  /**
   * @brief 同上，但调用方**已经**把目录的真实路径解析好了（生产路径用）。
   *
   * @param upload_dir_real `upload_dir` 的真实路径。**由调用方保证正确**：它
   *        既是包含判断的基准，也是 `open()` 与 `path()` 用的那串字符 ——
   *        三者共用一个字符串，"判断通过"才真正等于"落点受控"。传空则退化成
   *        用 `upload_dir`（两者相等时判断仍成立，只是少了展开符号链接那一层）。
   *
   * @return **两者都为空时返回 `nullptr`**。这是一条刻意提前的失败：落点为空
   *         会让包含判断对**每一个部件**都为假，于是配置错误会伪装成
   *         "落盘名生成器返回了不安全的叶子名"—— 指向生成器，而问题在配置。
   */
  static std::shared_ptr<uvcpp_web_upload> create(
      uvcpp_loop* loop, const std::string& upload_dir,
      const std::string& upload_dir_real);

  virtual ~uvcpp_web_upload();

  /** @brief 每个文件写完是否 fsync。默认开 —— 关掉更快，但掉电时会丢最近的数据。 */
  void set_fsync(bool on) { fsync_ = on; }
  bool fsync_enabled() const { return fsync_; }

  void set_done_callback(done_callback cb) { done_cb_ = cb; }

  /**
   * @brief 注入流控通道（`pause`/`resume`/`abort`）。见 `uvcpp_web_upload_flow`。
   *
   * 没注入也能用：会话照常落盘，只是没有背压 —— 只有 `set_max_pending_bytes()`
   * 这一道兜底。**这在真服务器上是不能接受的**（等于把 DoS 面敞开），所以
   * `uvcpp_web_app::wire_upload()` 一定会注入。
   */
  void set_flow(const uvcpp_web_upload_flow& f) { flow_ = f; }

  /**
   * @brief 落盘**叶子名**的生成器（不含目录）。默认见 `default_leaf_name()`。
   *
   * 入参是**已清洗**的客户端文件名（`web_sanitize_filename()` 的产物，可能为空
   * 或退化值），返回一个不含分隔符的叶子名。
   *
   * 这个接缝存在的**唯一理由**是让 `O_EXCL` 这件事可测：不注入就没法让两次
   * `open` 撞同一个名字，而"撞名之后 `O_EXCL` 让 `open` 失败并重摇，而不是
   * 顺着一个预先种好的文件/符号链接写下去"是本模块最重要的一条安全性质。
   * 用例注入一个"第一次返回预置名、第二次返回新名"的生成器即可钉死它。
   *
   * @warning 返回值**必须是叶子**。会话每次都查（含 `/`、`\`、`:`、空、`.`、
   *          `..` 一律按失败处理），拼出的最终路径再过一次
   *          `web_is_within_root()`。两道都在，是因为这一条是安全性质，
   *          不该只有一个判据 —— 但**不要指望会话替你兜底**，契约是你的。
   */
  typedef std::function<std::string(const std::string& sanitized_original)>
      name_generator;
  void set_name_generator(name_generator fn) { name_gen_ = fn; }

  /**
   * @brief 在途写期间**允许攒下**的字节上限，默认 1 MiB。**0 = 不限**。
   *
   * 为什么需要它：`pause()` 不是瞬时的，暂停之前已经在解析器缓冲里的字节还会
   * 继续交付；而对端也完全可以无视背压继续灌。没有上限的话，"暂停了还在灌"
   * 这条路径会无界地吃内存 —— 那正是本模块存在的意义所在（不把文件攒进内存）
   * 被绕开的形状。
   *
   * 越界时的行为：`abort(413)` 掐断连接（没有 `abort` 回调时就地按失败收场）。
   * 这是**不该发生**的分支 —— 正常的背压会让对端先停下来 —— 但必须有确定的
   * 行为，不能是"继续涨"。
   *
   * @note 上界是**软**的：单块交付的字节数（一次 TCP read 的量级）不会被切开，
   *       所以峰值可能是 `max_pending + 一块`。真正的上界由"一个写在途 + 一块
   *       新到"决定，与上传总长无关。
   */
  void set_max_pending_bytes(size_t n) { max_pending_ = n; }
  size_t max_pending_bytes() const { return max_pending_; }

  /**
   * @brief 接上工作池在途上限：**每次 fs 操作**提交前取名额、完成时归还。
   *
   * 为什么是"每次操作"而不是"整个会话占一个"：会话大部分时间在**等网络**，
   * 并不占线程池 —— 让一个慢上传长期扣着一个名额，就等于把小池子（默认 4 线程）
   * 白白锁死。
   *
   * 拿不到名额时**不拒绝、不排队**：`pause()` 把压力退回去，并向 `work_limit`
   * 注册一个唤醒回调（`uvcpp_web_work_limit::add_wakeup()`）。**那个唤醒是
   * 必需的** —— `acquire()`/`release()` 本身没有任何等待队列，没有唤醒就是
   * 永久卡死（会话永远停在这一步，连接既不关也不回）。
   *
   * @param lim 上限对象；**必须比会话活得久**（典型是 App 的成员）。传 `nullptr`
   *            表示不参与限流（默认）。
   */
  void set_work_limit(uvcpp_web_work_limit* lim) { work_limit_ = lim; }

  // -----------------------------------------------------------------------
  // 背压观测（用例与运维都靠它）
  // -----------------------------------------------------------------------

  /** @brief 当前是否**要求**流层暂停中（`pause()` 调过、`resume()` 还没调）。 */
  bool flow_paused() const { return flow_paused_; }
  /** @brief 一共下发过几次 `pause()`（跳变为"要暂停"时才算一次）。 */
  size_t pause_events() const { return pause_events_; }
  /** @brief 一共因为"攒得太满"掐断过几次（正常恒为 0）。 */
  size_t overflow_events() const { return overflow_events_; }
  /** @brief 当前是否在等一个工作池名额。 */
  bool waiting_for_slot() const { return waiting_slot_; }
  /** @brief 在途写缓冲的当前字节数（`pending` + `incoming`，全部 job 合计）。 */
  size_t buffered_bytes() const;

  // -----------------------------------------------------------------------
  // uvcpp_web_multipart_sink
  // -----------------------------------------------------------------------

  virtual bool on_part_begin(const uvcpp_web_part_info& info);
  virtual bool on_part_data(const char* data, size_t len);
  virtual void on_part_end(bool truncated);

  /**
   * @brief 解析器已经吃完整个 body。
   *
   * @param ok `uvcpp_web_multipart::finish()` 是否返回 `DONE`。传 `false` 表示
   *           报文被截断或解析失败 —— 此时按失败处理：本次创建的文件全删。
   *
   * 为什么由调用方喊这一声、而不是给 sink 接口加第四个回调：`uvcpp_web_multipart`
   * 的 sink 接口是**已经验收过的**三回调形状（`on_part_begin/data/end`），
   * 而"整个报文结束了"是**流**这一层的事实（`uvcpp_web_stream::on_end`），
   * 不该让纯解析器去替它表态。
   */
  void notify_parse_done(bool ok);

  // -----------------------------------------------------------------------
  // 读状态
  // -----------------------------------------------------------------------

  const uvcpp_web_upload_result& result() const { return result_; }
  bool done() const { return done_; }
  /** @brief 失败原因；成功时为**空串**。 */
  const std::string& error() const { return error_; }
  /**
   * @brief 交给 sink 的**部件 body** 字节总数。
   *
   * 不含前导、边界行与部件头 —— 它统计的是 `on_part_data` 收到的量，也就是
   * "上传的内容有多大"。报文整体的长度属于流层（`uvcpp_web_stream::received()`）。
   */
  uint64_t received_bytes() const { return received_; }
  /** @brief 当前仍打开着的文件数（用例拿它钉"不泄漏 fd"）。 */
  size_t open_file_count() const;
  /** @brief 已认识的文件部件数（含尚未写完的）。 */
  size_t file_count() const { return jobs_.size(); }

 private:
  /// @param dir_real **已解析**的落盘目录，非空（`create()` 已经把这条守住）。
  ///
  /// 构造函数只收真实路径、不收调用方写的那串字符：会话内部每一处拼路径都用
  /// 它，多存一份原始串只会让人误以为"两个基准随便用哪个都行"—— 而那正是
  /// 本步第一次跑用例时踩到的假失败（见 `.cpp` 的 `resolve_leaf()`）。
  uvcpp_web_upload(uvcpp_loop* loop, const std::string& dir_real);
  uvcpp_web_upload(const uvcpp_web_upload&);
  uvcpp_web_upload& operator=(const uvcpp_web_upload&);

  /// 无任务的哨兵（`current_` 用：字段部件或不收字节的状态）。
  static size_t no_job() { return static_cast<size_t>(-1); }

  /// 一个文件部件的全部状态（元数据 + 写盘进度）。
  struct upload_job {
    uvcpp_web_upload_file meta;
    /// 打开成功后的 fd；`fd_valid` 为假时无意义。
    ///
    /// 存 `int` 而不是 `uv_file`，是为了让本头不依赖 `<uv.h>` —— 而这是安全的：
    /// libuv 在 `uv/unix.h:125` 与 `uv/win.h:231` 两处都是 `typedef int uv_file;`
    /// （本仓 pin 的是 libuv 1.51）。.cpp 里做一次 `static_cast<uv_file>`。
    int fd;
    std::string pending;      ///< 正在写 / 待写的字节（在途时**不可改**）
    std::string incoming;     ///< 在途写期间新到的字节（单槽双缓冲）
    /// fd **有效**（open 成功过），而不是"open 提交过"。
    ///
    /// 这个区分是必须的：open 失败时若也算"开过了"，清理路径会去 `close(-1)`
    /// 并 `unlink` 一个根本没被创建的文件。语义定成"fd 有效"之后，失败的 job
    /// 在丢弃路径上直接出队，一个多余的 syscall 都不发。
    bool fd_valid;
    bool end_seen;            ///< 解析器已宣告该部件结束
    bool closed;              ///< fsync（可选）+ close 都跑完了
    bool removed;             ///< 失败路径：文件已删
    bool kept;                ///< 成功路径：文件已交给调用方，不许再删
    bool failed;
    uint64_t written;         ///< 已确认写入磁盘的字节数
    /// 已经因为 `UV_EEXIST` 重摇过几次名字。见 `kOpenRetries`。
    ///
    /// 注意它**不置 `failed`**：`pump_step()` 有一条防御性的
    /// `if (j.failed) { queue_.pop_front(); return true; }`（挡住"open 失败 →
    /// 重试 → 再失败"的死循环），而重摇之后 `fd_valid` 仍为假，正好让
    /// `pump_step()` 走"再提交一次 open"那条正常分支。
    size_t open_attempts;
    upload_job();
  };

  /// 换一个落盘名（`O_EXCL` 撞名时重摇）。`false` = 生成器给了非法叶子。
  ///
  /// **声明必须在 `upload_job` 之后**：类体里的名字查找是**顺序**的，成员函数
  /// 声明里的参数类型看不到后面才声明的嵌套类（写成前面那句会得到
  /// `error C2061: 语法错误: 标识符"upload_job"`，而报错位置指向声明本身，
  /// 看起来完全不像"顺序问题"）。
  bool reroll_path(upload_job& j);

  /**
   * @brief 把一个**候选叶子名**算成落盘全路径；不安全则返回 `false`。
   *
   * 两道判据（叶子形状 + 目录包含）合在一个地方，是因为它们必须成对出现而
   * 有**两个**调用点（`on_part_begin()` 与 `reroll_path()`）—— 分开写就是
   * 两处各写一遍，迟早漂移成一处强一处弱。
   */
  bool resolve_leaf(const std::string& leaf, std::string& out) const;

  // 串行调度：一次只允许一笔 fs 操作在途（`uvcpp_fs` 的单笔在途守卫要求）。
  //
  // `pump()` = `while (pump_step()) {}` + `sync_flow()`。拆出 `pump_step()` 只是
  // 为了**让背压的判定有唯一出口**：`pump()` 里有八九个 `return`，在每个 `return`
  // 前面各写一次 `sync_flow()` 迟早会漏掉一个（而漏掉的表现是"流层永远停在被
  // 暂停的状态"，是一次安静的吞吐归零）。
  void pump();
  bool pump_step();
  void submit_open(size_t idx);
  void submit_write(size_t idx);
  void submit_fsync(size_t idx);
  void submit_close(size_t idx);
  void submit_unlink(size_t idx);

  void on_open_done(size_t idx, long long rc);
  void on_write_done(size_t idx, long long rc);
  void on_fsync_done(size_t idx, long long rc);
  void on_close_done(size_t idx, long long rc);
  void on_unlink_done(size_t idx, long long rc);

  void fail(const std::string& what);
  void complete_if_ready();
  void invoke_done_callback();

  // -----------------------------------------------------------------------
  // 背压与工作池名额
  // -----------------------------------------------------------------------

  /**
   * @brief 现在需不需要请流层停下来。
   *
   * 判据是"**还有字节没写出去**"这个事实本身，而不是"刚才有一块很大的数据"：
   * - 有 fs 操作在途（含 open/fsync/close）⇒ 写不动，停；
   * - 任何 job 的 `pending`/`incoming` 非空 ⇒ 攒着没写，停；
   * - 正在等名额 ⇒ 停。
   */
  bool backpressure_needed() const;

  /// 把 `backpressure_needed()` 的**跳变**翻译成一次 `pause()`/`resume()`。
  void sync_flow();

  /// 取一个工作池名额；拿不到就注册唤醒并返回 false（调用方**不要**提交）。
  bool acquire_slot();
  /// 归还名额（幂等；没持有时空操作）。
  void release_slot();
  /// 注销等待中的唤醒（失败/析构路径：不能把"等人来叫我"留在表里）。
  void clear_wakeup();
  /// 名额变松的回调：重新试着推进。
  void on_slot_available();

  uvcpp_loop* loop_;
  /// 落盘目录的**真实路径**：拼路径与包含判断（第 3 道防线）共用的**唯一**基准。
  ///
  /// 它同时是 `open()` 真正要用的路径 —— 这是有意的：判断用的路径与实际落盘的
  /// 路径必须是同一个字符串，否则"判断通过"证明不了"落点受控"。
  std::string upload_dir_real_;
  std::unique_ptr<uvcpp_fs> fs_;
  name_generator name_gen_;

  std::deque<upload_job> jobs_;
  std::deque<size_t> queue_;  ///< 待处理的 job 下标，按部件顺序
  size_t current_;            ///< 当前正在收字节的 job；`no_job()` = 字段/无

  std::string field_name_;
  std::string field_value_;

  uvcpp_web_upload_result result_;
  done_callback done_cb_;
  std::string error_;

  uint64_t received_;
  bool busy_;         ///< 有一笔 fs 操作在途
  bool parse_done_;   ///< 调用方已喊过 `notify_parse_done`
  bool discarding_;   ///< 失败/中止：只关 fd + 删文件，不再写
  bool done_;         ///< 完成回调已经发过了
  bool fsync_;

  uvcpp_web_upload_flow flow_;
  size_t max_pending_;   ///< `incoming` 上限；0 = 不限
  uvcpp_web_work_limit* work_limit_;
  size_t wakeup_id_;     ///< 在 `work_limit_` 上的注册 id；`INVALID_WAKEUP` = 没注册
  bool slot_held_;       ///< 当前这一笔 fs 操作占了名额（完成时归还）
  bool waiting_slot_;    ///< 正等名额（已经注册了唤醒）
  bool flow_paused_;     ///< 当前**要求**流层暂停中（跳变才下发）
  size_t pause_events_;
  size_t overflow_events_;
};

}  // namespace uvcpp

#endif  // SRC_WEBAPP_UVCPP_WEB_UPLOAD_H

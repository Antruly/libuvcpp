/**
 * @file src/webapp/uvcpp_web_connection.cpp
 * @brief uvcpp_web_connection_registry 的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include <webapp/uvcpp_web_connection.h>

#include <net/uvcpp_tcp_client.h>

namespace uvcpp {

uvcpp_web_connection::uvcpp_web_connection()
    : id(UVCPP_WEB_INVALID_CONN_ID),
      client(nullptr),
      peer_port(0),
      last_read_ms(0),
      request_start_ms(0) {}

uvcpp_web_connection_registry::uvcpp_web_connection_registry()
    // 从 1 开始：0 要留给 UVCPP_WEB_INVALID_CONN_ID，不然"没登记过"和
    // "第 0 条连接"就分不开了。
    : next_id_(1) {}

uvcpp_web_connection_registry::~uvcpp_web_connection_registry() {}

uvcpp_web_conn_id uvcpp_web_connection_registry::add(uvcpp_tcp_client* client,
                                                    const std::string& peer_ip,
                                                    int peer_port,
                                                    int64_t now_ms) {
  // 同一个 client 重复登记：先把旧记录摘掉。不清的话 by_client_ 会指着一条
  // 已经不在 by_id_ 里的 id，之后 remove_by_client 就会摘掉一条不存在的记录
  // 并把新记录留在表里 —— 那才是最坏的结果（新连接永远不被回收）。
  if (client != nullptr) {
    std::map<uvcpp_tcp_client*, uvcpp_web_conn_id>::iterator it =
        by_client_.find(client);
    if (it != by_client_.end()) {
      by_id_.erase(it->second);
      by_client_.erase(it);
    }
  }

  // **先自增再赋值**：保证 id 单调且永远不等于 next_id_ 之后的任何值，
  // issued() 的 `id < next_id_` 判据才成立。
  const uvcpp_web_conn_id id = next_id_++;

  uvcpp_web_connection conn;
  conn.id        = id;
  conn.client    = client;
  conn.peer_ip   = peer_ip;
  conn.peer_port = peer_port;
  // 建立时刻就是第一次活动。给 0 表示"调用方没提供"，那就留成未知 ——
  // 扫描器会给它补基准（见 `touch()`），而不是拿 0 当"很久以前"直接杀掉。
  conn.last_read_ms    = now_ms > 0 ? now_ms : 0;
  conn.request_start_ms = 0;

  by_id_[id] = conn;
  if (client != nullptr) {
    by_client_[client] = id;
  }
  return id;
}

bool uvcpp_web_connection_registry::note_read(uvcpp_web_conn_id id,
                                             int64_t now_ms) {
  std::map<uvcpp_web_conn_id, uvcpp_web_connection>::iterator it =
      by_id_.find(id);
  if (it == by_id_.end()) return false;

  it->second.last_read_ms = now_ms;
  // 没有半截请求 ⇒ 这一块字节开了一个新的。有半截请求就**不动** ——
  // 那正是慢速滴字节要被抓住的地方：预算从第一个字节起算，不随字节刷新。
  if (it->second.request_start_ms == 0) {
    it->second.request_start_ms = now_ms;
  }
  return true;
}

bool uvcpp_web_connection_registry::note_request_done(uvcpp_web_conn_id id) {
  std::map<uvcpp_web_conn_id, uvcpp_web_connection>::iterator it =
      by_id_.find(id);
  if (it == by_id_.end()) return false;

  it->second.request_start_ms = 0;
  return true;
}

int64_t uvcpp_web_connection_registry::activity_since(
    uvcpp_web_conn_id id) const {
  std::map<uvcpp_web_conn_id, uvcpp_web_connection>::const_iterator it =
      by_id_.find(id);
  if (it == by_id_.end()) return 0;

  if (it->second.request_start_ms != 0) return it->second.request_start_ms;
  return it->second.last_read_ms;
}

bool uvcpp_web_connection_registry::touch(uvcpp_web_conn_id id,
                                         int64_t now_ms) {
  std::map<uvcpp_web_conn_id, uvcpp_web_connection>::iterator it =
      by_id_.find(id);
  if (it == by_id_.end()) return false;

  // 只在"还不知道"时写：已经有基准的连接不该被每拍扫描刷新，否则闲置
  // 超时永远不会触发 —— 那就成了"扫描器自己让所有连接保持活跃"。
  if (it->second.last_read_ms == 0) it->second.last_read_ms = now_ms;
  return true;
}

bool uvcpp_web_connection_registry::remove(uvcpp_web_conn_id id) {
  std::map<uvcpp_web_conn_id, uvcpp_web_connection>::iterator it =
      by_id_.find(id);
  if (it == by_id_.end()) return false;

  if (it->second.client != nullptr) {
    // 只在反向索引确实指着这一条时才擦 —— 重复登记可能让同一个 client 的
    // 反向索引指向**新的**那个 id，那时擦掉它就等于把新连接弄丢了。
    std::map<uvcpp_tcp_client*, uvcpp_web_conn_id>::iterator rit =
        by_client_.find(it->second.client);
    if (rit != by_client_.end() && rit->second == id) {
      by_client_.erase(rit);
    }
  }

  by_id_.erase(it);
  return true;
}

bool uvcpp_web_connection_registry::remove_by_client(
    uvcpp_tcp_client* client, uvcpp_web_conn_id* out_id) {
  if (client == nullptr) return false;

  std::map<uvcpp_tcp_client*, uvcpp_web_conn_id>::iterator it =
      by_client_.find(client);
  if (it == by_client_.end()) return false;

  const uvcpp_web_conn_id id = it->second;
  by_id_.erase(id);
  by_client_.erase(it);

  if (out_id != nullptr) *out_id = id;
  return true;
}

uvcpp_tcp_client* uvcpp_web_connection_registry::client(
    uvcpp_web_conn_id id) const {
  std::map<uvcpp_web_conn_id, uvcpp_web_connection>::const_iterator it =
      by_id_.find(id);
  if (it == by_id_.end()) return nullptr;
  return it->second.client;
}

uvcpp_web_conn_id uvcpp_web_connection_registry::id_of(
    uvcpp_tcp_client* client) const {
  if (client == nullptr) return UVCPP_WEB_INVALID_CONN_ID;
  std::map<uvcpp_tcp_client*, uvcpp_web_conn_id>::const_iterator it =
      by_client_.find(client);
  if (it == by_client_.end()) return UVCPP_WEB_INVALID_CONN_ID;
  return it->second;
}

bool uvcpp_web_connection_registry::alive(uvcpp_web_conn_id id) const {
  return by_id_.find(id) != by_id_.end();
}

bool uvcpp_web_connection_registry::issued(uvcpp_web_conn_id id) const {
  return id != UVCPP_WEB_INVALID_CONN_ID && id < next_id_;
}

const uvcpp_web_connection* uvcpp_web_connection_registry::find(
    uvcpp_web_conn_id id) const {
  std::map<uvcpp_web_conn_id, uvcpp_web_connection>::const_iterator it =
      by_id_.find(id);
  if (it == by_id_.end()) return nullptr;
  return &it->second;
}

size_t uvcpp_web_connection_registry::size() const { return by_id_.size(); }

std::vector<uvcpp_web_conn_id> uvcpp_web_connection_registry::ids() const {
  std::vector<uvcpp_web_conn_id> out;
  out.reserve(by_id_.size());
  // std::map 按 key 升序，所以直接遍历就是 id 升序。
  for (std::map<uvcpp_web_conn_id, uvcpp_web_connection>::const_iterator it =
           by_id_.begin();
       it != by_id_.end(); ++it) {
    out.push_back(it->first);
  }
  return out;
}

uint64_t uvcpp_web_connection_registry::issued_count() const {
  // next_id_ 指向下一个要发的号，所以已经发出去的数量是 next_id_ - 1。
  return next_id_ - 1;
}

void uvcpp_web_connection_registry::clear() {
  by_id_.clear();
  by_client_.clear();
  // **不动 next_id_** —— 这是"永不复用"的全部意义所在。清空登记表不等于
  // 允许旧 id 复活。
}

}  // namespace uvcpp

/**
 * @file src/webapp/uvcpp_ws_rooms.cpp
 * @brief 房间表与广播的实现。
 * @author zhuweiye
 * @version 1.0.0
 */

#include "webapp/uvcpp_ws_rooms.h"

#if UVCPP_WEB_ENABLE && UVCPP_WEB_WS_ENABLE

#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <web/uvcpp_ws_connection.h>
#include <web/uvcpp_ws_frame.h>

namespace uvcpp {

uvcpp_ws_broadcast_options::uvcpp_ws_broadcast_options()
    : max_queue_bytes(1024u * 1024u),
      policy(uvcpp_ws_slow_client::DROP) {}

struct uvcpp_ws_rooms::impl {
  std::map<std::string, std::vector<uvcpp_ws_connection*> > rooms;
  /// 连接 → 它加入过的房间（摘除时不必扫全表）。
  std::map<uvcpp_ws_connection*, std::set<std::string> > conns;
  /// 连接 → 终结观察者 id（析构时摘掉）。
  std::map<uvcpp_ws_connection*, int> obs_ids;
  uvcpp_ws_room_stats                 s;

  void on_closed(uvcpp_ws_connection* c) {
    // 终结路径上调用：只改自己的表，**不再碰会话**（它马上要被回收）。
    std::map<uvcpp_ws_connection*, std::set<std::string> >::iterator it =
        conns.find(c);
    if (it == conns.end()) return;
    const std::set<std::string> joined = it->second;
    for (std::set<std::string>::const_iterator r = joined.begin();
         r != joined.end(); ++r) {
      std::map<std::string, std::vector<uvcpp_ws_connection*> >::iterator rm =
          rooms.find(*r);
      if (rm == rooms.end()) continue;
      std::vector<uvcpp_ws_connection*>& v = rm->second;
      v.erase(std::remove(v.begin(), v.end(), c), v.end());
      if (v.empty()) rooms.erase(rm);
    }
    conns.erase(it);
    obs_ids.erase(c);
  }

  /// 会话已经没了（或正在关）：先摘观察者再清表。
  ///
  /// **摘观察者这一步不能省**：本函数还会在"连接还活着、只是发送失败/被策略
  /// 关掉"时调用。留着观察者的话，连接稍后终结会回调进本对象；如果那时本对象
  /// 已经析构，就是一次悬垂调用。
  void drop_conn(uvcpp_ws_connection* c) {
    if (c != nullptr) {
      std::map<uvcpp_ws_connection*, int>::iterator it = obs_ids.find(c);
      if (it != obs_ids.end()) c->remove_close_observer(it->second);
    }
    on_closed(c);
  }

  size_t broadcast(const std::string& room, const char* data, size_t len,
                   bool binary, const uvcpp_ws_broadcast_options& opts,
                   size_t* dropped_out) {
    ++s.broadcasts;
    size_t dropped = 0;
    if (dropped_out != nullptr) *dropped_out = 0;

    std::map<std::string, std::vector<uvcpp_ws_connection*> >::iterator it =
        rooms.find(room);
    if (it == rooms.end()) return 0;

    // 遍历副本：策略 CLOSE 会改表（`drop_conn`），而当场的发送**不会**同步
    // 跑用户回调（本类不传每条消息的 done），所以副本足够且最省心。
    const std::vector<uvcpp_ws_connection*> members = it->second;
    size_t delivered = 0;
    for (size_t i = 0; i < members.size(); ++i) {
      uvcpp_ws_connection* c = members[i];
      if (c == nullptr || !c->is_open()) {
        ++dropped;
        drop_conn(c);
        continue;
      }
      if (opts.max_queue_bytes > 0 &&
          c->pending_send_bytes() > opts.max_queue_bytes) {
        if (opts.policy == uvcpp_ws_slow_client::CLOSE) {
          ++s.closed_slow;
          c->close(ws_close_code::GOING_AWAY, "slow client");
          drop_conn(c);
        }
        ++dropped;
        continue;
      }
      const int rc = binary ? c->send_binary(data, len)
                            : c->send_text(data, len);
      if (rc != 0) {
        ++dropped;
        drop_conn(c);
        continue;
      }
      ++delivered;
    }
    s.delivered += delivered;
    s.dropped += dropped;
    if (dropped_out != nullptr) *dropped_out = dropped;
    return delivered;
  }
};

uvcpp_ws_rooms::uvcpp_ws_rooms() : impl_(new impl()) {}

uvcpp_ws_rooms::~uvcpp_ws_rooms() {
  // 摘掉自己挂的观察者：本对象先于连接析构时，连接终结时不该再回调到这里。
  for (std::map<uvcpp_ws_connection*, int>::iterator it =
           impl_->obs_ids.begin();
       it != impl_->obs_ids.end(); ++it) {
    if (it->first != nullptr) it->first->remove_close_observer(it->second);
  }
  impl_->obs_ids.clear();
  impl_->conns.clear();
  impl_->rooms.clear();
}

int uvcpp_ws_rooms::join(uvcpp_ws_connection* c, const std::string& room) {
  if (c == nullptr || room.empty()) return UV_EINVAL;
  std::set<std::string>& joined = impl_->conns[c];
  if (joined.find(room) != joined.end()) return 0;  // 幂等
  joined.insert(room);
  std::vector<uvcpp_ws_connection*>& v = impl_->rooms[room];
  if (std::find(v.begin(), v.end(), c) == v.end()) v.push_back(c);

  if (impl_->obs_ids.find(c) == impl_->obs_ids.end()) {
    // 终结观察者必须**每条连接只挂一个**：上面用 `conns[c]` 的默认构造建立
    // 了条目，观察者按"首次 join"决定。
    impl_->obs_ids[c] = c->add_close_observer([this, c] { impl_->on_closed(c); });
  }
  return 0;
}

int uvcpp_ws_rooms::leave(uvcpp_ws_connection* c, const std::string& room) {
  if (c == nullptr) return UV_EINVAL;
  std::map<uvcpp_ws_connection*, std::set<std::string> >::iterator it =
      impl_->conns.find(c);
  if (it == impl_->conns.end()) return 0;
  if (it->second.erase(room) == 0) return 0;

  std::map<std::string, std::vector<uvcpp_ws_connection*> >::iterator rm =
      impl_->rooms.find(room);
  if (rm != impl_->rooms.end()) {
    std::vector<uvcpp_ws_connection*>& v = rm->second;
    v.erase(std::remove(v.begin(), v.end(), c), v.end());
    if (v.empty()) impl_->rooms.erase(rm);
  }
  if (it->second.empty()) {
    std::map<uvcpp_ws_connection*, int>::iterator o = impl_->obs_ids.find(c);
    if (o != impl_->obs_ids.end()) {
      c->remove_close_observer(o->second);
      impl_->obs_ids.erase(o);
    }
    impl_->conns.erase(it);
  }
  return 0;
}

size_t uvcpp_ws_rooms::leave_all(uvcpp_ws_connection* c) {
  if (c == nullptr) return 0;
  std::map<uvcpp_ws_connection*, std::set<std::string> >::iterator it =
      impl_->conns.find(c);
  if (it == impl_->conns.end()) return 0;
  const size_t n = it->second.size();
  // 先摘观察者，再走通用清理（通用清理不再碰会话）。
  std::map<uvcpp_ws_connection*, int>::iterator o = impl_->obs_ids.find(c);
  if (o != impl_->obs_ids.end()) {
    c->remove_close_observer(o->second);
    impl_->obs_ids.erase(o);
  }
  impl_->on_closed(c);
  return n;
}

size_t uvcpp_ws_rooms::room_size(const std::string& room) const {
  std::map<std::string, std::vector<uvcpp_ws_connection*> >::const_iterator it =
      impl_->rooms.find(room);
  return it == impl_->rooms.end() ? 0 : it->second.size();
}

size_t uvcpp_ws_rooms::room_count() const { return impl_->rooms.size(); }

size_t uvcpp_ws_rooms::member_count() const { return impl_->conns.size(); }

size_t uvcpp_ws_rooms::broadcast_text(
    const std::string& room, const char* data, size_t len,
    const uvcpp_ws_broadcast_options& opts, size_t* dropped_out) {
  return impl_->broadcast(room, data, len, /*binary=*/false, opts, dropped_out);
}

size_t uvcpp_ws_rooms::broadcast_binary(
    const std::string& room, const void* data, size_t len,
    const uvcpp_ws_broadcast_options& opts, size_t* dropped_out) {
  return impl_->broadcast(room, static_cast<const char*>(data), len,
                          /*binary=*/true, opts, dropped_out);
}

size_t uvcpp_ws_rooms::broadcast_all_text(
    const char* data, size_t len, const uvcpp_ws_broadcast_options& opts,
    size_t* dropped_out) {
  // 全量广播按"每个房间各发一次"会重复投递给多房间成员；按**成员去重**发。
  ++impl_->s.broadcasts;
  size_t dropped = 0;
  if (dropped_out != nullptr) *dropped_out = 0;
  const std::map<uvcpp_ws_connection*, std::set<std::string> > snapshot =
      impl_->conns;
  size_t delivered = 0;
  for (std::map<uvcpp_ws_connection*, std::set<std::string> >::const_iterator
           it = snapshot.begin();
       it != snapshot.end(); ++it) {
    uvcpp_ws_connection* c = it->first;
    if (c == nullptr || !c->is_open()) {
      ++dropped;
      impl_->drop_conn(c);
      continue;
    }
    if (opts.max_queue_bytes > 0 &&
        c->pending_send_bytes() > opts.max_queue_bytes) {
      if (opts.policy == uvcpp_ws_slow_client::CLOSE) {
        ++impl_->s.closed_slow;
        c->close(ws_close_code::GOING_AWAY, "slow client");
        impl_->drop_conn(c);
      }
      ++dropped;
      continue;
    }
    if (c->send_text(data, len) != 0) {
      ++dropped;
      impl_->drop_conn(c);
      continue;
    }
    ++delivered;
  }
  impl_->s.delivered += delivered;
  impl_->s.dropped += dropped;
  if (dropped_out != nullptr) *dropped_out = dropped;
  return delivered;
}

uvcpp_ws_room_stats uvcpp_ws_rooms::stats() const {
  uvcpp_ws_room_stats out = impl_->s;
  out.rooms   = impl_->rooms.size();
  out.members = impl_->conns.size();
  return out;
}

}  // namespace uvcpp

#endif  // UVCPP_WEB_ENABLE && UVCPP_WEB_WS_ENABLE

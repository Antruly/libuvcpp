/**
 * @file src/req/uvcpp_fs.h
 * @brief Wrapper for libuv filesystem requests (uv_fs_t).
 * @author zhuweiye
 * @version 1.0.0
 *
 * Provides asynchronous filesystem operations (open/read/write/stat/etc.)
 * with C++ friendly callbacks.
 */

#pragma once
#ifndef SRC_REQ_UVCPP_FS_H
#define SRC_REQ_UVCPP_FS_H

#include <handle/uvcpp_loop.h>
#include <req/uvcpp_req.h>
#include <uvcpp/uvcpp_dir.h>
#include <uvcpp/uvcpp_dirent.h>

namespace uvcpp {
class UVCPP_API uvcpp_fs : public uvcpp_req {
 public:
  UVCPP_DEFINE_FUNC(uvcpp_fs)
  UVCPP_DEFINE_COPY_FUNC_DELETE(uvcpp_fs)
 
  int init();

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 19
  uv_fs_type get_type();
  ssize_t get_result();
  void* get_ptr();
  const char* get_path();
  uv_stat_t* get_statbuf();
#endif
#endif


#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 38
  int get_system_error();
#endif
#endif
 

  void req_cleanup();

  int close(uvcpp_loop* loop, uv_file file);

  int open(uvcpp_loop* loop, const char* path, int flags, int mode);

  int read(uvcpp_loop* loop,
           uv_file file,
           const uv_buf bufs[],
           unsigned int nbufs,
           int64_t offset);

  int unlink(uvcpp_loop* loop, const char* path);

  int write(uvcpp_loop* loop,
            uv_file file,
            const uv_buf bufs[],
            unsigned int nbufs,
            int64_t offset);

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 14
  int copyfile(uvcpp_loop* loop, const char* path, const char* new_path, int flags);
#endif
#endif
  int mkdir(uvcpp_loop* loop, const char* path, int mode);

  int mkdtemp(uvcpp_loop* loop, const char* tpl);

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 36
  int mkstemp(uvcpp_loop* loop, const char* tpl);
#endif
#endif

  int rmdir(uvcpp_loop* loop, const char* path);

  int scandir(uvcpp_loop* loop, const char* path, int flags);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 28
  int opendir(uvcpp_loop* loop, const char* path);

  int readdir(uvcpp_loop* loop, uvcpp_dir* dir);

  int closedir(uvcpp_loop* loop, uvcpp_dir* dir);
#endif
#endif


  int stat(uvcpp_loop* loop, const char* path);

  int fstat(uvcpp_loop* loop, uv_file file);

  int rename(uvcpp_loop* loop, const char* path, const char* new_path);

  int fsync(uvcpp_loop* loop, uv_file file);

  int fdatasync(uvcpp_loop* loop, uv_file file);

  int ftruncate(uvcpp_loop* loop, uv_file file, int64_t offset);

  int sendfile(uvcpp_loop* loop,
               uv_file out_fd,
               uv_file in_fd,
               int64_t in_offset,
               size_t length);

  int access(uvcpp_loop* loop, const char* path, int mode);

  int chmod(uvcpp_loop* loop, const char* path, int mode);

  int utime(uvcpp_loop* loop, const char* path, double atime, double mtime);

  int futime(uvcpp_loop* loop, uv_file file, double atime, double mtime);

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 36
int lutime(uvcpp_loop* loop, const char* path, double atime, double mtime);
#endif
#endif
  
  int lstat(uvcpp_loop* loop, const char* path);

  int link(uvcpp_loop* loop, const char* path, const char* new_path);

  int symlink(uvcpp_loop* loop, const char* path, const char* new_path, int flags);

  int readlink(uvcpp_loop* loop, const char* path);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 8
  int realpath(uvcpp_loop* loop, const char* path);
#endif
#endif

  int fchmod(uvcpp_loop* loop, uv_file file, int mode);
  int chown(uvcpp_loop* loop, const char* path, uv_uid_t uid, uv_gid_t gid);
  int fchown(uvcpp_loop* loop, uv_file file, uv_uid_t uid, uv_gid_t gid);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 21
  int lchown(uvcpp_loop* loop, const char* path, uv_uid_t uid, uv_gid_t gid);
#endif
#endif

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 30
  int statfs(uvcpp_loop* loop, const char* path);
#endif
#endif


  int close(uvcpp_loop* loop, uv_file file, ::std::function<void(uvcpp_fs*)> close_cb);

  int open(uvcpp_loop* loop,
           const char* path,
           int flags,
           int mode,
           ::std::function<void(uvcpp_fs*)> open_cb);

  int read(uvcpp_loop* loop,
           uv_file file,
           const uv_buf bufs[],
           unsigned int nbufs,
           int64_t offset,
           ::std::function<void(uvcpp_fs*)> read_cb);

  int unlink(uvcpp_loop* loop,
             const char* path,
             ::std::function<void(uvcpp_fs*)> unlink_cb);

  int write(uvcpp_loop* loop,
            uv_file file,
            const uv_buf bufs[],
            unsigned int nbufs,
            int64_t offset,
            ::std::function<void(uvcpp_fs*)> write_cb);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 14
  int copyfile(uvcpp_loop* loop,
               const char* path,
               const char* new_path,
               int flags,
               ::std::function<void(uvcpp_fs*)> copyfile_cb);

#endif
#endif


  int mkdir(uvcpp_loop* loop,
            const char* path,
            int mode,
            ::std::function<void(uvcpp_fs*)> mkdir_cb);

  int mkdtemp(uvcpp_loop* loop,
              const char* tpl,
              ::std::function<void(uvcpp_fs*)> mkdtemp_cb);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 34
  int mkstemp(uvcpp_loop* loop,
              const char* tpl,
              ::std::function<void(uvcpp_fs*)> mkstemp_cb);
#endif
#endif

  int rmdir(uvcpp_loop* loop, const char* path, ::std::function<void(uvcpp_fs*)> rmdir_cb);  
  int scandir(uvcpp_loop* loop,
              const char* path,
              int flags,
              ::std::function<void(uvcpp_fs*)> scandir_cb);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 28
  int opendir(uvcpp_loop* loop,
              const char* path,
              ::std::function<void(uvcpp_fs*)> opendir_cb);

  int readdir(uvcpp_loop* loop, uvcpp_dir* dir, ::std::function<void(uvcpp_fs*)> readdir_cb);

  int closedir(uvcpp_loop* loop, uvcpp_dir* dir, ::std::function<void(uvcpp_fs*)> closedir_cb);
#endif
#endif


  int stat(uvcpp_loop* loop, const char* path, ::std::function<void(uvcpp_fs*)> stat_cb);

  int fstat(uvcpp_loop* loop, uv_file file, ::std::function<void(uvcpp_fs*)> fstat_cb);

  int rename(uvcpp_loop* loop,
             const char* path,
             const char* new_path,
             ::std::function<void(uvcpp_fs*)> rename_cb);

  int fsync(uvcpp_loop* loop, uv_file file, ::std::function<void(uvcpp_fs*)> fsync_cb);

  int fdatasync(uvcpp_loop* loop,
                uv_file file,
                ::std::function<void(uvcpp_fs*)> fdatasync_cb);

  int ftruncate(uvcpp_loop* loop,
                uv_file file,
                int64_t offset,
                ::std::function<void(uvcpp_fs*)> ftruncate_cb);

  int sendfile(uvcpp_loop* loop,
               uv_file out_fd,
               uv_file in_fd,
               int64_t in_offset,
               size_t length,
               ::std::function<void(uvcpp_fs*)> sendfile_cb);

  int access(uvcpp_loop* loop,
             const char* path,
             int mode,
             ::std::function<void(uvcpp_fs*)> access_cb);

  int chmod(uvcpp_loop* loop,
            const char* path,
            int mode,
            ::std::function<void(uvcpp_fs*)> chmod_cb);

  int utime(uvcpp_loop* loop,
            const char* path,
            double atime,
            double mtime,
            ::std::function<void(uvcpp_fs*)> utime_cb);

  int futime(uvcpp_loop* loop,
             uv_file file,
             double atime,
             double mtime,
             ::std::function<void(uvcpp_fs*)> futime_cb);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 34
  int lutime(uvcpp_loop* loop,
             const char* path,
             double atime,
             double mtime,
             ::std::function<void(uvcpp_fs*)> lutime_cb);
#endif
#endif


  int lstat(uvcpp_loop* loop, const char* path, ::std::function<void(uvcpp_fs*)> lstat_cb);

  int link(uvcpp_loop* loop,
           const char* path,
           const char* new_path,
           ::std::function<void(uvcpp_fs*)> link_cb);

  int symlink(uvcpp_loop* loop,
              const char* path,
              const char* new_path,
              int flags,
              ::std::function<void(uvcpp_fs*)> symlink_cb);

  int readlink(uvcpp_loop* loop,
               const char* path,
               ::std::function<void(uvcpp_fs*)> readlink_cb);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 8
  int realpath(uvcpp_loop* loop,
               const char* path,
               ::std::function<void(uvcpp_fs*)> realpath_cb);
#endif
#endif


  int fchmod(uvcpp_loop* loop,
             uv_file file,
             int mode,
             ::std::function<void(uvcpp_fs*)> fchmod_cb);

  int chown(uvcpp_loop* loop,
            const char* path,
            uv_uid_t uid,
            uv_gid_t gid,
            ::std::function<void(uvcpp_fs*)> chown_cb);

  int fchown(uvcpp_loop* loop,
             uv_file file,
             uv_uid_t uid,
             uv_gid_t gid,
             ::std::function<void(uvcpp_fs*)> fchown_cb);
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 21
  int lchown(uvcpp_loop* loop,
             const char* path,
             uv_uid_t uid,
             uv_gid_t gid,
             ::std::function<void(uvcpp_fs*)> lchown_cb);
#endif
#endif

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 30
  int statfs(uvcpp_loop* loop,
             const char* path,
             ::std::function<void(uvcpp_fs*)> statfs_cb);
#endif
#endif

  int scandir_next(uv_dirent_t* ent);
 protected:
  ::std::function<void(uvcpp_fs*)> fs_close_cb;
  ::std::function<void(uvcpp_fs*)> fs_open_cb;
  ::std::function<void(uvcpp_fs*)> fs_read_cb;
  ::std::function<void(uvcpp_fs*)> fs_unlink_cb;
  ::std::function<void(uvcpp_fs*)> fs_write_cb;
  ::std::function<void(uvcpp_fs*)> fs_copyfile_cb;
  ::std::function<void(uvcpp_fs*)> fs_mkdir_cb;
  ::std::function<void(uvcpp_fs*)> fs_mkdtemp_cb;
  ::std::function<void(uvcpp_fs*)> fs_mkstemp_cb;
  ::std::function<void(uvcpp_fs*)> fs_rmdir_cb;
  ::std::function<void(uvcpp_fs*)> fs_scandir_cb;
  ::std::function<void(uvcpp_fs*)> fs_opendir_cb;
  ::std::function<void(uvcpp_fs*)> fs_readdir_cb;
  ::std::function<void(uvcpp_fs*)> fs_closedir_cb;
  ::std::function<void(uvcpp_fs*)> fs_stat_cb;
  ::std::function<void(uvcpp_fs*)> fs_fstat_cb;
  ::std::function<void(uvcpp_fs*)> fs_rename_cb;
  ::std::function<void(uvcpp_fs*)> fs_fsync_cb;
  ::std::function<void(uvcpp_fs*)> fs_fdatasync_cb;
  ::std::function<void(uvcpp_fs*)> fs_ftruncate_cb;
  ::std::function<void(uvcpp_fs*)> fs_sendfile_cb;
  ::std::function<void(uvcpp_fs*)> fs_access_cb;
  ::std::function<void(uvcpp_fs*)> fs_chmod_cb;
  ::std::function<void(uvcpp_fs*)> fs_utime_cb;
  ::std::function<void(uvcpp_fs*)> fs_futime_cb;
  ::std::function<void(uvcpp_fs*)> fs_lutime_cb;
  ::std::function<void(uvcpp_fs*)> fs_lstat_cb;
  ::std::function<void(uvcpp_fs*)> fs_link_cb;
  ::std::function<void(uvcpp_fs*)> fs_symlink_cb;
  ::std::function<void(uvcpp_fs*)> fs_readlink_cb;
  ::std::function<void(uvcpp_fs*)> fs_realpath_cb;
  ::std::function<void(uvcpp_fs*)> fs_fchmod_cb;
  ::std::function<void(uvcpp_fs*)> fs_chown_cb;
  ::std::function<void(uvcpp_fs*)> fs_fchown_cb;
  ::std::function<void(uvcpp_fs*)> fs_lchown_cb;
  ::std::function<void(uvcpp_fs*)> fs_statfs_cb;

 private:
  static void callback_close(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_close_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_close_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }
  static void callback_open(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_open_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_open_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_read(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_read_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_read_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_unlink(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_unlink_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_unlink_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_write(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_write_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_write_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_copyfile(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_copyfile_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_copyfile_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_mkdir(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_mkdir_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_mkdir_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_mkdtemp(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_mkdtemp_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_mkdtemp_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_mkstemp(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_mkstemp_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_mkstemp_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_rmdir(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_rmdir_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_rmdir_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_scandir(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_scandir_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_scandir_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_opendir(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_opendir_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_opendir_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_readdir(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_readdir_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_readdir_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_closedir(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_closedir_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_closedir_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_stat(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_stat_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_stat_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_fstat(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_fstat_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_fstat_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_rename(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_rename_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_rename_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_fsync(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_fsync_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_fsync_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_fdatasync(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_fdatasync_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_fdatasync_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_ftruncate(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_ftruncate_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_ftruncate_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_sendfile(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_sendfile_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_sendfile_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_access(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_access_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_access_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_chmod(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_chmod_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_chmod_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_utime(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_utime_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_utime_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_futime(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_futime_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_futime_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_lutime(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_lutime_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_lutime_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_lstat(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_lstat_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_lstat_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_link(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_link_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_link_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_symlink(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_symlink_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_symlink_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_readlink(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_readlink_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_readlink_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_realpath(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_realpath_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_realpath_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_fchmod(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_fchmod_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_fchmod_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_chown(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_chown_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_chown_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_fchown(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_fchown_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_fchown_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_lchown(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_lchown_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_lchown_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

  static void callback_statfs(uv_fs_t* req) {
    if (reinterpret_cast<uvcpp_fs*>(req->data)->fs_statfs_cb)
      reinterpret_cast<uvcpp_fs*>(req->data)->fs_statfs_cb(
          reinterpret_cast<uvcpp_fs*>(req->data));
  }

 private:
};

} // namespace uvcpp

#endif // SRC_REQ_UVCPP_FS_H
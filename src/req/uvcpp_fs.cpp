#include "uvcpp_fs.h"
#include <uvcpp/uv_alloc.h>
namespace uvcpp {
uvcpp_fs::uvcpp_fs() : uvcpp_req() {
  uv_fs_t* req = uvcpp::uv_alloc<uv_fs_t>();
  this->set_req(req);
  this->init();
}
uvcpp_fs::~uvcpp_fs() {}

int uvcpp_fs::init() {
  memset(UVCPP_FS_REQ, 0, sizeof(uv_fs_t));
  this->set_req_data();
  return 0;
}

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 19
uv_fs_type uvcpp_fs::get_type() { return uv_fs_get_type(UVCPP_FS_REQ); }

ssize_t uvcpp_fs::get_result() { return uv_fs_get_result(UVCPP_FS_REQ); }

void* uvcpp_fs::get_ptr() { return uv_fs_get_ptr(UVCPP_FS_REQ); }

const char* uvcpp_fs::get_path() { return uv_fs_get_path(UVCPP_FS_REQ); }

uv_stat_t* uvcpp_fs::get_statbuf() { return uv_fs_get_statbuf(UVCPP_FS_REQ); }
#endif
#endif


#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 38
int uvcpp_fs::get_system_error() { return uv_fs_get_system_error(UVCPP_FS_REQ); }
#endif
#endif


void uvcpp_fs::req_cleanup() { uv_fs_req_cleanup(UVCPP_FS_REQ); }

int uvcpp_fs::close(uvcpp_loop* loop, uv_file file) {
  return uv_fs_close(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, nullptr);
}

int uvcpp_fs::open(uvcpp_loop* loop, const char* path, int flags, int mode) {
  return uv_fs_open(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, flags, mode,
                    nullptr);
}

int uvcpp_fs::read(uvcpp_loop* loop, uv_file file, const uv_buf bufs[], unsigned int nbufs,
              int64_t offset) {
  return uv_fs_read(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file,
                    reinterpret_cast<const ::uv_buf_t*>(bufs), nbufs, offset, nullptr);
}

int uvcpp_fs::unlink(uvcpp_loop* loop, const char* path) {
  return uv_fs_unlink(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, nullptr);
}

int uvcpp_fs::write(uvcpp_loop *loop, uv_file file, const uv_buf bufs[],
                    unsigned int nbufs,
               int64_t offset) {
  return uv_fs_write(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file,
                     reinterpret_cast<const ::uv_buf_t*>(bufs), nbufs, offset, nullptr);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 14
int uvcpp_fs::copyfile(uvcpp_loop* loop, const char* path, const char* new_path,
                  int flags) {
  return uv_fs_copyfile(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, new_path, flags,
                        nullptr);
}
#endif
#endif


int uvcpp_fs::mkdir(uvcpp_loop* loop, const char* path, int mode) {
  return uv_fs_mkdir(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, mode, nullptr);
}

int uvcpp_fs::mkdtemp(uvcpp_loop* loop, const char* tpl) {
  return uv_fs_mkdtemp(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, tpl, nullptr);
}

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 34
int uvcpp_fs::mkstemp(uvcpp_loop* loop, const char* tpl) {
  return uv_fs_mkstemp(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, tpl, nullptr);
}
#endif
#endif


int uvcpp_fs::rmdir(uvcpp_loop* loop, const char* path) {
  return uv_fs_rmdir(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, nullptr);
}

int uvcpp_fs::scandir(uvcpp_loop* loop, const char* path, int flags) {
  return uv_fs_scandir(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, flags, nullptr);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 28
int uvcpp_fs::opendir(uvcpp_loop* loop, const char* path) {
  return uv_fs_opendir(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, nullptr);
}

int uvcpp_fs::readdir(uvcpp_loop* loop, uvcpp_dir* dir) {
  return uv_fs_readdir(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ,
                       (uv_dir_t*)dir->get_dir(), nullptr);
}

int uvcpp_fs::closedir(uvcpp_loop* loop, uvcpp_dir* dir) {
  return uv_fs_closedir(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ,
                        (uv_dir_t*)dir->get_dir(), nullptr);
}
#endif
#endif


int uvcpp_fs::stat(uvcpp_loop* loop, const char* path) {
  return uv_fs_stat(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, nullptr);
}

int uvcpp_fs::fstat(uvcpp_loop* loop, uv_file file) {
  return uv_fs_fstat(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, nullptr);
}

int uvcpp_fs::rename(uvcpp_loop* loop, const char* path, const char* new_path) {
  return uv_fs_rename(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, new_path,
                      nullptr);
}

int uvcpp_fs::fsync(uvcpp_loop* loop, uv_file file) {
  return uv_fs_fsync(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, nullptr);
}

int uvcpp_fs::fdatasync(uvcpp_loop* loop, uv_file file) {
  return uv_fs_fdatasync(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, nullptr);
}

int uvcpp_fs::ftruncate(uvcpp_loop* loop, uv_file file, int64_t offset) {
  return uv_fs_ftruncate(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, offset,
                         nullptr);
}

int uvcpp_fs::sendfile(uvcpp_loop* loop, uv_file out_fd, uv_file in_fd, int64_t in_offset,
                  size_t length) {
  return uv_fs_sendfile(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, out_fd, in_fd,
                        in_offset, length, nullptr);
}

int uvcpp_fs::access(uvcpp_loop* loop, const char* path, int mode) {
  return uv_fs_access(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, mode, nullptr);
}

int uvcpp_fs::chmod(uvcpp_loop* loop, const char* path, int mode) {
  return uv_fs_chmod(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, mode, nullptr);
}

int uvcpp_fs::utime(uvcpp_loop* loop, const char* path, double atime, double mtime) {
  return uv_fs_utime(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, atime, mtime,
                     nullptr);
}

int uvcpp_fs::futime(uvcpp_loop* loop, uv_file file, double atime, double mtime) {
  return uv_fs_futime(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, atime, mtime,
                      nullptr);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 36
int uvcpp_fs::lutime(uvcpp_loop* loop, const char* path, double atime, double mtime) {
  return uv_fs_lutime(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, atime, mtime,
                      nullptr);
}
#endif
#endif


int uvcpp_fs::lstat(uvcpp_loop* loop, const char* path) {
  return uv_fs_lstat(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, nullptr);
}

int uvcpp_fs::link(uvcpp_loop* loop, const char* path, const char* new_path) {
  return uv_fs_link(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, new_path, nullptr);
}

int uvcpp_fs::symlink(uvcpp_loop* loop, const char* path, const char* new_path,
                 int flags) {
  return uv_fs_symlink(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, new_path, flags,
                       nullptr);
}

int uvcpp_fs::readlink(uvcpp_loop* loop, const char* path) {
  return uv_fs_readlink(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, nullptr);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 8
int uvcpp_fs::realpath(uvcpp_loop* loop, const char* path) {
  return uv_fs_realpath(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, nullptr);
}
#endif
#endif


int uvcpp_fs::fchmod(uvcpp_loop* loop, uv_file file, int mode) {
  return uv_fs_fchmod(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, mode, nullptr);
}

int uvcpp_fs::chown(uvcpp_loop* loop, const char* path, uv_uid_t uid, uv_gid_t gid) {
  return uv_fs_chown(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, uid, gid, nullptr);
}

int uvcpp_fs::fchown(uvcpp_loop* loop, uv_file file, uv_uid_t uid, uv_gid_t gid) {
  return uv_fs_fchown(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, uid, gid,
                      nullptr);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 21
int uvcpp_fs::lchown(uvcpp_loop* loop, const char* path, uv_uid_t uid, uv_gid_t gid) {
  return uv_fs_lchown(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, uid, gid,
                      nullptr);
}
#endif
#endif

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 30
int uvcpp_fs::statfs(uvcpp_loop* loop, const char* path) {
  return uv_fs_statfs(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, nullptr);
}
#endif
#endif


int uvcpp_fs::close(uvcpp_loop* loop,
                      uv_file file,
                      ::std::function<void(uvcpp_fs*)> close_cb) {
  fs_close_cb = close_cb;
  return uv_fs_close(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, callback_close);
}

int uvcpp_fs::open(uvcpp_loop* loop,
                     const char* path,
                     int flags,
                     int mode,
                     ::std::function<void(uvcpp_fs*)> open_cb) {
  fs_open_cb = open_cb;
  return uv_fs_open(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, flags, mode,
                    callback_open);
}

int uvcpp_fs::read(uvcpp_loop* loop,
                     uv_file file, const uv_buf bufs[],
                     unsigned int nbufs,
                     int64_t offset,
                     ::std::function<void(uvcpp_fs*)> read_cb) {
  fs_read_cb = read_cb;
  return uv_fs_read(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file,
                    reinterpret_cast<const ::uv_buf_t*>(bufs), nbufs, offset, callback_read);
}

int uvcpp_fs::unlink(uvcpp_loop* loop,
                       const char* path,
                       ::std::function<void(uvcpp_fs*)> unlink_cb) {
  fs_unlink_cb = unlink_cb;
  return uv_fs_unlink(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, callback_unlink);
}

int uvcpp_fs::write(uvcpp_loop* loop,
                     uv_file file, const uv_buf bufs[],
                     unsigned int nbufs,
                     int64_t offset,
                     ::std::function<void(uvcpp_fs*)> write_cb) {
  fs_write_cb = write_cb;
  return uv_fs_write(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file,
                     reinterpret_cast<const ::uv_buf_t*>(bufs), nbufs, offset, callback_write);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 14
int uvcpp_fs::copyfile(uvcpp_loop* loop,
                         const char* path,
                         const char* new_path,
                         int flags,
                         ::std::function<void(uvcpp_fs*)> copyfile_cb) {
  fs_copyfile_cb = copyfile_cb;
  return uv_fs_copyfile(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, new_path, flags,
                        callback_copyfile);
}
#endif
#endif


int uvcpp_fs::mkdir(uvcpp_loop* loop,
                      const char* path,
                      int mode,
                      ::std::function<void(uvcpp_fs*)> mkdir_cb) {
  fs_mkdir_cb = mkdir_cb;
  return uv_fs_mkdir(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, mode,
                     callback_mkdir);
}

int uvcpp_fs::mkdtemp(uvcpp_loop* loop,
                        const char* tpl,
                        ::std::function<void(uvcpp_fs*)> mkdtemp_cb) {
  fs_mkdtemp_cb = mkdtemp_cb;
  return uv_fs_mkdtemp(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, tpl, callback_mkdtemp);
}

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 34
int uvcpp_fs::mkstemp(uvcpp_loop* loop,
                        const char* tpl,
                        ::std::function<void(uvcpp_fs*)> mkstemp_cb) {
  fs_mkstemp_cb = mkstemp_cb;
  return uv_fs_mkstemp(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, tpl, callback_mkstemp);
}
#endif
#endif


int uvcpp_fs::rmdir(uvcpp_loop* loop,
                      const char* path,
                      ::std::function<void(uvcpp_fs*)> rmdir_cb) {
  fs_rmdir_cb = rmdir_cb;
  return uv_fs_rmdir(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, callback_rmdir);
}

int uvcpp_fs::scandir(uvcpp_loop* loop,
                        const char* path,
                        int flags,
                        ::std::function<void(uvcpp_fs*)> scandir_cb) {
  fs_scandir_cb = scandir_cb;
  return uv_fs_scandir(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, flags,
                       callback_scandir);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 28
int uvcpp_fs::opendir(uvcpp_loop* loop,
                        const char* path,
                        ::std::function<void(uvcpp_fs*)> opendir_cb) {
  fs_opendir_cb = opendir_cb;
  return uv_fs_opendir(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path,
                       callback_opendir);
}

int uvcpp_fs::readdir(uvcpp_loop* loop,
                        uvcpp_dir* dir,
                        ::std::function<void(uvcpp_fs*)> readdir_cb) {
  fs_readdir_cb = readdir_cb;
  return uv_fs_readdir(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ,
                       (uv_dir_t*)dir->get_dir(), callback_readdir);
}

int uvcpp_fs::closedir(uvcpp_loop* loop,
                         uvcpp_dir* dir,
                         ::std::function<void(uvcpp_fs*)> closedir_cb) {
  fs_closedir_cb = closedir_cb;
  return uv_fs_closedir(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ,
                        (uv_dir_t*)dir->get_dir(), callback_closedir);
}
#endif
#endif


int uvcpp_fs::stat(uvcpp_loop* loop,
                     const char* path,
                     ::std::function<void(uvcpp_fs*)> stat_cb) {
  fs_stat_cb = stat_cb;
  return uv_fs_stat(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, callback_stat);
}

int uvcpp_fs::fstat(uvcpp_loop* loop,
                      uv_file file,
                      ::std::function<void(uvcpp_fs*)> fstat_cb) {
  fs_fstat_cb = fstat_cb;
  return uv_fs_fstat(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, callback_fstat);
}

int uvcpp_fs::rename(uvcpp_loop* loop,
                       const char* path,
                       const char* new_path,
                       ::std::function<void(uvcpp_fs*)> rename_cb) {
  fs_rename_cb = rename_cb;
  return uv_fs_rename(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, new_path,
                      callback_rename);
}

int uvcpp_fs::fsync(uvcpp_loop* loop,
                      uv_file file,
                      ::std::function<void(uvcpp_fs*)> fsync_cb) {
  fs_fsync_cb = fsync_cb;
  return uv_fs_fsync(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, callback_fsync);
}

int uvcpp_fs::fdatasync(uvcpp_loop* loop,
                          uv_file file,
                          ::std::function<void(uvcpp_fs*)> fdatasync_cb) {
  fs_fdatasync_cb = fdatasync_cb;
  return uv_fs_fdatasync(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file,
                         callback_fdatasync);
}

int uvcpp_fs::ftruncate(uvcpp_loop* loop,
                          uv_file file,
                          int64_t offset,
                          ::std::function<void(uvcpp_fs*)> ftruncate_cb) {
  fs_ftruncate_cb = ftruncate_cb;
  return uv_fs_ftruncate(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, offset,
                         callback_ftruncate);
}

int uvcpp_fs::sendfile(uvcpp_loop* loop,
                         uv_file out_fd,
                         uv_file in_fd,
                         int64_t in_offset,
                         size_t length,
                         ::std::function<void(uvcpp_fs*)> sendfile_cb) {
  fs_sendfile_cb = sendfile_cb;
  return uv_fs_sendfile(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, out_fd, in_fd,
                        in_offset, length, callback_sendfile);
}

int uvcpp_fs::access(uvcpp_loop* loop,
                       const char* path,
                       int mode,
                       ::std::function<void(uvcpp_fs*)> access_cb) {
  fs_access_cb = access_cb;
  return uv_fs_access(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, mode,
                      callback_access);
}

int uvcpp_fs::chmod(uvcpp_loop* loop,
                      const char* path,
                      int mode,
                      ::std::function<void(uvcpp_fs*)> chmod_cb) {
  fs_chmod_cb = chmod_cb;
  return uv_fs_chmod(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, mode,
                     callback_chmod);
}

int uvcpp_fs::utime(uvcpp_loop* loop,
                      const char* path,
                      double atime,
                      double mtime,
                      ::std::function<void(uvcpp_fs*)> utime_cb) {
  fs_utime_cb = utime_cb;
  return uv_fs_utime(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, atime, mtime,
                     callback_utime);
}

int uvcpp_fs::futime(uvcpp_loop* loop,
                       uv_file file,
                       double atime,
                       double mtime,
                       ::std::function<void(uvcpp_fs*)> futime_cb) {
  fs_futime_cb = futime_cb;
  return uv_fs_futime(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, atime, mtime,
                      callback_futime);
}

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 36
int uvcpp_fs::lutime(uvcpp_loop* loop,
                       const char* path,
                       double atime,
                       double mtime,
                       ::std::function<void(uvcpp_fs*)> lutime_cb) {
  fs_lutime_cb = lutime_cb;
  return uv_fs_lutime(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, atime, mtime,
                      callback_lutime);
}
#endif
#endif


int uvcpp_fs::lstat(uvcpp_loop* loop,
                      const char* path,
                      ::std::function<void(uvcpp_fs*)> lstat_cb) {
  fs_lstat_cb = lstat_cb;
  return uv_fs_lstat(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, callback_lstat);
}

int uvcpp_fs::link(uvcpp_loop* loop,
                     const char* path,
                     const char* new_path,
                     ::std::function<void(uvcpp_fs*)> link_cb) {
  fs_link_cb = link_cb;
  return uv_fs_link(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, new_path,
                    callback_link);
}

int uvcpp_fs::symlink(uvcpp_loop* loop,
                        const char* path,
                        const char* new_path,
                        int flags,
                        ::std::function<void(uvcpp_fs*)> symlink_cb) {
  fs_symlink_cb = symlink_cb;
  return uv_fs_symlink(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, new_path, flags,
                       callback_symlink);
}

int uvcpp_fs::readlink(uvcpp_loop* loop,
                         const char* path,
                         ::std::function<void(uvcpp_fs*)> readlink_cb) {
  fs_readlink_cb = readlink_cb;
  return uv_fs_readlink(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path,
                        callback_readlink);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 8
int uvcpp_fs::realpath(uvcpp_loop* loop,
                         const char* path,
                         ::std::function<void(uvcpp_fs*)> realpath_cb) {
  fs_realpath_cb = realpath_cb;
  return uv_fs_realpath(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path,
                        callback_realpath);
}
#endif
#endif


int uvcpp_fs::fchmod(uvcpp_loop* loop,
                       uv_file file,
                       int mode,
                       ::std::function<void(uvcpp_fs*)> fchmod_cb) {
  fs_fchmod_cb = fchmod_cb;
  return uv_fs_fchmod(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, mode,
                      callback_fchmod);
}

int uvcpp_fs::chown(uvcpp_loop* loop,
                      const char* path,
                      uv_uid_t uid,
                      uv_gid_t gid,
                      ::std::function<void(uvcpp_fs*)> chown_cb) {
  fs_chown_cb = chown_cb;
  return uv_fs_chown(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, uid, gid,
                     callback_chown);
}

int uvcpp_fs::fchown(uvcpp_loop* loop,
                       uv_file file,
                       uv_uid_t uid,
                       uv_gid_t gid,
                       ::std::function<void(uvcpp_fs*)> fchown_cb) {
  fs_fchown_cb = fchown_cb;
  return uv_fs_fchown(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, file, uid, gid,
                      callback_fchown);
}
#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 21
int uvcpp_fs::lchown(uvcpp_loop* loop,
                       const char* path,
                       uv_uid_t uid,
                       uv_gid_t gid,
                       ::std::function<void(uvcpp_fs*)> lchown_cb) {
  fs_lchown_cb = lchown_cb;
  return uv_fs_lchown(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, uid, gid,
                      callback_lchown);
}
#endif
#endif

#if UV_VERSION_MAJOR >= 1
#if UV_VERSION_MINOR >= 30
int uvcpp_fs::statfs(uvcpp_loop* loop,
                       const char* path,
                       ::std::function<void(uvcpp_fs*)> statfs_cb) {
  fs_statfs_cb = statfs_cb;
  return uv_fs_statfs(OBJ_UVCPP_LOOP_HANDLE(*loop), UVCPP_FS_REQ, path, callback_statfs);
}
#endif
#endif


int uvcpp_fs::scandir_next(uv_dirent_t* ent) {
  return uv_fs_scandir_next(UVCPP_FS_REQ, ent);
}

}


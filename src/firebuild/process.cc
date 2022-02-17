/* Copyright (c) 2014 Balint Reczey <balint@balintreczey.hu> */
/* This file is an unpublished work. All rights reserved. */


#include "firebuild/process.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <utility>

#include "common/firebuild_common.h"
#include "firebuild/file.h"
#include "firebuild/pipe_recorder.h"
#include "firebuild/platform.h"
#include "firebuild/execed_process.h"
#include "firebuild/forked_process.h"
#include "firebuild/execed_process_env.h"
#include "firebuild/debug.h"
#include "firebuild/utils.h"

namespace firebuild {

static int fb_pid_counter;

Process::Process(const int pid, const int ppid, const int exec_count, const FileName *wd,
                 Process * parent, std::vector<std::shared_ptr<FileFD>>* fds)
    : parent_(parent), state_(FB_PROC_RUNNING),
      fb_pid_(fb_pid_counter++), pid_(pid), ppid_(ppid), exec_count_(exec_count), exit_status_(-1),
      wd_(wd), fds_(fds), fork_children_(), expected_child_(), exec_child_(NULL) {
  TRACKX(FB_DEBUG_PROC, 0, 1, Process, this, "pid=%d, ppid=%d, parent=%s", pid, ppid, D(parent));
}


const Process* Process::fork_parent() const {
  return fork_point() ? fork_point()->parent() : nullptr;
}

void Process::update_rusage(const int64_t utime_u, const int64_t stime_u) {
  ExecedProcess* ep = exec_point();
  if (ep) {
    ep->add_utime_u(utime_u);
    ep->add_stime_u(stime_u);
  }
}

void Process::exit_result(const int status, const int64_t utime_u,
                          const int64_t stime_u) {
  /* The kernel only lets the low 8 bits of the exit status go through.
   * From the exit()/_exit() side, the remaining bits are lost (they are
   * still there in on_exit() handlers).
   * From wait()/waitpid() side, additional bits are used to denote exiting
   * via signal.
   * We use -1 if there's no exit status available (the process is still
   * running, or exited due to an unhandled signal). */
  exit_status_ = status & 0xff;
  update_rusage(utime_u, stime_u);
}

/* This is a static function operating on any std::vector<std::shared_ptr<FileFD>>,
 * without requiring a Process object. */
std::shared_ptr<FileFD>
Process::add_filefd(std::vector<std::shared_ptr<FileFD>>* fds,
                    const int fd,
                    std::shared_ptr<FileFD> ffd) {
  TRACK(FB_DEBUG_PROC, "fd=%d", fd);

  if (fds->size() <= static_cast<unsigned int>(fd)) {
    fds->resize(fd + 1, nullptr);
  }
  if ((*fds)[fd]) {
    firebuild::fb_error("Fd " + d(fd) + " is already tracked as being open.");
  }
  /* the shared_ptr takes care of cleaning up the old fd if needed */
  (*fds)[fd] = ffd;
  return ffd;
}

/* This is the member function operating on `this` Process. */
std::shared_ptr<FileFD>
Process::add_filefd(const int fd, std::shared_ptr<FileFD> ffd) {
  TRACK(FB_DEBUG_PROC, "fd=%d", fd);

  return Process::add_filefd(fds_, fd, ffd);
}

void Process::add_pipe(std::shared_ptr<Pipe> pipe) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "pipe=%s", D(pipe.get()));

  exec_point()->add_pipe(pipe);
}

void Process::drain_all_pipes() {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "");

  for (auto file_fd : *fds_) {
    if (!file_fd || !is_wronly(file_fd->flags())) {
      continue;
    }
    auto pipe = file_fd->pipe().get();
    if (pipe) {
      pipe->drain_fd1_end(file_fd.get());
    }
  }
}

std::vector<std::shared_ptr<FileFD>>* Process::pass_on_fds(const bool execed) const {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "execed=%s", D(execed));

  const int fds_size = fds_->size();
  auto ret_fds = new std::vector<std::shared_ptr<FileFD>>(fds_size);
  int last_fd = -1;
  for (int i = 0; i < fds_size; i++) {
    const FileFD* const raw_file_fd = (*fds_)[i].get();
    if (raw_file_fd != nullptr) {
      if (!(execed && raw_file_fd->cloexec())) {
        /* The operations on the fds in the new process don't affect the fds in the parent,
         * thus create a copy of the parent's FileFD pointed to by a new shared pointer. */
        (*ret_fds)[i] = std::make_shared<FileFD>(*raw_file_fd);
        last_fd = i;
        if (execed && raw_file_fd->close_on_popen()) {
          /* The newly exec()-ed process will not close inherited popen()-ed fds on pclose() */
          (*ret_fds)[i]->set_close_on_popen(false);
        }
      }
    }
  }

  if (last_fd + 1 < fds_size) {
    /* A few of the last elements of the ret_fds vector is not used. Cut the size to the used
     * ones. */
    ret_fds->resize(last_fd + 1);
  }

  return ret_fds;
}

void Process::AddPopenedProcess(int fd, const char *fifo, ExecedProcess *proc, int type_flags) {
  /* The stdin/stdout of the child needs to be conected to parent's fd */
  int child_fileno = is_wronly(type_flags) ? STDIN_FILENO : STDOUT_FILENO;
  if (child_fileno == STDOUT_FILENO) {
    /* Since only fd0 is passed only fd0's FileFD inherits O_CLOEXEC from type_flags. */
    auto pipe = handle_pipe_internal(fd, -1, type_flags, 0 /* fd1_flags */, 0, fifo, nullptr,
                                     true, false, this);
    auto ffd1 = std::make_shared<FileFD>(child_fileno, O_WRONLY, pipe->fd1_shared_ptr(),
                                         proc->parent_);

    (*proc->parent_->fds_)[child_fileno] = ffd1;
    (*proc->fds_)[child_fileno] = ffd1;
    add_pipe(pipe);
  } else {
    /* child_fileno == STDIN_FILENO */
    /* Create the pipe later, in accept_exec_child(). */
    assert(!fifo);
  }

  fd2popen_child_[fd] = proc;
}

int Process::handle_open(const int dirfd, const char * const ar_name, const size_t ar_len,
                         const int flags, const int fd, const int error, int fd_conn,
                         const int ack_num) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this,
         "dirfd=%d, ar_name=%s, flags=%d, fd=%d, error=%d, fd_conn=%s, ack_num=%d",
         dirfd, D(ar_name), flags, fd, error, D_FD(fd_conn), ack_num);

  const FileName* name = get_absolute(dirfd, ar_name, ar_len);
  if (!name) {
    // FIXME don't disable shortcutting if openat() failed due to the invalid dirfd
    exec_point()->disable_shortcutting_bubble_up("Invalid dirfd passed to openat()");
    if (ack_num != 0) {
      ack_msg(fd_conn, ack_num);
    }
    return -1;
  }

  if (fd >= 0) {
    add_filefd(fd, std::make_shared<FileFD>(name, fd, flags, this));
  }

  if (ack_num != 0) {
    ack_msg(fd_conn, ack_num);
  }

  /* Registering parent directory is obsolete when the file is opened as read-only
   * or without O_CREAT because it must have existed before the call.
   * Even with O_CREAT the file could exist before, but register parent to stay on the safe side. */
  if (!error && ((flags & O_ACCMODE) != O_RDONLY) && (flags & O_CREAT)
      && !exec_point()->register_parent_directory(name)) {
    exec_point()->disable_shortcutting_bubble_up(
        "Could not register an implicit parent directory", *name);
    return -1;
  }

  if (!exec_point()->register_file_usage(name, name, FILE_ACTION_OPEN, flags, error)) {
    exec_point()->disable_shortcutting_bubble_up("Could not register the opening of a file", *name);
    return -1;
  }

  return 0;
}

/* Handle freopen(). See #650 for some juicy details. */
int Process::handle_freopen(const char * const ar_name, const size_t ar_len,
                            const int flags, const int oldfd, const int fd, const int error,
                            int fd_conn, const int ack_num) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this,
         "ar_name=%s, flags=%d, oldfd=%d, fd=%d, error=%d, fd_conn=%s, ack_num=%d",
         D(ar_name), flags, oldfd, fd, error, D_FD(fd_conn), ack_num);

  if (ar_name != NULL) {
    /* old_fd is always closed, even if freopen() fails. It comes from stdio's bookkeeping, we have
     * no reason to assume that this close attempt failed. */
    handle_close(oldfd, 0);

    /* Register the opening of the new file, no matter if succeeded or failed. */
    return handle_open(AT_FDCWD, ar_name, ar_len, flags, fd, error, fd_conn, ack_num);
  } else {
    /* Find oldfd. */
    FileFD *file_fd = get_fd(oldfd);
    if (!file_fd || file_fd->origin_type() != FD_ORIGIN_FILE_OPEN) {
      /* Can't find oldfd, or wasn't opened by filename. Don't know what to do. */
      exec_point()->disable_shortcutting_bubble_up(
        "Could not figure out old file name for freopen(..., NULL)", oldfd);
      if (ack_num != 0) {
        ack_msg(fd_conn, ack_num);
      }
      return -1;
    } else {
      /* Remember oldfd's filename before closing oldfd. We've tried to reopen the same file. */
      const FileName *filename = file_fd->filename();

      /* old_fd is always closed, even if freopen() fails. It comes from stdio's bookkeeping, we have
       * no reason to assume that this close attempt failed. */
      handle_close(oldfd, 0);

      /* Register the reopening, no matter if succeeded or failed. */
      return handle_open(AT_FDCWD, filename->c_str(), filename->length(),
                         flags, fd, error, fd_conn, ack_num);
    }
  }
}

/* close that fd if open, silently ignore if not open */
int Process::handle_force_close(const int fd) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "fd=%d", fd);

  if (get_fd(fd)) {
    return handle_close(fd, 0);
  }
  return 0;
}

int Process::handle_close(const int fd, const int error) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "fd=%d, error=%d", fd, error);

  FileFD* file_fd = get_fd(fd);

  if (error == EIO) {
    exec_point()->disable_shortcutting_bubble_up("IO error closing fd", fd);
    return -1;
  } else if (error == EINTR) {
    /* We don't know if the fd was closed or not, see #723. */
    exec_point()->disable_shortcutting_bubble_up("EINTR while closing fd", fd);
    return -1;
  } else if (error == 0 && !file_fd) {
    exec_point()->disable_shortcutting_bubble_up(
        "Process closed an unknown fd successfully, "
        "which means interception missed at least one open()", fd);
    return -1;
  } else if (error == EBADF) {
    /* Process closed an fd unknown to it. Who cares? */
    assert(!get_fd(fd));
    return 0;
  } else {
    if (!file_fd) {
      /* closing an unknown fd with not EBADF prevents shortcutting */
      exec_point()->disable_shortcutting_bubble_up(
          "Process closed an unknown fd successfully, "
          "which means interception missed at least one open()", fd);
      return -1;
    } else {
      auto pipe = file_fd->pipe().get();
      if (pipe) {
        /* There may be data pending, drain it and register closure. */
        pipe->handle_close(file_fd);
        file_fd->set_pipe(nullptr);
      }
      (*fds_)[fd].reset();
      return 0;
    }
  }
}

int Process::handle_unlink(const int dirfd, const char * const ar_name, const size_t ar_len,
                           const int flags, const int error) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "dirfd=%d, ar_name=%s, flags=%d, error=%d",
         dirfd, D(ar_name), flags, error);

  const FileName* name = get_absolute(dirfd, ar_name, ar_len);
  if (!name) {
    // FIXME don't disable shortcutting if unlinkat() failed due to the invalid dirfd
    exec_point()->disable_shortcutting_bubble_up("Invalid dirfd passed to unlinkat()");
    return -1;
  }

  if (!error) {
    /* There is no need to call register_parent_directory().
     * If the process created the file to unlink it is already registered and if it was present
     * before the process started then registering the file to unlink already implies the existence
     * of the parent dir.
     */

    // FIXME When a directory is removed, register that it was an _empty_ directory
    const FileUsage* fu = FileUsage::Get(flags & AT_REMOVEDIR ? ISDIR : ISREG, true);
    if (!exec_point()->register_file_usage(name, fu)) {
      exec_point()->disable_shortcutting_bubble_up(
          "Could not register the unlink or rmdir", *name);
      return -1;
    }
  }

  return 0;
}

int Process::handle_fstat(const int fd, const int st_mode, const int error) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this,
         "fd=%d, st_mode=%d, error=%d", fd, st_mode, error);

  // TODO(rbalint) handle file size and some file types
  (void)fd;
  (void)st_mode;
  (void)error;

  return 0;
}

int Process::handle_stat(const int dirfd, const char * const ar_name, const size_t ar_len,
                         const int flags, const int st_mode, const int error) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this,
         "dirfd=%d, ar_name=%s, flags=%d, st_mode=%d, error=%d",
         dirfd, D(ar_name), flags, st_mode, error);

  if (flags & AT_EMPTY_PATH) {
    // TODO(rbalint) add support for AT_EMPTY_PATH
    exec_point()->disable_shortcutting_bubble_up(
        "fstatat() with AT_EMPTY_PATH flag is not supported");
    return -1;
  }

  const FileName* name = get_absolute(dirfd, ar_name, ar_len);
  if (!name) {
    // FIXME don't disable shortcutting if stat() failed due to the invalid dirfd
    exec_point()->disable_shortcutting_bubble_up(
        "Invalid dirfd or filename passed to stat() variant");
    return -1;
  }

  if (!exec_point()->register_file_usage(
          name, name, S_ISDIR(st_mode) ? FILE_ACTION_STATDIR : FILE_ACTION_STATFILE,
          flags, error)) {
    exec_point()->disable_shortcutting_bubble_up("Could not register the opening of a file", *name);
    return -1;
  }

  return 0;
}

int Process::handle_rmdir(const char * const ar_name, const size_t ar_name_len, const int error) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "ar_name=%s, error=%d", D(ar_name), error);

  return handle_unlink(AT_FDCWD, ar_name, ar_name_len, AT_REMOVEDIR, error);
}

int Process::handle_mkdir(const int dirfd, const char * const ar_name, const size_t ar_len,
                          const int error) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "dirfd=%d, ar_name=%s, error=%d",
         dirfd, D(ar_name), error);

  const FileName* name = get_absolute(dirfd, ar_name, ar_len);
  if (!name) {
    // FIXME don't disable shortcutting if mkdirat() failed due to the invalid dirfd
    exec_point()->disable_shortcutting_bubble_up("Invalid dirfd passed to mkdirat()");
    return -1;
  }

  if (!error && !exec_point()->register_parent_directory(name)) {
    exec_point()->disable_shortcutting_bubble_up(
        "Could not register the implicit parent directory", *name);
    return -1;
  }

  if (!exec_point()->register_file_usage(name, name, FILE_ACTION_MKDIR, O_WRONLY, error)) {
    exec_point()->disable_shortcutting_bubble_up(
        "Could not register the directory creation ", *name);
    return -1;
  }

  return 0;
}

std::shared_ptr<Pipe> Process::handle_pipe_internal(const int fd0, const int fd1,
                                                    const int fd0_flags, const int fd1_flags,
                                                    const int error, const char *fd0_fifo,
                                                    const char *fd1_fifo, bool fd0_close_on_popen,
                                                    bool fd1_close_on_popen,  Process *fd0_proc) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this,
         "fd0=%d, fd1=%d, fd0_flags=%d, fd1_flags=%d, error=%d, fd0_fifo=%s, fd1_fifo=%s,"
         " fd0_proc=%s",
         fd0, fd1, fd0_flags, fd1_flags, error, fd0_fifo, fd1_fifo, D(fd0_proc));

  if (error) {
    return std::shared_ptr<Pipe>(nullptr);
  }

  /* validate fd-s */
  if (fd0_proc->get_fd(fd0)) {
    /* we already have this fd, probably missed a close() */
    fd0_proc->exec_point()->disable_shortcutting_bubble_up(
        "Process created an fd which is known to be open, "
        "which means interception missed at least one close()", fd0);
    return std::shared_ptr<Pipe>(nullptr);
  }

  if (fd1 != -1 && get_fd(fd1)) {
    /* we already have this fd, probably missed a close() */
    exec_point()->disable_shortcutting_bubble_up(
        "Process created an fd which is known to be open, "
        "which means interception missed at least one close()", fd1);
    return std::shared_ptr<Pipe>(nullptr);
  }

  assert(fd0_fifo);
  int fd0_conn = open(fd0_fifo, O_WRONLY | O_NONBLOCK);
  assert(fd0_conn != -1 && "opening fd0_fifo failed");
  firebuild::bump_fd_age(fd0_conn);

#ifdef __clang_analyzer__
  /* Scan-build reports a false leak for the correct code. This is used only in static
   * analysis. It is broken because all shared pointers to the Pipe must be copies of
   * the shared self pointer stored in it. */
  auto pipe = std::make_shared<Pipe>(fd0_conn, this);
#else
  auto pipe = (new Pipe(fd0_conn, this))->shared_ptr();
#endif
  fd0_proc->add_filefd(fd0, std::make_shared<FileFD>(
      fd0, (fd0_flags & ~O_ACCMODE) | O_RDONLY, pipe->fd0_shared_ptr(), fd0_proc,
      fd0_close_on_popen));
  /* Fd1_fifo may not be set. */
  if (fd1_fifo) {
    int fd1_conn = open(fd1_fifo, O_NONBLOCK | O_RDONLY);
    assert(fd1_conn != -1 && "opening fd1_fifo failed");
    firebuild::bump_fd_age(fd1_conn);
    auto ffd1 = std::make_shared<FileFD>(fd1, (fd1_flags & ~O_ACCMODE) | O_WRONLY,
                                         pipe->fd1_shared_ptr(),
                                         this, fd1_close_on_popen);
    add_filefd(fd1, ffd1);
    /* Empty recorders array. We don't start recording after a pipe(), this data wouldn't be
     * used anywhere. We only start recording after an exec(), to catch the traffic as seen
     * from that potential shortcutting point. */
    auto recorders = std::vector<std::shared_ptr<PipeRecorder>>();
    pipe->add_fd1_and_proc(fd1_conn, ffd1.get(), exec_point(), recorders);
  }
  return pipe;
}

int Process::handle_dup3(const int oldfd, const int newfd, const int flags,
                         const int error) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "oldfd=%d, newfd=%d, flags=%d, error=%d",
         oldfd, newfd, flags, error);

  switch (error) {
    case EBADF:
    case EBUSY:
    case EINTR:
    case EINVAL:
    case ENFILE: {
      /* dup() failed */
      return 0;
    }
    case 0:
    default:
      break;
  }

  /* validate fd-s */
  if (!get_fd(oldfd)) {
    /* we already have this fd, probably missed a close() */
    exec_point()->disable_shortcutting_bubble_up(
        "Process created an fd which is known to be open, "
        "which means interception missed at least one close()", oldfd);
    return -1;
  }

  /* dup2()'ing an existing fd to itself returns success and does nothing */
  if (oldfd == newfd) {
    return 0;
  }

  handle_force_close(newfd);

  add_filefd(newfd, std::make_shared<FileFD>(
      newfd, (((*fds_)[oldfd]->flags() & ~O_CLOEXEC) | flags), FD_ORIGIN_DUP,
      (*fds_)[oldfd]));
  return 0;
}

int Process::handle_rename(const int olddirfd, const char * const old_ar_name,
                           const size_t old_ar_len,
                           const int newdirfd, const char * const new_ar_name,
                           const size_t new_ar_len,
                           const int error) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this,
         "olddirfd=%d, old_ar_name=%s, newdirfd=%d, new_ar_name=%s, error=%d",
         olddirfd, D(old_ar_name), newdirfd, D(new_ar_name), error);

  if (error) {
    return 0;
  }

  /*
   * Note: rename() is different from "mv" in at least two aspects:
   *
   * - Can't move to a containing directory, e.g.
   *     rename("/home/user/file.txt", "/tmp");  (or ... "/tmp/")
   *   fails, you have to specify the full new name like
   *     rename("/home/user/file.txt", "/tmp/file.txt");
   *   instead.
   *
   * - If the source is a directory then the target can be an empty directory,
   *   which will be atomically removed beforehand. E.g. if "mytree" is a directory then
   *     mkdir("/tmp/target");
   *     rename("mytree", "/tmp/target");
   *   will result in the former "mytree/file.txt" becoming "/tmp/target/file.txt",
   *   with no "mytree" component. "target" has to be an empty directory for this to work.
   */

  const FileName* old_name = get_absolute(olddirfd, old_ar_name, old_ar_len);
  const FileName* new_name = get_absolute(newdirfd, new_ar_name, new_ar_len);
  if (!old_name || !new_name) {
    // FIXME don't disable shortcutting if renameat() failed due to the invalid dirfd
    exec_point()->disable_shortcutting_bubble_up("Invalid dirfd passed to renameat()");
    return -1;
  }

  struct stat64 st;
  if (lstat64(new_name->c_str(), &st) < 0 ||
      !S_ISREG(st.st_mode)) {
    exec_point()->disable_shortcutting_bubble_up(
        "Could not register the renaming of non-regular file", *old_name);
    return -1;
  }

  /* No need to register the parent dir of old_name, registering new_name should be enough. */
  if (!exec_point()->register_parent_directory(new_name)) {
    exec_point()->disable_shortcutting_bubble_up(
        "Could not register the implicit parent directory", *new_name);
    return -1;
  }

  /* It's tricky because the renaming has already happened, there's supposedly nothing
   * at the old filename. Yet we need to register that we read that file with its
   * particular hash value.
   * FIXME we compute the hash twice, both for the old and new location.
   * FIXME refactor so that it plays nicer together with register_file_usage(). */

  /* Register the opening for reading at the old location */
  if (!exec_point()->register_file_usage(old_name, new_name, FILE_ACTION_OPEN, O_RDONLY, error)) {
    exec_point()->disable_shortcutting_bubble_up(
        "Could not register the renaming (from)", *old_name);
    return -1;
  }

  /* Register the opening for writing at the new location */
  if (!exec_point()->register_file_usage(new_name, new_name,
                                         FILE_ACTION_OPEN, O_CREAT|O_WRONLY|O_TRUNC, error)) {
    exec_point()->disable_shortcutting_bubble_up(
        "Could not register the renaming (to)", *new_name);
    return -1;
  }

  return 0;
}

int Process::handle_symlink(const char * const old_ar_name,
                            const int newdirfd, const char * const new_ar_name,
                            const int error) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this,
         "old_ar_name=%s, newdirfd=%d, new_ar_name=%s, error=%d",
         D(old_ar_name), newdirfd, D(new_ar_name), error);

  if (!error) {
    exec_point()->disable_shortcutting_bubble_up(
        "Process created a symlink",
        " ([" + d(newdirfd) + "]" + d(new_ar_name) + " -> " + d(old_ar_name) + ")");
    return -1;
  }
  return 0;
}

int Process::handle_clear_cloexec(const int fd) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "fd=%d", fd);

  if (!get_fd(fd)) {
    exec_point()->disable_shortcutting_bubble_up(
        "Process successfully cleared cloexec on fd which is known to be closed, "
        "which means interception missed at least one open()", fd);
    return -1;
  }
  (*fds_)[fd]->set_cloexec(false);
  return 0;
}

int Process::handle_fcntl(const int fd, const int cmd, const int arg,
                          const int ret, const int error) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "fd=%d, cmd=%d, arg=%d, ret=%d, error=%d",
         fd, cmd, arg, ret, error);

  switch (cmd) {
    case F_DUPFD:
      return handle_dup3(fd, ret, 0, error);
    case F_DUPFD_CLOEXEC:
      return handle_dup3(fd, ret, O_CLOEXEC, error);
    case F_SETFD:
      if (error == 0) {
        if (!get_fd(fd)) {
          exec_point()->disable_shortcutting_bubble_up(
              "Process successfully fcntl'ed on fd which is known to be closed, "
              "which means interception missed at least one open()", fd);
          return -1;
        }
        (*fds_)[fd]->set_cloexec(arg & FD_CLOEXEC);
      }
      return 0;
    default:
      exec_point()->disable_shortcutting_bubble_up("Process executed unsupported fcntl ", d(cmd));
      return 0;
  }
}

int Process::handle_ioctl(const int fd, const int cmd,
                          const int ret, const int error) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "fd=%d, cmd=%d, ret=%d, error=%d",
         fd, cmd, ret, error);

  (void) ret;

  switch (cmd) {
    case FIOCLEX:
      if (error == 0) {
        if (!get_fd(fd)) {
          exec_point()->disable_shortcutting_bubble_up(
              "Process successfully ioctl'ed on fd which is known to be closed, "
              "which means interception missed at least one open()", fd);
          return -1;
        }
        (*fds_)[fd]->set_cloexec(true);
      }
      return 0;
    case FIONCLEX:
      if (error == 0) {
        if (!get_fd(fd)) {
          exec_point()->disable_shortcutting_bubble_up(
              "Process successfully ioctl'ed on fd which is known to be closed, "
              "which means interception missed at least one open()", fd);
          return -1;
        }
        (*fds_)[fd]->set_cloexec(false);
      }
      return 0;
    default:
      exec_point()->disable_shortcutting_bubble_up("Process executed unsupported ioctl",
                                                   " " + d(cmd));
      return 0;
  }
}

void Process::handle_read_from_inherited(const int fd) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "fd=%d", fd);

  if (!get_fd(fd)) {
    exec_point()->disable_shortcutting_bubble_up(
        "Process successfully read from fd which is known to be closed, which means interception"
        " missed at least one open()", fd);
    return;
  }
  /* Note: this doesn't disable any shortcutting if (*fds_)[fd]->opened_by() == this,
   * i.e. the file was opened by the current process. */
  Process* opened_by = (*fds_)[fd]->opened_by();
  exec_point()->disable_shortcutting_bubble_up_to_excl(
      opened_by ? opened_by->exec_point() : nullptr, "Process read from inherited fd ", fd);
}

void Process::handle_write_to_inherited(const int fd) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "fd=%d", fd);

  if (!get_fd(fd)) {
    exec_point()->disable_shortcutting_bubble_up(
        "Process successfully wrote to fd which is known to be closed, which means interception"
        " missed at least one open()", fd);
    return;
  }
  if (!(*fds_)[fd]->pipe()) {
    /* Note: this doesn't disable any shortcutting if (*fds_)[fd]->opened_by() == this,
     * i.e. the file was opened by the current process. */
    Process* opened_by = (*fds_)[fd]->opened_by();
    exec_point()->disable_shortcutting_bubble_up_to_excl(
        opened_by ? opened_by->exec_point() : nullptr,
        "Process wrote to inherited non-pipe fd ", fd);
  }
}

void Process::handle_set_wd(const char * const ar_d, const size_t ar_d_len) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "ar_d=%s", ar_d);

  wd_ = get_absolute(AT_FDCWD, ar_d, ar_d_len);
  assert(wd_);
  add_wd(wd_);
}

void Process::handle_set_fwd(const int fd) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "fd=%d", fd);

  const FileFD* ffd = get_fd(fd);
  if (!ffd) {
    exec_point()->disable_shortcutting_bubble_up(
        "Process successfully fchdir()'ed to an  fd which is known to be closed, which means "
        "interception missed at least one open()", fd);
    return;
  }
  wd_ = ffd->filename();
  assert(wd_);
  add_wd(wd_);
}

const FileName* Process::get_absolute(const int dirfd, const char * const name, ssize_t length) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "dirfd=%d, name=%s, length=%ld",
         dirfd, D(name), length);

  if (platform::path_is_absolute(name)) {
    return FileName::Get(name, length);
  } else {
    char on_stack_buf[2048], *buf;

    const FileName* dir;
    if (dirfd == AT_FDCWD) {
      dir = wd();
    } else {
      const FileFD* ffd = get_fd(dirfd);
      if (ffd) {
        dir = ffd->filename();
        if (!dir) {
          return nullptr;
        }
      } else {
        return nullptr;
      }
    }

    const ssize_t name_length = (length == -1) ? strlen(name) : length;
    /* Both dir and name are in canonical form thanks to the interceptor, only "." and ""
     * need handling here. See make_canonical() in the interceptor. */
    if (name_length == 0 || (name_length == 1 && name[0] == '.')) {
      return dir;
    }

    const size_t on_stack_buffer_size = sizeof(on_stack_buf);
    const bool separator_needed = (dir->c_str()[dir->length() - 1] != '/');
    /* Only "/" should end with a separator */
    assert(separator_needed || dir->length() == 1);
    const size_t total_buf_len = dir->length() + (separator_needed ? 1 : 0) + name_length + 1;
    if (on_stack_buffer_size < total_buf_len) {
      buf = reinterpret_cast<char *>(malloc(total_buf_len));
    } else {
      buf = reinterpret_cast<char *>(on_stack_buf);
    }

    memcpy(buf, dir->c_str(), dir->length());
    size_t offset = dir->length();
    if (separator_needed) {
      buf[offset++] = '/';
    }

    memcpy(buf + offset, name, name_length);
    buf[total_buf_len - 1] = '\0';
    const FileName* ret = FileName::Get(buf, total_buf_len - 1);
    if (on_stack_buffer_size < total_buf_len) {
      free(buf);
    }
    return ret;
  }
}

static bool argv_matches_expectation(const std::vector<std::string>& actual,
                                     const std::vector<std::string>& expected) {
  /* When launching ["foo", "arg1"], the new process might be something like
   * ["/bin/bash", "-e", "./foo", "arg1"].
   *
   * If the program name already contained a slash, or if it refers to a binary executable,
   * it remains unchanged. If it didn't contain a slash and it refers to an interpreted file,
   * it's replaced by an absolute or relative path containing at least one slash (e.g. "./foo").
   *
   * A single interpreter might only add one additional command line parameter, but there can be
   * a chain of interpreters, so allow for arbitrarily many prepended parameters.
   */
  if (actual.size() < expected.size()) {
    return false;
  }

  if (actual.size() == expected.size()) {
    /* If the length is the same, exact match is required. */
    return actual == expected;
  }

  /* If the length grew, check the expected arg0. */
  int offset = actual.size() - expected.size();
  if (expected[0].find('/') != std::string::npos) {
    /* If it contained a slash, exact match is needed. */
    if (actual[offset] != expected[0]) {
      return false;
    }
  } else {
    /* If it didn't contain a slash, it needed to get prefixed with some path. */
    int expected_arg0_len = expected[0].length();
    int actual_arg0_len = actual[offset].length();
    /* Needs to be at least 1 longer. */
    if (actual_arg0_len <= expected_arg0_len) {
      return false;
    }
    /* See if the preceding character is a '/'. */
    if (actual[offset][actual_arg0_len - expected_arg0_len - 1] != '/') {
      return false;
    }
    /* See if the stuff after the '/' is the same. */
    if (actual[offset].compare(actual_arg0_len - expected_arg0_len,
                               expected_arg0_len, expected[0]) != 0) {
      return false;
    }
  }

  /* For the remaining parameters exact match is needed. */
  for (unsigned int i = 1; i < expected.size(); i++) {
    if (actual[offset + i] != expected[i]) {
      return false;
    }
  }
  return true;
}

std::vector<std::shared_ptr<FileFD>>*
Process::pop_expected_child_fds(const std::vector<std::string>& argv,
                                LaunchType *launch_type_p,
                                int *type_flags_p,
                                const bool failed) {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "failed=%s", D(failed));

  if (expected_child_) {
    if (argv_matches_expectation(argv, expected_child_->argv())) {
      auto fds = expected_child_->pop_fds();
      if (launch_type_p)
          *launch_type_p = expected_child_->launch_type();
      if (type_flags_p)
          *type_flags_p = expected_child_->type_flags();
      delete(expected_child_);
      expected_child_ = nullptr;
      return fds;
    } else {
      exec_point()->disable_shortcutting_bubble_up(
          "Unexpected system/popen/posix_spawn child appeared", ": " + d(argv) + " "
          "while waiting for: " + d(expected_child_));
    }
    delete(expected_child_);
    expected_child_ = nullptr;
  } else {
    exec_point()->disable_shortcutting_bubble_up("Unexpected system/popen/posix_spawn child",
                                   std::string(failed ? "  failed: " : " appeared: ") + d(argv));
  }
  return nullptr;
}

bool Process::can_ack_parent_wait() const {
  return state_ == FB_PROC_FINALIZED;
}

bool Process::any_child_not_finalized() {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "");

  if (exec_pending_ || pending_popen_child_) {
    return true;
  }
  if (exec_child() && exec_child()->state() != FB_PROC_FINALIZED) {
    /* The exec child is not yet finalized. We're not ready to finalize either. */
    return true;
  }

  for (auto fork_child : fork_children_) {
    if (fork_child->state_ != FB_PROC_FINALIZED) {
      return true;
    }
  }
  return false;
}

/**
 * Finalize the current process.
 */
void Process::do_finalize() {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "");

  reset_file_fd_pipe_refs();

  assert_cmp(state(), ==, FB_PROC_TERMINATED);
  set_state(FB_PROC_FINALIZED);
}

/**
 * Finalize the current process if possible, and if successful then
 * bubble it up.
 */
void Process::maybe_finalize() {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "");

  if (state() != FB_PROC_TERMINATED) {
    return;
  }
  if (any_child_not_finalized()) {
    /* A child is yet to be finalized. We're not ready to finalize. */
    return;
  }

  /* Only finalize the process after it is clear that if the parent has waited for it. */
  // TODO(rbalint) collect/confirm exit status in the parent to make sure that the exit
  // status saved in the cache will be correct.
  if (!been_waited_for()) {
    /* Here we check only the fork parent. In theory the fork parent could exec() and the
     * execed process could wait for this child, but this is rare and costly to detect thus
     * we disable shortcutting in more cases than it is absolutely needed.
     */
    const Process* fork_parent_ptr = fork_parent();
    if (fork_parent_ptr) {
      if (fork_parent_ptr->state() == FB_PROC_TERMINATED) {
        exec_point()->disable_shortcutting_bubble_up("Orphan processes can't be shortcut",
                                                     exec_point());
        /* Can proceed with finalizing this process, it won't be saved to the cache. */
      } else {
        assert_cmp(fork_parent_ptr->state(), ==, FB_PROC_RUNNING);
        /* Wait for the fork parent to wait() for this child or to terminate. */
        return;
      }
    } else {
      /* The top process will be waited for later. */
      return;
    }
  }

  drain_all_pipes();
  do_finalize();

  if (parent()) {
    parent()->maybe_finalize();
  }
}

void Process::finish() {
  TRACKX(FB_DEBUG_PROC, 1, 1, Process, this, "");

  if (FB_DEBUGGING(FB_DEBUG_PROC) && expected_child_) {
    FB_DEBUG(FB_DEBUG_PROC, "Expected system()/popen()/posix_spawn() children that did not appear"
                            " (e.g. posix_spawn() failed in the pre-exec or exec step):");
    FB_DEBUG(FB_DEBUG_PROC, "  " + d(expected_child_));
  }

  if (!exec_pending_ && !pending_popen_child_) {
    /* The pending child may show up and it needs to inherit the pipes. */
    reset_file_fd_pipe_refs();
  }

  set_state(FB_PROC_TERMINATED);
  /* Here we check only the fork children. In theory this parent could exec() and the
   * execed process could wait for its children, but this is rare and costly to detect thus
   * we disable shortcutting in more cases than it is absolutely needed.
   */
  for (Process* fork_child : fork_children()) {
    if (!fork_child->been_waited_for()) {
      Process* last_exec_descendant = fork_child;
      while (last_exec_descendant->exec_child()) {
        last_exec_descendant = last_exec_descendant->exec_child();
      }
      /* This disables shortcutting the fork child and maybe finalizes it. Since shortcutting is
       * disabled up to the top process this process will not be shortcuttable either. */
      last_exec_descendant->maybe_finalize();
    }
  }

  maybe_finalize();
}

void Process::export2js_recurse(const unsigned int level, FILE* stream,
                                unsigned int *nodeid) {
  if (exec_child() != NULL) {
    exec_child_->export2js_recurse(level + 1, stream, nodeid);
  }
  for (auto& fork_child : fork_children_) {
    fork_child->export2js_recurse(level, stream, nodeid);
  }
}

/* Member debugging method. Not to be called directly, call the global d(obj_or_ptr) instead.
 * level is the nesting level of objects calling each other's d(), bigger means less info to print.
 * See #431 for design and rationale. */
std::string Process::d_internal(const int level) const {
  (void)level;  /* unused */
  return "{Process " + pid_and_exec_count() + "}";
}

Process::~Process() {
  TRACKX(FB_DEBUG_PROC, 1, 0, Process, this, "");

  free(pending_popen_fifo_);
  delete(expected_child_);
  delete(fds_);
}

/* Global debugging methods.
 * level is the nesting level of objects calling each other's d(), bigger means less info to print.
 * See #431 for design and rationale. */
std::string d(const Process& p, const int level) {
  return p.d_internal(level);
}
std::string d(const Process *p, const int level) {
  if (p) {
    return d(*p, level);
  } else {
    return "{Process NULL}";
  }
}

}  /* namespace firebuild */

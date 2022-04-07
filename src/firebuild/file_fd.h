/* Copyright (c) 2014 Balint Reczey <balint@balintreczey.hu> */
/* This file is an unpublished work. All rights reserved. */

#ifndef FIREBUILD_FILE_FD_H_
#define FIREBUILD_FILE_FD_H_

#include <fcntl.h>

#include <cassert>
#include <memory>
#include <string>

#include "firebuild/file.h"
#include "firebuild/file_name.h"
#include "firebuild/cxx_lang_utils.h"
#include "firebuild/pipe.h"

namespace firebuild {
enum fd_origin : char {
  FD_ORIGIN_FILE_OPEN, /* backed by open()-ed file */
  FD_ORIGIN_INTERNAL,  /* backed by memory (e.g. using fmemopen()) */
  FD_ORIGIN_PIPE,      /* pipe endpoint (e.g. using pipe()) */
  FD_ORIGIN_DUP,       /* created using dup() */
  FD_ORIGIN_ROOT,       /* inherited in the root process (stdin...) */
};

class Process;
class Pipe;

class FileFD {
 public:
  /** Constructor for fds inherited from the supervisor (stdin, stdout, stderr). */
  FileFD(int fd, int flags)
      : fd_(fd), curr_flags_(flags), origin_type_(FD_ORIGIN_ROOT), origin_fd_(NULL),
        filename_(), pipe_(), opened_by_(NULL) {
    assert(fd >= 0);
  }
  /** Constructor for fds backed by internal memory. */
  FileFD(int fd, int flags, Process * const p)
      : fd_(fd), curr_flags_(flags), origin_type_(FD_ORIGIN_INTERNAL), origin_fd_(NULL),
        filename_(), pipe_(), opened_by_(p) {
    assert(fd >= 0);
  }
  /** Constructor for fds backed by a pipe including ones created by popen(). */
  FileFD(int fd, int flags, std::shared_ptr<Pipe> pipe, Process * const p,
         bool close_on_popen = false)
      : fd_(fd), curr_flags_(flags), origin_type_(FD_ORIGIN_PIPE), close_on_popen_(close_on_popen),
        origin_fd_(NULL),
        filename_(), pipe_(pipe), opened_by_(p) {
    assert(fd >= 0);
  }
  /** Constructor for fds created from other fds through dup() or exec() */
  FileFD(int fd, int flags, fd_origin o, std::shared_ptr<FileFD> o_fd)
      : fd_(fd), curr_flags_(flags), origin_type_(o), origin_fd_(o_fd),
        filename_(o_fd->filename()), pipe_(o_fd->pipe_),
        opened_by_(o_fd->opened_by()) {
    assert(fd >= 0);
    if (filename_ && is_write(curr_flags_)) {
      filename_->open_for_writing();
    }
    if (pipe_) {
      pipe_->handle_dup(o_fd.get(), this);
    }
  }
  /** Constructor for fds obtained through opening files. */
  FileFD(const FileName* f, int fd, int flags, Process * const p)
      : fd_(fd), curr_flags_(flags), origin_type_(FD_ORIGIN_FILE_OPEN), origin_fd_(NULL),
        filename_(f), pipe_(), opened_by_(p) {
    assert(fd >= 0);
    if (is_write(curr_flags_)) {
      f->open_for_writing();
    }
  }
  FileFD(const FileFD& other)
      : fd_(other.fd_), curr_flags_(other.curr_flags_), origin_type_(other.origin_type_),
        close_on_popen_(other.close_on_popen_), read_(other.read_), written_(other.written_),
        origin_fd_(other.origin_fd_), filename_(other.filename_), pipe_(other.pipe_),
        opened_by_(other.opened_by_) {
    if (filename_ && is_write(curr_flags_)) {
      filename_->open_for_writing();
    }
  }
  FileFD& operator= (const FileFD& other) {
    fd_ = other.fd_;
    origin_type_ = other.origin_type_;
    close_on_popen_ = other.close_on_popen_;
    read_ = other.read_;
    written_ = other.written_;
    origin_fd_ = other.origin_fd_;
    if (filename_ != other.filename_) {
      if (filename_ && is_write(curr_flags_)) {
        filename_->close_for_writing();
      }
      filename_ = other.filename_;
      if (filename_ && is_write(other.curr_flags_)) {
        filename_->open_for_writing();
      }
    }
    curr_flags_ = other.curr_flags_;
    pipe_ = other.pipe_;
    opened_by_ = other.opened_by_;
    return *this;
  }
  ~FileFD() {
    if (is_write(curr_flags_) && filename_) {
      filename_->close_for_writing();
    }
  }
  int fd() const {return fd_;}
  int flags() const {return curr_flags_;}
  Process * opened_by() {return opened_by_;}
  bool cloexec() const {return curr_flags_ & O_CLOEXEC;}
  void set_cloexec(bool value) {
    if (value) {
      curr_flags_ |= O_CLOEXEC;
    } else {
      curr_flags_ &= ~O_CLOEXEC;
    }
  }
  fd_origin origin_type() {return origin_type_;}
  bool close_on_popen() const {return close_on_popen_;}
  void set_close_on_popen(bool c) {close_on_popen_ = c;}
  bool read() {return read_;}
  bool written() {return written_;}
  const FileName* filename() const {return filename_;}
  void set_pipe(std::shared_ptr<Pipe> pipe) {
    assert((origin_type_ == FD_ORIGIN_ROOT && !pipe_) || pipe_);
    if (pipe_) {
      pipe_->handle_close(this);
    }
    pipe_ = pipe;
  }
  std::shared_ptr<Pipe> pipe() {return pipe_;}
  const std::shared_ptr<Pipe> pipe() const {return pipe_;}

 private:
  int fd_;
  int curr_flags_;
  fd_origin origin_type_;
  bool close_on_popen_ = false;
  bool read_ = false;
  bool written_ = false;
  std::shared_ptr<FileFD> origin_fd_;
  const FileName* filename_;
  std::shared_ptr<Pipe> pipe_;
  /** Process that opened this file by name.
   *  Remains the same (doesn't get updated to the current process) at dup2() or alike.
   *  NULL if the topmost intercepted process already inherited it from the supervisor. */
  Process* opened_by_;
};

/* Global debugging methods.
 * level is the nesting level of objects calling each other's d(), bigger means less info to print.
 * See #431 for design and rationale. */
std::string d(const FileFD& ffd, const int level = 0);
std::string d(const FileFD *ffd, const int level = 0);

}  /* namespace firebuild */
#endif  // FIREBUILD_FILE_FD_H_

/* Copyright (c) 2014 Balint Reczey <balint@balintreczey.hu> */
/* This file is an unpublished work. All rights reserved. */


#ifndef FIREBUILD_EXECED_PROCESS_H_
#define FIREBUILD_EXECED_PROCESS_H_

// Workaround for https://github.com/Tessil/hopscotch-map/issues/55
#ifndef __clang__
#pragma GCC optimize ("-fno-strict-aliasing")   // NOLINT(whitespace/parens)
#endif
#include <tsl/hopscotch_map.h>
#include <tsl/hopscotch_set.h>
#ifndef __clang__
#pragma GCC reset_options
#endif

#include <cassert>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "firebuild/file_info.h"
#include "firebuild/file_name.h"
#include "firebuild/file_usage.h"
#include "firebuild/file_usage_update.h"
#include "firebuild/pipe.h"
#include "firebuild/pipe_recorder.h"
#include "firebuild/process.h"
#include "firebuild/cxx_lang_utils.h"
#include "firebuild/debug.h"

namespace firebuild {

class ExecedProcessCacher;

/**
 * Represents one open file description that this process inherited, along with the list of
 * corresponding file descriptors. (An open file description might have multiple file descriptors,
 * as per dup() and friends. They are stored in ascending order. There's at least one fd.)
 *
 * The structure always refers to how things were when the process started,
 * it isn't modified later as the process does various things with its file descriptors.
 *
 * Accordingly, for pipes, it does not hold a pointer to the Pipe object, since that one might go
 * away while we still need to keep this structure.
 */
typedef struct inherited_file_ {
  /* Type. */
  fd_type type {FD_UNINITIALIZED};
  /* The client-side file descriptor numbers, sorted */
  std::vector<int> fds {};
  /* For FD_PIPE_OUT only: The recorder of the traffic, as seen from this exec point */
  std::shared_ptr<PipeRecorder> recorder {};
} inherited_file_t;

class ExecedProcess : public Process {
 public:
  explicit ExecedProcess(const int pid, const int ppid, const FileName *initial_wd,
                         const FileName *executable, const FileName *executed_path,
                         char* original_executed_path,
                         const std::vector<std::string>& args,
                         const std::vector<std::string>& env_vars,
                         const std::vector<const FileName*>& libs,
                         const mode_t umask,
                         Process * parent,
                         std::vector<std::shared_ptr<FileFD>>* fds);
  virtual ~ExecedProcess();
  virtual bool exec_started() const {return true;}
  ExecedProcess* exec_point() {return this;}
  const ExecedProcess* exec_point() const {return this;}
  ExecedProcess* common_exec_ancestor(ExecedProcess* other);
  ForkedProcess* fork_point() {return fork_point_;}
  const ForkedProcess* fork_point() const {return fork_point_;}
  void set_parent(Process *parent);
  bool been_waited_for() const;
  void set_been_waited_for();
  void add_utime_u(int64_t t) {utime_u_ += t;}
  int64_t utime_u() const {return utime_u_;}
  void add_stime_u(int64_t t) {stime_u_ += t;}
  int64_t stime_u() const {return stime_u_;}
  int64_t cpu_time_u() const {return utime_u_ + stime_u_;}
  void add_children_cpu_time_u(const int64_t t) {children_cpu_time_u_ += t;}
  int64_t aggr_cpu_time_u() const {return cpu_time_u() + children_cpu_time_u_;}
  const FileName* initial_wd() const {return initial_wd_;}
  const tsl::hopscotch_set<const FileName*>& wds() const {return wds_;}
  const tsl::hopscotch_set<const FileName*>& wds() {return wds_;}
  const tsl::hopscotch_set<const FileName*>& failed_wds() const {return wds_;}
  tsl::hopscotch_set<const FileName*>& failed_wds() {return failed_wds_;}
  const std::vector<std::string>& args() const {return args_;}
  std::vector<std::string>& args() {return args_;}
  const std::vector<std::string>& env_vars() const {return env_vars_;}
  std::vector<std::string>& env_vars() {return env_vars_;}
  const FileName* executable() const {return executable_;}
  const FileName* executed_path() const {return executed_path_;}
  const char* original_executed_path() const {return original_executed_path_;}
  std::vector<const FileName*>& libs() {return libs_;}
  const std::vector<const FileName*>& libs() const {return libs_;}
  tsl::hopscotch_map<const FileName*, const FileUsage*>& file_usages() {return file_usages_;}
  const tsl::hopscotch_map<const FileName*, const FileUsage*>& file_usages() const {
    return file_usages_;
  }
  void set_cacher(ExecedProcessCacher *cacher) {cacher_ = cacher;}
  void do_finalize();
  void set_on_finalized_ack(int id, int fd);
  Process* exec_proc() const {return const_cast<ExecedProcess*>(this);}
  void resource_usage(const int64_t utime_u, const int64_t stime_u);

  void initialize();
  bool register_file_usage_update(const FileName *name, const FileUsageUpdate& update);
  bool register_parent_directory(const FileName *name, FileType type = ISDIR);
  void add_pipe(std::shared_ptr<Pipe> pipe) {created_pipes_.insert(pipe);}
  std::vector<inherited_file_t>& inherited_files() {return inherited_files_;}
  const std::vector<inherited_file_t>& inherited_files() const {return inherited_files_;}
  void set_inherited_files(std::vector<inherited_file_t> inherited_files)
      {inherited_files_ = inherited_files;}

  /**
   * Fail to change to a working directory
   */
  void handle_fail_wd(const char * const d) {
    failed_wds_.insert(FileName::Get(d));
  }
  /**
   * Record visited working directory
   */
  void add_wd(const FileName *d) {
    wds_.insert(d);
  }

  /** Returns if the process can be short-cut */
  bool can_shortcut() const {return can_shortcut_;}

  bool shortcut();

  /**
   * This particular process can't be short-cut because it performed calls preventing that.
   * @param reason reason for can't being short-cut
   * @param p process the event preventing shortcutting happened in, or
   *     omitted for the current process
   */
  virtual void disable_shortcutting_only_this(const char* reason,
                                              const ExecedProcess *p = NULL);
  /**
   * Process and parents (transitively) up to (excluding) "stop" can't be short-cut because
   * it performed calls preventing that.
   * @param stop Stop before this process
   * @param reason reason for can't being short-cut
   * @param p process the event preventing shortcutting happened in, or
   *     omitted for the current process
   * @param shortcutable_ancestor this ancestor will be the nearest shortcutable ancestor
   *        for all visited execed processes after this call
   *        (when shortcutable_ancestor_is_set is true)
   * @param shortcutable_ancestor_is_set the shortcutable_ancestor is computed
   */
  void disable_shortcutting_bubble_up_to_excl(ExecedProcess *stop, const char* reason,
                                              const ExecedProcess *p = NULL,
                                              ExecedProcess *shortcutable_ancestor = nullptr,
                                              bool shortcutable_ancestor_is_set = false);
  void disable_shortcutting_bubble_up_to_excl(ExecedProcess *stop, const char* reason, int fd,
                                              const ExecedProcess *p = NULL,
                                              ExecedProcess *shortcutable_ancestor = nullptr,
                                              bool shortcutable_ancestor_is_set = false);
  /**
   * Process and parents (transitively) can't be short-cut because it performed
   * calls preventing that.
   * @param reason reason for can't being short-cut
   * @param p process the event preventing shortcutting happened in, or
   *     omitted for the current process
   */
  void disable_shortcutting_bubble_up(const char* reason, const ExecedProcess *p = NULL);
  void disable_shortcutting_bubble_up(const char* reason, const int fd,
                                      const ExecedProcess *p = NULL);
  void disable_shortcutting_bubble_up(const char* reason, const FileName& file,
                                      const ExecedProcess *p = NULL);
  void disable_shortcutting_bubble_up(const char* reason, const std::string& str,
                                      const ExecedProcess *p = NULL);

  bool was_shortcut() const {return was_shortcut_;}
  void set_was_shortcut(bool value) {was_shortcut_ = value;}

  void export2js(const unsigned int level, FILE* stream,
                 unsigned int * nodeid);
  void export2js_recurse(const unsigned int level, FILE* stream,
                         unsigned int *nodeid);

  std::string args_to_short_string() const;
  /* Member debugging method. Not to be called directly, call the global d(obj_or_ptr) instead.
   * level is the nesting level of objects calling each other's d(), bigger means less info to print.
   * See #431 for design and rationale. */
  virtual std::string d_internal(const int level = 0) const;

 private:
  bool can_shortcut_:1;
  bool was_shortcut_:1;
  /** If points to this (self), the process can be shortcut.
      Otherwise the process itself is not shortcutable, but the ancestor is, if the ancestor's
      maybe_shortcutable_ancestor points at itself, etc. */
  ForkedProcess *fork_point_ {};
  ExecedProcess * maybe_shortcutable_ancestor_;
  /** Sum of user time in microseconds for all forked but not exec()-ed children */
  int64_t utime_u_ = 0;
  /** Sum of system time in microseconds for all forked but not exec()-ed children */
  int64_t stime_u_ = 0;
  /** Sum of user and system time in microseconds for all finalized exec()-ed children */
  int64_t children_cpu_time_u_ = 0;
  /** Directory the process exec()-started in */
  const FileName* initial_wd_;
  /** Working directories visited by the process and all fork()-children */
  tsl::hopscotch_set<const FileName*> wds_;
  /** Working directories the process and all fork()-children failed to chdir() to */
  tsl::hopscotch_set<const FileName*> failed_wds_;
  std::vector<std::string> args_;
  /** Environment variables in deterministic (sorted) order. */
  std::vector<std::string> env_vars_;
  /**
   * The executable running. In case of scripts this is the interpreter or in case of invoking
   * an executable via a symlink this is the executable the symlink points to. */
  const FileName* executable_;
  /**
   * The path executed converted to absolute and canonical form.
   * In case of scripts this is the script's name or in case of invoking executable via a symlink
   * this is the name of the symlink. */
  const FileName* executed_path_;
  /**
    * The path executed. In case of scripts this is the script's name or in case of invoking
    * executable via a symlink this is the name of the symlink.
    * May be not absolute nor canonical, like ./foo.
    * It may point to executed_path_.c_str() and in that case it should not be freed. */
  char* original_executed_path_;
  /**
   * DSO-s loaded by the linker at process startup, in the same order.
   * (DSO-s later loaded via dlopen(), and DSO-s of descendant processes are registered as regular
   * file open operations.) */
  std::vector<const FileName*> libs_;
  /** File usage per path for p and f. c. (t.) */
  tsl::hopscotch_map<const FileName*, const FileUsage*> file_usages_;
  /**
   * Pipes created by this process.
   */
  tsl::hopscotch_set<std::shared_ptr<Pipe>> created_pipes_ = {};
  /**
   * The files this process had at startup, grouped by "open file description".
   * Each such "open file description" might have multiple client-side file descriptors (see dup()
   * and friends), they are in sorted order. Also, this inherited_files_ array is sorted according
   * to the first (lowest) fd for each inherited file.
   */
  std::vector<inherited_file_t> inherited_files_ = {};
  void store_in_cache();
  ExecedProcess* next_shortcutable_ancestor() {
    if (maybe_shortcutable_ancestor_ == nullptr || maybe_shortcutable_ancestor_->can_shortcut_) {
      return maybe_shortcutable_ancestor_;
    } else {
      ExecedProcess* next = maybe_shortcutable_ancestor_->maybe_shortcutable_ancestor_;
      while (next != nullptr && !next->can_shortcut_)  {
        next = next->maybe_shortcutable_ancestor_;
      }
      maybe_shortcutable_ancestor_ = next;
      return next;
    }
  }
  ExecedProcess* closest_shortcut_point() {
    return can_shortcut() ? this : next_shortcutable_ancestor();
  }
  /** Reason for this process can't be short-cut */
  const char* cant_shortcut_reason_ = nullptr;
  /** Process the event preventing short-cutting happened in */
  const Process *cant_shortcut_proc_ = NULL;
  /**
   * Helper object for storing in / retrieving from cache.
   * NULL if we prefer not to (although probably could)
   * cache / shortcut this process. */
  ExecedProcessCacher *cacher_;
  DISALLOW_COPY_AND_ASSIGN(ExecedProcess);
};


}  /* namespace firebuild */
#endif  // FIREBUILD_EXECED_PROCESS_H_

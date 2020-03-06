/* Copyright (c) 2014 Balint Reczey <balint@balintreczey.hu> */
/* This file is an unpublished work. All rights reserved. */


#include "firebuild/execed_process.h"

#include <map>
#include <sstream>

#include <libconfig.h++>

#include "firebuild/file_db.h"
#include "firebuild/execed_process_cacher.h"
#include "firebuild/platform.h"
#include "firebuild/utils.h"

extern libconfig::Config * cfg;

namespace firebuild {

/**
 * Escape std::string for JavaScript
 * from http://stackoverflow.com/questions/7724448/simple-json-string-escape-for-c
 * TODO: use JSONCpp instead to handle all cases
 */
static std::string escapeJsonString(const std::string& input) {
  std::ostringstream ss;
  for (auto iter = input.cbegin(); iter != input.cend(); iter++) {
    switch (*iter) {
      case '\\': ss << "\\\\"; break;
      case '"': ss << "\\\""; break;
      case '/': ss << "\\/"; break;
      case '\b': ss << "\\b"; break;
      case '\f': ss << "\\f"; break;
      case '\n': ss << "\\n"; break;
      case '\r': ss << "\\r"; break;
      case '\t': ss << "\\t"; break;
      default: ss << *iter; break;
    }
  }
  return ss.str();
}

ExecedProcess::ExecedProcess(const int pid, const int ppid,
                             const std::string &cwd,
                             const std::string &executable,
                             Process * parent)
    : Process(pid, ppid, cwd, parent, true), can_shortcut_(true), was_shortcut_(false),
      sum_utime_u_(0), sum_stime_u_(0), cwd_(cwd),
      wds_(), failed_wds_(), args_(), env_vars_(), executable_(executable),
      libs_(), file_usages_(), cacher_(NULL) {
  if (NULL != parent) {
    // add as exec child of parent
    parent->set_exec_pending(false);
    parent->set_state(FB_PROC_TERMINATED);
    // clear a previous exit status, just in case an atexit handler performed the exec
    parent->set_exit_status(-1);
    parent->set_exec_child(this);
  }
}

/**
 * Initialization stuff that can only be done after placing the
 * ExecedProcess in the ProcessTree.
 */
void ExecedProcess::initialize() {
  /* Propagate the opening of the executable and libraries upwards as
   * regular file open events. */
  if (parent_exec_point()) {
    parent_exec_point()->register_file_usage(executable(), O_RDONLY, 0);
    for (auto lib : libs()) {
      parent_exec_point()->register_file_usage(lib, O_RDONLY, 0);
    }
  }
}

void ExecedProcess::propagate_exit_status(const int status) {
  if (parent()) {
    parent()->set_exit_status(status);
    parent()->propagate_exit_status(status);
  }
}

void ExecedProcess::exit_result(const int status, const int64_t utime_u,
                                const int64_t stime_u) {
  // store results for this process
  Process::exit_result(status, utime_u, stime_u);
  // propagate to parents exec()-ed this FireBuild process
  propagate_exit_status(status);
}

void ExecedProcess::do_finalize() {
  // store data for shortcutting
  if (cacher_ && !was_shortcut() && can_shortcut()) {
    cacher_->store(this);
  }

  // Call the base class's method
  Process::do_finalize();
}

/**
 * This is called on file operation methods, for all the ExecedProcess
 * ancestors, excluding the exec_point where the file operation
 * occurred. The method does the necessary administration, and bubbles
 * it upwards to the root.
 *
 * In case of non-shortcutted processes, in the exec_point the method
 * register_file_usage() performed the necessary administration before
 * beginning to bubble up this event.
 *
 * In case of shortcutted processes, it's the shortcutting itself that
 * performs the file operations, no administration is necessary there.
 */
void ExecedProcess::propagate_file_usage(const std::string &name,
                                         const FileUsage &fu_change) {
  FileUsage *fu;
  if (file_usages().count(name) > 0) {
    fu = file_usages()[name];
  } else {
    fu = new FileUsage();
    file_usages()[name] = fu;
  }
  fu->merge(fu_change);
  if (parent_exec_point()) {
    parent_exec_point()->propagate_file_usage(name, fu_change);
  }
}

/**
 * This is called on the exec_point of a non-shortcutted process when an
 * open() or similar call is intercepted. Converts the input into a
 * FileUsage, stat'ing the file, computing its checksum if necessary.
 * Registers the file operation, and bubbles it upwards to the root via
 * propagate_file_usage().
 */
bool ExecedProcess::register_file_usage(const std::string &name,
                                        const int flags,
                                        const int error) {
  libconfig::Setting& ignores = cfg->getRoot()["ignore_locations"];
  for (int i = 0; i < ignores.getLength(); i++) {
    if (path_begins_with(name, ignores[i])) {
      return true;
    }
  }

  FileUsage *fu;
  if (file_usages().count(name) > 0) {
    /* The process already used this file. The initial state was already
     * recorded. We create a new FileUsage object which represents the
     * modifications to apply currently, which is at most the written_
     * flag, and then we propagate this upwards to be applied.
     */
    fu = file_usages()[name];
    FileUsage *fu_change = new FileUsage();
    if (!fu_change->update_from_open_params(name, flags, error, false)) {
      /* Error */
      return false;
    }
    fu->merge(*fu_change);
    if (parent_exec_point()) {
      parent_exec_point()->propagate_file_usage(name, *fu_change);
    }
    delete fu_change;
  } else {
    /* The process opens this file for the first time. Compute whatever
     * we need to know about its initial state. Use that same object to
     * propagate the changes upwards. */
    fu = new FileUsage();
    if (!fu->update_from_open_params(name, flags, error, true)) {
      /* Error */
      delete fu;
      return false;
    }
    file_usages()[name] = fu;
    if (parent_exec_point()) {
      parent_exec_point()->propagate_file_usage(name, *fu);
    }
  }
  return true;
}

/* Find and apply shortcut */
bool ExecedProcess::shortcut() {
  if (can_shortcut() && cacher_) {
    return cacher_->shortcut(this);
  } else {
    FB_DEBUG(FB_DEBUG_SHORTCUT, "┌─");
    FB_DEBUG(FB_DEBUG_SHORTCUT, "│ Shortcutting disabled:");
    FB_DEBUG(FB_DEBUG_SHORTCUT, "│   exe = " + pretty_print_string(executable()));
    FB_DEBUG(FB_DEBUG_SHORTCUT, "│   arg = " + pretty_print_array(args()));
    /* FB_DEBUG(FB_DEBUG_SHORTCUT, "│   env = " + pretty_print_array(env_vars())); */
    FB_DEBUG(FB_DEBUG_SHORTCUT, "└─");
    return false;
  }
}

int64_t ExecedProcess::sum_rusage_recurse() {
  int64_t aggr_time = utime_u() + stime_u();
  sum_utime_u_ = 0;
  sum_stime_u_ = 0;
  sum_rusage(&sum_utime_u_, &sum_stime_u_);
  if (parent() && parent()->pid() == pid()) {
    sum_utime_u_ -= parent()->utime_u();
    sum_stime_u_ -= parent()->stime_u();
    aggr_time -= parent()->utime_u();
    aggr_time -= parent()->stime_u();
  }
  set_aggr_time(aggr_time);
  return Process::sum_rusage_recurse();
}

void ExecedProcess::export2js_recurse(const unsigned int level, FILE* stream,
                                      unsigned int *nodeid) {
  if (level > 0) {
    fprintf(stream, "\n");
  }
  fprintf(stream, "%s{", std::string(2 * level, ' ').c_str());

  export2js(level, stream, nodeid);
  fprintf(stream, "%s children: [", std::string(2 * level, ' ').c_str());
  Process::export2js_recurse(level, stream, nodeid);
  if (level == 0) {
    fprintf(stream, "]};\n");
  } else {
    fprintf(stream, "]},\n");
  }
}


void ExecedProcess::export2js(const unsigned int level,
                              FILE* stream, unsigned int * nodeid) {
  // TODO(rbalint): escape all strings properly
  auto indent_str = std::string(2 * level, ' ');
  const char* indent = indent_str.c_str();

  fprintf(stream, "name:\"%s\",\n", args()[0].c_str());
  fprintf(stream, "%s id: %u,\n", indent, (*nodeid)++);
  fprintf(stream, "%s pid: %u,\n", indent, pid());
  fprintf(stream, "%s ppid: %u,\n", indent, ppid());
  fprintf(stream, "%s fb_pid: %u,\n", indent, fb_pid());
  fprintf(stream, "%s cwd:\"%s\",\n", indent, cwd().c_str());
  fprintf(stream, "%s exe:\"%s\",\n", indent, executable().c_str());
  fprintf(stream, "%s state: %u,\n", indent, state());
  if (!can_shortcut_) {
    fprintf(stream, "%s cant_sc_reason: \"%s\",\n", indent, cant_shortcut_reason_.c_str());
    if (cant_shortcut_proc_->exec_proc()->fb_pid() != fb_pid()) {
      fprintf(stream, "%s cant_sc_fb_pid: \"%u\",\n", indent, cant_shortcut_proc_->exec_proc()->fb_pid());
    }
  }
  fprintf(stream, "%s args: [", indent);
  for (auto arg : args()) {
    fprintf(stream, "\"%s\",", escapeJsonString(arg).c_str());
  }
  fprintf(stream, "],\n");

  fprintf(stream, "%s env: [", indent);
  for (auto env : env_vars()) {
    fprintf(stream, "\"%s\",", escapeJsonString(env).c_str());
  }
  fprintf(stream, "],\n");

  fprintf(stream, "%s libs: [", indent);
  for (auto lib : libs()) {
    fprintf(stream, "\"%s\",", lib.c_str());
  }
  fprintf(stream, "],\n");

  fprintf(stream, "%s wds: [", indent);
  for (auto wd : wds()) {
    fprintf(stream, "\"%s\",", wd.c_str());
  }
  fprintf(stream, "],\n");

  fprintf(stream, "%s failed_wds: [", indent);
  for (auto f_wd : failed_wds()) {
    fprintf(stream, "\"%s\",", f_wd.c_str());
  }
  fprintf(stream, "],\n");

  // sort files before printing
  std::map<std::string, FileUsage*> ordered_file_usages;
  for (auto pair : file_usages()) {
    ordered_file_usages[pair.first] =  pair.second;
  }

  fprintf(stream, "%s fcreated: [", indent);
  for (auto pair : ordered_file_usages) {
    if (pair.second->initial_state() != EXIST_WITH_HASH && pair.second->written()) {
      fprintf(stream, "\"%s\",", pair.first.c_str());
    }
  }
  fprintf(stream, "],\n");

  fprintf(stream, "%s fmodified: [", indent);
  for (auto pair : ordered_file_usages) {
    if (pair.second->initial_state() == EXIST_WITH_HASH && pair.second->written()) {
      fprintf(stream, "\"%s\",", pair.first.c_str());
    }
  }
  fprintf(stream, "],\n");

  fprintf(stream, "%s fread: [", indent);
  for (auto pair : ordered_file_usages) {
    if (pair.second->initial_state() == EXIST_WITH_HASH && !pair.second->written()) {
      fprintf(stream, "\"%s\",", pair.first.c_str());
    }
  }
  fprintf(stream, "],\n");

  fprintf(stream, "%s fnotf: [", indent);
  for (auto pair : ordered_file_usages) {
    if (pair.second->initial_state() != EXIST_WITH_HASH && !pair.second->written()) {
      fprintf(stream, "\"%s\",", pair.first.c_str());
    }
  }
  fprintf(stream, "],\n");

  if (state() != FB_PROC_FINALIZED) {
    // something went wrong
  }
  if (exit_status() != -1) {
    fprintf(stream, "%s exit_status: %u,\n", indent, exit_status());
  }
  fprintf(stream, "%s utime_u: %lu,\n", indent, utime_u());
  fprintf(stream, "%s stime_u: %lu,\n", indent, stime_u());
  fprintf(stream, "%s aggr_time: %lu,\n", indent, aggr_time());
  fprintf(stream, "%s sum_utime_u: %lu,\n", indent, sum_utime_u());
  fprintf(stream, "%s sum_stime_u: %lu,\n", indent, sum_stime_u());
}

ExecedProcess::~ExecedProcess() {
  for (auto pair : file_usages()) {
    delete(pair.second);
  }
  if (cacher_) {
    cacher_->erase_fingerprint(this);
  }
}

}  // namespace firebuild

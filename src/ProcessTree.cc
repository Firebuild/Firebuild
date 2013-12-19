#include <stdexcept>
#include <iostream>
#include <sstream>
#include <limits>
#include <math.h>

#include "ProcessTree.h"

using namespace std;
namespace firebuild {
  

/**
 * Escape string for JavaScript
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

ProcessTree::~ProcessTree()
{
  // clean up all processes
  for (auto it = fb_pid2proc.begin(); it != fb_pid2proc.end(); ++it) {
    delete(it->second);
  }
}

void ProcessTree::insert (Process &p, const int sock)
{
  sock2proc[sock] = &p;
  fb_pid2proc[p.fb_pid] = &p;
  pid2proc[p.pid] = &p;
}

void ProcessTree::insert (ExecedProcess &p, const int sock)
{
  if (root == NULL) {
    root = &p;
  } else {
    // add as exec child of parent
    try {
      p.exec_parent = pid2proc.at(p.pid);
      p.exec_parent->exec_child = &p;
      p.exec_parent->state = FB_PROC_EXECED;
    } catch (const std::out_of_range& oor) {
      // root's exec_parent is firebuild which is not in the tree.
      // If any other parent is missing, FireBuild missed process
      // that can happen due to the missing process(es) being statically built
      cerr << "TODO handle: Process without known exec parent\n";
    }
  }

  this->insert((Process&)p, sock);
}

void ProcessTree::insert (ForkedProcess &p, const int sock)
{

  // add as fork child of parent
  try {
    p.fork_parent = pid2proc.at(p.ppid);
    p.fork_parent->children.push_back(&p);
  } catch (const std::out_of_range& oor) {
    if (!(*root == p) && (root->pid != p.pid)) {
      // root's fork_parent is firebuild which is not in the tree.
      // If any other parent is missing, FireBuild missed process
      // that can happen due to the missing process(es) being statically built
      cerr << "TODO handle: Process without known parent\n";
    }
  }

  this->insert((Process&)p, sock);
}

void ProcessTree::exit (Process &p, const int sock)
{
  (void)p;
  // TODO maybe this is not needed
  sock2proc.erase(sock);
}

long int ProcessTree::sum_rusage_recurse(Process &p)
{
  p.aggr_time = p.utime_m + p.stime_m;
  if (p.type == FB_PROC_EXEC_STARTED) {
    ExecedProcess *e = (ExecedProcess*)&p;
    e->sum_rusage(&e->sum_utime_m,
                  &e->sum_stime_m);
    if (e->exec_parent) {
      e->sum_utime_m -= e->exec_parent->utime_m;
      e->sum_stime_m -= e->exec_parent->stime_m;
      e->aggr_time -= e->exec_parent->utime_m;
      e->aggr_time -= e->exec_parent->stime_m;
    }
  }
  if (p.exec_child != NULL) {
    p.aggr_time += sum_rusage_recurse(*p.exec_child);
  }
  for (unsigned int i = 0; i < p.children.size(); i++) {
    p.aggr_time += sum_rusage_recurse(*p.children[i]);
  }
  return p.aggr_time;
}

void ProcessTree::export2js_recurse(Process &p, unsigned int level, ostream& o)
{
  if (p.type == FB_PROC_EXEC_STARTED) {
    if (level > 0) {
      o << endl;
    }
    o << string(2 * level, ' ') << "{";
    export2js((ExecedProcess&)p, level, o);
    o << string(2 * level, ' ') << " children : [";
  }
  if (p.exec_child != NULL) {
    export2js_recurse(*p.exec_child, level + 1, o);
  }
  for (unsigned int i = 0; i < p.children.size(); i++) {
    export2js_recurse(*p.children[i], level, o);
  }
  if (p.type == FB_PROC_EXEC_STARTED) {
    if (level == 0) {
      o << "]};" << endl;
    } else {
      o << "]},";
    }
  }
}

void ProcessTree::export2js(ostream& o)
{
  o << "root = ";
  export2js_recurse(*root, 0, o);
}

void ProcessTree::export2js(ExecedProcess &p, unsigned int level, ostream& o)
{
  // TODO: escape all strings properly
  unsigned int indent = 2 * level;
  o << "name :\"" << p.args[0] << "\"," << endl;
  o << string(indent + 1, ' ') << "id :" << p.fb_pid << "," << endl;
  o << string(indent + 1, ' ') << "pid :" << p.pid << "," << endl;
  o << string(indent + 1, ' ') << "ppid :" << p.ppid << "," << endl;
  o << string(indent + 1, ' ') << "cwd :\"" << p.cwd << "\"," << endl;
  o << string(indent + 1, ' ') << "exe :\"" << p.executable << "\"," << endl;
  o << string(indent + 1, ' ') << "state : " << p.state << "," << endl;
  o << string(indent + 1, ' ') << "args : " << "[";
  for (unsigned int i = 1; i < p.args.size(); i++) {
    o << "\"" << escapeJsonString(p.args[i]) <<"\", ";
  }
  o << "]," << endl;

  o << string(indent + 1, ' ') << "env : " << "[";
  for (auto it = p.env_vars.begin(); it != p.env_vars.end(); ++it) {
    o << "\"" << escapeJsonString(*it) << "\",";
  }
  o << "]," << endl;

  o << string(indent + 1, ' ') << "libs : " << "[";
  for (auto it = p.libs.begin(); it != p.libs.end(); ++it) {
    o << "\"" << *it << "\",";
  }
  o << "]," << endl;

  o << string(indent + 1, ' ') << "fcreated : " << "[";
  for (auto it = p.file_usages.begin(); it != p.file_usages.end(); ++it) {
    if (it->second->created) {
      o << "\"" << it->first << "\",";
    }
  }
  o << "]," << endl;

  // TODO replace write/read flag checks with more accurate tests
  o << string(indent + 1, ' ') << "fmodified : " << "[";
  for (auto it = p.file_usages.begin(); it != p.file_usages.end(); ++it) {
    if ((!it->second->created) && (it->second->open_flags & (O_WRONLY | O_RDWR))) {
      o << "\"" << it->first << "\",";
    }
  }
  o << "]," << endl;

  o << string(indent + 1, ' ') << "fread : " << "[";
  for (auto it = p.file_usages.begin(); it != p.file_usages.end(); ++it) {
    if (it->second->open_flags & (O_RDONLY | O_RDWR)) {
      o << "\"" << it->first << "\",";
    }
  }
  o << "]," << endl;

  switch (p.state) {
    case FB_PROC_FINISHED: {
      o << string(indent + 1, ' ') << "exit_status : " << p.exit_status << "," << endl;
      // break; is missing intentionally
    }
    case FB_PROC_EXECED: {
      o << string(indent + 1, ' ') << "utime_m : " << p.utime_m << "," << endl;
      o << string(indent + 1, ' ') << "stime_m : " << p.stime_m << "," << endl;
      o << string(indent + 1, ' ') << "aggr_time : " << p.aggr_time << "," << endl;
      o << string(indent + 1, ' ') << "sum_utime_m : " << p.sum_utime_m << "," << endl;
      o << string(indent + 1, ' ') << "sum_stime_m : " << p.sum_stime_m << "," << endl;
      // break; is missing intentionally
    }
    case FB_PROC_RUNNING: {
      // something went wrong
    }
  }
}

void ProcessTree::profile_collect_cmds(Process &p,
                                       unordered_map<string, subcmd_prof> &cmds,
                                       set<string> &ancestors)
{
  if (p.exec_child != NULL) {
    ExecedProcess *ec = (ExecedProcess*)(p.exec_child);
    if (0 == ancestors.count(ec->args[0])) {
      cmds[ec->args[0]].sum_aggr_time += p.exec_child->aggr_time;
    } else {
      if (!cmds[ec->args[0]].recursed) {
        cmds[ec->args[0]].recursed = true;
      }
    }
    cmds[ec->args[0]].count += 1;
  }
  for (unsigned int i = 0; i < p.children.size(); i++) {
    profile_collect_cmds(*p.children[i], cmds, ancestors);
  }

}

void ProcessTree::build_profile(Process &p, set<string> &ancestors)
{
  bool first_visited = false;
  if (p.type == FB_PROC_EXEC_STARTED) {
    ExecedProcess *e = (ExecedProcess*)&p;
    auto &cmd_prof = cmd_profs[e->args[0]];
    if (0 == ancestors.count(e->args[0])) {
      cmd_prof.aggr_time += e->aggr_time;
      ancestors.insert(e->args[0]);
      first_visited = true;
    }
    cmd_prof.cmd_time += e->sum_utime_m +  e->sum_stime_m;
    profile_collect_cmds(p, cmd_prof.subcmds, ancestors);
  }
  if (p.exec_child != NULL) {
    build_profile(*p.exec_child, ancestors);
  }
  for (unsigned int i = 0; i < p.children.size(); i++) {
    build_profile(*(p.children[i]), ancestors);
  }

  if (first_visited) {
    ancestors.erase(((ExecedProcess*)&p)->args[0]);
  }
}


/**
 * Convert HSL color to HSV color
 *
 * From http://ariya.blogspot.hu/2008/07/converting-between-hsl-and-hsv.html
 */
static void hsl_to_hsv(const double hh, const double ss, const double ll,
                       double *h, double *s, double *v)
{
  double ss_tmp;
  *h = hh;
  ss_tmp = ss * ((ll <= 0.5) ? ll : 1 - ll);
  *v = ll + ss_tmp;
  *s = (2 * ss_tmp) / (ll + ss_tmp);
}

/**
 * Ratio to HSL color string
 * @param r 0.0 .. 1.0
 */
static string pct_to_hsv_str(const double p) {
  const double hsl_min[] = {2.0/3.0, 0.80, 0.25}; // blue
  const double hsl_max[] = {0.0, 1.0, 0.5}; // red
  const double r = p / 100;
  double hsl[3];
  double hsv[3];

  hsl[0] = hsl_min[0] + r * (hsl_max[0] - hsl_min[0]);
  hsl[1] = hsl_min[1] + r * (hsl_max[1] - hsl_min[1]);
  hsl[2] = hsl_min[2] + r * (hsl_max[2] - hsl_min[2]);
  hsl_to_hsv(hsl[0], hsl[1], hsl[2], &(hsv[0]), &(hsv[1]), &(hsv[2]));

  return to_string(hsv[0]) + ", " + to_string(hsv[1]) + ", " + to_string(hsv[2]);
}

static double percent_of (double val, double of)
{
  return (((of < numeric_limits<double>::epsilon()) &&
           (of > -numeric_limits<double>::epsilon()))?(0.0):
          (round(val * 10000 / of) / 100));
}

void ProcessTree::export_profile2dot(ostream &o)
{
  set<string> cmd_chain;
  double min_penwidth = 1, max_penwidth = 8;
  long int build_time;

  // build profile
  build_profile(*root, cmd_chain);
  build_time = root->aggr_time;

  // print it
  o << "digraph {" << endl;
  o << "graph [dpi=63, ranksep=0.25, rankdir=LR, bgcolor=transparent,";
  o << " fontname=Helvetica, fontsize=12, nodesep=0.125];" << endl;
  o << "node [fontname=Helvetica, fontsize=12, style=filled, height=0, width=0, shape=box, fontcolor=white];" << endl;
  o << "edge [fontname=Helvetica, fontsize=12]" << endl;

  for (auto it = cmd_profs.begin(); it != cmd_profs.end(); ++it) {
    o << string(4, ' ') << "\"" << it->first << "\" [label=<<B>";
    o << it->first << "</B><BR/>";
    o << percent_of(it->second.aggr_time, build_time) << "%<BR/>(";
    o << percent_of(it->second.cmd_time, build_time);
    o << "%)>, color=\"" << pct_to_hsv_str(percent_of(it->second.aggr_time, build_time)) << "\"];" << endl;
    for (auto it2 = it->second.subcmds.begin(); it2 != it->second.subcmds.end(); ++it2) {
      o << string(4, ' ') << "\"" << it->first << "\" -> \""<< it2->first << "\" [label=\"" ;
      if (!it2->second.recursed) {
        o << percent_of(it2->second.sum_aggr_time, build_time) << "%\\n";
      }
      o << it2->second.count << "×\", color=\"";
      o << pct_to_hsv_str(percent_of(it2->second.sum_aggr_time, build_time));
      o << "\"," << " penwidth=\"";
      o << (min_penwidth  + ((percent_of(it2->second.sum_aggr_time, build_time) / 100)
                             * (max_penwidth - min_penwidth)));
      o << "\"];" << endl;
    }
  }

  o << "}" << endl;
}
}


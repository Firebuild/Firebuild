/* Copyright (c) 2014 Balint Reczey <balint@balintreczey.hu> */
/* This file is an unpublished work. All rights reserved. */

#ifndef FIREBUILD_DEBUG_H_
#define FIREBUILD_DEBUG_H_

#include <stdarg.h>
#include <string.h>

#include <string>
#include <vector>

#include "firebuild/cxx_lang_utils.h"

namespace firebuild {

/** Print error message */
void fb_error(const std::string &msg);

/** Possible debug flags. Keep in sync with debug.cc! */
enum {
  /* Firebuild's configuration */
  FB_DEBUG_CONFIG       = 1 << 0,
  /* Events with one process, e.g. shortcut, exit */
  FB_DEBUG_PROC         = 1 << 1,
  /* How processes are organized into ProcTree */
  FB_DEBUG_PROCTREE     = 1 << 2,
  /* Communication */
  FB_DEBUG_COMM         = 1 << 3,
  /* File system */
  FB_DEBUG_FS           = 1 << 4,
  /* Checksum computation */
  FB_DEBUG_HASH         = 1 << 5,
  /* The data stored in the cache */
  FB_DEBUG_CACHE        = 1 << 6,
  /* Placing in / retrieving from the cache */
  FB_DEBUG_CACHING      = 1 << 7,
  /* Shortcutting */
  FB_DEBUG_SHORTCUT     = 1 << 8,
  /* Entering and leaving functions */
  FB_DEBUG_FUNC         = 1 << 10,
  /* Tracking the server-side file descriptors */
  FB_DEBUG_FD           = 1 << 11,
};


/**
 * Test if debugging this kind of events is enabled.
 */
#define FB_DEBUGGING(flag) ((firebuild::debug_flags) & flag)

/**
 * Print debug message if the given debug flag is enabled.
 */
#define FB_DEBUG(flag, msg) if (FB_DEBUGGING(flag)) \
    firebuild::fb_debug(msg)

void fb_debug(const std::string &msg);

/** Current debugging flags */
extern int32_t debug_flags;

int32_t parse_debug_flags(const std::string& str);

inline std::string d(int value) {
  return std::to_string(value);
}
inline std::string d(long value) {  /* NOLINT(runtime/int) */
  return std::to_string(value);
}
inline std::string d(long long value) {  /* NOLINT(runtime/int) */
  return std::to_string(value);
}
inline std::string d(unsigned int value) {
  return std::to_string(value);
}
inline std::string d(unsigned long value) {  /* NOLINT(runtime/int) */
  return std::to_string(value);
}
inline std::string d(unsigned long long value) {  /* NOLINT(runtime/int) */
  return std::to_string(value);
}

inline std::string d(bool value) {
  return value ? "true" : "false";
}

std::string d(const std::string& str);
std::string d(const char *str);

std::string d(const std::vector<std::string>& arr,
              const std::string& sep = ", ");

/* Convenience wrapper around our various d(...) debugging functions.
 * Instead of returning a std::string, as done by d(), this gives the raw C char* pointer
 * which is valid only inside the expression where D() is called. */
#define D(var) firebuild::d(var).c_str()

/** Get a human-readable timestamp according to local time. */
std::string pretty_timestamp();


#ifdef NDEBUG
#define TRACK(...)
#define TRACKX(...)
#else
/* Global, shared across all MethodTracker<T>s, for nice indentation */
extern int method_tracker_level;

/**
 * Track entering and leaving a function (or any brace-block of code).
 * Print some variables when entering.
 *
 * @param flag Do the logging if FB_DEBUG_FUNC or any of these debug flags specified here is enabled
 * @param fmt printf format string, may be empty
 * @param ... The additional parameters for printf
 */
#define TRACK(flag, fmt, ...) \
  firebuild::MethodTracker<void> method_tracker(__func__, __FILE__, __LINE__, flag, 0, 0, \
                                                "", NULL, NULL, fmt, ##__VA_ARGS__)

/**
 * Track entering and leaving a function (or any brace-block of code).
 * Print one variable both when entering and leaving.
 * Print some more variables when entering.
 *
 * The variable to be printed when leaving the block (obj_ptr) has to have a corresponding global
 * std::string d(classname *obj_ptr, int level) debugging method, which will be used to format it.
 * The variable is remembered via its pointer, so if you reassign it in the function body then you
 * can't print it upon exit.
 *
 * Having to pass the classsname of obj_ptr is a technical necessity, remove it if you know how to.
 *
 * @param flag Do the logging if FB_DEBUG_FUNC or any of these debug flags specified here is enabled
 * @param print_obj_on_enter Whether to log obj_ptr when entering the block
 * @param print_obj_on_leave Whether to log obj_ptr when leaving the block
 * @param classname The classname of obj_ptr, without 'const', or '*' for pointer
 * @param obj_ptr The object to print on entering of leaving, of type 'classname *'.
 * @param fmt printf format string for the additional variables to log when entering, may be empty
 * @param ... The additional parameters for printf
 */
#define TRACKX(flag, print_obj_on_enter, print_obj_on_leave, classname, obj_ptr, fmt, ...) \
  /* Find the address of the correct ovedloaded d() method belonging to obj_ptr. */ \
  /* Needs to happen in the context of the macro's caller, because debug.h doesn't see the */ \
  /* specific d() method, so d() inside MethodTracker() would pick the one taking a boolean. */ \
  std::string (*resolved_d)(const classname *, int) = &firebuild::d; \
  firebuild::MethodTracker<classname> method_tracker(__func__, __FILE__, __LINE__, flag, \
                                                     print_obj_on_enter, print_obj_on_leave, \
                                                     #obj_ptr, obj_ptr, resolved_d, \
                                                     fmt, ##__VA_ARGS__)

template <typename T>
class MethodTracker {
 public:
  MethodTracker(const char *func, const char *file, int line,
                int flag, bool print_obj_on_enter, bool print_obj_on_leave,
                const char *obj_name, const T *obj_ptr, std::string (*resolved_d)(const T *, int),
                const char *fmt, ...)
      __attribute__((format(printf, 11, 12)))
      : func_(func), file_(file), line_(line),
        flag_(flag | FB_DEBUG_FUNC), print_obj_on_leave_(print_obj_on_leave),
        obj_name_(obj_name), obj_ptr_(obj_ptr), resolved_d_(resolved_d) {
    if (FB_DEBUGGING(flag_)) {
      const char *last_slash = strrchr(file_, '/');
      if (last_slash) {
        file_ = last_slash + 1;
      }
      char buf[1024];
      int offset = snprintf(buf, sizeof(buf), "%*s-> %s()  (%s:%d)%s",
                            2 * method_tracker_level, "", func_, file_, line_,
                            print_obj_on_enter || fmt[0] ? "  " : "");
      if (print_obj_on_enter) {
        offset += snprintf(buf + offset, sizeof(buf) - offset, "%s=%s%s",
                           obj_name_, ((*resolved_d_)(obj_ptr_, 0)).c_str(), fmt[0] ? ", " : "");
      }
      va_list ap;
      va_start(ap, fmt);
      vsnprintf(buf + offset, sizeof(buf) - offset, fmt, ap);
      va_end(ap);
      FB_DEBUG(flag_, buf);
      method_tracker_level++;
    }
  }
  ~MethodTracker() {
    if (FB_DEBUGGING(flag_)) {
      method_tracker_level--;
      char buf[1024];
      int offset = snprintf(buf, sizeof(buf), "%*s<- %s()  (%s:%d)",
                            2 * method_tracker_level, "", func_, file_, line_);
      if (print_obj_on_leave_) {
        snprintf(buf + offset, sizeof(buf) - offset, "  %s=%s",
                 obj_name_, ((*resolved_d_)(obj_ptr_, 0)).c_str());
      }
      FB_DEBUG(flag_, buf);
    }
  }

 private:
  const char *func_;
  const char *file_;
  int line_;
  int flag_;
  bool print_obj_on_leave_;
  const char *obj_name_;
  const T *obj_ptr_;
  std::string (*resolved_d_)(const T *, int);

  DISALLOW_COPY_AND_ASSIGN(MethodTracker<T>);
};
#endif  /* NDEBUG */

#ifndef NDEBUG
/*
 * Like an "assert(a op b)" statement, "assert_cmp(a, op, b)" makes sure that the "a op b" condition
 * is true. Note the required commas. Example: "assert_cmp(foo, >=, 0)".
 *
 * In case of failure, prints both values.
 *
 * Based on the idea of GLib's g_assert_cmp*(). With C++'s overloading we can do better, though.
 *
 * The two values can be of any type that's printable using d() and comparable, and accordingly,
 * they are indeed printed using d() if the comparison fails.
 *
 * Note: because d(NULL) doesn't work, you can't do "assert_cmp(p, ==, NULL)" or
 * "assert_cmp(p, !=, NULL)". For the former, use our "assert_null(p)". For the latter, use the
 * standard "assert(p)".
 */
#define assert_cmp(a, op, b) do { \
  if (!(a op b)) { \
    std::string source = #a " " #op " " #b; \
    std::string actual = firebuild::d(a) + " " + #op + " " + firebuild::d(b); \
    fprintf(stderr, "Assertion `%s': `%s' failed.\n", source.c_str(), actual.c_str()); \
    assert(0 && "see previous message"); \
  } \
} while (0)
/*
 * Like an assert(p == NULL), but if fails then prints the value using d().
 */
#define assert_null(p) do { \
  if (p != NULL) { \
    std::string source = #p " != NULL"; \
    std::string actual = firebuild::d(p) + " != NULL"; \
    fprintf(stderr, "Assertion `%s': `%s' failed.\n", source.c_str(), actual.c_str()); \
    assert(0 && "see previous message"); \
  } \
} while (0)
#else
#define assert_cmp(a, op, b)
#define assert_null(p)
#endif  /* NDEBUG */

}  // namespace firebuild
#endif  // FIREBUILD_DEBUG_H_

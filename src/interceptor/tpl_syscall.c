{# ------------------------------------------------------------------ #}
{# Copyright (c) 2022 Firebuild Inc.                                  #}
{# All rights reserved.                                               #}
{# Free for personal use and commercial trial.                        #}
{# Non-trial commercial use requires licenses available from          #}
{# https://firebuild.com.                                             #}
{# Modification and redistribution are permitted, but commercial use  #}
{# of derivative works is subject to the same requirements of this    #}
{# license.                                                           #}
{# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,    #}
{# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF #}
{# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND              #}
{# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT        #}
{# HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,       #}
{# WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, #}
{# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER      #}
{# DEALINGS IN THE SOFTWARE.                                          #}
{# ------------------------------------------------------------------ #}
{# Template for the syscall() call.                                   #}
{# ------------------------------------------------------------------ #}
### extends "tpl.c"

### block impl_c

###     if ifdef_guard
{{ ifdef_guard }}
###     endif

/* Make the intercepting function visible */
#pragma GCC visibility push(default)
#pragma GCC diagnostic push

{{ rettype }} {{ func }} ({{ sig_str }}) {
  bool skip_interception = false;

  switch (number) {

#include "interceptor/gen_impl_syscalls.c.inc"

   default_syscall_handling:
    default: {
      /* Warm up */
      int saved_errno = errno;
      if (!skip_interception && !ic_init_done) fb_ic_load();
      /* use a copy, in case another thread modifies it */
      bool i_am_intercepting = !skip_interception && intercepting_enabled;
      (void)i_am_intercepting;  /* sometimes it's unused, silence warning */

#ifdef FB_EXTRA_DEBUG
      if (insert_trace_markers) {
        char debug_buf[256];
        snprintf(debug_buf, sizeof(debug_buf), "%s%s{{ debug_before_fmt }}",
            i_am_intercepting ? "" : "[not intercepting] ",
            "{{ func }}"{{ debug_before_args }});
        insert_begin_marker(debug_buf);
      }
#endif

      /* Notify the supervisor */
      bool i_locked = false;  /* "i" as in "me, myself and I" */
      if (!skip_interception
          && (number < 0 || number >= IC_CALLED_SYSCALL_SIZE || !ic_called_{{ func }}[number])) {
        /* Grabbing the global lock (unless it's already ours, e.g. we're in a signal handler) */
        if (i_am_intercepting) {
          grab_global_lock(&i_locked, "{{ func }}");
        }
        /* Global lock grabbed */
      }
      /* Pass on several long parameters unchanged, see #178. */
      va_list ap_pass;
      va_start(ap_pass, number);
      long arg1 = va_arg(ap_pass, long);
      long arg2 = va_arg(ap_pass, long);
      long arg3 = va_arg(ap_pass, long);
      long arg4 = va_arg(ap_pass, long);
      long arg5 = va_arg(ap_pass, long);
      long arg6 = va_arg(ap_pass, long);
      long arg7 = va_arg(ap_pass, long);
      long arg8 = va_arg(ap_pass, long);
      va_end(ap_pass);

      if (!skip_interception) {
        errno = saved_errno;
      }
      {{ rettype }} ret = get_ic_orig_{{ func }}()(number, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8);
      if (!skip_interception) {
        saved_errno = errno;
        if (number < 0 || number >= IC_CALLED_SYSCALL_SIZE || !ic_called_{{ func }}[number]) {
          if (number >= 0 && number < IC_CALLED_SYSCALL_SIZE) {
            ic_called_{{ func }}[number] = true;
          }
          FBBCOMM_Builder_gen_call ic_msg;
          fbbcomm_builder_gen_call_init(&ic_msg);
          char call[32];
          snprintf(call, sizeof(call), "{{ func }}(%ld)", number);
          fbbcomm_builder_gen_call_set_call(&ic_msg, call);
          fb_fbbcomm_send_msg(&ic_msg, fb_sv_conn);

          /* Releasing the global lock (if we grabbed it in this pass) */
          if (i_locked) {
            release_global_lock();
          }
          /* Global lock released */
        }
      }
#ifdef FB_EXTRA_DEBUG
      if (insert_trace_markers) {
        char debug_buf[256];
        snprintf(debug_buf, sizeof(debug_buf), "%s%s{{ debug_after_fmt }}",
            i_am_intercepting ? "" : "[not intercepting] ",
            "{{ func }}"{{ debug_after_args }});
        insert_end_marker(debug_buf);
      }
#endif

      if (!skip_interception) {
        errno = saved_errno;
      }
      return ret;
    }
  }
}

#pragma GCC visibility pop

###     if ifdef_guard
#endif
###     endif

### endblock impl_c

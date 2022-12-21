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
{# Template for clone().                                              #}
{# ------------------------------------------------------------------ #}
### extends "tpl.c"

{% set msg_skip_fields = ["fn", "stack", "arg"] %}

### block call_orig

### if syscall
  /* Need to extract 'flags'. See clone(2) NOTES about differences between architectures. */
#if defined(__s390__) || defined(__cris__)
  va_arg(ap, void*);  /* skip over 'stack' */
#endif
  unsigned long flags = va_arg(ap, unsigned long);
### endif

  if (i_am_intercepting) {
    pre_clone_disable_interception(flags, &i_locked);
  }

### if not syscall
  int vararg_count = 0;
  if (flags & (CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID)) {
    vararg_count = 3;
  } else if (flags & CLONE_SETTLS) {
    vararg_count = 2;
  } else if (flags & (CLONE_PARENT_SETTID | CLONE_PIDFD)) {
    vararg_count = 1;
  }

  if (vararg_count == 0) {
    ret = ic_orig_{{ func }}(fn, stack, flags, arg);
  } else {
    pid_t *parent_tid = va_arg(ap, pid_t *);
    if (vararg_count == 1) {
      ret = ic_orig_{{ func }}(fn, stack, flags, arg, parent_tid);
    } else {
      void *tls = va_arg(ap, void *);
      if (vararg_count == 2) {
        ret = ic_orig_{{ func }}(fn, stack, flags, arg, parent_tid, tls);
      } else {
        pid_t *child_tid = va_arg(ap, pid_t *);
        ret = ic_orig_{{ func }}(fn, stack, flags, arg, parent_tid, tls, child_tid);
      }
    }
  }
### else
  /* The order of parameters is heavily architecture dependent. */
  /* Pass on several long parameters unchanged, as in tpl_syscall.c. */
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
  ret = ic_orig_{{ func }}(arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8);
### endif

### endblock call_orig

### block send_msg
### endblock send_msg

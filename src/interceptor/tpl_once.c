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
{# Template for methods where we only need to notify the supervisor   #}
{# once per such method.                                              #}
{# ------------------------------------------------------------------ #}
### extends "tpl.c"

### block decl_h
extern bool ic_called_{{ func }};
{{ super() }}
### endblock decl_h

### block reset_c
ic_called_{{ func }} = false;
### endblock reset_c

### block def_c
bool ic_called_{{ func }};
{{ super() }}
### endblock def_c

### block grab_lock
###       if global_lock == 'before'
{% set grab_lock_condition = "i_am_intercepting && !ic_called_" + func %}
  {{ grab_lock_if_needed(grab_lock_condition) }}
###       endif
### endblock grab_lock

### block send_msg
  /* Notify the supervisor */
  if (!ic_called_{{ func }}) {
    ic_called_{{ func }} = true;
    FBBCOMM_Builder_{{ msg }} ic_msg;
    fbbcomm_builder_{{ msg }}_init(&ic_msg);
###   if msg == 'gen_call'
    fbbcomm_builder_{{ msg }}_set_call(&ic_msg, "{{ func }}");
###   endif

###   if send_msg_on_error
    /* Send errno on failure */
###     if not msg_skip_fields or 'error_no' not in msg_skip_fields
###       if not no_saved_errno
    if (!success) fbbcomm_builder_{{ msg }}_set_error_no(&ic_msg, saved_errno);
###       else
    if (!success) fbbcomm_builder_{{ msg }}_set_error_no(&ic_msg, errno);
###       endif
###     endif
###   endif
###   if ack
    /* Send and wait for ack */
    fb_fbbcomm_send_msg_and_check_ack(&ic_msg, fb_sv_conn);
###   else
    /* Send and go on, no ack */
    fb_fbbcomm_send_msg(&ic_msg, fb_sv_conn);
###   endif
  }
### endblock send_msg

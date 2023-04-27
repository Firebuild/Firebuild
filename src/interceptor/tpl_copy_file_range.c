{# ------------------------------------------------------------------ #}
{# Copyright (c) 2023 Firebuild Inc.                                  #}
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
{# Template for copy_file_range and sendfile. Only need to notify the #}
{# supervisor once per such methods if they fail. Otherwise notify    #}
{# Supervisor as if the intercepted process performed a (p)read() and #}
{# a (p)write().                                                      #}
{# ------------------------------------------------------------------ #}

{% set msg = 'gen_call' %}
{% set sendfile_variant = func in ['sendfile', 'SYS_sendfile', 'sendfile64', 'SYS_sendfile64'] %}
{% set copy_file_range_variant = func in ['copy_file_range', 'SYS_copy_file_range'] %}


{# No locking around the write(): see issue #279 #}
{% set global_lock = 'never' %}

### extends "tpl_once.c"

### block send_msg
  if (i_am_intercepting) {
    if (success) {
###   if copy_file_range_variant
      const bool is_pread = off_in;
      const bool is_pwrite = off_out;
###   elif sendfile_variant
      const bool is_pread = offset;
      const bool is_pwrite = false;
###   endif
###   if sendfile_variant
###     if target == "darwin"
      const int fd_in = fd, fd_out = s;
###     else
      const int fd_in = in_fd, fd_out = out_fd;
###     endif
###   endif
      if (notify_on_read(fd_in, is_pread) || notify_on_write(fd_out, is_pwrite)) {
        bool i_locked = false;
        grab_global_lock(&i_locked, "{{ func }}");
        if (notify_on_read(fd_in, is_pread)) {
          FBBCOMM_Builder_read_from_inherited ic_msg;
          fbbcomm_builder_read_from_inherited_init(&ic_msg);
          fbbcomm_builder_read_from_inherited_set_fd(&ic_msg, fd_in);
          fbbcomm_builder_read_from_inherited_set_is_pread(&ic_msg, is_pread);
          fb_fbbcomm_send_msg(&ic_msg, fb_sv_conn);
          set_notify_on_read_state(fd_in, is_pread);
        }
        if (notify_on_write(fd_out, is_pwrite)) {
          FBBCOMM_Builder_write_to_inherited ic_msg;
          fbbcomm_builder_write_to_inherited_init(&ic_msg);
          fbbcomm_builder_write_to_inherited_set_fd(&ic_msg, fd_out);
          fbbcomm_builder_write_to_inherited_set_is_pwrite(&ic_msg, is_pwrite);
          fb_fbbcomm_send_msg(&ic_msg, fb_sv_conn);
        }
        set_notify_on_write_state(fd_out, is_pwrite);
        if (i_locked) {
          release_global_lock();
        }
      }
    } else if (!ic_called_{{ func }}
               && !(ret == -1 && (saved_errno == EINVAL || saved_errno == EBADF))) {
      /* Notify the supervisor */
      ic_called_{{ func }} = true;
      FBBCOMM_Builder_{{ msg }} ic_msg;
      fbbcomm_builder_{{ msg }}_init(&ic_msg);
###   if msg == 'gen_call'
      fbbcomm_builder_{{ msg }}_set_call(&ic_msg, "{{ func }}");
      fbbcomm_builder_{{ msg }}_set_error_no(&ic_msg, saved_errno);
###   endif

###   if ack
      /* Send and wait for ack */
      fb_fbbcomm_send_msg_and_check_ack(&ic_msg, fb_sv_conn);
###   else
      /* Send and go on, no ack */
      fb_fbbcomm_send_msg(&ic_msg, fb_sv_conn);
###   endif
    }
  }
### endblock send_msg

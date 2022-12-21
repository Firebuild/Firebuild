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
{# Template for the pclose() call.                                    #}
{# ------------------------------------------------------------------ #}
### extends "tpl.c"

### block before
  /* save it here, we can't do fileno() after the pclose() */
  int fd = safe_fileno(stream);
  int was_popened = voidp_set_contains(&popened_streams, stream);
  if (was_popened) {
    voidp_set_erase(&popened_streams, stream);
  }

  if (i_am_intercepting) {
    /* Send a synthetic close before the pclose() to avoid a deadlock in wait4. */
    FBBCOMM_Builder_close ic_msg;
    fbbcomm_builder_close_init(&ic_msg);
    fbbcomm_builder_close_set_fd(&ic_msg, fd);
    fb_fbbcomm_send_msg(&ic_msg, fb_sv_conn);
  }
### endblock before


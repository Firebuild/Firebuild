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
{# Template for the exit() call (which calls the atexit / on_exit     #}
{# handlers).                                                         #}
{# ------------------------------------------------------------------ #}
### extends "tpl.c"

### block body
  /* Exit handlers may call intercepted functions, so release the lock */
  thread_signal_danger_zone_enter();
  if (FB_THREAD_LOCAL(has_global_lock)) {
    pthread_mutex_unlock(&ic_global_lock);
    FB_THREAD_LOCAL(has_global_lock) = false;
    FB_THREAD_LOCAL(intercept_on) = NULL;
  }
  thread_signal_danger_zone_leave();
  assert(FB_THREAD_LOCAL(signal_danger_zone_depth) == 0);

  /* Mark the end now */
  insert_end_marker("{{ func }}");

  /* Perform the call.
   * This will call the registered atexit / on_exit handlers,
   * including our handle_exit() which will notify the supervisor. */
  get_ic_orig_{{ func }}()({{ names_str }});

  /* Make scan-build happy */
  (void)i_locked;

  /* Should not be reached */
  assert(0 && "{{ func }} did not exit");
  abort(); /* for NDEBUG */
### endblock body

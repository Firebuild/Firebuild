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
{# Template for the readlink() family.                                #}
{# ------------------------------------------------------------------ #}
### extends "tpl.c"

### block set_fields
    {{ super() }}
    /* Create a zero-terminated copy on the stack.
     * Make sure it lives until we send the message. */
    int len = 0;
    if (ret >= 0 && (size_t)labs(ret) <= bufsiz) {
      len = ret;
    }
    char ret_target[len + 1];
    if (len > 0) {
      memcpy(ret_target, buf, len);
      ret_target[len] = '\0';
      /* Returned path is a raw string, not to be resolved. */
      fbbcomm_builder_{{ msg }}_set_ret_target(&ic_msg, ret_target);
    }
### endblock set_fields

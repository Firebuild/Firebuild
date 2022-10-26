{# ------------------------------------------------------------------ #}
{# Copyright (c) 2022 Firebuild Inc.                                  #}
{# All rights reserved.                                               #}
{# Free for personal use and commercial trial.                        #}
{# Non-trial commercial use requires licenses available from          #}
{# https://firebuild.com                                              #}
{# ------------------------------------------------------------------ #}
{# Template for functions reading from a (regular or special) file,   #}
{# including                                                          #}
{# - low-level [p]read*() family                                      #}
{# - high-level stdio like fread(), getc(), scanf() etc.              #}
{# - low-level socket reading recv*() family                          #}
{# and perhaps more.                                                  #}
{# ------------------------------------------------------------------ #}
### extends "tpl.c"

### if is_pread is not defined
###   set is_pread = "false"
### endif

### if msg_skip_fields is not defined
###   set msg_skip_fields = []
### endif
### do msg_skip_fields.append("error_no")

{% set msg = "read_from_inherited" %}
{# No locking around the read(): see issue #279 #}
{% set global_lock = 'never' %}

### block set_fields
  {{ super() }}
  fbbcomm_builder_{{ msg }}_set_is_pread(&ic_msg, is_pread);
### endblock set_fields

### block send_msg
  bool is_pread = {{ is_pread }};

  {# Acquire the lock if sending a message #}
  if (fd < 0 || fd >= IC_FD_STATES_SIZE ||
      (is_pread == false && ic_fd_states[fd].notify_on_read == true) ||
      (is_pread == true && ic_fd_states[fd].notify_on_pread == true)) {
    /* Need to notify the supervisor */

    {{ grab_lock_if_needed('true') | indent(2) }}

    {{ super() | indent(2) }}

    if (fd >= 0 && fd < IC_FD_STATES_SIZE) {
      ic_fd_states[fd].notify_on_read = false;
      if (is_pread) {
        ic_fd_states[fd].notify_on_pread = false;
      }
    }

    {{ release_lock_if_needed() | indent(2) }}
  }
### endblock send_msg

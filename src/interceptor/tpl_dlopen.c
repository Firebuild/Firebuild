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
{# Template for the dlopen() family.                                  #}
{# ------------------------------------------------------------------ #}
### extends "tpl.c"

{% set msg_add_fields = ["assert(filename);",
                         "if (strrchr(filename, '/')) {",
                         "  BUILDER_MAYBE_SET_ABSOLUTE_CANONICAL(dlopen, AT_FDCWD, filename);",
                         "} else {",
                         "  fbbcomm_builder_dlopen_set_filename(&ic_msg, filename);",
                         "}",
                         "fbbcomm_builder_dlopen_set_libs_with_count(&ic_msg, (const char * const *)new_libs, new_libs_count);",
                         "fbbcomm_builder_dlopen_set_error(&ic_msg, !success);"] %}

### block before
  /* Count already loaded shared libs. */
###   if target == "darwin"
  const int lib_count_before = _dyld_image_count();
###  else
  size_t lib_count_before = 0;
  dl_iterate_phdr(count_shared_libs_cb, &lib_count_before);
###  endif
  /* Release lock to allow intercepting shared library constructors. */
  if (i_locked) {
    release_global_lock();
  }
### endblock before

### block after
  if (i_am_intercepting) {
    grab_global_lock(&i_locked, "{{ func }}");
  }
  /* Count loaded shared libs after the call. */
###   if target == "darwin"
  const size_t lib_count_after = _dyld_image_count();
###   else
  size_t lib_count_after = 0;
  dl_iterate_phdr(count_shared_libs_cb, &lib_count_after);
###   endif
  /* Allocate enough space for all newly loaded libraries. */
  const char ** new_libs = alloca((lib_count_after - lib_count_before) * sizeof(char*));
  size_t new_libs_count = 0;
  size_t i;
###   if target == "darwin"
  for (i = lib_count_before; i < lib_count_after; i++) {
    new_libs[new_libs_count] = _dyld_get_image_name(i);
    new_libs_count++;
  }
###   else
  shared_libs_as_char_array_cb_data_t cb_data_after =
      {new_libs, 0, lib_count_after - lib_count_before, lib_count_before};
  dl_iterate_phdr(shared_libs_as_char_array_cb, &cb_data_after);
  /* A few libraries may have been skipped. */
  new_libs_count = cb_data_after.collected_entries;
###   endif
  assert(new_libs_count == lib_count_after - lib_count_before);
  for (i = 0; i < new_libs_count; i++) {
    const char *new_lib_orig = new_libs[i];
    const int orig_len = strlen(new_lib_orig);
    if (!is_canonical(new_lib_orig, orig_len)) {
      /* Don't use strdupa(), because it does not exist on macOS. */
      char* new_lib = alloca(orig_len + 1);
      memcpy(new_lib, new_lib_orig, orig_len + 1);
      make_canonical(new_lib, orig_len + 1);
      new_libs[i] = new_lib;
    }
  }
### endblock after

/* Copyright (c) 2014 Balint Reczey <balint@balintreczey.hu> */
/* This file is an unpublished work. All rights reserved. */

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "common/firebuild_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void string_array_init(string_array *array) {
  memset(array, 0, sizeof(*array));
}

/* Does NOT deep copy the string */
void string_array_append(string_array *array, char *s) {
  if (array->size_alloc == 0) {
    array->size_alloc = 16 /* whatever */;
    array->p = malloc(sizeof(char *) * array->size_alloc);
  } else if (array->len + 1 == array->size_alloc) {
    array->size_alloc *= 2;
    array->p = realloc(array->p, sizeof(char *) * array->size_alloc);
  }
  array->p[array->len++] = s;
  array->p[array->len] = NULL;
}

void string_array_deep_free(string_array *array) {
  for (int i = 0; i < array->len; i++)
    free(array->p[i]);
  free(array->p);
}

/**
 * Checks if a path semantically begins with the given subpath.
 *
 * Does string operations only, does not look at the file system.
 */
bool is_path_at_locations(const char * const path, string_array *location_array) {
  const size_t path_len = strlen(path);
  for (int i = 0; i < location_array->len; i++) {
    const char * const location = location_array->p[i];
    size_t location_len = strlen(location);
    while (location_len > 0 && location[location_len - 1] == '/') {
      location_len--;
    }

    if (path_len < location_len) {
      continue;
    }

    if (memcmp(location, path, location_len) != 0) {
      continue;
    }

    if (path_len == location_len) {
      return true;
    }

    if (path[location_len] == '/') {
      return true;
    }
  }
  return false;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif


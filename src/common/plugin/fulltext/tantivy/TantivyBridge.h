/* Copyright (c) 2026. Licensed under Apache 2.0. */
#ifndef COMMON_PLUGIN_FULLTEXT_TANTIVY_TANTIVYBRIDGE_H_
#define COMMON_PLUGIN_FULLTEXT_TANTIVY_TANTIVYBRIDGE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NebulaTantivySlice {
  const uint8_t* data;
  size_t len;
} NebulaTantivySlice;

typedef void (*NebulaTantivyHitCallback)(void*, const uint8_t*, size_t, double);

/* Errors are thread local, valid until the next operation on the calling thread. */
const char* nebula_tantivy_last_error(void);
void* nebula_tantivy_open(const char* path, const NebulaTantivySlice* fields,
                         size_t field_count, int writable, size_t memory_bytes,
                         NebulaTantivySlice analyzer);
void nebula_tantivy_close(void* handle);
int nebula_tantivy_set_writable(void* handle, int writable);
int nebula_tantivy_upsert(void* handle, NebulaTantivySlice key,
                        const NebulaTantivySlice* values, size_t value_count);
int nebula_tantivy_remove(void* handle, NebulaTantivySlice key);
int nebula_tantivy_clear(void* handle);
int nebula_tantivy_commit(void* handle);
int nebula_tantivy_reload(void* handle);
/* modes: 0=query parser, 1=contains, 2=starts with, 3=ends with.
 * limit=0 means all verified hits; key_prefix may be arbitrary binary bytes.
 * Callback arguments are valid only for the duration of that callback.
 */
int nebula_tantivy_search(void* handle, NebulaTantivySlice field,
                        NebulaTantivySlice pattern, int mode, size_t limit,
                        NebulaTantivySlice key_prefix,
                        NebulaTantivyHitCallback callback, void* context);

#ifdef __cplusplus
}
#endif
#endif

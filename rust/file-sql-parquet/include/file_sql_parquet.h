#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* open_fd consumes fd, including when the file is invalid. */
void *fs_parquet_open_fd(int fd);
void fs_parquet_close(void *handle);
int64_t fs_parquet_column_count(const void *handle);
int64_t fs_parquet_column_name_len(const void *handle, int64_t index);
int fs_parquet_column_name_copy(const void *handle, int64_t index, char *out, size_t capacity);
int fs_parquet_column_type(const void *handle, int64_t index);
/* 0: row, 1: end, -1: malformed/unsupported data. */
int fs_parquet_next(void *handle);
/* -1 means NULL, -2 means error; otherwise the byte length. */
int64_t fs_parquet_value_len(const void *handle, int64_t index);
int fs_parquet_value_copy(const void *handle, int64_t index, char *out, size_t capacity);

#ifdef __cplusplus
}
#endif

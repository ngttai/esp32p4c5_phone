/* Minimal ZIP reader for .bpk packages (stored or deflate, no ZIP64, no encryption). */
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Read one file of the archive into a malloc'ed, NUL-terminated buffer (free() it). 0 on success, else a negative value and `err`. */
int phone_zip_read_file(const char *zip_path, const char *name, char **out, size_t *len, char *err, size_t err_len);

/** Extract the archive into `dest_dir` (created). META-INF/ is skipped, unsafe paths and oversized archives are refused. 0 on success. */
int phone_zip_extract(const char *zip_path, const char *dest_dir, char *err, size_t err_len);

#ifdef __cplusplus
}
#endif

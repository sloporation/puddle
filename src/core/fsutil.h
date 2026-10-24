#ifndef PUDDLE_FSUTIL_H
#define PUDDLE_FSUTIL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* mkdir -p with mode 0700. 0 on success, -1 with errno set. */
int puddle_mkdir_p(const char *path);

/* Flush a file to stable storage (F_FULLFSYNC on macOS, which plain fsync() does not guarantee). */
int puddle_fsync_fd(int fd);

/* Write `data` to `final_path` so a crash leaves either the old file or the new one, never half:
 * temp file in `dir`, flush, rename, flush the directory. Mode 0600. 0 or -1 with errno set. */
int puddle_write_file_atomic(const char *dir, const char *final_path, const void *data, size_t len);

/* Flush a directory entry, so a newly created file survives a crash. */
int puddle_fsync_dir(const char *path);

/* The names in a directory, one at a time, without "." and "..", the same on every OS:
 *     puddle_dir *d = puddle_dir_open(path);   // NULL with errno set if it can't be read
 *     const char *name;
 *     while ((name = puddle_dir_next(d))) ...  // valid until the next call
 *     puddle_dir_close(d);
 * Order is whatever the file system gives. */
typedef struct puddle_dir puddle_dir;
puddle_dir *puddle_dir_open(const char *path);
const char *puddle_dir_next(puddle_dir *d);
void puddle_dir_close(puddle_dir *d);

#ifdef __cplusplus
}
#endif
#endif

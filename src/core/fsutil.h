#ifndef PUDDLE_FSUTIL_H
#define PUDDLE_FSUTIL_H

#ifdef __cplusplus
extern "C" {
#endif

/* mkdir -p with mode 0700. 0 on success, -1 with errno set. */
int puddle_mkdir_p(const char *path);

/* Flush a file to stable storage (F_FULLFSYNC on macOS, which plain fsync() does not guarantee). */
int puddle_fsync_fd(int fd);

/* Flush a directory entry, so a newly created file survives a crash. */
int puddle_fsync_dir(const char *path);

#ifdef __cplusplus
}
#endif
#endif

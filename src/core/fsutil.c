#include "fsutil.h"

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int puddle_mkdir_p(const char *path)
{
    char buf[PUDDLE_PATH_MAX];
    if (strlen(path) >= sizeof buf) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(buf, path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0700) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int puddle_fsync_fd(int fd)
{
#ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return 0;
#endif
    return fsync(fd);
}

int puddle_fsync_dir(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int rc = puddle_fsync_fd(fd);
    close(fd);
    return rc;
}

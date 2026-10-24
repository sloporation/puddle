#include "fsutil.h"

#include <cerrno>
#include <filesystem>
#include <new>
#include <string>

#include "config.h"

#include <errno.h>
#include <stdio.h>
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

int puddle_write_file_atomic(const char *dir, const char *final_path, const void *data, size_t len)
{
    char tmp[PUDDLE_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s/.tmp-%ld", dir, (long)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    const char *p = (const char *)data;
    size_t left = len;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            close(fd);
            unlink(tmp);
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    int rc = puddle_fsync_fd(fd);
    close(fd);
    if (rc != 0 || rename(tmp, final_path) != 0) {
        unlink(tmp);
        return -1;
    }
    puddle_fsync_dir(dir);
    return 0;
}

/* ---- reading a directory ---- */

struct puddle_dir {
    std::filesystem::directory_iterator it;
    std::string name;
};

puddle_dir *puddle_dir_open(const char *path)
{
    std::error_code ec;
    std::filesystem::directory_iterator it(path, ec);
    if (ec) {
        errno = ec.value();
        return NULL;
    }
    return new (std::nothrow) puddle_dir{ std::move(it), std::string() };
}

const char *puddle_dir_next(puddle_dir *d)
{
    std::error_code ec;
    if (d->it == std::filesystem::directory_iterator())
        return NULL;
    d->name = d->it->path().filename().string();
    d->it.increment(ec);
    if (ec)
        d->it = std::filesystem::directory_iterator(); /* a directory that changed under us ends here rather than failing midway */
    return d->name.c_str();
}

void puddle_dir_close(puddle_dir *d)
{
    delete d;
}

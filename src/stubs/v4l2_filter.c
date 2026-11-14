#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <ftw.h>
#include <limits.h>

typedef int (*real_open_t)(const char *pathname, int flags, mode_t mode);
typedef int (*real_openat_t)(int dirfd, const char *pathname, int flags, mode_t mode);
typedef int (*ftw64_cb_t)(const char *fpath, const struct stat64 *sb, int typeflag);
typedef int (*real_ftw64_t)(const char *dirpath, ftw64_cb_t fn, int nopenfd);

static real_open_t real_open = NULL;
static real_open_t real_open64 = NULL;
static real_openat_t real_openat = NULL;
static real_openat_t real_openat64 = NULL;
static real_ftw64_t real_ftw64 = NULL;

static void get_game_fonts_dir(char *out, size_t out_sz) {
    out[0] = '\0';
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        char *slash = strrchr(exe_path, '/');
        if (slash) {
            *slash = '\0';
            snprintf(out, out_sz, "%s/Fonts", exe_path);
            return;
        }
    }
    snprintf(out, out_sz, "Fonts");
}

__attribute__((constructor))
static void init_syms(void) {
    real_open = (real_open_t)dlsym(RTLD_NEXT, "open");
    real_open64 = (real_open_t)dlsym(RTLD_NEXT, "open64");
    if (!real_open64) real_open64 = real_open;
    real_openat = (real_openat_t)dlsym(RTLD_NEXT, "openat");
    real_openat64 = (real_openat_t)dlsym(RTLD_NEXT, "openat64");
    if (!real_openat64) real_openat64 = real_openat;
    real_ftw64 = (real_ftw64_t)dlsym(RTLD_NEXT, "ftw64");
}

int ftw64(const char *dirpath, ftw64_cb_t fn, int nopenfd) {
    if (!real_ftw64) real_ftw64 = (real_ftw64_t)dlsym(RTLD_NEXT, "ftw64");
    if (!real_ftw64) return -1;
    int ret = real_ftw64(dirpath, fn, nopenfd);
    if (ret == 0 && dirpath && strcmp(dirpath, "/usr/share/fonts") == 0) {
        char game_fonts[PATH_MAX];
        get_game_fonts_dir(game_fonts, sizeof(game_fonts));
        if (game_fonts[0] && access(game_fonts, R_OK | X_OK) == 0) {
            fprintf(stderr, "[Font Filter] Also scanning game Fonts dir: %s\n", game_fonts);
            int gret = real_ftw64(game_fonts, fn, nopenfd);
            if (gret != 0) ret = gret;
        }
    }
    return ret;
}

static int is_valid_color_webcam(int fd) {
    struct v4l2_fmtdesc fmtdesc;
    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmtdesc.index = 0;
    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        if (fmtdesc.pixelformat == V4L2_PIX_FMT_YUYV ||
            fmtdesc.pixelformat == V4L2_PIX_FMT_MJPEG ||
            fmtdesc.pixelformat == V4L2_PIX_FMT_RGB24 ||
            fmtdesc.pixelformat == V4L2_PIX_FMT_BGR24) {
            return 1;
        }
        fmtdesc.index++;
    }
    return 0;
}

int open(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (__builtin_expect(flags & (O_CREAT | O_TMPFILE), 0)) {
        va_list args; va_start(args, flags); mode = va_arg(args, mode_t); va_end(args);
    }
    int fd = real_open ? real_open(pathname, flags, mode) : -1;
    if (__builtin_expect(fd >= 0 && pathname && pathname[0] == '/' && strncmp(pathname, "/dev/video", 10) == 0, 0)) {
        if (!is_valid_color_webcam(fd)) {
            fprintf(stderr, "[V4L2 Filter] Filtered out unsupported/metadata device: %s\n", pathname);
            close(fd);
            errno = ENOENT;
            return -1;
        }
        fprintf(stderr, "[V4L2 Filter] Allowed color webcam device: %s\n", pathname);
    }
    return fd;
}

int open64(const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (__builtin_expect(flags & (O_CREAT | O_TMPFILE), 0)) {
        va_list args; va_start(args, flags); mode = va_arg(args, mode_t); va_end(args);
    }
    int fd = real_open64 ? real_open64(pathname, flags, mode) : -1;
    if (__builtin_expect(fd >= 0 && pathname && pathname[0] == '/' && strncmp(pathname, "/dev/video", 10) == 0, 0)) {
        if (!is_valid_color_webcam(fd)) {
            fprintf(stderr, "[V4L2 Filter] Filtered out unsupported/metadata device: %s\n", pathname);
            close(fd);
            errno = ENOENT;
            return -1;
        }
        fprintf(stderr, "[V4L2 Filter] Allowed color webcam device: %s\n", pathname);
    }
    return fd;
}

int openat(int dirfd, const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (__builtin_expect(flags & (O_CREAT | O_TMPFILE), 0)) {
        va_list args; va_start(args, flags); mode = va_arg(args, mode_t); va_end(args);
    }
    int fd = real_openat ? real_openat(dirfd, pathname, flags, mode) : -1;
    if (__builtin_expect(fd >= 0 && pathname && pathname[0] == '/' && strncmp(pathname, "/dev/video", 10) == 0, 0)) {
        if (!is_valid_color_webcam(fd)) {
            fprintf(stderr, "[V4L2 Filter] Filtered out unsupported/metadata device: %s\n", pathname);
            close(fd);
            errno = ENOENT;
            return -1;
        }
        fprintf(stderr, "[V4L2 Filter] Allowed color webcam device: %s\n", pathname);
    }
    return fd;
}

int openat64(int dirfd, const char *pathname, int flags, ...) {
    mode_t mode = 0;
    if (__builtin_expect(flags & (O_CREAT | O_TMPFILE), 0)) {
        va_list args; va_start(args, flags); mode = va_arg(args, mode_t); va_end(args);
    }
    int fd = real_openat64 ? real_openat64(dirfd, pathname, flags, mode) : -1;
    if (__builtin_expect(fd >= 0 && pathname && pathname[0] == '/' && strncmp(pathname, "/dev/video", 10) == 0, 0)) {
        if (!is_valid_color_webcam(fd)) {
            fprintf(stderr, "[V4L2 Filter] Filtered out unsupported/metadata device: %s\n", pathname);
            close(fd);
            errno = ENOENT;
            return -1;
        }
        fprintf(stderr, "[V4L2 Filter] Allowed color webcam device: %s\n", pathname);
    }
    return fd;
}

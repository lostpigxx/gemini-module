#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

static ssize_t (*real_write_fn)(int, const void*, size_t);
static int (*real_fsync_fn)(int);
static int (*real_fdatasync_fn)(int);
static atomic_int triggered;

__attribute__((constructor)) static void init_fault_injector(void) {
  real_write_fn = dlsym(RTLD_NEXT, "write");
  real_fsync_fn = dlsym(RTLD_NEXT, "fsync");
  real_fdatasync_fn = dlsym(RTLD_NEXT, "fdatasync");
  atomic_store(&triggered, 0);
}

static int control_enabled(void) {
  const char* path = getenv("BLOOM_IO_FAULT_CONTROL");
  return path && syscall(SYS_faccessat, AT_FDCWD, path, F_OK, 0) == 0;
}

static int fd_matches(int fd) {
  const char* needle = getenv("BLOOM_IO_FAULT_MATCH");
  if (!needle || !*needle) return 0;

  char link_path[64];
  char target[PATH_MAX];
  int length = snprintf(link_path, sizeof(link_path), "/proc/self/fd/%d", fd);
  if (length <= 0 || (size_t)length >= sizeof(link_path)) return 0;
  ssize_t target_length = readlink(link_path, target, sizeof(target) - 1);
  if (target_length <= 0) return 0;
  target[target_length] = '\0';
  return strstr(target, needle) != NULL;
}

static int claim_fault(int fd, const char* mode) {
  const char* configured = getenv("BLOOM_IO_FAULT_MODE");
  if (!configured || strcmp(configured, mode) != 0) return 0;
  if (!control_enabled() || !fd_matches(fd)) return 0;
  int expected = 0;
  return atomic_compare_exchange_strong(&triggered, &expected, 1);
}

static void record_hit(void) {
  const char* path = getenv("BLOOM_IO_FAULT_HIT");
  if (!path) return;
  int fd = syscall(SYS_openat, AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd >= 0) {
    const char marker[] = "hit\n";
    syscall(SYS_write, fd, marker, sizeof(marker) - 1);
    syscall(SYS_close, fd);
  }
}

ssize_t write(int fd, const void* buffer, size_t count) {
  if (count > 1 && claim_fault(fd, "short-write")) {
    record_hit();
    return real_write_fn(fd, buffer, count / 2);
  }
  return real_write_fn(fd, buffer, count);
}

int fsync(int fd) {
  if (claim_fault(fd, "fsync-error")) {
    record_hit();
    errno = EIO;
    return -1;
  }
  return real_fsync_fn(fd);
}

int fdatasync(int fd) {
  if (claim_fault(fd, "fsync-error")) {
    record_hit();
    errno = EIO;
    return -1;
  }
  return real_fdatasync_fn(fd);
}

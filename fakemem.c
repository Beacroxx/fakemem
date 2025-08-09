/**
 * @file fakemem.c
 * @brief LD_PRELOAD library that fakes /proc/meminfo by intercepting file operations
 *
 * This library creates a scaled/overridden copy of /proc/meminfo at TMP_MEMINFO_PATH
 * and transparently redirects open/fopen family calls to that file when the target
 * is /proc/meminfo.
 *
 * @section build Build
 * @code
 * clang -shared -fPIC -o fakemem.so fakemem.c -ldl
 * @endcode
 *
 * @section environment Environment Variables
 * @par FAKEMEM_MEM
 * Human-friendly size accepted by parse_numfmt_like_to_kib, e.g. "16G",
 * "800MiB", "1024", "4T". If set, becomes the target MemTotal (in KiB).
 *
 * @par FAKEMEM_SCALE_ALL
 * If set (to any value), all kB fields in /proc/meminfo are scaled by the
 * same factor. If not set, only MemTotal, MemFree, and MemAvailable are adjusted to
 * preserve the same used amount as the real system.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define MAX_MEMINFO_SIZE 8192
#define TMP_MEMINFO_PATH "/tmp/meminfo_scaled"

// Target MemTotal in KiB computed from FAKEMEM_MEM (or 16EiB).
static long double target_memory_kb;
// Synchronizes creation/update of the temp meminfo and watcher startup.
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
// Flag indicating whether the watcher thread has been started.
static int watcher_thread_started = 0;
// Handle for the background watcher thread.
static pthread_t watcher_thread;

/**
 * @brief Parses a human-friendly size string into KiB.
 * @param input Input string containing size with optional suffix
 * @param out_kib Output parameter for the parsed size in KiB
 * @return 1 on success, 0 on failure
 *
 * Examples: "1024", "1k", "1KiB", "2M", "2MiB", "3G", "3GiB".
 * On success, writes the result to out_kib; otherwise returns 0.
 */
static int parse_numfmt_like_to_kib(const char *input, long double *out_kib) {
  if (!input || !out_kib)
    return 0;

  while (*input && isspace((unsigned char)*input))
    input++;

  char *endptr;
  long double value = strtold(input, &endptr);
  if (endptr == input)
    return 0;

  while (*endptr && isspace((unsigned char)*endptr))
    endptr++;

  char suffix[8] = {0};
  for (int i = 0; endptr[i] && i < 7 && !isspace((unsigned char)endptr[i]); i++) {
    suffix[i] = (char)tolower((unsigned char)endptr[i]);
  }

  unsigned long long multiplier = 1ULL;
  const char c = suffix[0];
  if (c == '\0' || c == 'b') {
    multiplier = 1ULL;
  } else if (c == 'k') {
    multiplier = 1024ULL;
  } else if (c == 'm') {
    multiplier = 1024ULL * 1024ULL;
  } else if (c == 'g') {
    multiplier = 1024ULL * 1024ULL * 1024ULL;
  } else if (c == 't') {
    multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
  } else if (c == 'p') {
    multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL;
  } else {
    return 0;
  }

  *out_kib = (value * (long double)multiplier) / 1024.0L;
  return 1;
}

/**
 * @brief Creates a scaled version of /proc/meminfo
 *
 * Reads the real /proc/meminfo, computes the scaled view based on
 * FAKEMEM_MEM and FAKEMEM_SCALE_ALL environment variables, and writes
 * the result to TMP_MEMINFO_PATH.
 */
static void create_fake_meminfo_file() {
  static FILE *(*real_fopen)(const char *, const char *) = NULL;
  if (!real_fopen)
    real_fopen = dlsym(RTLD_NEXT, "fopen");

  FILE *real_file = real_fopen("/proc/meminfo", "r");
  if (!real_file)
    return;

  char buffer[MAX_MEMINFO_SIZE];
  size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, real_file);
  fclose(real_file);
  if (bytes_read == 0)
    return;
  buffer[bytes_read] = '\0';

  unsigned long long actual_total_mem_kb = 0;
  sscanf(buffer, "MemTotal: %llu kB", &actual_total_mem_kb);

  const char *mem_env = getenv("FAKEMEM_MEM");
  long double parsed_kib;
  target_memory_kb = (mem_env && parse_numfmt_like_to_kib(mem_env, &parsed_kib))
                         ? parsed_kib
                         : 16ULL * 1000 * 1000 * 1000 * 1000 * 1000;

  const char *scale_env = getenv("FAKEMEM_SCALE_ALL");
  long double scale_factor = actual_total_mem_kb ? (target_memory_kb / actual_total_mem_kb) : 1.0L;
  int do_scale_all = (scale_env && scale_env[0]) || (scale_factor < 1.0L);

  char scaled_meminfo[MAX_MEMINFO_SIZE * 2] = {0};
  char *pos = scaled_meminfo;
  size_t remaining = sizeof(scaled_meminfo);

  char *saveptr, *line = strtok_r(buffer, "\n", &saveptr);
  while (line) {
    char key[64], unit[8];
    unsigned long long value;

    if (sscanf(line, "%63[^:]: %llu %7s", key, &value, unit) == 3 && strcmp(unit, "kB") == 0) {
      unsigned long long new_value = value;

      if (do_scale_all) {
        new_value = (unsigned long long)(value * scale_factor);
      } else {
        if (strcmp(key, "MemTotal") == 0) {
          new_value = (unsigned long long)target_memory_kb;
        } else if (strcmp(key, "MemAvailable") == 0) {
          unsigned long long used_kb = (actual_total_mem_kb > value) ? actual_total_mem_kb - value : 0;
          unsigned long long target_total = (unsigned long long)target_memory_kb;
          new_value = (target_total > used_kb) ? target_total - used_kb : 0;
        } else if (strcmp(key, "MemFree") == 0) {
          new_value = (unsigned long long)target_memory_kb;
        }
      }

      int written = snprintf(pos, remaining, "%s: %llu kB\n", key, new_value);
      if (written > 0 && (size_t)written < remaining) {
        pos += written;
        remaining -= written;
      }
    } else {
      int written = snprintf(pos, remaining, "%s\n", line);
      if (written > 0 && (size_t)written < remaining) {
        pos += written;
        remaining -= written;
      }
    }
    line = strtok_r(NULL, "\n", &saveptr);
  }

  FILE *tmp_file = real_fopen(TMP_MEMINFO_PATH, "w");
  if (tmp_file) {
    fwrite(scaled_meminfo, 1, strlen(scaled_meminfo), tmp_file);
    fclose(tmp_file);
  }
}

/**
 * @brief Background thread function that monitors /proc/meminfo for changes
 * @param arg Unused thread argument
 * @return NULL
 *
 * Watches for changes to the real /proc/meminfo using inotify and regenerates
 * the scaled copy when changes occur. Runs indefinitely with a small sleep to
 * avoid a busy loop when no events are available.
 */
static void *watcher_func(void *arg) {
  (void)arg;
  int inotify_fd = inotify_init1(IN_NONBLOCK);
  if (inotify_fd < 0)
    return NULL;

  int wd = inotify_add_watch(inotify_fd, "/proc/meminfo", IN_MODIFY);
  if (wd < 0) {
    close(inotify_fd);
    return NULL;
  }

  char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
  while (1) {
    ssize_t num_read = read(inotify_fd, buf, sizeof(buf));
    if (num_read > 0) {
      pthread_mutex_lock(&mutex);
      create_fake_meminfo_file();
      pthread_mutex_unlock(&mutex);
    }
    // Avoid a busy loop if no data is available.
    sleep(1);
  }
  close(inotify_fd);
  return NULL;
}

/**
 * @brief Redirects /proc/meminfo requests to the scaled copy
 * @param pathname Original file path
 * @return TMP_MEMINFO_PATH if pathname is "/proc/meminfo", otherwise pathname unchanged
 *
 * Lazily spawns the watcher thread on first access and ensures the temp file exists.
 */
static const char *redirect_meminfo_path(const char *pathname) {
  if (strcmp(pathname, "/proc/meminfo") == 0) {
    pthread_mutex_lock(&mutex);
    if (!watcher_thread_started) {
      create_fake_meminfo_file();
      pthread_create(&watcher_thread, NULL, watcher_func, NULL);
      watcher_thread_started = 1;
    }
    pthread_mutex_unlock(&mutex);
    return TMP_MEMINFO_PATH;
  }
  return pathname;
}

#define GET_REAL(func, return_type, args)                                                                              \
  static return_type(*real_##func) args = NULL;                                                                        \
  if (!real_##func)                                                                                                    \
    real_##func = dlsym(RTLD_NEXT, #func);

/**
 * @brief Interposed open(2) system call
 * @param pathname File path to openA
 * @param flags Open flags
 * @param ... Optional mode argument for O_CREAT
 * @return File descriptor on success, -1 on error
 *
 * Redirects /proc/meminfo requests to TMP_MEMINFO_PATH.
 */
int open(const char *pathname, int flags, ...) {
  GET_REAL(open, int, (const char *, int, ...));
  pathname = redirect_meminfo_path(pathname);
  va_list args;
  va_start(args, flags);
  mode_t mode = va_arg(args, mode_t);
  va_end(args);
  return real_open(pathname, flags, mode);
}

/**
 * @brief Interposed open64(2) system call
 * @param pathname File path to open
 * @param flags Open flags
 * @param ... Optional mode argument for O_CREAT
 * @return File descriptor on success, -1 on error
 *
 * Redirects /proc/meminfo requests to TMP_MEMINFO_PATH.
 */
int open64(const char *pathname, int flags, ...) {
  GET_REAL(open64, int, (const char *, int, ...));
  pathname = redirect_meminfo_path(pathname);
  va_list args;
  va_start(args, flags);
  mode_t mode = va_arg(args, mode_t);
  int result = real_open64(pathname, flags, mode);
  va_end(args);
  return result;
}

/**
 * @brief Interposed openat(2) system call
 * @param dirfd Directory file descriptor
 * @param pathname File path to open
 * @param flags Open flags
 * @param ... Optional mode argument for O_CREAT
 * @return File descriptor on success, -1 on error
 *
 * Redirects /proc/meminfo requests to TMP_MEMINFO_PATH.
 */
int openat(int dirfd, const char *pathname, int flags, ...) {
  GET_REAL(openat, int, (int, const char *, int, ...));
  pathname = redirect_meminfo_path(pathname);
  va_list args;
  va_start(args, flags);
  mode_t mode = va_arg(args, mode_t);
  int result = real_openat(dirfd, pathname, flags, mode);
  va_end(args);
  return result;
}

/**
 * @brief Interposed fopen(3) library function
 * @param pathname File path to open
 * @param mode File open mode
 * @return FILE pointer on success, NULL on error
 *
 * Redirects /proc/meminfo requests to TMP_MEMINFO_PATH.
 */
FILE *fopen(const char *pathname, const char *mode) {
  GET_REAL(fopen, FILE *, (const char *, const char *));
  pathname = redirect_meminfo_path(pathname);
  return real_fopen(pathname, mode);
}

/**
 * @brief Interposed fopen64(3) library function
 * @param pathname File path to open
 * @param mode File open mode
 * @return FILE pointer on success, NULL on error
 *
 * Redirects /proc/meminfo requests to TMP_MEMINFO_PATH.
 */
FILE *fopen64(const char *pathname, const char *mode) {
  GET_REAL(fopen64, FILE *, (const char *, const char *));
  pathname = redirect_meminfo_path(pathname);
  return real_fopen64(pathname, mode);
}

/**
 * @brief Interposed freopen(3) library function
 * @param pathname File path to open
 * @param mode File open mode
 * @param stream Existing FILE stream to reuse
 * @return FILE pointer on success, NULL on error
 *
 * Redirects /proc/meminfo requests to TMP_MEMINFO_PATH.
 */
FILE *freopen(const char *pathname, const char *mode, FILE *stream) {
  GET_REAL(freopen, FILE *, (const char *, const char *, FILE *));
  pathname = redirect_meminfo_path(pathname);
  return real_freopen(pathname, mode, stream);
}

/**
 * @brief Interposed freopen64(3) library function
 * @param pathname File path to open
 * @param mode File open mode
 * @param stream Existing FILE stream to reuse
 * @return FILE pointer on success, NULL on error
 *
 * Redirects /proc/meminfo requests to TMP_MEMINFO_PATH.
 */
FILE *freopen64(const char *pathname, const char *mode, FILE *stream) {
  GET_REAL(freopen64, FILE *, (const char *, const char *, FILE *));
  pathname = redirect_meminfo_path(pathname);
  return real_freopen64(pathname, mode, stream);
}

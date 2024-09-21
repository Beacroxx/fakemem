#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TARGET_MEMORY_KB (16ULL * 1000 * 1000 * 1000 * 1000 * 1000)
#define MAX_MEMINFO_SIZE 8192

static char fake_meminfo[MAX_MEMINFO_SIZE];
static int fake_meminfo_initialized = 0;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void create_fake_meminfo(const char *real_meminfo) {
  unsigned long long actual_total_mem_kb = 0;
  sscanf(real_meminfo, "MemTotal: %llu kB", &actual_total_mem_kb);

  long double scale_factor =
      (long double)TARGET_MEMORY_KB / actual_total_mem_kb;

  char *saveptr;
  char *line = strtok_r((char *)real_meminfo, "\n", &saveptr);
  while (line) {
    char key[64];
    unsigned long long value;
    char unit[8];
    if (sscanf(line, "%63[^:]: %llu %7s", key, &value, unit) == 3) {
      if (strcmp(unit, "kB") == 0) {
        unsigned long long scaled_value =
            (unsigned long long)(value * scale_factor);
        snprintf(fake_meminfo + strlen(fake_meminfo),
                 sizeof(fake_meminfo) - strlen(fake_meminfo), "%s: %llu kB\n",
                 key, scaled_value);
      } else {
        snprintf(fake_meminfo + strlen(fake_meminfo),
                 sizeof(fake_meminfo) - strlen(fake_meminfo), "%s\n", line);
      }
    } else {
      snprintf(fake_meminfo + strlen(fake_meminfo),
               sizeof(fake_meminfo) - strlen(fake_meminfo), "%s\n", line);
    }
    line = strtok_r(NULL, "\n", &saveptr);
  }
}

ssize_t read(int fd, void *buf, size_t count) {
  ssize_t (*original_read)(int, void *, size_t);
  original_read = dlsym(RTLD_NEXT, "read");

  char path[256];
  snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
  char target[256];
  ssize_t len = readlink(path, target, sizeof(target) - 1);
  if (len != -1) {
    target[len] = '\0';
    if (strcmp(target, "/proc/meminfo") == 0) {
      pthread_mutex_lock(&mutex);
      if (!fake_meminfo_initialized) {
        char buffer[MAX_MEMINFO_SIZE] = {0};
        ssize_t bytes_read =
            original_read(fd, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
         buffer[bytes_read] = '\0';
          create_fake_meminfo(buffer);
          fake_meminfo_initialized = 1;
        }
      }
      pthread_mutex_unlock(&mutex);

      size_t fake_meminfo_len = strlen(fake_meminfo);
      size_t copy_size = (count < fake_meminfo_len) ? count : fake_meminfo_len;
      memcpy(buf, fake_meminfo, copy_size);
      return copy_size;
    }
  }

  return original_read(fd, buf, count);
}

FILE *fopen(const char *pathname, const char *mode) {
  FILE *(*original_fopen)(const char *pathname, const char *mode);
  original_fopen = dlsym(RTLD_NEXT, "fopen");

  if (strcmp(pathname, "/proc/meminfo") == 0) {
    pthread_mutex_lock(&mutex);
    if (!fake_meminfo_initialized) {
      FILE *real_file = original_fopen(pathname, "r");
      if (real_file) {
        char buffer[MAX_MEMINFO_SIZE];
        size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, real_file);
        fclose(real_file);
        if (bytes_read > 0) {
          buffer[bytes_read] = '\0';
          create_fake_meminfo(buffer);
          fake_meminfo_initialized = 1;
        }
      }
    }
    pthread_mutex_unlock(&mutex);

    return fmemopen(fake_meminfo, strlen(fake_meminfo), mode);
  }

  return original_fopen(pathname, mode);
}

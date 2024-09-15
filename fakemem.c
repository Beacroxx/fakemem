#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>

#define TARGET_MEMORY_KB (16ULL * 1000 * 1000 * 1000 * 1000 * 1000)
#define MAX_MEMINFO_SIZE 8192

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
      char meminfo_buffer[MAX_MEMINFO_SIZE] = {0};
      ssize_t bytes_read =
          original_read(fd, meminfo_buffer, sizeof(meminfo_buffer) - 1);
      if (bytes_read > 0) {
        meminfo_buffer[bytes_read] = '\0';

        unsigned long long actual_total_mem_kb = 0;
        sscanf(meminfo_buffer, "MemTotal:        %llu kB",
               &actual_total_mem_kb);

        long double scale_factor =
            (long double)TARGET_MEMORY_KB / actual_total_mem_kb;

        char fake_meminfo[MAX_MEMINFO_SIZE] = {0};
        char *line = strtok(meminfo_buffer, "\n");
        while (line) {
          char key[64];
          unsigned long long value;
          char unit[8];
          if (sscanf(line, "%63[^:]: %llu %7s", key, &value, unit) == 3) {
            if (strcmp(unit, "kB") == 0) {
              unsigned long long scaled_value =
                  (unsigned long long)(value * scale_factor);
              snprintf(fake_meminfo + strlen(fake_meminfo),
                       sizeof(fake_meminfo) - strlen(fake_meminfo),
                       "%s: %llu kB\n", key, scaled_value);
            } else {
              snprintf(fake_meminfo + strlen(fake_meminfo),
                       sizeof(fake_meminfo) - strlen(fake_meminfo), "%s\n",
                       line);
            }
          } else {
            snprintf(fake_meminfo + strlen(fake_meminfo),
                     sizeof(fake_meminfo) - strlen(fake_meminfo), "%s\n", line);
          }
          line = strtok(NULL, "\n");
        }

        size_t fake_meminfo_len = strlen(fake_meminfo);
        size_t copy_size =
            (count < fake_meminfo_len) ? count : fake_meminfo_len;
        memcpy(buf, fake_meminfo, copy_size);
        return copy_size;
      }
      return bytes_read;
    }
  }

  return original_read(fd, buf, count);
}

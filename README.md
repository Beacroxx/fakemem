# FakeMem

FakeMem is a C library that tricks applications into thinking your computer has more memory than it actually does. It works by intercepting file operations on `/proc/meminfo` and redirecting them to a dynamically generated scaled version.

## How it works

1. The library intercepts file open operations (`open`, `fopen`, etc.) using LD_PRELOAD.
2. When a process tries to open `/proc/meminfo`, FakeMem redirects it to a temporary scaled copy.
3. A background watcher thread monitors the real `/proc/meminfo` for changes and updates the scaled copy accordingly.
4. Memory values are scaled based on environment variables, with support for human-friendly size formats.
5. The modified data is seamlessly served to applications without them knowing.

## Compilation

To compile the program, use:

```
clang -shared -fPIC -o fakemem.so fakemem.c -ldl
```

This will create a shared object file named `fakemem.so`.

## Environment Variables

FakeMem can be configured using the following environment variables:

`FAKEMEM_MEM`  
Specifies the target total memory size using human-friendly formats. Examples:
- `"1024"` - 1024 KiB
- `"16G"` - 16 gigabytes  
- `"800MiB"` - 800 mebibytes
- `"4T"` - 4 terabytes
- `"16E"` - 16 exabytes (default if not set)

`FAKEMEM_SCALE_ALL`  
If set to any value, all memory fields in `/proc/meminfo` are scaled by the same factor. If not set, only `MemTotal`, `MemFree`, and `MemAvailable` are adjusted to preserve the actual used memory amount.

## Usage

To use FakeMem, you need to preload it before running other applications:

1. For most shells:
```bash
export LD_PRELOAD=/path/to/fakemem.so
# Optionally configure memory size
export FAKEMEM_MEM="32G"
```

2. For Elvish shell specifically:
```elvish
set-env LD_PRELOAD /path/to/fakemem.so
# Optionally configure memory size
set-env FAKEMEM_MEM "32G"
```

After setting `LD_PRELOAD`, any program you run in that shell session will see the modified memory values.

## Examples

Show 32GB of memory:
```bash
export LD_PRELOAD=./fakemem.so
export FAKEMEM_MEM="32G"
free -h
```

Scale all memory fields proportionally:
```bash
export LD_PRELOAD=./fakemem.so
export FAKEMEM_MEM="64G"
export FAKEMEM_SCALE_ALL=1
fastfetch
```

## Technical Details

- FakeMem creates a temporary file at `/tmp/meminfo_scaled` with the modified memory information
- A background thread using inotify monitors `/proc/meminfo` for changes and updates the temporary file accordingly
- All file operations targeting `/proc/meminfo` are transparently redirected to the temporary file
- The library intercepts: `open()`, `open64()`, `openat()`, `fopen()`, `fopen64()`, `freopen()`, and `freopen64()`

## Note

This is for educational purposes only. Be cautious when using this in production environments as it may cause unexpected behavior in applications that rely on accurate memory information.

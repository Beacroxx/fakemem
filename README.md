# FakeMem

FakeMem is a simple C program that tricks applications into thinking your computer has more memory than it actually does. It does this by intercepting and modifying the output of `/proc/meminfo`.

## How it works

1. The program intercepts the `read` system call.
2. When a process tries to read `/proc/meminfo`, FakeMem captures the data.
3. It then scales up the memory values to make it appear as if the system has 16 exabytes of memory. (configurable via `TARGET_MEMORY_KB` on line 13). 16 EB is the maximum that `fastfetch` will display. Other applications like `btop` max out at ~180 PB. `free -h` is funny as it will show negative values past 8 EB.
4. The modified data is returned to the calling process.

## Compilation

To compile the program, use:

```
clang -shared -fPIC -o fakemem.so fakemem.c -ldl
```

This will create a shared object file named `fakemem.so`.

## Usage

To use FakeMem, you need to preload it before running other applications. You can do this in two ways:

1. For most shells:
```
export LD_PRELOAD=/path/to/fakemem.so
```

2. For Elvish shell specifically:
```
set-env LD_PRELOAD /home/bea/fakemem/fakemem.so
```

After setting `LD_PRELOAD`, any program you run in that shell session will see the inflated memory values.

## Note

This is for educational purposes only. Be cautious when using this in production environments as it may cause unexpected behavior in applications that rely on accurate memory information.

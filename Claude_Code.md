# Claude Code on Windows with MSYS2 Bash

## Out-of-box behavior

Claude Code's shell tool is named "Bash" and prefers the Bash environment provided by Git for Windows. Without Git for Windows, it falls back to PowerShell. Another "PowerShell" tool that always uses PowerShell is also available, but may need you set `CLAUDE_CODE_USE_POWERSHELL_TOOL=1` to enable it.

Claude Code automatically discovers `bash.exe` by locating the `git.exe` provided by Git for Windows and infers the location of `bash.exe` from there, but you can override it explicitly with `CLAUDE_CODE_GIT_BASH_PATH` variable.

The `bash.exe` Claude Code found is a wrapper provided by Git for Windows, which always launches the corresponding MSYS2 environment - e.g. if you installed `mingw-w64-ucrt-x86_64-git` then it always launches a UCRT64 environment.

So out-of-box you can get a functioning Claude Code using your MSYS2 Bash, but you always get UCRT64, even if you launch Claude Code from another environment like CLANG64.

This is not always ideal. UCRT64 provides the standard GCC toolchain, while CLANG64 provides LLVM toolchain with Sanitizers (ASAN, UBSAN) support, so based on the situation you may want to switch the environment to use. 

## Our Design

### Constraints

First, let's define what we want.

- We need a `bash.exe` wrapper that honors the current environment.
  - Inside MSYS2 it uses the environment as is.
  - Outside MSYS2 it initializes MSYS2 environment.

And actually that's all we want. We can point `CLAUDE_CODE_GIT_BASH_PATH` to it.

### Design

The constraints lead to our design:

1. The `bash.exe` wrapper should simply forward to the real `C:\msys64\usr\bin\bash.exe` executable if it's already inside MSYS2 environment.
  - This can be tested via the `MSYSTEM_PREFIX` environment variable.

2. The `bash.exe` wrapper picks the correct environment variant via the environment variable `MSYSTEM`.
  - By default, it uses `UCRT64` on `x86-64` or `CLANGARM64` on `aarch64`.

### Implementation

See `bash-wrapper.c`.

### Installation

1. Name the compiled binary of `bash-wrapper.c` as `bash.exe` and place it under `C:\msys64\cmd\bash-site` (a new dir).
2. Set `CLAUDE_CODE_GIT_BASH_PATH` to `C:\msys64\cmd\bash-site\bash.exe`.

And launch `claude` from a non-default MSYS2 environment like CLANG64. Now Claude Code can run commands in the CLANG64 environment.

## Process tree

### High-level native-launch flow

    native cmd.exe or PowerShell
      |
      v
    Claude Code executable
      |
      +-- PowerShell tool ---------> native environment
      |
      +-- Forced `CLAUDE_CODE_GIT_BASH_PATH`
            |
            v
          C:\msys64\cmd\bash-site\bash.exe
            |
            | configure MSYSTEM, CHERE_INVOKING
            v
          C:\msys64\usr\bin\bash.exe -l
            |
            | initialize MSYS2 environment with --login
            v
          selected MSYS2 environment

### High-level already-in-MSYS2 flow

    initialized UCRT64, CLANG64, MSYS, or another MSYS2 shell
      |
      | MSYSTEM_PREFIX is already set by /etc/msystem
      v
    Claude Code executable
      |
      | directly forward to REAL /usr/bin/bash
      v
    existing MSYS2 environment is preserved

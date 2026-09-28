# Codex on Windows with MSYS2 Bash

## Out-of-box behavior

Codex is very flexible to the Agent. The Agent can [dynamically choose the shell it wants to use](https://github.com/openai/codex/blob/main/codex-rs/tools/src/tool_config.rs#L9-L15); however, there's one important restriction:

The shell executable must live in `PATH`, and a manual full path specification does **NOT** work.

Codex:

1. Silently strips away the explicit path
2. Detect the type of the shell from the executable name
3. Use the executable found in `PATH` to execute the command
4. If the shell type is unknown, or the executable cannot be found, fallback to platform default (`PowerShell` on Windows)

MSYS2 Bash lives in `C:\msys64\usr\bin\bash.exe`, and putting `C:\msys64\usr\bin\` into system `PATH` is a bad idea.

Based on the behavior, you actually can launch Codex inside a MSYS2 shell, and when the Agent wants to run `bash` it will get the MSYS2 Bash automatically. The downside is that the `cmd` and `Powershell` shells also get MSYS2 environment and would behave weirdly. However, without extra configuration, MSYS2 launcher strips away extra `PATH` from Windows, so you must launch via the full path `"$USERPROFILE/AppData/Local/Programs/OpenAI/Codex/bin/Codex.exe"`.

The official executable `C:\msys64\ucrt64.exe` is not recognized as a known type of shell; while the official launcher `C:\msys64\msys2_shell.cmd -defterm -here -no-start -ucrt64` doesn't even fit in the shape Codex considers a "shell".

Moreover, if you have WSL installed (you likely have), it will create a `%userprofile%\AppData\Local\Microsoft\WindowsApps\bash.exe` which is in your `PATH` and redirects the commands to inside your default WSL VM. So even if you tell Agent to use `C:\msys64\usr\bin\bash.exe`, those command will be sent to `%userprofile%\AppData\Local\Microsoft\WindowsApps\bash.exe` and runs inside WSL. Ridiculous.

Before continue, it's strongly recommended to remove that WSL `bash.exe`. It brings more confusion than convenience, especially if you have installed MSYS2, and by asking for "bash", you more likely want the MSYS2 Bash instead.

## Our Design

### Constraints

First, let's define what we want.

- Expose `bash.exe` in `PATH` for Codex

Besides, it is standard behavior that the environment outside Codex will be inherited by the shell Codex launched; at the same time, Codex's ability to dynamically select the shell is sometimes useful, so we want to preserve that.

- Keep `cmd` and `Powershell` shells clean from MSYS2 environment variables.

And also our background rule:

- Never put a `bash.exe` into `PATH` systemwide.

### Design

The constraints lead to our design:

1. create a Codex wrapper that prepends an extra `PATH` and launches the real Codex.
  - We cannot let the wrapper set up MSYS2 environment directly, or the `cmd` and `Powershell` shells won't be clean.

2. The Codex wrapper should avoid modifying `PATH` if it's already inside the MSYS2 environment.
  - This can be tested via the `MSYSTEM_PREFIX` environment variable.

3. That extra `PATH` only contains a wrapper `bash.exe`
  - It will set up a proper MSYS2 environment like `msys2_shell.cmd` and forward the command to the real MSYS2 `bash.exe`

4. The `bash.exe` wrapper picks the correct environment variant via the environment variable `MSYSTEM`.
  - By default, it uses `UCRT64` on `x86-64` or `CLANGARM64` on `aarch64`.

### Implementation

See `bash-wrapper.c` and `codex-wrapper.c`.

### Installation

You should have added `C:\msys64\cmd` to `PATH`, which holds executables that are "safe" to be made available system-wide.

1. Name the compiled binary of `codex-wrapper.c` as `codex.exe` and place it under `C:\msys64\cmd`.
2. Name the compiled binary of `bash-wrapper.c` as `bash.exe` and place it under `C:\msys64\cmd\bash-site` (a new dir).

And launch `codex`. Now bash commands will "just work" for Codex.

Note: Codex installer adds its installation directory to user-level `PATH`, so your `C:\msys64\cmd` in system-level `PATH` should take precedence.

## Process tree

### High-level native-launch flow

    native cmd.exe or PowerShell
      |
      | resolves C:\msys64\cmd\codex.exe
      v
    Codex launcher trampoline
      |
      | MSYSTEM_PREFIX is absent
      | prepend C:\msys64\cmd\bash-site to this Codex process's PATH
      v
    real native Codex executable
      |
      +-- direct cmd.exe ----------> native environment
      |
      +-- direct pwsh.exe ---------> native environment
      |
      +-- bash lookup
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
    C:\msys64\cmd\codex.exe
      |
      | do not inject bash-site
      v
    real native Codex executable
      |
      | existing PATH resolves the real /usr/bin/bash
      v
    existing MSYS2 environment is preserved

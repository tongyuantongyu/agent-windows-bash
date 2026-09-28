# Cursor on Windows with MSYS2 Bash

## Out-of-box behavior

Cursor is VSCode-based and *inherits* the configurations in `settings.json`. So this config using the official MSYS2 launcher works fine for the Terminal Tab:

```json
{
    "terminal.integrated.defaultProfile.windows": "MinGW UCRT64",
    "terminal.integrated.profiles.windows": {
        "MinGW UCRT64": {
            "path": "C:\\msys64\\msys2_shell.cmd",
            "args": ["-defterm", "-here", "-no-start", "-ucrt64"],
            "overrideName": true,
            "icon": "terminal-bash",
            "color": "terminal.ansiYellow"
        }
    }
}
```

You can set the per-project default shell, and Agent shell will follow that. However, you can't switch the shell without restarting Cursor. 

```json
{
    "terminal.integrated.defaultProfile.windows": "PowerShell"
}
```

Unfortunately for MSYS2 Shell, the Agent side doesn't like the batch script. Specifically, on Windows it expects a shell named `git*bash.exe`, or "`bash.exe` that looks like from Git for Windows" (*1). Otherwise, it falls back to the default PowerShell shell silently.

*1: exact condition, from Cursor's code `cursor\resources\app\extensions\cursor-agent-host\dist\agent-host-daemon\dist\bin\daemon.cjs`:

```js
  const isWindows5 = process.platform === "win32";
  const gitBashFromEnv = isWindows5 && !userTerminalHint ? detectGitBashFromEnvironment() : void 0;
  const isGitBash = gitBashFromEnv !== void 0 || /git.*bash\.exe$/i.test(shell) || /program.*git.*bin.*bash\.exe$/i.test(shell);
  const bashIsOkay = !isWindows5 || isGitBash;
```

Additionally, the configuration in `settings.json` "*inherited*" is fact only a "hint", so the `args` or `envs` we configured are NOT honored for Agent shell.

## Our Design

### Constraints

First, let's define what we want.

- We need a Bash wrapper that replicates `C:\msys64\msys2_shell.cmd`, but is a proper executable with a name that masquerades the Git Bash.

- The wrapper must determine the desired `MSYSTEM` from its own name.

### Design

The constraints lead to our design:

1. The wrapper is named with prefix `git-cursor-bash.exe` or `git-cursor-<MSYSTEM>-bash.exe`.
  - The Git for Windows path is more complicated, so we satisfy the `git*bash.exe` branch.
  - We must include the desired `MSYSTEM` in the executable name.
2. The wrapper reads its own name to determine the `MSYSTEM` value, or use the default.
  - By default, it uses `UCRT64` on `x86-64` or `CLANGARM64` on `aarch64`.

### Implementation

See `bash-wrapper.c`.

### Installation

1. Name the compiled binary of `bash-wrapper.c` as `git-cursor-bash.exe` and place it under `C:\msys64\cmd`.
2. Configure in `settings.json`:

```json
{
    "terminal.integrated.defaultProfile.windows": "MinGW UCRT64",
    "terminal.integrated.profiles.windows": {
        "MinGW UCRT64": {
            "path": "C:\\msys64\\cmd\\git-cursor-bash.exe",
            "overrideName": true,
            "icon": "terminal-bash",
            "color": "terminal.ansiYellow"
        }
    }
}
```

If you want alternative `MSYSTEM`, make a copy or link of the wrapper with your desired `MSYSTEM` in it, for example:

```json
{
    "terminal.integrated.defaultProfile.windows": "MinGW CLANG64",
    "terminal.integrated.profiles.windows": {
        "MinGW CLANG64": {
            "path": "C:\\msys64\\cmd\\git-cursor-clang64-bash.exe",
            "overrideName": true,
            "icon": "terminal-bash",
            "color": "terminal.ansiRed"
        }
    }
}
```

Now Cursor Agents should pick up the MSYS2 Bash shell (They will be told that it's Git Bash though).

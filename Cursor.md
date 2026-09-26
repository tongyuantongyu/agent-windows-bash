# Cursor on Windows with MSYS2 Bash

## Out of box behavior

Cursor is VSCode based and inherits the configurations in `settings.json`. So this config using the official MSYS2 launcher works fine for the Terminal Tab:

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

You can config per-project default shell, and Agent shell will follow that. However you can't switch the shell without restarting Cursor. 

```json
{
    "terminal.integrated.defaultProfile.windows": "PowerShell",
}
```

Unfortunately for MSYS2 Shell, the Agent side doesn't like the batch script. Specifically, it expects a shell with path named `bash.exe` or `git-bash.exe` and doesn't care about `args`. Otherwise it fallbacks to the default PowerShell shell.

## Our Design

### Constraints

First let's define what do we want.

- We need a `bash.exe` wrapper that replicates `C:\msys64\msys2_shell.cmd`, but is a proper executable named `bash.exe`.

And we can configure our `MinGW UCRT64` profile to use the wrapper.

### Implementation

See `bash-site-wrapper.c`.

### Installation

1. Name the compiled binary of `bash-site-wrapper.c` as `bash.exe` and place it under `C:\msys64\cmd\bash-site` (a new dir).
2. Configure in `settings.json`:

```json
{
    "terminal.integrated.defaultProfile.windows": "MinGW UCRT64",
    "terminal.integrated.profiles.windows": {
        "MinGW UCRT64": {
            "path": "C:\\msys64\\cmd\\bash-site\\bash.exe",
            "overrideName": true,
            "icon": "terminal-bash",
            "color": "terminal.ansiYellow"
        }
    }
}
```

Now Cursor Agents should pick up the MSYS2 Bash shell (They will be told that it's Git Bash though).

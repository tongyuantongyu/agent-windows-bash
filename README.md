# MSYS2 Bash for Agents

This repo explains how to configure Agents to use **Bash** provided by MSYS2 as their shell.

LLMs typically get a standard Linux environment most of the time during their RL, so while they now have to run on Windows, we can at least give them their familiar bash environment.

Git Bash provided by Git for Windows is actually a cut-down version of the MSYS2 Bash. Using the full-featured MSYS2 additionally allows you to install common command-line utilities directly from the package manager.

## Installing MSYS2

This part should be easy. Simply follow the official guide.

After finishing installation, you should install `mingw-w64-ucrt-x86_64-git-for-windows-addons`, which basically gives you Git for Windows in the form of some MSYS2 packages.

## Configure for Agents

- [Codex](Codex.md)
- [Claude Code](Claude_Code.md)
- [Cursor](Cursor.md)

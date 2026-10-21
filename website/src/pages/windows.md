---
layout: ../layouts/Guide.astro
title: Make yourself at home.
description: Your favorite shell, your Linux tools, your kind of configuration. Here’s how the Windows pieces fit together.
eyebrow: YOUR WORKFLOW / WINDOWS & WSL
---

## Pick a shell

Kitsu is the terminal window around your shell. Use the shell you already work with:

```powershell
# PowerShell 7, if installed
kitsu pwsh.exe

# Windows PowerShell
kitsu powershell.exe

# Classic Command Prompt
kitsu cmd.exe
```

Kitsu uses ConPTY to connect Windows shell processes to its native Win32 windows. Shells still need to be installed on your machine; Kitsu does not install PowerShell 7 for you.

For scripts that need to wait for a Kitsu command and receive its output, use the console shim, `kitsu.com`. The GUI launcher, `kitsu.exe`, opens without an additional console window.

## WSL

Kitsu can host a Windows Subsystem for Linux session. Install WSL and your preferred Linux distribution first, then launch it through Kitsu:

```powershell
kitsu wsl.exe
```

For a particular installed distribution, pass its name to WSL:

```powershell
# Replace Ubuntu with a name from wsl --list
kitsu wsl.exe -d Ubuntu
```

### Set up the Linux kitten helper

Use the Kitsu installer’s WSL task, or run this command from cmd or PowerShell after an installer-managed installation:

```powershell
kitsu +wsl-setup
```

Inside the Linux distribution, use the bundled **Linux `kitten`** helper. Don’t substitute the Windows `kitten.exe`. The Linux helper is shipped through the installer, not as a separate release download.

For example, after setup, run inside WSL:

```bash
kitten --version
```

If setup does not work as expected, see [WSL troubleshooting](../troubleshooting/#wsl-tools-arent-working).

## Configuration

Kitsu keeps Kitty’s configuration names for compatibility. The default Windows configuration is:

```text
%APPDATA%\kitty\kitty.conf
```

In PowerShell, you can create the directory and open the file with:

```powershell
New-Item -ItemType Directory -Force "$env:APPDATA\kitty"
notepad "$env:APPDATA\kitty\kitty.conf"
```

If you set `KITTY_CONFIG_DIRECTORY`, Kitsu uses that directory instead. Configuration belongs in `kitty.conf`, not `kitsu.conf`.

### A small starting point

Paste these settings into `kitty.conf`, adjust them to your taste, and open a new Kitsu window:

```text
font_family Cascadia Mono
font_size 12.0
window_padding_width 12
cursor_shape beam
scrollback_lines 10000
enabled_layouts splits,tall,stack
```

Choose a font installed on your machine. For the complete list of options, use the [upstream Kitty configuration reference](https://sw.kovidgoyal.net/kitty/conf/). Windows-specific behavior can differ; report Kitsu problems in the Kitsu issue tracker.

### A fresh coat of paint

Try Kitty’s theme kitten from cmd or PowerShell:

```powershell
kitsu +kitten themes
```

Explore [kittens](https://sw.kovidgoyal.net/kitty/kittens_intro/), [tabs and layouts](https://sw.kovidgoyal.net/kitty/overview/), and [key bindings](https://sw.kovidgoyal.net/kitty/conf/#keyboard-shortcuts) in the shared upstream documentation. When an example launches `kitty`, use `kitsu` on Windows.

## Names that stay familiar

The `kitty` Python package, `kitten` command, `KITTY_*` environment variables, `xterm-kitty` terminfo, and terminal protocols retain their upstream names. Existing scripts and configurations can keep those names, and the Windows launchers retain `kitty.exe` and `kitty.com` aliases.

**Need a hand?** [Check your installation →](../troubleshooting/)

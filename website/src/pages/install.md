---
layout: ../layouts/Guide.astro
title: Meet your new terminal.
description: A few steps from download to your first Kitsu session. Choose the installer for an everyday home, or the ZIP for a portable setup.
eyebrow: GETTING STARTED / INSTALLATION
slug: install
---

## Before you start

Kitsu’s Windows packages are for **Windows 10 version 1809 or newer**, including Windows 11, on **x86_64**. Kitsu uses native Win32 windows and ConPTY for its shell sessions.

The package includes its runtime. You don’t need a separate Python installation to run it. PowerShell 7 and WSL are optional tools you can install separately if you want to use them.

## The installer — recommended

1. Download the Windows installer from the buttons above or [GitHub Releases](https://github.com/fhaze/kitsu/releases/latest). Its filename ends in `-windows-x86_64-setup.exe`.
2. Run it and follow the setup instructions. Choose the **PATH** task if you want to launch Kitsu from cmd or PowerShell.
3. If you use WSL, choose the WSL integration task to install the bundled Linux `kitten` helper into your distributions.
4. Open Kitsu from the Start menu, or open a **new** PowerShell/cmd window and run:

```powershell
kitsu --version
kitsu pwsh.exe
```

`pwsh.exe` launches PowerShell 7, if installed. For a classic Command Prompt session, run `kitsu cmd.exe`. For the Windows PowerShell that comes with Windows, run `kitsu powershell.exe`.

> Windows SmartScreen can show a reputation warning for a new release. Check that your download came from the Kitsu repository, and review the publisher and signature details shown by Windows before deciding whether to run it.

## The portable ZIP

Download the asset ending in `-windows-x86_64.zip`, extract the **entire** archive into a folder, and run `bin\kitsu.exe` from that folder. Keep the bundled `bin`, `lib`, and other package directories together.

To check the version from PowerShell while in the extracted directory:

```powershell
.\bin\kitsu.com --version
```

The `kitsu.com` console shim waits for command-line invocations and preserves their output. `kitsu.exe` opens the GUI without creating another console window. The portable ZIP does not install Start menu entries or add itself to PATH.

WSL helpers are distributed through the installer. They are not published as standalone release downloads.

## Check that everything is settled in

Run a quick, read-only check from cmd or PowerShell:

```powershell
kitsu +health-check --mode=quick
```

This checks the packaged runtime and resources without opening a terminal window. See [troubleshooting](../troubleshooting/) if it reports a problem.

## Keeping up to date

For an installation managed by the installer, run:

```powershell
kitsu +update
```

The updater is for installer-managed installations. For a portable setup, download a fresh ZIP and extract it into a new folder. Your normal configuration lives outside the application folder.

You can always find release notes and older versions on [GitHub Releases](https://github.com/fhaze/kitsu/releases).

## Coming from Kitty for Windows?

The installer keeps the same application ID and install directory, so Kitsu upgrades existing installations in place. Existing selected installer tasks are retained. The `kitty.exe` and `kitty.com` compatibility aliases remain available for scripts and shell integration.

Your configuration remains at `%APPDATA%\kitty\kitty.conf`, or the location selected through `KITTY_CONFIG_DIRECTORY`. Existing configurations and WSL setups do not need to be renamed.

**Next:** [Set up your shells and configuration →](../windows/)

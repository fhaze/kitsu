---
layout: ../layouts/Guide.astro
title: Let’s find your way back.
description: Start with a quick health check, follow the clues, and get back to your terminal. A few common Windows fixes are collected here.
eyebrow: A LITTLE HELP / TROUBLESHOOTING
---

## Start with a health check

From cmd or PowerShell, run:

```powershell
kitsu +health-check --mode=quick
```

Quick mode is read-only. It checks the runtime, native imports, fonts, shader compilation, package resources, PATH, and configuration parsing without opening a window. In a release package, it needs no separate Python installation, network access, downloads, or administrator privileges.

To save a structured report:

```powershell
kitsu +health-check --format=json --output health.json
```

The file is written locally. Nothing is uploaded. Review it before sharing: reports redact your home-directory prefix and credentials, but installation paths outside your home directory can still identify your setup.

### Try the native checks

If you need to exercise terminal launch and rendering:

```powershell
kitsu +health-check --mode=native --timeout=180 --probe-timeout=20
```

Native mode opens a **visible, disposable window** using temporary configuration and cache directories. It includes quick checks and exercises shell launch, screen behavior, panes, tabs, resizing, remote control, and a small rendered image.

A passing result confirms the checks that ran. It does not certify every GPU driver, physical keyboard and mouse input, clipboard interactions, WSL sessions, or every feature. See the [full Windows diagnostics reference](https://github.com/fhaze/kitsu/blob/master/docs/windows-health-check.rst) for coverage and report details.

## “Kitsu” is not recognized

Open a new cmd or PowerShell window after installation so it receives the updated PATH. Check which executable Windows finds:

```powershell
where.exe kitsu
```

If nothing is found, run the installer again and select its PATH task, or launch Kitsu from the Start menu. For portable ZIPs, use the launcher’s path directly:

```powershell
.\bin\kitsu.com --version
```

PATH may contain an older Kitty-for-Windows installation. The quick health check can flag installation identity and PATH conflicts. Avoid copying individual runtime DLLs onto PATH; keep the package’s directories together.

## WSL tools aren’t working

Verify that your distribution is installed and can start outside Kitsu:

```powershell
wsl --list --verbose
```

Then run `kitsu +wsl-setup` from Windows. Inside the distribution, use the Linux `kitten` helper installed by Kitsu. The Windows `kitten.exe` is not its replacement. WSL helpers are included with the installer; use that distribution method for integration.

The quick health check verifies bundled WSL payloads without starting a distribution. A passing payload check does not mean a WSL session was tested.

## Configuration changes don’t appear

Confirm that you edited `%APPDATA%\kitty\kitty.conf`. If you set `KITTY_CONFIG_DIRECTORY`, check the file in that directory instead. Kitsu retains Kitty’s configuration filename and environment variables.

Open a new Kitsu window after saving, then run the quick health check to look for configuration parsing warnings. Configuration values are withheld from the diagnostic report.

## The runtime cannot start

The installed PowerShell bootstrap can help when Python cannot initialize. Use the path to your actual installation:

```powershell
powershell -NoProfile -File 'C:\path\to\kitsu\bin\health-check.ps1' -Format json
```

The bootstrap starts the sibling executable and reports if the runtime cannot produce a health report. It does not change execution policy or repair your installation.

## Reading the results

| Status  | What it tells you                                          |
| ------- | ---------------------------------------------------------- |
| PASS    | The named check observed the expected behavior.            |
| FAIL    | A tested assertion failed. Read the suggested remediation. |
| WARN    | The check completed and found an advisory limitation.      |
| SKIP    | This check was not applicable.                             |
| BLOCKED | A prerequisite or timeout prevented testing.               |

Warnings alone do not fail the health check. Exit code `1` means a feature check failed; `2` means required coverage was incomplete; `3` means a runner or output error. Ctrl+C produces a partial report and exits with `130`.

## Still stuck? Send a useful report

Open a [Kitsu issue](https://github.com/fhaze/kitsu/issues) with your Kitsu version, Windows version, installer or portable setup, the shell you were using, and steps to reproduce the problem. Add the relevant health-check results after reviewing them.

Kitsu is an independent fork. Please send Windows-specific reports to **Kitsu’s** issue tracker so they reach the right project.

**Back to the basics:** [Installation →](../install/) · [Windows and WSL →](../windows/)

Windows health check
====================

Run ``kitty +health-check`` from cmd or PowerShell for read-only quick diagnostics.
Use the matching ``kitty.com`` if invoking it from a script that must wait. No
test suite, Python installation, network connection, downloads or administrator
access is needed in a release package.

.. code-block:: powershell

    kitty +health-check
    kitty +health-check --format=json --output health.json
    kitty +health-check --mode=native --timeout=180 --probe-timeout=20
    powershell -NoProfile -File 'C:\path\to\kitty\bin\health-check.ps1' -Format json

The PowerShell bootstrap is for a runtime that cannot initialize, not a second
probe engine. It launches the sibling executable, waits for it, and reports a
fixed error if Python cannot produce a report. It does not change execution policy.

Quick mode checks runtime/native imports, SSL trust availability (offline), an
in-memory SQLite query, GLFW loading (without creating a window), font enumeration,
the bundled symbol font, a tiny shader compilation, terminfo, shell-integration
resources/line endings, release WSL payloads, PATH conflicts, installation identity
and configuration parsing. It also flags missing ``PATHEXT`` entries that prevent
PowerShell from treating console helpers as executables. Generated config includes
are never executed. Config values and errors containing those values are withheld. WSL payload checks do not
start WSL; source builds may omit Linux payloads. Package resource failures are
required, not silently skipped. Installation/PATH/config warnings are advisory.

Native mode includes quick checks and opens a **visible disposable window**. It
uses a clean temporary configuration/cache, a random authenticated loopback
endpoint, absolute executables and an owned Windows process job. It never connects
to your existing Kitty instances. The job includes ConPTY and descendant processes
and is terminated and checked empty on completion, timeout or cancellation. Worker
processes wait for job assignment before doing any work. Temporary files and
passwords are deleted; no repairs, uploads or persistent configuration changes are
performed. The overall timeout limits probes; bounded process cleanup can take a
few additional seconds. Ctrl-C emits a partial report and returns 130.

Native probes exercise:

* Console-shim waiting, stdin/stdout/stderr, cmd redirection, argument quoting and
  nonzero exit propagation.
* Authenticated remote control and explicit rejection of unauthenticated clients.
* Screen SGR, cursor/erase, wrapping, scrollback, alternate-screen restoration and reset.
* Repeated cmd/available PowerShell terminal launches, Unicode, spaced paths and
  arguments, and observed child status/output (not merely RC command success).
* Pane creation/focus/close, tabs, live child dimensions after OS-window resize.
* A generated two-color image streamed using ``icat`` in the child terminal and
  coarse screenshot pixel sampling from Kitty's rendered framebuffer.
* Renderer identity and the **selected** system/bundled ConPTY provider, separately
  from DLL presence. Software rendering is a warning, not a GPU certification.

Reports and exit status
----------------------

Every result has a stable ID, required flag, prerequisites, timeout, elapsed
duration, status, summary, evidence and remediation. JSON has ``schema_version: 1``
and no progress/ANSI noise. Diagnostics go to stderr. ``--output`` duplicates the
chosen report to a local UTF-8 file; no file is sent anywhere. Reports redact the
home-directory prefix and per-run credentials, omit raw subprocess errors and
never dump environment variables, clipboard data or user terminal history.
Executable paths outside the home directory can still identify the installation;
review reports before sharing them.

``PASS`` means the listed assertion was observed. ``FAIL`` means a tested feature
failed. ``WARN`` means a completed check found an advisory limitation. ``SKIP``
means not applicable. ``BLOCKED`` means a prerequisite, deadline or environment
prevented testing. Unrun checks stay in partial reports, never become passes.

Exit codes: **0** selected required checks completed without failures; **1** a
feature failed (even an optional one); **2** required coverage is incomplete;
**3** a runner/output/internal error; **130** cancellation. Warnings alone do not
fail the run. Each probe and the run as a whole have bounded deadlines. Independent
checks continue after a probe timeout.

Limits
------

Passing does **not** mean all features work. WSL sessions, physical keyboard/mouse,
clipboard/paste confirmation, IME, suspend/resume, multi-monitor and physical
display/compositor correctness are not tested here. Screenshots verify the
framebuffer, not a physical monitor. Neither software nor hardware framebuffer
success certifies other GPU drivers. Optional PowerShell 7 is tested only if
installed; no shells are installed automatically. The runner is Windows-only.

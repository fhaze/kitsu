Install kitty
========================

Binary install
----------------

.. highlight:: sh

You can install pre-built binaries of |kitty| if you are on macOS or Linux using
the following simple command:

.. code-block:: sh

    _kitty_install_cmd


The binaries will be installed in the standard location for your OS,
:file:`/Applications/kitty.app` on macOS and :file:`~/.local/kitty.app` on
Linux. The installer only touches files in that directory. To update kitty,
simply re-run the command.

.. warning::
   **Do not** copy the kitty binary out of the installation folder. If you want
   to add it to your :envvar:`PATH`, create a symlink in :file:`~/.local/bin` or
   :file:`/usr/bin` or wherever. You should create a symlink for the :file:`kitten`
   binary as well. Whichever folder you choose to create the symlink in should
   be in the **systemwide** PATH, a folder added to the PATH in your shell rc
   files will not work when running kitty from your desktop environment.


Manually installing
---------------------

If something goes wrong or you simply do not want to run the installer, you can
manually download and install |kitty| from the `GitHub releases page
<https://github.com/kovidgoyal/kitty/releases>`__. If you are on macOS, download
the :file:`.dmg` and install as normal. If you are on Linux, download the
tarball and extract it into a directory. The |kitty| executable will be in the
:file:`bin` sub-directory.


Windows
------------

On Windows, download :file:`kitty-<version>-windows-x86_64-setup.exe` from the
`GitHub releases page <https://github.com/kovidgoyal/kitty/releases>`__ and run
it. The installer:

* installs for the current user by default (no administrator rights needed),
  or for all users if you choose so
* can add the :file:`bin` directory containing :program:`kitty` and
  :program:`kitten` to your :envvar:`PATH`
* can install the Linux :program:`kitten` binary into your WSL distributions
  (see below)

To update kitty, simply run the installer for the newer version. It upgrades
the existing installation in place, keeping the install location and the
options you chose the first time. Your configuration is not touched, it lives
in :file:`%USERPROFILE%\\.config\\kitty\\kitty.conf` outside the install
directory.

Unattended installation and upgrades are supported via the usual
`Inno Setup <https://jrsoftware.org/ishelp/index.php?topic=setupcmdline>`__
switches, for example::

    kitty-0.44.0-windows-x86_64-setup.exe /VERYSILENT /CURRENTUSER /TASKS=addtopath,wsl

If you prefer not to run an installer, :file:`kitty-<version>-windows-x86_64.zip`
contains the same files, extract it anywhere and run :file:`bin\\kitty.exe`.

The :file:`bin` directory contains both :file:`kitty.exe` and :file:`kitty.com`.
:file:`kitty.exe` is a GUI program, so that starting it from the start menu
does not open a console window, but that also means :program:`cmd.exe` and
PowerShell do not wait for it. :file:`kitty.com` is a small console program
that runs :file:`kitty.exe` and waits for it, and since ``.COM`` precedes
``.EXE`` in the :code:`PATHEXT` environment variable, typing ``kitty`` in a
shell runs it. This is what makes ``kitty @ ls``, ``kitty +kitten ssh`` or
``kitty --version`` work from the shell prompt. Use :file:`kitty.exe` explicitly (or ``kitty --detach``)
when you do not want the shell to wait.


Command paths on Windows
^^^^^^^^^^^^^^^^^^^^^^^^

On native Windows, command lines in :file:`kitty.conf` and session ``launch``
lines keep backslashes in drive-letter and UNC paths. This applies to ``shell``,
``editor`` (including :envvar:`VISUAL` and :envvar:`EDITOR`), ``command_on_bell``,
``scrollback_pager``, ``open_url_with`` and mapped ``launch`` actions. Quote each
path or argument containing spaces; an unquoted space still separates arguments::

    shell "C:\Program Files\PowerShell\7\pwsh.exe" -NoLogo
    editor "C:\Program Files\Neovim\bin\nvim.exe"
    map f1 launch --cwd="C:\Users\me\My Project" C:\tools\app.exe

Single quotes also group arguments and keep all enclosed backslashes literal.
Outside single quotes, backslashes are literal except immediately before a double
quote: each pair produces one backslash, and an odd remaining backslash escapes
the quote. For example, write a quoted directory ending in a backslash as
``"C:\My Project\\"`` or ``'C:\My Project\'``. UNC prefixes such as
``\\server\share`` are preserved. Forward slashes remain usable. These are
configuration quoting rules, not PowerShell or cmd.exe syntax; POSIX/WSL builds
retain their existing shell-style quoting. Native ``kitten run-shell --shell``
uses the same rules for its shell value (which must first pass through your
invoking shell's quoting).


SSH from Windows
^^^^^^^^^^^^^^^^^^^

kitty sets :envvar:`TERM` to ``xterm-kitty`` and plain :program:`ssh` (including
``gcloud compute ssh``) forwards it to the remote host. On a host without the
kitty terminfo files this results in errors such as ``'xterm-kitty': unknown
terminal type.`` from :program:`clear`, and
terminfo based programs such as :program:`vim`, :program:`less` or :program:`zsh`
may misbehave, for example not erasing on :kbd:`Backspace`. This is the same
on all platforms, not a Windows or WSL specific problem. Use the :doc:`ssh
kitten </kittens/ssh>` instead of :program:`ssh`, it copies the terminfo files
and the :ref:`shell integration <shell_integration>` to the remote host
automatically::

    kitten ssh myserver
    kitty +kitten ssh myserver

Both forms are equivalent, the kitten also works from inside WSL once the WSL
:program:`kitten` is installed, see above. If you must use plain
:program:`ssh` against hosts you cannot install terminfo on, set ``term
xterm-256color`` in :file:`kitty.conf` instead, see :opt:`term`.


kitten in WSL
^^^^^^^^^^^^^^^^

The Windows package bundles Linux builds of :program:`kitten` (``amd64`` and
``arm64``) so that :doc:`kittens </kittens_intro>`, the :ref:`shell integration
<shell_integration>` and the :doc:`remote control </remote-control>` work inside
WSL just like on Linux. Select the *WSL* task in the installer, or run at any
time::

    kitty +wsl-setup

This copies :program:`kitten` to :file:`~/.local/bin/kitten` in every WSL
distribution and adds that directory to the :envvar:`PATH` of your login shell,
by editing :file:`~/.bashrc` (bash), :file:`~/.zshrc` (zsh),
:file:`~/.config/fish/conf.d/kitty-wsl-kitten.fish` (fish),
:file:`~/.tcshrc`/:file:`~/.cshrc` (tcsh/csh) or :file:`~/.profile` (any other
shell). The edit is a clearly marked block that is updated, never duplicated,
on subsequent runs. Distributions belonging to other programs, such as
``docker-desktop``, are skipped. To only set up some distributions or to skip
editing shell startup files::

    kitty +wsl-setup Ubuntu-24.04 Debian
    kitty +wsl-setup --no-path

``kitty +wsl-setup --uninstall`` removes the binary and the :envvar:`PATH`
block again, the Windows uninstaller does this automatically when the WSL task
was selected.

To start kitty directly in the default WSL distribution, add this to
:file:`kitty.conf`::

    shell wsl.exe --cd ~ --exec ./.local/bin/kitten run-shell

This starts in the WSL user's home directory and uses the installed
:program:`kitten` to enable shell integration. To always use a particular
distribution, add ``--distribution Ubuntu`` before ``--cd``. Without the WSL
:program:`kitten` setup, use ``shell wsl.exe --cd ~`` instead.


Desktop integration on Linux
--------------------------------

If you want the kitty icon to appear in the taskbar and an entry for it to be
present in the menus, you will need to install the :file:`kitty.desktop` file.
The details of the following procedure may need to be adjusted for your
particular desktop, but it should work for most major desktop environments.

.. code-block:: sh

    # Create symbolic links to add kitty and kitten to PATH (assuming ~/.local/bin is in
    # your system-wide PATH)
    ln -sf ~/.local/kitty.app/bin/kitty ~/.local/kitty.app/bin/kitten ~/.local/bin/
    # Place the kitty.desktop file somewhere it can be found by the OS
    cp ~/.local/kitty.app/share/applications/kitty.desktop ~/.local/share/applications/
    # If you want to open text files and images in kitty via your file manager also add the kitty-open.desktop file
    cp ~/.local/kitty.app/share/applications/kitty-open.desktop ~/.local/share/applications/
    # Update the paths to the kitty and its icon in the kitty desktop file(s)
    sed -i "s|Icon=kitty|Icon=$(readlink -f ~)/.local/kitty.app/share/icons/hicolor/256x256/apps/kitty.png|g" ~/.local/share/applications/kitty*.desktop
    sed -i "s|Exec=kitty|Exec=$(readlink -f ~)/.local/kitty.app/bin/kitty|g" ~/.local/share/applications/kitty*.desktop
    # Make xdg-terminal-exec (and hence desktop environments that support it use kitty)
    echo 'kitty.desktop' > ~/.config/xdg-terminals.list

.. note::
    In :file:`kitty-open.desktop`, kitty is registered to handle some supported
    MIME types. This will cause kitty to take precedence on some systems where
    the default apps are not explicitly set. For example, if you expect to use
    other GUI file managers to open dir paths when using commands such as
    :program:`xdg-open`, you should configure the default opener for the MIME
    type ``inode/directory``::

        xdg-mime default org.kde.dolphin.desktop inode/directory

.. note::
    If you use the venerable `stow <https://www.gnu.org/software/stow/>`__
    command to manage your manual installations, the following takes care of the
    above for you (use with :code:`dest=~/.local/stow`)::

        cd ~/.local/stow
        stow -v kitty.app


Customizing the installation
--------------------------------

.. _nightly:

* You can install the latest nightly kitty build with ``installer``:

  .. code-block:: sh

     _kitty_install_cmd \
         installer=nightly

  If you want to install it in parallel to the released kitty specify a
  different install locations with ``dest``:

  .. code-block:: sh

     _kitty_install_cmd \
         installer=nightly dest=/some/other/location

* You can specify a specific version to install, with:

  .. code-block:: sh

     _kitty_install_cmd \
         installer=version-0.35.2

* You can tell the installer not to launch |kitty| after installing it with
  ``launch=n``:

  .. code-block:: sh

     _kitty_install_cmd \
         launch=n

* You can use a previously downloaded dmg/tarball, with ``installer``:

  .. code-block:: sh

     _kitty_install_cmd \
         installer=/path/to/dmg or tarball


Uninstalling
----------------

All the installer does is copy the kitty files into the install directory. To
uninstall, simply delete that directory.


Building from source
------------------------

|kitty| is easy to build from source, follow the :doc:`instructions <build>`.

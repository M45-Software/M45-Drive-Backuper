# M45 Drive Backuper

A native Qt 6 KDE Plasma tray app for configuring and monitoring this machine's existing Borg backup job. Based on the DriveWatch project's native Qt window/tray structure, palette-aware presentation, single-instance D-Bus integration, XDG launcher/autostart, CMake build and Debian packaging. DriveWatch remains unchanged; its MIT attribution is retained in LICENSE.

## Features

- System tray status, click-to-open menu, backup shortcut, settings and journal.
- Overview shows the service result, next scheduled run, destination UUID and available space.
- Detects the existing backup filesystem at a different mount path. The user can choose its detected path and review the change before saving.
- Configure schedule (1/2/3/4/6/8/12 hours or daily), hourly/daily/weekly/monthly retention, and Borg exclusions.
- Start a backup, enable/disable its existing timer, and read the repository archive catalog.
- Login startup toggle, attention notifications and Qt desktop colors.
- Read-only monitoring uses the desktop user's journal permissions. Administrative actions use `pkexec` with a root-owned fixed-purpose helper; the GUI never runs as root.

Opening or installing M45 Drive Backuper does **not** modify the backup script, schedule, exclusions, repository or mounts, and does not run a backup. All backup changes are explicit app actions. Settings saves show a review dialog, validate values and the destination UUID, reject changes during an active backup, save recovery copies under `/var/lib/m45-drive-backuper/settings-backups/`, and atomically replace each file. A failed save restores previous files. Schedule changes retain the existing boot trigger and randomized delay. Turning off the schedule allows an active run to finish.

This release adapts `/usr/local/sbin/local-system-backup`, `/etc/local-system-backup.exclude`, and `local-system-backup.{service,timer}`. It recognizes that script's current structure and refuses unsupported formats. It is not yet a general-purpose repository setup wizard. The source is `/home/dist`; destination UUID, compression, free-space threshold and the backup implementation remain managed by the existing script. It does not initialize repositories, mount disks, restore files, change encryption, or run pruning independently.

## Build, test and install

Requires CMake, C++17, Qt 6 Widgets/DBus development packages, Python 3, Borg 1.2, systemd and PolicyKit. Python tests use the distribution Python because it supplies Borg's pattern parser.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build build --parallel
ctest --test-dir build --output-on-failure
(cd build && cpack -G DEB)
sudo apt install ./build/m45-drive-backuper_0.1.0_amd64.deb
m45-drive-backuper --show
```

The package adds an Applications menu entry and login autostart. No root background collector is needed: the app reads status asynchronously every 15 seconds. Install the package to use real monitoring/actions; the helper has a fixed installed path. Members of `adm` or `systemd-journal` can read the service journal. If journal access is restricted, the app reports that limitation; status is still available. Quitting the tray monitor does not affect system backups. Existing system failure notifications remain in place.

```sh
QT_QPA_PLATFORM=offscreen ./build/m45-drive-backuper --demo --screenshot docs/screenshots/overview.png
QT_QPA_PLATFORM=offscreen ./build/m45-drive-backuper --demo --page settings --screenshot docs/screenshots/settings.png
```

Demo mode uses synthetic data and never invokes the helper, saves preferences, registers the real D-Bus name, or sends notifications. Screenshot timestamps illustrate the sample failure rather than the current state of a machine.

## Interpretation and limitations

The service's latest result is an attempt result, not proof of a valid or recent archive. Borg warnings are normalized to service success by the existing job; inspect the journal for details. The archive tab reads stored archive names/timestamps on demand with administrator authentication and a short Borg lock wait. Checkpoint archives may be incomplete. No archive integrity check or live percentage is provided; the existing script emits statistics after its runs. Root authentication may be cancelled without changing anything.

Retention settings can delete old archives on a future backup. Copies of old settings cannot recover deleted archives. Settings files are replaced individually; rollback handles ordinary errors but cannot provide a filesystem-wide transaction across a power loss. An already-open Settings editor is not overwritten by status polling; reopen the app to load external edits before saving.

Remove with `sudo apt remove m45-drive-backuper`, then quit the tray monitor. This preserves the existing backup system and per-user startup/preferences.

Implementation references: [Qt system tray](https://doc.qt.io/qt-6/qsystemtrayicon.html), [Borg archive listing](https://borgbackup.readthedocs.io/en/1.2.8/usage/list.html).

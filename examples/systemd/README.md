# rustLaunchSite systemd service example

## Introduction
This directory contains an example of how to set up rustLaunchSite to run as a systemd service on modern Linux distros. This was developed on Debian 13.

## Usage
### Primary service installation
1. Create a `rust` user account and group
   - **NOTE:** This is recommended because `steamcmd` caches updates in the invoking user's home directory
1. Run `chown -R rust:rust` on your Rust server installation directory
   - **NOTE:** This is required in order for rustLaunchSite to be able to install updates, and to write `log.txt` if you decide to put it there
1. Copy `rustLaunchSited` to `/opt/rustLaunchSite`
1. Create an `/opt/rustLaunchSite/rustLaunchSite.jsonc` config file with appropriate settings
1. Copy all files from this directory to `/opt/rustLaunchSite`
1. Run `chown -R rust:rust /opt/rustLaunchSite`
   - **NOTE:** This is recommended so that `reason.txt` can be written by the daily restart script, and so that rustLaunchSite can delete it upon read
1. Run `systemctl enable /opt/rustLaunchSite/rustLaunchSite.service` to activate the rustLaunchSite systemd service configuration
   - **NOTE:** This will cause rustLaunchSite (and your Rust server) to auto-start on every OS boot
   - *Optional:* Run `systemctl start rustLaunchSite.service` to start rustLaunchSite (and your Rust server) immediately

### *Optional: Daily restart installation*
Run the following to enable daily restarts:
```sh
systemctl enable /opt/rustLaunchSite/rlsDailyRestart.service
systemctl enable --now /opt/rustLaunchSite/rlsDailyRestart.timer
```
See `rlsDailyRestart.sh` for customizing the restart message presented to online players.

### Updating service/timer config(s)
If you update any of the `*.service` or `*.timer` files, you must run `systemctl daemon-reload`. You may also then need to restart the applicable unit(s) if already running.

### Manually (re)starting or stopping the server
To manually (re)start the server, run `systemctl restart rustLaunchSite.service`.

To manually stop the server, run `systemctl stop rustLaunchSite.service`.

### Viewing logs
rustLaunchSite uses POSIX `syslog()` calls for logging. On modern Linux, this output should be visible by running `journalctl`.

### Help, how do I undo all of this?!
To back out all systemd changes described above:
```sh
systemctl disable rlsDailyRestart.timer
systemctl disable rlsDailyRestart.service
systemctl disable rustLaunchSite.service
```

## File Descriptions
This section describes all the files in this directory.

Note that I tried to include helpful comments in each file to point out and explain various features, so only a high level summary will be provided here.

### rustLaunchSite.service
This is the main systemd service config file, which runs rustLaunchSite as a system service. By default, it waits for the OS to fully boot and setup a network connection.

### rlsDailyRestart.sh
This is meant to be run automatically by `rlsDailyRestart.service` to perform daily restart duties:
1. Writes a `reason.txt` file to the working directory, which is messaged to online players in the case that their presence causes a shutdown delay.
1. Triggers a rustLaunchSite service restart.

### rlsDailyRestart.service
This is a "one-shot" systemd helper service that runs `rlsDailyRestart.sh` whenever it is triggered - unless `rustLaunchSite.service` is not active, in which case it does nothing.

### rlsDailyRestart.timer
This is a systemd timer that triggers the `rlsDailyRestart.service` helper on a configured schedule.

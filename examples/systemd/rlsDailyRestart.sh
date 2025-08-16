#!/usr/bin/env bash
# this script is meant to be run as a systemd one-shot service in order to
#  perform a daily restart

# it is assumed that the reason file should be named `reason.txt` and that it
#  should be created in the current working directoy
echo "Daily restart - server will be back up momentarily" > reason.txt

# systemd doesn't seem to like to run this script as non-root, so this gives
#  ownership of `reason.txt` to the rust account/group after the fact; this is
#  needed so that RLS can remove the file after reading it
chown rust:rust reason.txt

# it is also assumed that the service name used for RLS is `rustLaunchSite`
systemctl try-restart rustLaunchSite.service

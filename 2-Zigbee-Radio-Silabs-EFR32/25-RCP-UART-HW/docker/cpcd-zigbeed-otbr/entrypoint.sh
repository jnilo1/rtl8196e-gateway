#!/bin/sh
set -eu

# OTBR's normal installation expects an init system. Run the three required
# services under one Docker PID 1 and fail the container if any child exits.
mkdir -p /run/dbus /var/lib/thread
rm -f /run/dbus/pid

cleanup() {
    trap - EXIT INT TERM
    for pid_file in /run/otbr-agent.pid /run/mdnsd.pid /run/dbus-daemon.pid; do
        if [ -s "$pid_file" ]; then
            kill "$(cat "$pid_file")" 2>/dev/null || true
        fi
    done
    wait 2>/dev/null || true
    /etc/init.d/otbr-firewall stop >/dev/null 2>&1 || true
    rm -f /run/dbus/pid /run/otbr-agent.pid /run/mdnsd.pid /run/dbus-daemon.pid
}

trap cleanup EXIT
trap 'exit 143' INT TERM

/etc/init.d/otbr-firewall start

dbus-daemon --system --nofork &
dbus_pid=$!
printf '%s\n' "$dbus_pid" > /run/dbus-daemon.pid

/usr/sbin/mdnsd -debug &
mdns_pid=$!
printf '%s\n' "$mdns_pid" > /run/mdnsd.pid

/usr/sbin/otbr-agent "$@" &
otbr_pid=$!
printf '%s\n' "$otbr_pid" > /run/otbr-agent.pid

# POSIX sh has no portable wait -n. Treat a missing or zombie child as a
# service-set failure so the Compose restart policy can recover all services.
while :; do
    for child in "$dbus_pid" "$mdns_pid" "$otbr_pid"; do
        state=$(awk '{print $3}' "/proc/$child/stat" 2>/dev/null || true)
        if [ -z "$state" ] || [ "$state" = Z ]; then
            status=1
            wait "$child" 2>/dev/null || status=$?
            exit "$status"
        fi
    done
    sleep 2
done

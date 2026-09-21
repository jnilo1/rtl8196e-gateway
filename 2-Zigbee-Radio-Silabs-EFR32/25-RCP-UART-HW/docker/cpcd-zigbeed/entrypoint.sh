#!/bin/bash
set -e

echo "=== cpcd-zigbeed container starting ==="
echo "RCP endpoint: ${RCP_HOST}:${RCP_PORT}"
echo "cpcd instance: ${CPCD_INSTANCE}"

: "${ZIGBEED_TRANSPORT:=tcp}"
: "${ZIGBEED_PORT:=9999}"
: "${ZIGBEED_PTY:=/tmp/ttyZigbeed}"
: "${ZIGBEED_PTY_HOST:=/tmp/ttyZigbeedHost}"
case "${ZIGBEED_TRANSPORT}" in
    tcp)
        : "${ZIGBEED_BIND:=0.0.0.0}"
        : "${ZIGBEED_INTERFACE:=tcp-listen://${ZIGBEED_BIND}:${ZIGBEED_PORT}}"
        ZIGBEED_SUPERVISOR_CONFIG=/etc/supervisor/conf.d/supervisord.conf
        echo "EZSP transport: TCP listen ${ZIGBEED_BIND}:${ZIGBEED_PORT}"
        ;;
    pty)
        : "${ZIGBEED_INTERFACE:=${ZIGBEED_PTY}}"
        ZIGBEED_SUPERVISOR_CONFIG=/etc/supervisor/conf.d/supervisord.pty.conf
        echo "EZSP transport: PTY ${ZIGBEED_INTERFACE}"
        ;;
    *) echo "ERROR: ZIGBEED_TRANSPORT must be tcp or pty (got ${ZIGBEED_TRANSPORT})" >&2; exit 1 ;;
esac
export ZIGBEED_TRANSPORT ZIGBEED_PORT ZIGBEED_PTY ZIGBEED_PTY_HOST ZIGBEED_INTERFACE

# Create required directories
mkdir -p /dev/shm/cpcd/${CPCD_INSTANCE}
mkdir -p /var/log/supervisor
mkdir -p /var/log/cpcd
mkdir -p /var/lib/zigbeed

# Generate cpcd.conf from template
envsubst < /etc/cpcd/cpcd.conf.template > /etc/cpcd/cpcd.conf
echo "Generated /etc/cpcd/cpcd.conf:"
cat /etc/cpcd/cpcd.conf

# Wait for gateway to be reachable
echo "Waiting for RCP endpoint ${RCP_HOST}:${RCP_PORT}..."
timeout=60
while ! nc -z ${RCP_HOST} ${RCP_PORT} 2>/dev/null; do
    timeout=$((timeout - 1))
    if [ $timeout -le 0 ]; then
        echo "ERROR: Cannot reach ${RCP_HOST}:${RCP_PORT} after 60 seconds"
        exit 1
    fi
    sleep 1
done
echo "RCP endpoint reachable"

# Select the transport-specific supervisor configuration. Supplying another
# command remains supported for container diagnostics.
if [ "$#" -eq 0 ] || [ "$1" = "/usr/bin/supervisord" ]; then
    exec /usr/bin/supervisord -c "${ZIGBEED_SUPERVISOR_CONFIG}"
fi
exec "$@"

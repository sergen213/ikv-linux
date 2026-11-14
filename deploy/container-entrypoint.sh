#!/bin/sh
set -eu
umask 077
# Compose secrets are host bind mounts; copy to the writable private tmpfs.
# Source files must be readable only by the configured service UID (10001).
for name in dsn tls-cert tls-key tls-ca; do
    test -r "/run/secrets/$name" || { echo "missing readable service secret: $name" >&2; exit 1; }
    test -f "/run/secrets/$name" && test ! -L "/run/secrets/$name" || { echo "service secret must be a regular non-symlink file: $name" >&2; exit 1; }
    test "$(stat -c %u "/run/secrets/$name")" = "$(id -u)" || { echo "service secret owner mismatch: $name" >&2; exit 1; }
    case "$(stat -c %a "/run/secrets/$name")" in
        400|600) ;;
        *) echo "service secret must have private mode0400 or0600: $name" >&2; exit 1 ;;
    esac
    cp "/run/secrets/$name" "/run/ikv/$name"
    chmod 600 "/run/ikv/$name"
done
service=${1:-world}
shift
# Automatic updates (deploy/compose.release.yaml): IKV_RELEASE names an extracted signed release (ikv-auto-update.sh),
# whose AppRun runs its own ikv-auth/ikv-server with its bundled configuration, migrations and web panel.
if [ -n "${IKV_RELEASE:-}" ]; then
    test -x "$IKV_RELEASE/AppRun" || { echo "IKV_RELEASE has no AppRun: $IKV_RELEASE" >&2; exit 1; }
    share="$IKV_RELEASE/usr/share/ikv"
    run() { tool=$1; shift; exec "$IKV_RELEASE/AppRun" "$tool" "$@"; }
else
    share=/usr/local/share/ikv
    run() { tool=$1; shift; exec "/usr/local/bin/$tool" "$@"; }
fi
case "$service" in
    auth)
        run ikv-auth --bind 0.0.0.0 --port 7443 \
            --admin-bind 127.0.0.1 --admin-port 7444 \
            --cert /run/ikv/tls-cert --key /run/ikv/tls-key --dsn-file /run/ikv/dsn \
            --migrations "$share/database/migrations" \
            --web-root "$share/web" --backup-dir /var/lib/ikv/backups "$@"
        ;;
    world)
        asset_root=${IKV_ASSET_ROOT:-/srv/ikv/original}
        test -d "$asset_root" && test -r "$asset_root" && test -x "$asset_root" || {
            echo "world requires a readable original asset directory at IKV_ASSET_ROOT: $asset_root" >&2
            exit 1
        }
        # NONCANONICAL wave 30 shards (deploy/compose.release.yaml profile "realms"): IKV_REALM, IKV_WORLD_PORT (container
        # port = published port: the shard registers it for clients), IKV_CAPACITY and IKV_PUBLIC_HOST (the address
        # players reach the shard at; default: the account service's host).
        if [ -n "${IKV_PUBLIC_HOST:-}" ]; then set -- --public-host "$IKV_PUBLIC_HOST" "$@"; fi
        run ikv-server --bind 0.0.0.0 --port "${IKV_WORLD_PORT:-7000}" \
            --realm "${IKV_REALM:-1}" --capacity "${IKV_CAPACITY:-32}" \
            --cert /run/ikv/tls-cert --key /run/ikv/tls-key --dsn-file /run/ikv/dsn \
            --gameplay "$share/config/gameplay.json" --asset-root "$asset_root" "$@"
        ;;
    *) echo "expected service auth or world" >&2; exit 2 ;;
esac

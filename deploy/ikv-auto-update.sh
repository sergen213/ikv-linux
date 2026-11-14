#!/bin/sh
# Automatic signed updates for dedicated/headless servers (docs/SELF_HOSTING.md "Automatic updates").
#
# 1. `ikv-launcher --update-once --server` checks the release channel and installs a newer signed AppImage into
#    $IKV_RELEASE_ROOT/releases/<version> (manifest + Ed25519 signature + per-channel sequence highwater; nothing is
#    installed otherwise). Exit 0 = current, 10 = installed.
# 2. A newly current release is extracted once to $IKV_RELEASE_ROOT/extracted/<version> and the `active` symlink is
#    switched atomically; the services run `active/AppRun ikv-auth|ikv-server` (deploy/release-units drop-ins or
#    deploy/compose.release.yaml).
# 3. ikv-auth restarts (it applies new migrations); the world gets the management "restart" command with a countdown:
#    it announces to every online player, saves all characters and exits 75, and systemd/Compose restarts it from the
#    new release. Without an administrator account the world is restarted by SIGTERM (it still saves).
#
# Configuration (environment, e.g. /etc/ikv/update.env):
#   IKV_RELEASE_ROOT   root-owned release root (default /var/lib/ikv-release; must not be group/world writable)
#   IKV_UPDATE_MODE    systemd (default) or compose
#   IKV_UPDATE_DELAY   world restart countdown in seconds (default 60)
#   IKV_DSN_FILE       systemd mode: the services' DSN (default /etc/ikv/dsn)
#   IKV_COMPOSE        compose mode: the docker/podman compose command with its -f files, run in IKV_COMPOSE_DIR
#   IKV_UPDATE_ARGS    extra launcher options (--channel, --repo, --trust, --update-ca, --release-api)
#   IKV_LAUNCHER       launcher command (default: the active release's AppRun ikv-launcher, else ikv-launcher on PATH)
set -eu
umask 022
root=${IKV_RELEASE_ROOT:-/var/lib/ikv-release}
mode=${IKV_UPDATE_MODE:-systemd}
delay=${IKV_UPDATE_DELAY:-60}
case "$mode" in systemd|compose) ;; *) echo "IKV_UPDATE_MODE must be systemd or compose" >&2; exit 2 ;; esac
case "$delay" in ''|*[!0-9]*) echo "IKV_UPDATE_DELAY must be 0..3600 seconds" >&2; exit 2 ;; esac
[ "$delay" -le 3600 ] || { echo "IKV_UPDATE_DELAY must be 0..3600 seconds" >&2; exit 2; }
log() { echo "ikv-auto-update: $*"; }

mkdir -p "$root"
if [ -n "${IKV_LAUNCHER:-}" ]; then
    launcher=$IKV_LAUNCHER
elif [ -x "$root/active/AppRun" ]; then
    launcher="$root/active/AppRun ikv-launcher"
else
    launcher=ikv-launcher
fi
status=0
# shellcheck disable=SC2086 # launcher and IKV_UPDATE_ARGS are word lists
QT_QPA_PLATFORM=offscreen $launcher --update-once --server --install-root "$root" ${IKV_UPDATE_ARGS:-} || status=$?
case "$status" in
    0|10) ;;
    *) log "update check failed (exit $status); the running release is kept"; exit 1 ;;
esac

target=$(readlink "$root/current" 2>/dev/null || true)
version=${target#releases/}
[ -n "$target" ] && [ "$target" = "releases/$version" ] || { log "no installed release in $root"; exit 0; }
case "$version" in *[!A-Za-z0-9._-]*|'') log "unsafe release version: $version"; exit 1 ;; esac
[ "$(readlink "$root/active" 2>/dev/null || true)" = "extracted/$version" ] && { log "v$version active"; exit 0; }

# Extract the verified AppImage once (no FUSE needed by the sandboxed services), then switch `active` atomically.
mkdir -p "$root/extracted"
if [ ! -x "$root/extracted/$version/AppRun" ]; then
    work=$(mktemp -d "$root/extracted/.extract-XXXXXX")
    trap 'rm -rf "$work"' EXIT
    (cd "$work" && "$root/releases/$version/bin/ikv-linux.AppImage" --appimage-extract >/dev/null)
    rm -rf "$root/extracted/$version"
    mv "$work/squashfs-root" "$root/extracted/$version"
fi
ln -sfn "extracted/$version" "$root/.active-new"
mv -T "$root/.active-new" "$root/active"
log "v$version active; restarting services"

announce() { # stdin SQL client; prints the number of queued commands
    printf '%s\n' "WITH queued AS (INSERT INTO world_commands(command, arguments, actor_id)
SELECT 'restart', jsonb_build_object('delay_seconds', $delay, 'message', 'Sunucu v$version sürümüne güncelleniyor; karakterleriniz kaydedilir.'), id
FROM accounts WHERE admin ORDER BY id LIMIT 1 RETURNING id) SELECT count(*) FROM queued;"
}
if [ "$mode" = systemd ]; then
    systemctl restart ikv-auth.service
    dsn=$(cat "${IKV_DSN_FILE:-/etc/ikv/dsn}")
    password=$(printf '%s' "$dsn" | sed -n 's/.* password=\([^ ]*\).*/\1/p')
    conninfo=$(printf '%s' "$dsn" | sed 's/ password=[^ ]*//')
    queued=$(announce | PGPASSWORD=$password psql --no-psqlrc --quiet --tuples-only --no-align --set ON_ERROR_STOP=1 \
        --dbname "$conninfo" --file - 2>&1 || true)
    if [ "$queued" = 1 ]; then
        log "world restarts in $delay s (announced to online players; exit 75 restarts it from v$version)"
    else
        log "no announced restart possible ($queued); restarting the world (it saves on SIGTERM)"
        systemctl restart ikv-server.service
    fi
else
    cd "${IKV_COMPOSE_DIR:-.}"
    compose=${IKV_COMPOSE:-docker compose -f compose.yaml -f deploy/compose.release.yaml}
    $compose restart auth
    queued=$(announce | $compose exec -T database psql -U postgres -d ikv --no-psqlrc --quiet --tuples-only --no-align \
        --set ON_ERROR_STOP=1 --file - 2>&1 || true)
    if [ "$queued" = 1 ]; then
        log "world restarts in $delay s (announced to online players; restart: on-failure restarts it from v$version)"
    else
        log "no announced restart possible ($queued); restarting the world (it saves on SIGTERM)"
        $compose restart world
    fi
fi

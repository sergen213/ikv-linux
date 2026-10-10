#!/bin/sh
set -eu
case "${1:-}" in
    auth)
        # Actual HTTPS handler + database query, not an open TCP port.
        exec curl --fail --silent --show-error --max-time 4 \
            --cacert /run/ikv/tls-ca https://localhost:7443/v1/health
        ;;
    world)
        # TLS listener and trust/SAN validity only; gameplay is tested separately.
        timeout 4 openssl s_client -connect 127.0.0.1:7000 -servername localhost \
            -CAfile /run/ikv/tls-ca -verify_hostname localhost -verify_return_error \
            -brief </dev/null
        ;;
    *) echo "expected health target auth or world" >&2; exit 2 ;;
esac

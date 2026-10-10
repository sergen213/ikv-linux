# Asset-free native backend. Bookworm/OpenSSL 3.0 cannot supply Argon2id.
FROM debian:trixie-slim AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends cmake g++ ninja-build libboost-dev \
        nlohmann-json3-dev libpq-dev libssl-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /source
COPY CMakeLists.txt ./
COPY include/ikv/protocol.hpp include/ikv/store.hpp include/ikv/world.hpp include/ikv/https_service.hpp include/ikv/
COPY src/protocol.cpp src/store.cpp src/world.cpp src/server_main.cpp src/auth_main.cpp src/https_service.cpp src/
COPY include/ikv/assets.hpp include/ikv/scene.hpp include/ikv/renderer.hpp include/ikv/audio_asset.hpp include/ikv/
COPY src/assets.cpp src/adf_codec.cpp src/scene.cpp src/scene_source_sort.hpp src/scene_animation.cpp src/scene_resources.cpp src/audio_asset.cpp src/
COPY database/migrations/ database/migrations/
COPY config/gameplay.json config/gameplay.json
COPY web/ web/
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DIKV_BUILD_CLIENT=OFF -DIKV_BUILD_LAUNCHER=OFF \
        -DIKV_BUILD_ASSET_TOOLS=OFF -DIKV_BUILD_ORIGINAL_TOOLS=OFF -DBUILD_TESTING=OFF \
    && cmake --build /build --target ikv-auth ikv-server --parallel 2 \
    && cmake --install /build --prefix /install

FROM debian:trixie-slim AS runtime
RUN apt-get update \
    && apt-get install -y --no-install-recommends libstdc++6 libgcc-s1 libpq5 libssl3t64 zlib1g \
        openssl ca-certificates curl postgresql-client-17 \
    && openssl list -kdf-algorithms | grep -q ARGON2ID \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /var/lib/ikv/backups \
    && chown -R 10001:10001 /var/lib/ikv
COPY --from=build /install/ /usr/local/
COPY deploy/container-entrypoint.sh deploy/container-health.sh /usr/local/libexec/ikv/
USER 10001:10001
WORKDIR /usr/local/share/ikv
EXPOSE 7000/tcp 7443/tcp
ENTRYPOINT ["/bin/sh", "/usr/local/libexec/ikv/container-entrypoint.sh"]
CMD ["world"]

# Gateway container. Build context is the repository root (see testbed/docker-compose.yml).
# UNVERIFIED in Phase 1: written without a running Docker daemon. First thing to try on a machine
# with Docker: docker compose -f testbed/docker-compose.yml --profile gateway up --build
FROM ubuntu:22.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends build-essential cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY third_party ./third_party
COPY tests ./tests
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPG_BUILD_TESTS=OFF && cmake --build build --target gateway

FROM ubuntu:22.04
COPY --from=build /src/build/gateway /usr/local/bin/gateway
EXPOSE 8000 9000
ENTRYPOINT ["gateway"]
CMD ["--config", "/etc/gateway/gateway.json"]

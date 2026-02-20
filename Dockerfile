# =============================================================================
# Dedup Integration Test - Multi-stage Docker Build
# TU Dresden Research: "Deduplikation in Datenhaltungssystemen"
#
# Stage 1: Build (gcc:14-bookworm) -- compiles dedup-test with all deps
# Stage 2: Runtime (debian:bookworm-slim) -- minimal image with binary only
#
# Build:  docker build -t dedup-test:latest .
# Run:    docker run --rm dedup-test:latest --help
# =============================================================================

# --- Stage 1: Build ---
FROM gcc:14-bookworm AS builder

RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
    cmake \
    libpq-dev \
    libcurl4-openssl-dev \
    libhiredis-dev \
    librdkafka-dev \
    libmariadb-dev \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY src/cpp/ /src/cpp/

RUN cmake /src/cpp \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_COMDARE_DB=OFF \
    && make -j$(nproc) dedup-test dedup-smoke-test

# --- Stage 2: Runtime ---
FROM debian:bookworm-slim

RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends \
    libpq5 \
    libcurl4 \
    libhiredis1.1.0 \
    librdkafka1 \
    libmariadb3 \
    ca-certificates \
    git \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/dedup-test /usr/local/bin/dedup-test
COPY --from=builder /build/dedup-smoke-test /usr/local/bin/dedup-smoke-test

RUN mkdir -p /results /datasets
WORKDIR /workspace

ENTRYPOINT ["dedup-test"]
CMD ["--help"]

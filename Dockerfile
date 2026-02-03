# syntax=docker/dockerfile:1

# ==============================================================================
# Stage 1 -- builder
# ==============================================================================
FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    ninja-build \
    git \
    && update-ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_TESTING=OFF \
        -DENABLE_BENCHMARKS=OFF \
    && cmake --build build --parallel

# ==============================================================================
# Stage 2 -- dev
#
# The full toolchain plus test/lint tooling, for `docker compose run --rm dev`.
# Not the default stage; build it explicitly:
#     docker build --target dev -t axolotldb:dev .
# ==============================================================================
FROM builder AS dev

RUN apt-get update && apt-get install -y --no-install-recommends \
    clang \
    clang-format \
    clang-tidy \
    lcov \
    gdb \
    valgrind \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
CMD ["/bin/bash"]

# ==============================================================================
# Stage 3 -- runtime (default)
#
# Ships only the binaries and the C++ runtime. spdlog, yaml-cpp and the
# header-only dependencies are all linked in statically by the build above.
# ==============================================================================
FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

# Run as a non-root user.
RUN useradd --create-home --shell /bin/bash axolotl \
    && mkdir -p /var/lib/axolotldb \
    && chown axolotl:axolotl /var/lib/axolotldb

COPY --from=builder /src/build/axolotldb-server /usr/local/bin/axolotldb-server
COPY --from=builder /src/build/axolotldb-cli    /usr/local/bin/axolotldb-cli

USER axolotl
WORKDIR /var/lib/axolotldb

EXPOSE 5432

CMD ["axolotldb-server"]

FROM debian:bookworm-slim AS build

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    ca-certificates \
    cmake \
    g++ \
    make \
    ninja-build \
    libboost-system-dev \
    libpqxx-dev \
    nlohmann-json3-dev \
    pkg-config \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  && cmake --build build --parallel \
  && ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim AS runtime

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    ca-certificates \
    libboost-system1.74.0 \
    libpqxx-6.4 \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=build /app/build/vix-arena /app/build/vix-arena
COPY --from=build /app/public /app/public
COPY --from=build /app/migrations /app/migrations

ENV APP_HOST=0.0.0.0
ENV APP_PORT=18080
ENV PUBLIC_URL=http://localhost:18080
ENV ALLOWED_ORIGINS=http://localhost:18080,http://127.0.0.1:18080
ENV DATA_DIR=/app/data

RUN groupadd --system --gid 10001 vix \
  && useradd --system --uid 10001 --gid vix --home-dir /nonexistent --shell /usr/sbin/nologin vix \
  && mkdir -p /app/data \
  && chown -R vix:vix /app/data

EXPOSE 18080

USER 10001:10001

CMD ["./build/vix-arena"]

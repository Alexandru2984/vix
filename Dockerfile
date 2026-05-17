FROM debian:bookworm-slim

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    ca-certificates \
    cmake \
    g++ \
    make \
    ninja-build \
    libboost-system-dev \
    nlohmann-json3-dev \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  && cmake --build build --parallel \
  && ctest --test-dir build --output-on-failure

ENV APP_HOST=0.0.0.0
ENV APP_PORT=18080
ENV PUBLIC_URL=http://localhost:18080

EXPOSE 18080

CMD ["./build/vix-arena"]

# --- Build stage ---
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config \
    libcurl4-openssl-dev libsqlite3-dev nlohmann-json3-dev libzip-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt .
COPY src/ src/

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"

# --- Runtime stage ---
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4 libsqlite3-0 libzip4 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/data_flight .
COPY public/ public/

# Separate from /app (code, replaced on every image rebuild) so a named
# volume can be mounted here without shadowing the app binary/public dir --
# mounting a volume directly at /app would freeze its contents at whatever
# was there when the volume was first created, silently serving stale code
# on every later `docker run` even after a fresh `docker build`.
RUN mkdir -p /data

EXPOSE 5050

# Default: real data only (live global aircraft + airspace load regardless;
# drone tracks come from real POST /api/detections, /api/remoteid/decode,
# /api/rf/analyze, or /api/telemetry/import). The old default ran a
# synthetic demo scenario centered on Washington, DC -- re-enable it for a
# demo at `docker run` time with:
#   docker run -p 5050:5050 data-flight ./data_flight --simulate --sim-speed 6
CMD ["./data_flight"]

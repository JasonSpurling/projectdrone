# Data Flight

A C++ rewrite of the multi-sensor drone-fusion dashboard, with a global live
aircraft feed, real restricted-airspace overlays, and a MapLibre GL JS
frontend with 2D/3D/terrain support.

## What changed from the Python version

- **Language**: full C++17 rewrite. Zero external HTTP library dependency —
  the server is a small hand-rolled HTTP/1.1 server over raw POSIX sockets
  (see "Why no libmicrohttpd" below).
- **Live plane data**: OpenSky Network, polled **globally** (no bounding
  box) — matches "planes all over the world, not just one area." OpenSky is
  the best *free* option; Flightradar24/FlightAware have larger networks
  but are paid commercial APIs.
- **No-fly zones**: real data from two public sources —
  FAA (UAS Facility Map + National Security UAS Flight Restrictions, via
  ArcGIS REST) and OpenAIP (worldwide airspace data, free API key required).
- **Frontend**: MapLibre GL JS (open source, no API key needed for the
  library itself), with a 2D/3D globe toggle, terrain on/off (free Terrarium
  elevation tiles), and the same Esri satellite imagery as before, plus a
  Road basemap option.
- **Layout**: matches the specified design — header (status/clock/connection),
  narrow left nav (Live/History/Alerts/Zones/Settings), large central map
  with floating top-right controls, bottom-left searchable list, bottom-right
  detail/alert panel.

## Honest scope decisions (things I cut or simplified, and why)

- **FlightRadar24 dropped in favor of OpenSky.** You explicitly asked for
  "the best free" tracker — OpenSky is that; FR24 is a paid product.
- **FAA UAS Sightings Report (.xlsx historical dataset) dropped.** Parsing
  Excel files in C++ needs a zip+XML pipeline (libzip + an XML parser) that
  adds real complexity for a dataset that's historical, not live. The
  History/Alerts tabs are populated from this system's own real incident
  log instead, which is live and immediately useful.
- **Camera snapshot placeholder images dropped.** The Python version used
  Pillow to draw fake bounding-box images for demo purposes; there's no
  equivalent lightweight image library reliably available without extra
  build complexity, so this version omits it. Camera detections still
  carry their metadata (label, bounding box) — just no generated thumbnail.
- **"Helicopter"/"UAV" filters now use OpenSky's real ADS-B category field**
  (`extended=1` on `/api/states/all`) when an aircraft reports one, falling
  back to the old low-altitude + low-speed heuristic only when it doesn't.
  Every raw state-vector field OpenSky provides is exposed via `/api/aircraft`
  (time_position, last_contact, baro/geo altitude, squawk, SPI, position
  source, category) — not just the subset needed for the map pins.
- **FAA Class Airspace (Class B/C/D/E boundaries) evaluated and dropped.**
  Tried this via the official ArcGIS FeatureServer; its polygons are
  extremely high-vertex (precise circular-arc boundaries) — a 200-record
  test page alone ran 5-6MB and took ~20s even with server-side geometry
  simplification. The full ~6,061-feature dataset extrapolates to
  150-400MB and several minutes to fetch, which would either block startup
  for minutes or reintroduce the exact large-payload page-load slowness
  this app was already fixed for once. Airports themselves are still fully
  covered — see the next point.
- **Worldwide airports/heliports added via OurAirports** (`/api/airports`,
  free public-domain CSV, no API key) — ~72,000 facilities with name, type,
  ICAO/IATA codes, elevation, and location, refreshed weekly since this
  dataset barely changes. This is what "mark the airports" resolves to,
  since the official controlled-airspace boundary shapes weren't practical
  (see above).
- **Settings tab is informational, not live-editable.** It shows which data
  sources are configured; changing thresholds/API keys still means editing
  environment variables and restarting, not a live settings form.

## Why no libmicrohttpd

I considered using libmicrohttpd (a mature, well-known C HTTP server
library) instead of hand-rolling sockets. I backed off that because its
callback API has had breaking changes across versions (the return type of
access-handler callbacks changed from `int` to `enum MHD_Result` in more
recent releases), and I had no way to verify which version Ubuntu 24.04's
package would give me without network access to actually test-compile
against it. Given you'd already hit several Docker build failures, I judged
a slightly-larger hand-rolled server with **zero version-dependent risk**
was the safer bet — it only needs libc and pthreads, which are guaranteed
in any Linux base image. I wrote and **actually ran** this server in this
sandbox (real HTTP requests, routing with path parameters, POST bodies,
query strings, 404s — all verified working).

## What was actually tested vs. what wasn't (read this before you build)

This sandbox has **no internet access**, so I could not install
`libcurl4-openssl-dev`, `libsqlite3-dev`, or `nlohmann-json3-dev` to
compile-test against their real headers here. Here's exactly what I did
verify directly:

| Component | Tested? | How |
|---|---|---|
| Geometry math (haversine, point-in-polygon) | ✅ Yes | Compiled + ran real assertions |
| Fusion classification logic | ✅ Yes | Compiled + ran real test cases matching the Python version's verified behavior |
| Track association + geofencing | ✅ Yes | Compiled + ran a full integration test (drone scenario, zone entry) |
| HTTP server (routing, POST, query strings) | ✅ Yes | Compiled, ran the real binary, hit it with real `curl` requests |
| JS syntax (frontend) | ✅ Yes | `node --check` |
| SQLite storage layer | ⚠️ Not compiled here | No `sqlite3.h` available; written against the well-documented, stable C API |
| OpenSky / FAA / OpenAIP fetchers (libcurl) | ⚠️ Not compiled here | No `curl.h` available; written against libcurl's standard easy-interface pattern |
| Full `docker build` | ⚠️ Not run here | Docker itself isn't available in this sandbox |

The package names in the Dockerfile (`libcurl4-openssl-dev`,
`libsqlite3-dev`, `nlohmann-json3-dev`) were confirmed to **exist** for
Ubuntu 24.04 (apt resolved them), just blocked from actually downloading
here. **The most likely thing to need a small fix on your first
`docker build` is a compile error in `storage.hpp`, `opensky_feed.hpp`, or
`faa_airspace.hpp`** — paste me the exact error and I'll fix it fast; these
are the three files I couldn't verify end-to-end.

## Setup

```bash
docker build -t data-flight .
docker volume create data-flight-db
docker run -p 5050:5050 -v data-flight-db:/data data-flight
```

Open **http://localhost:5050**

**The `-v data-flight-db:/data` volume is not optional** despite the flag
syntax suggesting otherwise: without it, the SQLite database (all incident
history, detection log, feedback) lives only in that specific container's
writable layer and is gone the moment the container is removed and
recreated (`docker rm` + `docker run`, an image update, `docker compose
down`, etc.) — confirmed by directly inspecting the database file before
and after a recreation. `/data` is deliberately a separate path from
`/app` (where the code/binary live) specifically so the volume never
shadows a fresh image rebuild with stale mounted content — see the
Dockerfile comment. Override the path with `DB_PATH` if you'd rather bind-
mount a host directory instead of a named volume.

### Environment variables

Put these in a `.env` file next to the Dockerfile (already gitignored/
dockerignored — never bake secrets into the image with a Dockerfile `ENV`
instruction, since that embeds them in the image layers for anyone who
gets the image) and run with `--env-file`:

```bash
docker run -p 5050:5050 -v data-flight-db:/data --env-file .env data-flight
```

```bash
# .env
OPENSKY_CLIENT_ID=xxx           # higher OpenSky rate limit (free account, register at opensky-network.org)
OPENSKY_CLIENT_SECRET=yyy
OPENAIP_API_KEY=xxx             # worldwide restricted-airspace data (free key at openaip.net)
DETECTIONS_API_KEY=xxx          # required as X-API-Key header on POST /api/detections; if unset, ingestion is open to anyone with network access
ADSBLOL_LAT=38.9                # enables the adsb.lol community feed as a redundant/failover aircraft source around this point; unset = disabled
ADSBLOL_LON=-77.0
ADSBLOL_RADIUS_NM=250           # optional, defaults to 250 (adsb.lol's own max)
# ADSBLOL_REGIONS=CONUS         # alternative to LAT/LON: broad (not exhaustive) continental-US coverage via
                                 # 10 major-metro query points, since a single 250nm-radius query can't cover
                                 # the whole US. Or "NORTH_AMERICA" for CONUS plus major Canadian/Mexican
                                 # metros, or "lat,lon;lat,lon;..." for your own custom set of regions.
# AIRPLANESLIVE_LAT=38.9        # same idea, second free community feed (airplanes.live) -- independent
# AIRPLANESLIVE_LON=-77.0       # coverage/redundancy from adsb.lol since it's a different aggregator network.
# AIRPLANESLIVE_REGIONS=NORTH_AMERICA  # same LAT/LON, RADIUS_NM, REGIONS options as ADSBLOL_* above.
                                 # (adsb.fi and ADS-B Exchange's free tier were tried too -- neither had a
                                 # working free endpoint at the time this was wired up; see
                                 # community_adsb_feed.hpp if that changes.)
AIRCRAFT_SNAPSHOT_INTERVAL_SECONDS=300  # how often live aircraft positions are logged to SQLite for history/replay
AIRCRAFT_HISTORY_RETENTION_DAYS=30      # how long aircraft_history rows are kept before pruning
```

Without `OPENSKY_CLIENT_ID`/`SECRET`, OpenSky still works (anonymous, much
lower rate limit) and FAA airspace data still loads regardless — only
OpenAIP's layer stays empty until you add `OPENAIP_API_KEY`.

## API endpoints

```
GET  /api/tracks              live drone tracks
GET  /api/tracks/<id>         single track detail
GET  /api/tracks/<id>/trajectory   trajectory stats + anomaly flags for a live track
GET  /api/tracks/<id>/full_history  full flight-path reconstruction from the permanent log
GET  /api/history/window      historical replay: all tracks' positions + incidents in a time range
GET  /api/analytics/anomalies  active tracks with at least one anomaly flag set
GET  /api/analytics/links     identifier reappearances + common launch-area clustering
GET  /api/analytics/graph     graph view of the same data (nodes/edges); ?node=track:5 for just its neighbors
GET  /api/analytics/stats     aggregate historical statistics
GET  /api/analytics/model     learned baseline model state (see below)
GET  /api/incidents           logged alerts (classification + geofence), with evidence packages
GET  /api/incidents/export    downloadable CSV incident report
POST /api/incidents/<id>/feedback  operator confirms/corrects a past alert's classification
GET  /api/incidents/accuracy  confirmed-vs-corrected feedback stats, by original classification
GET  /api/sensors             sensor health status
GET  /api/zones               restricted zones (custom, if zones.json present -- else one DC example)
GET  /api/aircraft             live global aircraft, full OpenSky detail (OpenSky)
GET  /api/airspace            FAA + OpenAIP restricted airspace polygons
GET  /api/airports            worldwide airports/heliports directory (OurAirports)
GET  /api/runways             runway layouts (OurAirports), for drawing a selected airport's outline
GET  /api/airport_boundary    on-demand airport perimeter polygon (OpenStreetMap Overpass); ?ident=&icao=&lat=&lon=
GET  /api/registry/status     FAA registry load status + static-table sizes
GET  /api/registry/aircraft   FAA N-number registry lookup + Mode-S country guess; ?n_number= or ?icao24=
GET  /api/registry/type       ICAO type designator lookup (curated subset); ?code=
POST /api/detections          ingest a raw sensor detection
POST /api/remoteid/decode     decode a raw ASTM F3411/OpenDroneID payload, cross-reference + ingest
GET  /api/remoteid/manufacturers  list loaded manufacturer-code entries
POST /api/rf/analyze          heuristic RF signature analysis over spectrum data, ingest if confident
POST /api/telemetry/import    bulk-import recorded telemetry (CSV or JSON) into the fusion pipeline
GET  /api/health              liveness check
```

`POST /api/detections`, `/api/remoteid/decode`, `/api/rf/analyze`,
`/api/telemetry/import`, and `/api/incidents/<id>/feedback` all require the
`X-API-Key` header when `DETECTIONS_API_KEY` is set (see above) — they're
all the same class of data-injection surface.

### Fusion, classification & alerting

- **Multi-source association** is nearest-neighbor gating in time
  (`ASSOCIATION_GATE_SECONDS`, default 20s) and space
  (`ASSOCIATION_GATE_METERS`, default 250m) — a detection from any sensor
  type fuses into an existing track if it's within both gates, which is
  what lets e.g. a Remote ID hit and an RF hit converge on one track.
- **Position/velocity estimation** is a real constant-velocity Kalman
  filter per track (`kalman.hpp`), not a fixed-alpha exponential blend —
  it weights each measurement by sensor confidence, produces an actual
  damped velocity estimate (used for the aircraft/helicopter envelope
  checks and anomaly detection) instead of a noisy two-point finite
  difference, and its own uncertainty tracking is what makes it resistant
  to the "two detections land milliseconds apart" spurious-speed problem
  that used to need an ad-hoc minimum-dt patch. Tunable via
  `KALMAN_POS_NOISE_M` (measurement noise, default 15m) and
  `KALMAN_ACCEL_NOISE_MPS2` (how sharply a target can maneuver, default
  3 m/s²).
  All of `ASSOCIATION_GATE_METERS`, `ASSOCIATION_GATE_SECONDS`,
  `KALMAN_POS_NOISE_M`, `KALMAN_ACCEL_NOISE_MPS2`, `TRACK_STALE_SECONDS`,
  `TRACK_DROP_SECONDS`, and `GEOFENCE_MIN_CONFIDENCE` are overridable via
  env vars of the same name for tuning without a rebuild.
- **Classification** normally bands a noisy-OR confidence score across
  corroborating sensors into `confirmed_drone` / `likely_drone` /
  `possible_drone` / `unknown`. Altitude and an estimated ground speed
  (from consecutive track positions) can override that into `aircraft`
  (altitude or speed well outside anything a small UAS can plausibly do)
  or `helicopter` (above the small-UAS altitude ceiling, low/hovering
  speed, no Remote ID broadcast) — see `fusion.hpp` for the exact
  thresholds and reasoning.
- **Geofencing** is point-in-polygon against `zones.json` if present next
  to the binary (`[{"zone_id":"...", "name":"...", "polygon":[[lat,lon],...]}]`),
  falling back to one illustrative Washington-DC example zone if absent —
  this is what makes "restricted or custom zones" actually custom without
  a rebuild.
- **Alerts carry an evidence package**: `/api/incidents` includes an
  `evidence` field (Remote ID fields, RF summary, latest visual detection)
  captured at the moment the alert fired, not just the classification/
  confidence numbers.
- **`GET /api/incidents/export?format=csv`** gives a downloadable report
  for after-the-fact review in a spreadsheet.
- **Progressive improvement** is an honest-scope operator feedback loop,
  not automatic retraining (this is a rule-based heuristic system, not a
  model): `POST /api/incidents/<id>/feedback` with
  `{"correct": true|false, "actual_classification": "...", "note": "..."}`
  logs ground truth, and `GET /api/incidents/accuracy` summarizes
  confirmed-vs-corrected counts per original classification — the data
  needed to decide which threshold to adjust over time.

### Analytics & intelligence

All computed from data this system has itself collected (in-memory Track
history + the SQLite detection/incident log) — no external services, no
network calls.

- **Trajectory analysis** (`GET /api/tracks/<id>/trajectory`): distance
  traveled, duration, average/max speed, altitude range, accumulated
  heading change, and loiter/circling detection (stayed within ~150m of
  its own centroid for 30+ seconds despite several detections — consistent
  with hovering/circling rather than transit).
- **Anomaly detection**, same endpoint: rapid altitude change (>50m within
  5s), rapid speed change (>20 m/s between consecutive detections),
  loitering, repeated zone incursions (>1 geofence entry for the same
  track), and a statistical outlier flag from the learned baseline below.
  `GET /api/analytics/anomalies` scans all active tracks and returns only
  the ones with something flagged.
- **Link analysis** (`GET /api/analytics/links`): the same declared Remote
  ID identity reappearing under a different `track_id` (tracks get
  dropped and re-created when a drone goes out of range and comes back),
  and rough grid-based clustering of each track's first-seen position to
  spot common launch/operating areas.
- **Historical reconstruction** (`GET /api/tracks/<id>/full_history`):
  full flight-path reconstruction from the permanent SQLite log — works
  even for a track that's since been dropped, unlike the in-memory
  `Track.history` (capped at 50 detections). `GET /api/analytics/stats`
  gives aggregate counts (detections by sensor type, incidents by
  classification/zone, time range covered).
- **Local learned baseline** (`GET /api/analytics/model`): honest scope —
  this is Welford's online mean/variance algorithm (classical streaming
  statistics), not a neural network or any ML framework. Pulling in
  something like TensorFlow/libtorch for this would be a large,
  hard-to-verify build-complexity increase (against the project's
  zero-heavy-dependency approach) for a use case that doesn't need it: a
  per-classification running mean/stddev of speed and altitude, updated
  incrementally from every real detection as it arrives, used to flag
  statistically unusual tracks via z-score. It's genuinely fit on this
  system's own collected data and improves as more arrives — just not a
  deep model, and it's **in-memory only, resetting on restart** (it
  doesn't bootstrap from historical SQLite data, since the detections
  table doesn't record what classification a track had at the time of
  each past detection).

### Remote ID / RF / recorded-telemetry ingestion

Three additional ways to get data into the fusion pipeline beyond the raw
`POST /api/detections` format, each with an honest scope boundary (see the
comment block at the top of the corresponding header for the full
reasoning):

- **`POST /api/remoteid/decode`** — `{"payload_hex": "...", "sensor_id": "..."}`.
  Decodes a raw ASTM F3411 / OpenDroneID message-pack payload (Basic ID,
  Location/Vector, Self-ID, System, Operator ID messages) per the public
  wire format implemented by the open-source `opendroneid-core-c`
  reference. **This decodes the payload bytes only** — pulling those bytes
  out of a live Bluetooth advertisement or Wi-Fi Beacon vendor IE is
  capture-hardware/library specific (a BLE sniffer, Wireshark's built-in
  Open Drone ID dissector, or a monitor-mode Wi-Fi capture tool all produce
  it); parsing the 802.11/BLE link layer itself is out of scope here. If a
  Location/Vector message is present (or you pass `lat`/`lon` directly),
  the decode is ingested as a `remote_id` Detection through the normal
  fusion/geofence pipeline. Serial-number Basic IDs are cross-referenced
  against `manufacturer_codes.json` (see below); CAA/FAA registration IDs
  get an honest "no public reverse-lookup API exists for this" note rather
  than a fake lookup.
- **`GET /api/remoteid/manufacturers`** — lists what's currently loaded
  from `manufacturer_codes.json`. This repo does **not** ship a
  manufacturer-code table: ANSI/CTA-2063-A's Manufacturer Code Designator
  registry is administered by CTA under an application process, and I
  don't have a verified current copy of it — returning "unknown" is more
  honest than guessing. Drop a real export in as
  `manufacturer_codes.json` (flat `{"CODE": "Manufacturer Name"}`) next to
  the binary and it's picked up at startup.
- **`POST /api/rf/analyze`** — `{"sensor_id":"...", "lat":..., "lon":..., "samples":[{"freq_hz":...,"power_dbm":...}], "sweeps": [[...],...]}`.
  Heuristic classification over **already-computed spectrum data** — the
  kind of freq/power output `rtl_power`, a HackRF sweep, or a spectrum
  analyzer's CSV export already produces. This does not do signal
  processing on raw IQ samples (no FFT/demodulation); that belongs in the
  capture tool. Flags peaks in drone-relevant ISM bands (900MHz/2.4GHz/
  5.8GHz), estimates whether the bandwidth looks more like a narrowband C2
  hop, a Wi-Fi channel, or an FPV video downlink, and — if you pass
  multiple time-ordered `sweeps` — checks whether activity hops across
  channels (a classic FHSS tell for drone control links). Ingests as an
  `rf` Detection if confidence clears a threshold and you supplied a
  position. Treat its output as a lead to investigate, not a confirmed ID.
- **`POST /api/telemetry/import`** — `{"sensor_id":"...", "sensor_type":"...", "format":"csv"|"json", "data": "..."}`.
  Generic importer for recorded telemetry (a GCS log export, a converted
  flight log, any other timestamp+position source) — matches common column/
  field name aliases (`lat`/`latitude`, `lon`/`lng`/`longitude`, `alt`/
  `altitude`/`alt_m`, `timestamp`/`time`/`epoch`, `speed`/`velocity`,
  `heading`/`track`/`course`) rather than one fixed schema. Only numeric
  epoch timestamps (seconds or milliseconds, auto-detected) are supported —
  no date-string parsing in this pass. Each row is ingested through the
  same fusion/geofence pipeline as a live sensor, so imported tracks get
  real classification and zone-entry alerts.

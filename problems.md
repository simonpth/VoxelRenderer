# World streaming and rendering problems

## Current world creation

- `SvoWorld::getRegion` generates a region synchronously on the calling
  thread. A region is created by repeatedly calling recursive `fill` for every
  `(x, y)` column, so a single request performs thousands of tree mutations.
- The TCP world thread requests a 3x3x3 neighborhood and generates/serializes
  all 27 regions before the connection is released. The first request can
  therefore block on both generation and serialization.
- There is no generation budget or distance-based resolution. Near and far
  regions have the same node detail and cost.
- Regions are retained indefinitely in the server's map, so exploring the
  world grows server memory without a residency policy.

## Collision and simulation

- Collision queries can trigger synchronous region generation from the
  simulation thread. Movement can therefore pay the full generation cost at
  an unpredictable point in a frame.
- The simulation still carries the legacy dense `32^3` world and the server
  still sends legacy dense chunks over UDP. This duplicates world state and
  work while the SVO stream is becoming authoritative.

## Network streaming

- One TCP connection sends many region frames in a tight loop. A slow client
  or a large generated region delays all later regions.
- The client reconnects and requests the same neighborhood repeatedly, even
  when the region revision is unchanged.
- Region snapshots are copied through several queues and the client rebuilds
  packed node vectors for every cached region on every render update.

## GPU upload

- `uploadWorld` reconstructs one contiguous CPU arena and compares/copies the
  entire arena whenever the render state changes.
- The world buffer is only 64 MiB despite the intended architectural limit
  being 4 GiB; there is no explicit fixed-slot accounting or per-region
  eviction policy in the upload path.
- GPU data is not updated incrementally. A player movement update can cause
  unrelated region data to be recopied.
- The same mapped buffer is updated while frames may still be using it. This
  is safe only accidentally when updates are infrequent; explicit
  synchronization is required for region replacement.

## Remediation implemented

- Region generation now uses distance-selected coarse height-map samples.
- A TCP request produces one complete region frame, allowing the client to
  schedule regions independently.
- Client region requests are revision-filtered. Received nodes are transient
  upload payloads and are released after the renderer consumes them; no client
  CPU-side SVO cache remains.
- GPU residency uses fixed slots and updates only changed region slots.
- GPU residency has a hard 4 GiB maximum and remains bounded below the current
  64 MiB default allocation.

## Remaining limitation

- The server's CPU-side region map is still a cache without LRU eviction. The
  GPU SVO is hard-bounded, but a long-running server can retain generated CPU
  regions indefinitely. Server-side LRU eviction must be added before
  unbounded exploration is considered production-safe.

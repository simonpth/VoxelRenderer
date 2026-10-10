# VoxelRenderer

The simulation is built around a strict authority boundary:

- `simulation/authority` is the only code that changes player physics. It runs
  at 60 Hz, consumes intent, resolves Z-up movement and gravity against the
  server-owned voxel terrain, and publishes tick-numbered authoritative state.
- `network/protocol` serializes only intent and authoritative results. Packets
  are versioned, little-endian, finite-value checked, and rejected when their
  shape or tick ordering is invalid.
- `network/server` owns the simulation timeline. It retains the latest valid
  intent for each registered client, never accepts a client position or
  velocity, and sends snapshots containing all connected players.
- `network/client` receives snapshots on a worker thread and maintains a
  thread-safe snapshot queue. The render thread consumes only a copied
  interpolated `RenderState`; Vulkan has no access to sockets or network
  queues.

Clients register with a randomly generated 128-bit UUID before sending input.
The server assigns each UUID an independent authority and aggregates the
resulting player states into every connected client's snapshot.

The server creates terrain deterministically and sends player snapshots over
UDP. Voxel regions are requested over the reliable TCP world stream; dense
world chunks are not sent to or retained by the client.

The renderer uses a right-handed, Z-up coordinate system. The compute shader
casts a grid ray to find a voxel, derives the hit face normal, and casts a
second ray toward the sun to apply direct lighting and hard voxel shadows.
Collision detection remains exclusively in the server authority.
Shutdown joins network threads before their sockets are destroyed.

## Sparse world migration

The target world is a sparse, region-partitioned voxel octree spanning
`32768 x 32768 x 8192` cells. All three axes are finite; X and Y are bounded
to `[0, 32768)` and Z is bounded to `[0, 8192)`.
The migration contract and hardware constraints are documented in
[svo_plan.md](svo_plan.md).

The region implementation in `simulation/svo.*` uses fixed `1024^3` regions
(32 x 32 x 8 regions) and a depth-10 local SVO. Generation is deterministic
and lazy, so the server does not allocate the complete 32k x 32k x 8k volume.
The server recomputes desired region detail from the nearest player and the
per-client TCP request selects detail from that client's position.
The SVO also provides bounded lookup, split/merge edits, and on-demand region
creation. Server collision now queries one shared SVO world for all
authorities. Dense compatibility fields remain only for simulation ABI
compatibility and are not streamed to clients.

The reliable world-stream foundation is also present in
`network/tcp_socket.*`. The server listens on the UDP port plus one, validates
the registration and interest frames, and sends one complete length-framed
SVO region per TCP connection. The client schedules the radius-one
neighborhood as independent requests, while the server selects a coarser
height-map SVO depth for farther regions.
Revision deltas, client cache publication, and GPU upload are deliberately
subsequent phases.

TCP shutdown is bounded: the listener is non-blocking, accepted sockets have
a receive/send timeout, and stop signals shut down both listener and client
stream sockets before joining their threads. This prevents a stalled world
stream from preventing application exit.

The client keeps only region revisions and uses received node data as a
transient upload queue. The renderer uploads each update to a bounded storage
buffer arena, then the CPU payload is released. The compute shader traverses
1024-cell regions hierarchically with a fixed 64-entry stack. Region uploads
update only changed slots and synchronize only when residency changes; camera
movement alone does not rebuild the arena.
Missing regions are treated as unknown/missed data rather than fabricated
solid terrain.

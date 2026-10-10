# VoxelRenderer

The simulation is built around a strict authority boundary:

- `simulation/authority` is the only code that changes player physics. It runs
  at 60 Hz, consumes intent, resolves Z-up movement and gravity against the
  server-owned voxel terrain, and publishes tick-numbered authoritative state.
- `network/protocol` serializes only intent and authoritative results. Packets
  are versioned, little-endian, finite-value checked, and rejected when their
  shape or tick ordering is invalid.
- `network/server` owns the simulation timeline. It retains the latest valid
  intent and never accepts a client position or velocity.
- `network/client` receives snapshots on a worker thread and maintains a
  thread-safe snapshot queue. The render thread consumes only a copied
  interpolated `RenderState`; Vulkan has no access to sockets or network
  queues.

The server creates the terrain from a two-dimensional sine-height function at
startup. It sends that authoritative 32^3 voxel array as bounded UDP
`WorldChunk` packets before continuing with snapshots. Voxel edits use
separate tick/revision messages because voxel values are authoritative
discrete state, not interpolated motion.

The renderer uses a right-handed, Z-up coordinate system. The compute shader
casts a grid ray to find a voxel, derives the hit face normal, and casts a
second ray toward the sun to apply direct lighting and hard voxel shadows.
Collision detection remains exclusively in the server authority.
Shutdown joins network threads before their sockets are destroyed.

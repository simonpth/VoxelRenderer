# Multiplayer simulation plan

This document describes the current multiplayer simulation and the
responsibilities of each part of the game.

## Overall experience

The game presents a server-authoritative multiplayer world. Movement is
deliberately deferred: local movement and jump input are not applied directly
to the rendered position. They are accumulated, sent to the server, simulated
there, and returned in authoritative snapshots. View rotation remains
responsive locally while the server also receives the rotation deltas.
Rendering interpolates server snapshots so the display remains smooth between
simulation ticks.

The simulation must not become dependent on the renderer's frame rate or on
the timing of network packets. A slow frame, a fast frame, delayed packet, or
lost packet should not change the rules of the game.

## Intended world and player behavior

Each server-side player authority owns the complete simulation state for that
player and uses the shared terrain. The authoritative state includes:

- Every voxel and its visual properties
- Every object and its physical state
- The player position and velocity
- The player's view orientation
- Whether the player is grounded
- Whether a jump is allowed
- The current simulation tick

The player moves according to input intent, velocity, gravity, and collision
rules evaluated by the server. A client sends movement displacement and never
sends a position, velocity, or collision result. The client does not perform
movement prediction or client-side collision resolution.

The player should be able to look independently of movement. Looking should
respond to mouse input without waiting for a rendered frame or server response
to feel responsive. The server should still be the source of truth for the
orientation that is shared with other clients.

Jumping should only occur when the player was on the ground at the beginning
of the relevant simulation step and has not already consumed that jump.
Gravity, vertical movement, and contact with the initial stone layer should be
consistent for every client because they are decided by the server.

## Server purpose

The server provides one authoritative timeline and advances it at 60 Hz. Each
registered client has an independent `Authority`, input state, acknowledgement
tick, jump request, endpoint, and world synchronization status. Client UUIDs
are 128-bit values generated at client startup and are used to register and
re-identify a connection. The server assigns player IDs and supports up to
the 16 players that fit in an authoritative snapshot.

For every simulation step, the server should:

1. Receive registration packets and associate UUIDs with client connections.
2. Use the most recent valid input intent available for each connected player.
2. Apply that intent to movement and view orientation.
3. Evaluate jump eligibility from the previous grounded state.
4. Apply gravity, velocity, movement, and collision resolution.
5. Apply accepted voxel edits and other world changes.
6. Advance the authoritative tick.
7. Publish a snapshot to each client containing that client's local state and
   the current states of all connected players.

The server should never depend on how frequently a client renders. It should
continue simulating while a client is resizing a window, paused briefly, or
rendering at a different frame rate.

The server rejects malformed input, excessive horizontal displacement, and
excessive look deltas. It acknowledges input using the client tick included
in the accepted intent. Missing input is handled by the latest stored intent
and the authority's normal fixed-step behavior.

## Client purpose

The client should translate local input into commands, receive authoritative
results, and prepare a smooth state for rendering.

The client should maintain a clear distinction between:

### Local input state

This represents what the local user is doing now. It includes keyboard
movement, mouse look deltas, jump actions, mouse capture state, and the next
input tick. Movement displacement, look deltas, and jump intent are
accumulated until the client sends the next command. This state generates
commands and is not proof of physical movement.

### Authoritative game state

This is the newest valid state confirmed by the server. It is the reference
for debugging, correction, gameplay truth, and eventual reconciliation. It
must contain the position and velocity actually calculated by the server, not
a locally invented position.

### Interpolated render state

This is a presentation state derived from authoritative snapshots. It exists
only to make movement appear continuous between server ticks. It may be behind
the newest authoritative state by a small, intentional amount, but it must
never invent collisions or permanently replace authoritative data.

The renderer should receive only the interpolated render state. It should not
read the network queue, the server state, or state that another thread can
modify.

## Input behavior

Input should be sampled at render-frame frequency so mouse and keyboard
controls feel immediate. Each command should describe intent for a specific
client tick:

- Movement direction
- View rotation delta
- Jump action
- Any later gameplay actions

Input commands should not contain position, velocity, collision results, or
voxel ownership claims. Those values belong to the simulation authority.

The client sends a registration packet when it starts and retries until the
server acknowledges its UUID. After registration, it sends an initial input
packet and then sends the accumulated movement, look deltas, and jump intent
when a new authoritative snapshot is consumed. Thus, movement input is
deferred until it has been sent to and processed by the server; it is not
applied directly to the render position. Tick identifiers allow the server to
ignore stale or repeated commands.

Escape should release mouse capture, and clicking the game window should
restore it. This affects local input collection only; it must not alter the
authoritative player state.

## Synchronization behavior

The network should communicate changes in terms of authoritative ticks.
Snapshots should describe what the server believes the world and player looked
like after a particular simulation step.

The client does:

1. Receive packets independently of rendering.
2. Validate their protocol identity and contents.
3. Preserve valid snapshots until the render thread consumes them.
4. Ignore snapshots older than the newest accepted authoritative tick.
5. Update its authoritative state from accepted snapshots.
6. Send the next accumulated input batch after consuming a new snapshot.
7. Produce a render state from the surrounding authoritative snapshots.

Packet loss should cause a temporary lack of fresh correction, not corruption
of the last valid state. Packet reordering should not make the player move
backward in time. Duplicate packets should not cause a second simulation or
duplicate voxel edit.

Voxel state should be synchronized as authoritative world data. Voxel values
should not be interpolated like movement; a voxel is either in the old state
or the new state. Later versions may optimize this with deltas, but the
visible result must remain consistent with the server.

## Rendering result

Every rendered frame should use one coherent state. Position and velocity come
from interpolation between authoritative snapshots. Local yaw and pitch are
updated immediately from mouse input, while server snapshots also carry the
authoritative orientation for other clients.

The renderer should:

- Render the voxel world from the interpolated player camera
- Use the interpolated player position as the ray origin
- Use the interpolated view direction to construct rays
- Read world voxels from the GPU representation of the latest render state
- Never perform authoritative gameplay decisions
- Never write back a player position to the server

The debug overlay should show values from the same interpolated state used for
rendering. This makes it possible to compare what the player sees with the
state that produced the frame. Authoritative values may be shown separately
for diagnosis, but they must be clearly labeled.

## Threading and timing outcome

The render thread should remain responsible for window events, SDL input,
interpolation, and Vulkan submission. Network activity should not block it.
The server simulation should remain independent of the render loop.

The current timing relationship is:

```text
render frame:
  collect input
  accumulate movement, look deltas, and jump intent
  consume available authoritative snapshots
  after consuming a snapshot, send the accumulated input batch
  calculate interpolated render state
  render that state

server tick:
  receive registrations and input packets
  for each registered client:
    consume latest valid intent
    simulate movement, collision, jumping, and world changes
    publish that client's authoritative snapshot
```

Shutdown should stop simulation and network activity cleanly before destroying
their sockets or shared resources. No thread should continue writing state
after the renderer or its backing data has been destroyed.

## Success criteria

The design is working when:

- Every client receives a unique UUID and a server-assigned player ID.
- Multiple registered clients are simulated independently.
- Holding movement input changes the server's authoritative position after the
  deferred input reaches the server.
- The authoritative client state reflects that server position.
- The interpolated render position changes smoothly between snapshots.
- The displayed velocity corresponds to the state being rendered.
- The camera responds to mouse input while captured.
- Releasing and recapturing the mouse does not corrupt input.
- The server remains authoritative when packets are delayed or lost.
- The voxel world seen by the client eventually matches the server world.
- Movement is never applied directly to the rendered position.
- Rendering remains smooth and independent from the server tick rate.

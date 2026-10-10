# Multiplayer simulation plan

This document describes what the multiplayer game is intended to achieve. It
defines the expected behavior and responsibilities of each part of the game,
without prescribing a particular implementation.

## Overall experience

The game should feel like a responsive, continuous single-player experience
even though the shared world is controlled by a remote authoritative server.
Local input should be visible immediately, rendering should remain smooth at
the display frame rate, and all players should eventually observe the same
world, positions, collisions, jumps, camera orientation, and voxel changes.

The simulation must not become dependent on the renderer's frame rate or on
the timing of network packets. A slow frame, a fast frame, delayed packet, or
lost packet should not change the rules of the game.

## Intended world and player behavior

The server should own the complete shared world. This includes:

- Every voxel and its visual properties
- Every object and its physical state
- The player position and velocity
- The player's view orientation
- Whether the player is grounded
- Whether a jump is allowed
- The current simulation tick

The player should move according to input intent, velocity, gravity, and
collision rules evaluated by the server. A client must not be able to declare
that it is at a new position and have that position become authoritative.
This prevents divergent clients, invalid movement, and client-side collision
cheating.

The player should be able to look independently of movement. Looking should
respond to mouse input without waiting for a rendered frame or server response
to feel responsive. The server should still be the source of truth for the
orientation that is shared with other clients.

Jumping should only occur when the player was on the ground at the beginning
of the relevant simulation step and has not already consumed that jump.
Gravity, vertical movement, and contact with the initial stone layer should be
consistent for every client because they are decided by the server.

## Server purpose

The server should provide one authoritative timeline for the game. It should
advance at a fixed simulation frequency and make all gameplay decisions on
that timeline.

For every simulation step, the server should:

1. Use the most recent valid input intent available for each connected player.
2. Apply that intent to movement and view orientation.
3. Evaluate jump eligibility from the previous grounded state.
4. Apply gravity, velocity, movement, and collision resolution.
5. Apply accepted voxel edits and other world changes.
6. Advance the authoritative tick.
7. Publish the resulting state as a snapshot associated with that tick.

The server should never depend on how frequently a client renders. It should
continue simulating while a client is resizing a window, paused briefly, or
rendering at a different frame rate.

The server should reject malformed or invalid input rather than creating a
success-shaped state from it. It should tolerate missing input by continuing
with the last valid intent or the defined neutral behavior.

## Client purpose

The client should translate local input into commands, receive authoritative
results, and prepare a smooth state for rendering.

The client should maintain a clear distinction between:

### Local input state

This represents what the local user is doing now. It includes keyboard
movement, mouse look deltas, jump actions, mouse capture state, and the next
input tick. It exists to generate commands and should not be treated as proof
of physical movement.

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

The client should send commands promptly over UDP. A command may be lost,
duplicated, delayed, or arrive out of order. Tick identifiers should allow the
receiver to recognize stale or repeated commands.

Escape should release mouse capture, and clicking the game window should
restore it. This affects local input collection only; it must not alter the
authoritative player state.

## Synchronization behavior

The network should communicate changes in terms of authoritative ticks.
Snapshots should describe what the server believes the world and player looked
like after a particular simulation step.

The client should:

1. Receive packets independently of rendering.
2. Validate their protocol identity and contents.
3. Preserve valid snapshots until the render thread consumes them.
4. Ignore snapshots older than the newest accepted authoritative tick.
5. Update its authoritative state from accepted snapshots.
6. Produce a render state from the surrounding authoritative snapshots.

Packet loss should cause a temporary lack of fresh correction, not corruption
of the last valid state. Packet reordering should not make the player move
backward in time. Duplicate packets should not cause a second simulation or
duplicate voxel edit.

Voxel state should be synchronized as authoritative world data. Voxel values
should not be interpolated like movement; a voxel is either in the old state
or the new state. Later versions may optimize this with deltas, but the
visible result must remain consistent with the server.

## Rendering result

Every rendered frame should use one coherent interpolated state. The player
position, velocity, orientation, and world presented to the compute shader
should correspond to that state as closely as possible.

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

The desired timing relationship is:

```text
render frame:
  collect input
  send input intent
  consume available authoritative snapshots
  calculate interpolated render state
  render that state

server tick:
  consume latest valid intent
  simulate movement, collision, jumping, and world changes
  publish authoritative snapshot
```

Shutdown should stop simulation and network activity cleanly before destroying
their sockets or shared resources. No thread should continue writing state
after the renderer or its backing data has been destroyed.

## Success criteria

The design is working when:

- Holding movement input changes the server's authoritative position.
- The authoritative client state reflects that server position.
- The interpolated render position changes smoothly between snapshots.
- The displayed velocity corresponds to the state being rendered.
- The camera responds to mouse input while captured.
- Releasing and recapturing the mouse does not corrupt input.
- The server remains authoritative when packets are delayed or lost.
- The voxel world seen by the client eventually matches the server world.
- Rendering remains smooth and independent from the server tick rate.

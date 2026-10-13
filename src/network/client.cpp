#include "client.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

namespace voxel::net {

Client::Client(Peer server, std::string host)
    : m_server(std::move(server)), m_host(std::move(host)) {
    std::random_device random;
    for (auto& byte : m_uuid)
        byte = static_cast<std::uint8_t>(random());
    m_localYaw = 0.0f;
    m_localPitch = -18.0f;
}

// A destructor must never let an exception escape: that terminates the process.
Client::~Client() noexcept {
    try {
        stop();
    } catch (const std::exception& error) {
        std::cerr << "Client: shutdown failed: " << error.what() << '\n';
    }
}

void Client::start() {
    if (m_running.exchange(true)) return;
    m_socket.send(encode(Registration{m_uuid}), m_server);
    m_thread = std::thread(&Client::receiveLoop, this);
    m_worldThread = std::thread(&Client::worldReceiveLoop, this);
}

void Client::stop() {
    // Always join, even if stop() was already called: a std::thread whose
    // destructor runs while it is still joinable terminates the process.
    m_running.store(false);
    m_worldSocket.shutdown();
    if (m_worldThread.joinable()) m_worldThread.join();
    if (m_thread.joinable()) m_thread.join();
}

void Client::receiveLoop() {
    m_socket.nonBlocking();
    std::array<std::byte, 2048> buffer{};
    while (m_running.load()) {
        Peer peer;
        const auto size = m_socket.receive(buffer, peer);
        if (size <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const auto bytes = std::span<const std::byte>(
            buffer.data(), static_cast<std::size_t>(size));

        RegistrationAck acknowledgement{};
        if (decode(bytes, acknowledgement)) {
            std::scoped_lock lock(m_mutex);
            if (acknowledgement.uuid != m_uuid) return;
            m_registered = true;
            // The server assigns the id; it is needed for the world stream
            // handshake, which can happen before the first snapshot arrives.
            m_playerId = acknowledgement.playerId;
            continue;
        }
        AuthoritativeState snapshot{};
        if (decode(bytes, snapshot)) {
            acceptSnapshot(std::move(snapshot));
            continue;
        }
    }
}

void Client::worldReceiveLoop() {
    // Wait for UDP registration: the server refuses a stream it cannot tie to
    // a registered client.
    float waited = 0.0f;
    while (m_running.load()) {
        {
            std::scoped_lock lock(m_mutex);
            if (m_registered) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        waited += 0.02f;
        if (waited > 5.0f) {
            std::cerr << "Client: world stream waiting for registration\n";
            waited = 0.0f;
        }
    }
    if (!m_running.load()) return;

    for (;;) {
        try {
            m_worldSocket.connect(
                m_host.c_str(),
                static_cast<std::uint16_t>(m_server.port + 1));
            break;
        } catch (const std::runtime_error&) {
            if (!m_running.load()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    WorldInterest hello{};
    hello.uuid = m_uuid;
    {
        std::scoped_lock lock(m_mutex);
        hello.playerId = m_playerId;
        hello.radius = m_interestRadius;
    }
    if (!m_worldSocket.sendFrame(encode(hello))) return;

    std::vector<std::byte> frame;
    // 1. the world descriptor: geometry and the server-owned material table
    if (!m_worldSocket.receiveFrame(frame)) return;
    WorldDescriptor descriptor{};
    if (!decode(frame, descriptor)) {
        std::cerr << "Client: invalid world descriptor\n";
        return;
    }
    {
        std::scoped_lock lock(m_mutex);
        m_spec = voxel::WorldSpec{descriptor.sizeX, descriptor.sizeY,
                                  descriptor.sizeZ, descriptor.brick};
        m_streamSpec = m_spec;
        m_materials = descriptor.materials;
        m_hasSpec = true;
        m_render.sizeX = descriptor.sizeX;
        m_render.sizeY = descriptor.sizeY;
        m_render.sizeZ = descriptor.sizeZ;
        m_render.brick = descriptor.brick;
        m_render.rootDimX = descriptor.rootDimX;
        m_render.rootDimY = descriptor.rootDimY;
        m_render.rootDimZ = descriptor.rootDimZ;
        m_render.rootCount = descriptor.rootCount;
        // Clamp the requested radius to the world rather than to a constant.
        // The old cap of 6 was the bug: it silently truncated a window the
        // caller had sized to the view distance, leaving the renderer drawing
        // bricks that could never arrive. A window larger than the world is
        // merely wasteful, not wrong, so the clamp is against the geometry.
        const auto maxRadius = static_cast<std::uint8_t>(std::max<std::uint32_t>(
            1u, std::max({m_spec.rootDimX(), m_spec.rootDimY(),
                          m_spec.rootDimZ()})));
        m_interestRadius = std::min<std::uint8_t>(m_interestRadius, maxRadius);
        // Handed to the renderer once; the renderer clears it after upload.
        m_render.materials = std::move(descriptor.materials);
    }

    // 2. stream bricks, acknowledging each one so the server stops resending.
    constexpr int kWaitMs = 50;
    while (m_running.load() && m_worldSocket.valid()) {
        WorldInterest interest{};
        std::vector<WorldAck> acks;
        std::vector<std::uint32_t> forgets;
        bool sendInterest = false;
        {
            std::scoped_lock lock(m_mutex);
            if (m_hasSnapshots) {
                const std::int32_t brick =
                    static_cast<std::int32_t>(m_streamSpec.brick);
                const std::int32_t dimX =
                    static_cast<std::int32_t>(m_streamSpec.rootDimX());
                const std::int32_t dimY =
                    static_cast<std::int32_t>(m_streamSpec.rootDimY());
                const std::int32_t dimZ =
                    static_cast<std::int32_t>(m_streamSpec.rootDimZ());
                const std::int32_t cx = static_cast<std::int32_t>(
                    std::floor(m_render.localPlayer.position.x));
                const std::int32_t cy = static_cast<std::int32_t>(
                    std::floor(m_render.localPlayer.position.y));
                const std::int32_t cz = static_cast<std::int32_t>(
                    std::floor(m_render.localPlayer.position.z));
                const std::int32_t bx =
                    std::clamp(cx / brick, 0, dimX - 1) * brick;
                const std::int32_t by =
                    std::clamp(cy / brick, 0, dimY - 1) * brick;
                const std::int32_t bz =
                    std::clamp(cz / brick, 0, dimZ - 1) * brick;
                if (!m_hasInterest || bx != m_interestX || by != m_interestY ||
                    bz != m_interestZ) {
                    m_interestX = bx;
                    m_interestY = by;
                    m_interestZ = bz;
                    m_hasInterest = true;
                    sendInterest = true;
                    interest.uuid = m_uuid;
                    interest.playerId = m_playerId;
                    interest.centerX = bx;
                    interest.centerY = by;
                    interest.centerZ = bz;
                    interest.radius = m_interestRadius;
                }
            }
            acks.swap(m_pendingAcks);
            forgets.swap(m_pendingForgets);
        }
        if (sendInterest && !m_worldSocket.sendFrame(encode(interest))) return;
        for (const WorldAck& ack : acks)
            if (!m_worldSocket.sendFrame(encode(ack))) return;
        // A forget is only queued for a brick that was evicted and not
        // re-placed in the same frame -- `confirmUploads` excludes those --
        // so nothing here can retract a delivery the client still stands
        // behind.
        for (std::uint32_t rootIndex : forgets)
            if (!m_worldSocket.sendFrame(encode(WorldForget{
                    m_uuid, m_playerId, rootIndex})))
                return;

        if (!m_worldSocket.pollReadable(kWaitMs)) continue;
        if (!m_worldSocket.receiveFrame(frame)) return;
        BrickSnapshot snapshot{};
        if (!decode(frame, snapshot)) continue;
        enqueueBrick(std::move(snapshot));
    }
}

void Client::enqueueBrick(BrickSnapshot snapshot) {
    const voxel::WorldSpec spec = m_streamSpec;
    if (spec.rootCount() == 0 || snapshot.rootIndex >= spec.rootCount())
        return;
    // A frame carries one payload, and it has to be the one the level names.
    // An octree at level 3 is a frame that says the wrong thing about itself,
    // and a grid at level 0 would be an averaged stand-in for the exact
    // geometry the client needs at its feet.
    // The server sends exactly one payload, and it must be the one the level
    // names: an octree for level 0, one grid at the named level otherwise. A
    // frame with both, with neither, or with a grid at the wrong level is a
    // corrupt frame, and accepting it would book a level the payload does not
    // describe -- the ack would then confirm data the client never held.
    const bool shapeOk =
        snapshot.lodLevel == 0
            ? (!snapshot.nodes.empty() && snapshot.levels.empty())
            : (snapshot.nodes.empty() && snapshot.levels.size() == 1 &&
               snapshot.levels.front().level == snapshot.lodLevel);
    if (!shapeOk) return;
    std::scoped_lock lock(m_mutex);

    const std::uint64_t key = levelKey(snapshot.rootIndex, snapshot.lodLevel);

    // Three ways a frame can be redundant, and they are not the same thing:
    //
    //  * Already on the GPU at this level and revision: nothing to do. This is
    //    the only case that may drop the frame outright.
    //  * Queued or in flight: the server resent something we have not finished
    //    with. Dropping it is right, but it must *not* be treated as delivered.
    //  * Evicted and back: accepted again, which is the whole point of the
    //    server purging its record outside the interest window.
    //
    // An earlier version recorded "resident" the moment a brick was *queued*.
    // If the node pool then refused it, the entry was gone but the record
    // remained, so the resend was dismissed as a duplicate forever: the client
    // waited for an upload that could not arrive and the server waited for an
    // ack that was never coming.
    //
    // The level is part of the identity, not a detail of it. Holding this brick
    // at level 5 says nothing about whether it holds level 1, and a client that
    // only tracked the root index would refuse the finer delivery as a
    // duplicate the moment the player walked close enough to want it -- which
    // is exactly the moment it matters.
    const auto resident = m_resident.find(snapshot.rootIndex);
    if (resident != m_resident.end() &&
        (resident->second.levels & (1u << snapshot.lodLevel)) != 0 &&
        resident->second.revision == snapshot.revision)
        return;
    const auto inFlight = m_awaitingUpload.find(key);
    if (inFlight != m_awaitingUpload.end() &&
        inFlight->second == snapshot.revision)
        return;

    // Back-pressure, not a silent drop. The queue is bounded so a stalled
    // render thread cannot grow the heap without limit -- but the old code
    // satisfied that bound by *discarding* the oldest updates, which had
    // already been marked resident and acknowledged. The server then saw the
    // ack, stopped resending, and those bricks were never uploaded at all, so
    // they stayed magenta for the rest of the session. A brick that has been
    // received but not queued is simply never acknowledged, which makes the
    // server retry it on its next pass.
    if (m_pendingBricks.size() >= kMaxPendingBricks) {
        m_uploadOverflow = true;
        return;
    }

    voxel::BrickUpdate update{};
    update.rootIndex = snapshot.rootIndex;
    update.revision = snapshot.revision;
    // Carried through so the renderer can tell a full-resolution brick from an
    // averaged one, and so a test can see what the server actually shipped.
    update.lodLevel = snapshot.lodLevel;
    if (snapshot.lodLevel == 0) {
        update.nodes = std::move(snapshot.nodes);
    } else {
        update.edge = snapshot.levels.front().edge;
        update.cells = std::move(snapshot.levels.front().cells);
    }
    m_pendingBricks.push_back(std::move(update));
    // Not acknowledged here. The ack waits for the renderer to report the
    // upload in `confirmUploads`, so the server's idea of what we hold cannot
    // outrun what is actually on the GPU.
    m_awaitingUpload.emplace(key, snapshot.revision);
}

void Client::drainBricks(std::vector<BrickUpdate>& out) {
    std::scoped_lock lock(m_mutex);
    // Whatever is still in `out` is a brick the renderer *could not place* last
    // frame and chose to keep queued. It must go back at the front, ahead of
    // anything newly received, or it is silently dropped: the client never
    // acknowledged it, so the server still thinks it is in flight and declines
    // to re-offer it, and the brick is simply lost. Retrying it here is the
    // client's own retry path and does not depend on the server noticing.
    //
    // This is the other half of the swap. Ownership still moves rather than
    // being shared: at no point do two threads hold the same vector.
    if (!out.empty()) {
        out.insert(out.end(),
                   std::make_move_iterator(m_pendingBricks.begin()),
                   std::make_move_iterator(m_pendingBricks.end()));
        m_pendingBricks.clear();
    }
    out.swap(m_pendingBricks);
}

std::size_t Client::pendingQueueSize() const {
    std::scoped_lock lock(m_mutex);
    return m_pendingBricks.size();
}

void Client::confirmUploads() {
    std::scoped_lock lock(m_mutex);

    // The revision each delivered level was uploaded at. Taken before the
    // in-flight set is cleared below, since that is where the revision is still
    // known.
    std::unordered_map<std::uint64_t, std::uint32_t, LevelKeyHash> confirmed;
    for (const BrickRef& ref : m_render.uploadedRefs) {
        const auto pending = m_awaitingUpload.find(levelKey(ref.rootIndex,
                                                            ref.level));
        if (pending == m_awaitingUpload.end()) continue;   // evicted meanwhile
        confirmed.emplace(levelKey(ref.rootIndex, ref.level), pending->second);
    }

    // The levels the renderer dropped, grouped per brick. A level here was
    // acknowledged once and then thrown away, so it is no longer held -- the
    // record must go or the next resend is dismissed as a duplicate.
    //
    // One case is deliberately excluded: a brick evicted and re-placed in the
    // same frame. The renderer evicts while processing the queue in order, so
    // a brick dropped for an earlier update can be re-uploaded by a later one,
    // and it then appears in `uploadedRefs` too. It is resident, so it is
    // acknowledged and not forgotten; forgetting it would only cost a
    // redundant resend.
    std::unordered_map<std::uint32_t, std::uint8_t, RegionKeyHash> evicted;
    std::unordered_set<std::uint32_t> replaced;
    for (const BrickRef& ref : m_render.evictedRefs) {
        evicted[ref.rootIndex] |= static_cast<std::uint8_t>(1u << ref.level);
    }
    for (const auto& [key, revision] : confirmed) {
        const auto rootIndex = static_cast<std::uint32_t>(key >> 3);
        replaced.insert(rootIndex);
    }
    for (const auto& [rootIndex, levels] : evicted) {
        if (replaced.find(rootIndex) != replaced.end()) continue;
        auto resident = m_resident.find(rootIndex);
        if (resident == m_resident.end()) continue;
        resident->second.levels &= static_cast<std::uint8_t>(~levels);
        for (std::uint8_t level = 0; level < voxel::kLodLevels; ++level) {
            if ((levels & (1u << level)) != 0)
                m_awaitingUpload.erase(levelKey(rootIndex, level));
        }
        // One forget per brick, not per level. WorldForget retires the server's
        // whole record for the brick, so a second one for the same root index
        // would say nothing the first did not.
        m_pendingForgets.push_back(rootIndex);
    }

    // Only a level the renderer placed in the pool is recorded as held and
    // acknowledged. This is the sole source of both facts.
    for (const auto& [key, revision] : confirmed) {
        const auto rootIndex = static_cast<std::uint32_t>(key >> 3);
        const auto level = static_cast<std::uint8_t>(key & 0x7u);
        Resident& entry = m_resident[rootIndex];
        entry.revision = revision;
        entry.levels |= static_cast<std::uint8_t>(1u << level);
        m_pendingAcks.push_back(
            WorldAck{m_uuid, m_playerId, rootIndex, revision, level});
    }

    // Everything the renderer finished with is no longer queued, so nothing
    // awaits an upload that will never arrive -- including bricks it refused.
    // Clearing these is what keeps a refused brick eligible for the server's
    // next resend rather than dismissing it as a duplicate forever.
    for (const BrickRef& ref : m_render.processedRefs) {
        m_awaitingUpload.erase(levelKey(ref.rootIndex, ref.level));
        // Frees carry no data, so they are tracked by index and retired as
        // soon as the renderer reports them. This keeps the upload bound
        // accounting for uploads alone.
        if (m_pendingFrees.erase(ref.rootIndex) > 0 && m_evictionBacklog > 0)
            --m_evictionBacklog;
    }

    m_render.processedRefs.clear();
    m_render.uploadedRefs.clear();
    m_render.evictedRefs.clear();
}

void Client::submit(const InputIntent& intent, float frameSeconds) {
    std::scoped_lock lock(m_mutex);
    // For convenience the caller may hand raw keys instead of an intent; they
    // are resolved below against the local camera basis.
    m_registrationRetry -= frameSeconds;
    m_inputAccumulator += frameSeconds;
    if (!m_registered && m_registrationRetry <= 0.0f) {
        m_socket.send(encode(Registration{m_uuid}), m_server);
        m_registrationRetry = 0.5f;
    }

    // Resolve the keys into a world direction here, because this is the only side
    // that knows which way the screen is facing. The renderer builds its basis
    // from the same `m_localYaw` (see VulkanContext::drawFrame), so the
    // direction that moves the player is derived from the direction that draws
    // it -- there is no second copy of this maths anywhere, which is what the
    // old path got wrong: the authority rebuilt the basis from its own
    // replicated `yaw`, and the two disagreed whenever a look input was still
    // in flight, so W walked off in a direction the screen did not show.
    //
    // `m_localYaw` is the *predicted* yaw, updated below from this frame's look
    // first, so the direction uses the yaw the player is actually rendering
    // this frame rather than the last one the server confirmed.
    m_localYaw += intent.look.x;
    while (m_localYaw > 180.0f) m_localYaw -= 360.0f;
    while (m_localYaw < -180.0f) m_localYaw += 360.0f;

    const glm::vec2 move = glm::clamp(intent.rawMove.axes, glm::vec2(-1.0f),
                                      glm::vec2(1.0f));
    // Yaw is degrees on the wire, radians here, matching the renderer exactly.
    const float yaw = glm::radians(m_localYaw);
    const glm::vec2 forward{std::cos(yaw), std::sin(yaw)};
    // right = cross(forward, up) with up = +Z, which on the ground plane is
    // (forward.y, -forward.x). This is the renderer's basis, not a new one.
    const glm::vec2 right{forward.y, -forward.x};
    // Not normalised here: the magnitude carries how much of the movement is
    // held (W alone is full speed, W+D is still full speed but diagonal). The
    // authority normalises a diagonal so it cannot outrun a single axis.
    const glm::vec2 direction = forward * move.y + right * move.x;
    m_accumulatedLook += intent.look;
    m_accumulatedLook = glm::clamp(m_accumulatedLook,
                                   glm::vec2(-voxel::kMaxLookDegrees),
                                   glm::vec2(voxel::kMaxLookDegrees));
    m_latestClientTick = intent.clientTick;
    m_jumpRequested = m_jumpRequested || intent.jumpPressed;

    if (m_registered && m_inputAccumulator >= voxel::kTickSeconds) {
        InputIntent out{};
        out.clientTick = m_latestClientTick;
        out.moveDirection = direction;
        out.look = m_accumulatedLook;
        out.jumpPressed = m_jumpRequested;
        // The requested flight mode travels as intent: the authority owns the
        // applied state and mails it back in the snapshot.
        out.flying = intent.flying;
        out.descendPressed = intent.descendPressed;
        if (m_socket.send(encode(out), m_server)) {
            m_inputAccumulator -= voxel::kTickSeconds;
            m_accumulatedLook = glm::vec2(0.0f);
            m_jumpRequested = false;
            m_sentClientTick = out.clientTick;
        }
    }

    // Yaw was already folded in above, before the direction was resolved from it.
    // Look stays local for responsiveness; the snapshot still carries the
    // authoritative orientation for everyone else.
    m_localPitch = std::clamp(m_localPitch + intent.look.y, -89.0f, 89.0f);
    m_render.localPlayer.yaw = m_localYaw;
    m_render.localPlayer.pitch = m_localPitch;
}

void Client::acceptSnapshot(AuthoritativeState snapshot) {
    std::scoped_lock lock(m_mutex);
    const std::uint32_t newest =
        m_hasSnapshots ? m_next.tick : m_authoritative.tick;
    if (snapshot.tick <= newest) return;
    m_snapshots.push_back(std::move(snapshot));
    while (m_snapshots.size() > 8) m_snapshots.pop_front();
}

void Client::updateRender(float frameSeconds) {
    std::scoped_lock lock(m_mutex);
    while (!m_snapshots.empty()) {
        AuthoritativeState incoming = std::move(m_snapshots.front());
        m_snapshots.pop_front();
        if (!m_hasSnapshots) {
            m_previous = incoming;
            m_next = std::move(incoming);
            m_hasSnapshots = true;
        } else {
            m_previous = m_next;
            m_next = std::move(incoming);
        }
        m_authoritative = m_next;
        m_clock = 0.0f;
    }

    // Publish the interest window. The renderer uses it to decide what it is
    // willing to evict: a brick inside the window is never the first thing
    // thrown away to make room for one from across the world.
    //
    // Note what is *not* here any more: this used to evict every brick that
    // fell outside the window, on the grounds that the player cannot see it.
    // That is wrong once the client is allowed to hold a budget larger than the
    // window -- which is the whole point of a 3 GiB arena on a world that fits
    // in a few hundred megabytes. Evicting on window exit meant the cache was
    // always exactly one window deep, so streaming the rest of the world was
    // pointless: every brick sent was thrown away before it could be used, and
    // the player re-downloaded the ground they had already walked over on every
    // step. Retention is now the arena's decision, by capacity; this is only
    // about priority.
    if (m_hasSpec && m_hasInterest) {
        m_render.interestX = m_interestX;
        m_render.interestY = m_interestY;
        m_render.interestZ = m_interestZ;
        m_render.interestRadius = m_interestRadius;
    }

    m_clock = std::min(m_clock + frameSeconds, voxel::kTickSeconds);
    // Once the authority has consumed every intent the client sent, adopt its
    // yaw. The local look only ever previews the server, so a clamped or lost
    // intent cannot leave the camera facing somewhere the server is not, which
    // is the direction movement is resolved in.
    if (m_sentClientTick != 0 &&
        m_authoritative.acknowledgedInputTick >= m_sentClientTick)
        m_localYaw = m_authoritative.localPlayer.yaw;
    m_render.localPlayer = m_authoritative.localPlayer;
    m_render.localPlayer.yaw = m_localYaw;
    m_render.localPlayer.pitch = m_localPitch;
    if (!m_hasSnapshots) return;
    const float alpha = std::clamp(m_clock / voxel::kTickSeconds, 0.0f, 1.0f);
    m_render.localPlayer.position = glm::mix(m_previous.localPlayer.position,
                                             m_next.localPlayer.position, alpha);
    m_render.localPlayer.velocity = glm::mix(m_previous.localPlayer.velocity,
                                             m_next.localPlayer.velocity, alpha);
}

} // namespace voxel::net

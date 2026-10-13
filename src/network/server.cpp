#include "server.hpp"
#include "pacer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>

namespace voxel::net {
namespace {

// How many bricks the background fill examines per pass, and how many it may
// queue. The sweep is bounded so filling a world of a million bricks costs the
// same per iteration as filling one of a hundred, and the queue is bounded so a
// slow client cannot make the server hold the whole world in memory -- which is
// exactly the memory the world budget is for.
constexpr std::uint32_t kFillSweepPerPass = 4096;
constexpr std::size_t kMaxFillPerPass = 64;

// The fixed part of a frame's cost -- header, uuid, revision fields -- charged
// before the payload is even built, so the pacer is consulted before the
// expensive serialisation rather than after.
constexpr std::size_t kFrameOverheadBytes = 64;

} // namespace

Server::Server(std::uint16_t port, voxel::WorldSpec spec, std::uint64_t seed,
               std::string host)
    : m_port(port), m_host(std::move(host)), m_spec(spec),
      m_world(std::make_unique<voxel::World>(spec, seed)) {}

void Server::start() {
    if (m_running.exchange(true)) return;
    // Small worlds are cheap to build, and streaming them lazily would stall a
    // tick mid-frame, so materialise the whole root layer up front.
    //
    // At the production size of 65536 x 65536 x 8192 the root layer is
    // 16,777,216 bricks, so this never runs: generation is entirely lazy,
    // which is the only way that size is reachable at all. The guard is not
    // redundant, it is the switch between the two regimes -- and the test that
    // would catch it being wrong is VoxelBigWorldTests, which asserts the
    // world it runs on is above the threshold.
    if (m_world->rootCount() <= 4096) m_world->generateAll();
    m_thread = std::thread(&Server::run, this);
    m_worldThread = std::thread(&Server::worldRun, this);
}

void Server::stop() {
    // Always join, even if stop() was already called: a std::thread whose
    // destructor runs while it is still joinable terminates the process.
    m_running.store(false);
    m_worldListener.shutdown();
    if (m_thread.joinable()) m_thread.join();
    if (m_worldThread.joinable()) m_worldThread.join();
    std::scoped_lock lock(m_streamMutex);
    for (auto& entry : m_streamThreads)
        if (entry.first.joinable()) entry.first.join();
    m_streamThreads.clear();
}

// A destructor must never let an exception escape: that terminates the process.
Server::~Server() noexcept {
    try {
        stop();
    } catch (const std::exception& error) {
        std::cerr << "Server: shutdown failed: " << error.what() << '\n';
    }
}

Server::ClientConnection* Server::findClient(const ClientUuid& uuid) {
    const auto found =
        std::find_if(m_clients.begin(), m_clients.end(),
                     [&](const ClientConnection& client) { return client.uuid == uuid; });
    return found == m_clients.end() ? nullptr : &*found;
}

void Server::receive() {
    std::array<std::byte, 512> buffer{};
    for (;;) {
        Peer peer;
        const auto size = m_socket.receive(buffer, peer);
        if (size <= 0) return;
        const auto bytes = std::span<const std::byte>(
            buffer.data(), static_cast<std::size_t>(size));

        Registration registration{};
        if (decode(bytes, registration)) {
            std::scoped_lock lock(m_mutex);
            ClientConnection* client = findClient(registration.uuid);
            if (client == nullptr) {
                if (m_clients.size() >= voxel::kMaxPlayers) continue;
                ClientConnection connection{};
                connection.uuid = registration.uuid;
                connection.peer = peer;
                connection.authority =
                    std::make_unique<voxel::Authority>(m_nextPlayerId++, *m_world);
                m_clients.push_back(std::move(connection));
                client = &m_clients.back();
            } else {
                client->peer = peer;
            }
            m_socket.send(encode(RegistrationAck{
                              registration.uuid,
                              client->authority->state().localPlayer.id}),
                          peer);
            continue;
        }

        InputIntent intent{};
        if (!decode(bytes, intent)) continue;
        std::scoped_lock lock(m_mutex);
        const auto found =
            std::find_if(m_clients.begin(), m_clients.end(),
                         [&](const ClientConnection& client) {
                             return client.peer == peer;
                         });
        if (found == m_clients.end()) continue;
        // Stale or replayed ticks are ignored: the authority keeps the newest
        // valid intent and derives everything else itself.
        if (intent.clientTick <= found->lastClientTick) continue;
        found->jumpRequested = found->jumpRequested || intent.jumpPressed;
        found->latest = intent;
        found->lastClientTick = intent.clientTick;
    }
}

void Server::run() {
    try {
        m_socket.bind(m_port);
    } catch (const std::runtime_error& error) {
        std::cerr << "Server: UDP bind failed: " << error.what() << '\n';
        m_running.store(false);
        return;
    }
    m_socket.nonBlocking();

    using Clock = std::chrono::steady_clock;
    auto next = Clock::now();
    while (m_running.load()) {
        receive();
        if (Clock::now() < next) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        std::scoped_lock worldLock(m_worldMutex);
        std::scoped_lock lock(m_mutex);
        ++m_tick;
        const auto count = static_cast<std::uint8_t>(
            std::min<std::size_t>(m_clients.size(), voxel::kMaxPlayers));
        const std::uint32_t worldRevision = m_world->revision();
        for (auto& client : m_clients) {
            InputIntent intent = client.latest;
            intent.jumpPressed = client.jumpRequested;
            client.jumpRequested = false;
            if (intent.clientTick <=
                client.authority->state().acknowledgedInputTick) {
                // Already applied: this is a replayed tick, standing in for an
                // intent that did not arrive in time. Blanking the whole intent
                // would stop the player dead mid-stride -- a held movement key
                // would do nothing at all whenever the network delivered slower
                // than the tick rate, which is most of the time over UDP.
                //
                // Only the *event* fields are dropped, because they are events:
                // re-firing a jump that already happened is not idempotent. The
                // continuous inputs (where the player is looking, which way they
                // are walking, what they hold) are a held state, and holding it
                // across a dropped packet is exactly right.
                intent.clientTick = 0;
                intent.jumpPressed = false;
                intent.descendPressed = false;
                intent.flying = client.latest.flying;
            }
            client.authority->step(intent);
            client.authority->state().tick = m_tick;
            client.authority->state().worldRevision = worldRevision;
            client.authority->state().playerCount = count;
            for (std::uint8_t i = 0; i < count; ++i)
                client.authority->state().players[i] =
                    m_clients[i].authority->state().localPlayer;
            if (client.peer.port != 0)
                m_socket.send(encode(client.authority->state()), client.peer);
        }
        next += std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<float>(voxel::kTickSeconds));
        if (Clock::now() > next + std::chrono::seconds(1)) next = Clock::now();
    }
}

void Server::worldRun() {
    try {
        m_worldListener.listen(static_cast<std::uint16_t>(m_port + 1));
        m_worldListener.nonBlocking();
    } catch (const std::runtime_error& error) {
        std::cerr << "Server: world stream listen failed: " << error.what()
                  << '\n';
        m_running.store(false);
        return;
    }
    while (m_running.load()) {
        TcpSocket connection = m_worldListener.accept();
        if (!connection.valid()) {
            if (m_running.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        // Each connection gets its own thread. Only threads whose connection
        // has already finished are reaped: joining a live one here would make a
        // second client wait for the first to disconnect.
        auto finished = std::make_shared<std::atomic_bool>(false);
        std::scoped_lock lock(m_streamMutex);
        for (auto& entry : m_streamThreads) {
            if (*entry.second && entry.first.joinable()) entry.first.join();
            if (!entry.first.joinable()) entry.second.reset();
        }
        m_streamThreads.erase(
            std::remove_if(m_streamThreads.begin(), m_streamThreads.end(),
                           [](const StreamThread& entry) { return !entry.second; }),
            m_streamThreads.end());
        m_streamThreads.push_back(
            StreamThread{std::thread(&Server::worldConnection, this,
                                     std::move(connection), finished),
                         std::move(finished)});
    }
}

void Server::worldConnection(TcpSocket connection,
                             std::shared_ptr<std::atomic_bool> finished) {
    // Marks the stream thread joinable for the listener's reaper.
    struct Notifier {
        std::shared_ptr<std::atomic_bool> flag;
        ~Notifier() { flag->store(true); }
    } notifier{std::move(finished)};

    std::vector<std::byte> frame;
    WorldInterest hello{};
    if (!connection.receiveFrame(frame) || !decode(frame, hello)) return;

    // Every streaming pass reads the clock once. `kResendAfterMs` is the grace
    // period a sent-but-unacknowledged brick gets: the client acknowledges only
    // once the brick is in its GPU pool, so silence means lost, refused, or
    // back-pressured, and all three are fixed by sending again.
    using StreamClock = std::chrono::steady_clock;
    const auto nowMs = [] {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                StreamClock::now().time_since_epoch())
                .count());
    };
    // How long an unacknowledged frame may sit before it is resent.
    constexpr std::uint64_t kResendAfterMs = 500;

    // Bandwidth. The world is streamed in full -- not just the neighbourhood --
    // so without a rate limit the very first pass would push the entire world
    // down the socket as fast as it could be encoded, which on a local link
    // finishes before the client has drawn a frame and on a real one starves
    // gameplay of bandwidth entirely.
    //
    // This is a ceiling, not a target: a client that keeps up is sent data
    // continuously, and a client that cannot falls behind and catches up later
    // at this same rate. `bytesPerSecond` is filled in from the server's
    // configured budget below.
    Pacer pacer{m_bytesPerSecond, nowMs()};

    // Where the progressive fill has got to. The world is swept in root-index
    // order rather than scanned from scratch each pass, so filling a world of
    // a million bricks costs the same per pass as filling one of a hundred.
    // Bytes spent in the last window, republished once a second so the rate is
    // observable. One shared gauge across connections, because the bandwidth
    // ceiling is a property of the link rather than of any one client.
    std::uint64_t windowStart = nowMs();
    std::uint64_t windowBytes = 0;
    auto publishRate = [&] {
        const std::uint64_t now = nowMs();
        if (now - windowStart < 1000) return;
        m_streamedBytes.store(windowBytes * 1000 / (now - windowStart),
                              std::memory_order_relaxed);
        windowStart = now;
        windowBytes = 0;
    };

    std::uint32_t fillCursor = 0;
    // How many bricks the fill has been offered so far, and how many it has
    // finished. This is what "until the whole world is sent over" is measured
    // against, and it is reported so the client can show progress.
    std::uint64_t fillScanned = 0;

    std::uint32_t playerId = 0;
    {
        std::scoped_lock lock(m_mutex);
        ClientConnection* client = findClient(hello.uuid);
        // A client must register over UDP before it may stream world data.
        if (client == nullptr ||
            client->authority->state().localPlayer.id != hello.playerId)
            return;
        playerId = client->authority->state().localPlayer.id;
    }

    auto stream = std::make_shared<StreamClient>();
    stream->uuid = hello.uuid;
    stream->playerId = playerId;
    {
        std::scoped_lock lock(stream->mutex);
        stream->centerX = hello.centerX;
        stream->centerY = hello.centerY;
        stream->centerZ = hello.centerZ;
        stream->radius = hello.radius;
        stream->hasInterest = true;
    }

    WorldDescriptor descriptor{};
    descriptor.sizeX = m_spec.sizeX;
    descriptor.sizeY = m_spec.sizeY;
    descriptor.sizeZ = m_spec.sizeZ;
    descriptor.brick = m_spec.brick;
    descriptor.rootDimX = m_spec.rootDimX();
    descriptor.rootDimY = m_spec.rootDimY();
    descriptor.rootDimZ = m_spec.rootDimZ();
    descriptor.rootCount = m_spec.rootCount();
    descriptor.seed = m_world->seed();
    descriptor.materials = voxel::serverMaterials();
    if (!connection.sendFrame(encode(descriptor))) return;

    struct Target {
        std::uint32_t rootIndex;
        float distance;
        // Chebyshev ring distance in bricks, which is what the level of detail
        // is a function of. Kept as a separate field from `distance` because the
        // sort key is Euclidean (nearest first) while the LOD key is Chebyshev
        // (a square neighbourhood, matching the interest window).
        std::int32_t ring;
        // Which level of this brick's pyramid to send, derived from the ring.
        // Decided when the target is created rather than when it is sent, so
        // the delivery bookkeeping -- which is per (rootIndex, level) -- can
        // tell a brick it is already streaming at the right level from one it
        // is about to stream at the wrong one. Deriving it later would mean the
        // record and the payload could disagree.
        std::uint8_t level;
    };
    const int brick = static_cast<int>(m_spec.brick);
    const int dimX = static_cast<int>(m_spec.rootDimX());
    const int dimY = static_cast<int>(m_spec.rootDimY());
    const int dimZ = static_cast<int>(m_spec.rootDimZ());

    while (m_running.load() && connection.valid()) {
        bool serviced = false;
        while (connection.pollReadable(0)) {
            if (!connection.receiveFrame(frame)) {
                connection.shutdown();
                break;
            }
            WorldInterest interest{};
            if (decode(frame, interest)) {
                std::scoped_lock lock(stream->mutex);
                stream->centerX = interest.centerX;
                stream->centerY = interest.centerY;
                stream->centerZ = interest.centerZ;
                stream->radius = interest.radius;
                stream->hasInterest = true;
                serviced = true;
                continue;
            }
            WorldAck ack{};
            if (decode(frame, ack)) {
                std::scoped_lock lock(stream->mutex);
                const auto key = levelKey(ack.rootIndex, ack.lodLevel);
                stream->acknowledged[key] =
                    StreamClient::Delivery{ack.revision, 0};
                // Confirmed, so it no longer needs retrying. Leaving the entry
                // behind would be harmless (acknowledged is checked first) but
                // it would keep the map holding bricks twice.
                stream->sent.erase(key);
                serviced = true;
                continue;
            }
            WorldForget forget{};
            if (decode(frame, forget)) {
                std::scoped_lock lock(stream->mutex);
                // Retract a delivery that was already counted. This is the
                // other half of the client's bounded GPU arena: a brick can be
                // evicted to make room while still inside the interest window,
                // and the delivery record is exactly what makes the send loop
                // below skip it forever. Dropping both records puts the brick
                // back in "never sent" state, so the next pass streams it
                // again and the client re-uploads it.
                //
                // All levels go, not just level 0. The message carries no level
                // on purpose: the renderer evicts a brick, not a level of one,
                // so the honest thing is to clear the whole record and let the
                // next pass re-offer whatever is still wanted.
                //
                // Forgetting a brick the client genuinely holds is harmless:
                // it just gets resent, and the client's dedup drops the frame
                // if the revision matches what it already has.
                for (std::uint8_t level = 0; level < voxel::kLodLevels; ++level) {
                    stream->acknowledged.erase(
                        levelKey(forget.rootIndex, level));
                    stream->sent.erase(levelKey(forget.rootIndex, level));
                }
                serviced = true;
            }
        }
        if (!m_running.load() || !connection.valid()) break;

        // Build the pending brick list, in two tiers.
        //
        //   Tier 0 -- what the player can see right now: the interest window,
        //              nearest first. This always goes out first, so walking
        //              into unwatched terrain is never delayed by a fill that
        //              happens to be passing through it.
        //   Tier 1 -- everything else, in root-index order, as a background
        //              fill. The client has a budget far larger than the window
        //              and a world that fits inside it, so the rest of the
        //              world is worth having resident before the player walks
        //              there: it is the difference between stepping into
        //              instant terrain and stepping into a hole that fills in
        //              over the next second.
        //
        // The fill only offers bricks the client has never received. A brick it
        // holds is skipped even if it is outside the window -- that is the
        // whole reason the arena can be larger than the window. A brick it
        // dropped comes back through the retraction path instead, which is
        // deliberately not the fill: resending forgotten scenery would evict
        // freshly streamed terrain forever and never finish.
        std::vector<Target> priority;
        std::vector<Target> fill;
        {
            std::scoped_lock lock(stream->mutex);
            if (!stream->hasInterest) {
                if (!serviced) connection.pollReadable(20);
                continue;
            }
            const int cx = std::clamp(stream->centerX / brick, 0, dimX - 1);
            const int cy = std::clamp(stream->centerY / brick, 0, dimY - 1);
            const int cz = std::clamp(stream->centerZ / brick, 0, dimZ - 1);
            const int radius = static_cast<int>(stream->radius);
            const std::uint8_t brickRootLevel = static_cast<std::uint8_t>(
                voxel::brickLodLevel(m_spec.brick));
            // Held means "delivered and confirmed, at this level". A brick whose
            // level-0 octree is resident does not thereby have its level 5
            // grid, and the two are acked separately -- see levelKey().
            const auto held = [&](std::uint32_t index, std::uint8_t level) {
                const auto key = levelKey(index, level);
                return stream->acknowledged.find(key) !=
                           stream->acknowledged.end() ||
                       stream->sent.find(key) != stream->sent.end();
            };
            const auto shouldSkip =
                [&](std::uint32_t index, std::uint8_t level,
                    std::uint32_t current, std::uint64_t now) {
                    const auto key = levelKey(index, level);
                    const auto acked = stream->acknowledged.find(key);
                    if (acked != stream->acknowledged.end() &&
                        acked->second.revision == current)
                        return true;
                    if (acked == stream->acknowledged.end()) {
                        const auto inFlight = stream->sent.find(key);
                        if (inFlight != stream->sent.end() &&
                            inFlight->second.revision == current &&
                            now - inFlight->second.at < kResendAfterMs)
                            return true;
                    }
                    return false;
                };

            // Tier 0: the window, nearest first. This carries three different jobs and
            // all three matter:
            //
            //   * never sent -- the ordinary case, the player walked into
            //     unwatched terrain;
            //   * sent but unacknowledged for longer than the grace period --
            //     the client only acknowledges what reached its arena, so
            //     silence means lost, refused, or back-pressured, and all three
            //     are fixed by sending again;
            //   * acknowledged at a stale revision -- the world was edited under
            //     a brick the client already holds.
            //
            // A brick acknowledged at the current revision, or in flight within
            // the grace period, is skipped: it is already on its way or already
            // there.
            const std::uint64_t now = nowMs();
            for (int bz = cz - radius; bz <= cz + radius; ++bz)
                for (int by = cy - radius; by <= cy + radius; ++by)
                    for (int bx = cx - radius; bx <= cx + radius; ++bx) {
                        if (bx < 0 || by < 0 || bz < 0 || bx >= dimX ||
                            by >= dimY || bz >= dimZ)
                            continue;
                        const auto index = static_cast<std::uint32_t>(
                            bx + dimX * (by + dimY * bz));
                        const std::int32_t ring =
                            std::max({std::abs(bx - cx), std::abs(by - cy),
                                      std::abs(bz - cz)});
                        // lodForRing speaks the old numbering, where the brick
                        // root level (7 for a 128-cell brick) means "full
                        // octree". The pyramid wire speaks 0 for the octree
                        // and 1..7 for the averaged grids, so the top maps to
                        // 0 and everything else passes through unchanged: a
                        // reduced tree with 16-cell leaves and a level-4 grid
                        // of 16-cell cells are the same sampling, and keeping
                        // the number means the ring bands keep the sizes the
                        // renderer was tuned against.
                        const std::uint8_t leaf = voxel::lodForRing(
                            ring, brickRootLevel);
                        const std::uint8_t level =
                            leaf >= brickRootLevel ? 0 : leaf;
                        const std::uint32_t current =
                            m_world->brickRevision(index);
                        if (shouldSkip(index, level, current, now)) continue;
                        const float dx = static_cast<float>(bx - cx);
                        const float dy = static_cast<float>(by - cy);
                        const float dz = static_cast<float>(bz - cz);
                        priority.push_back(Target{index,
                                                  dx * dx + dy * dy + dz * dz,
                                                  ring, level});
                    }

            // Tier 1: sweep the whole world from the cursor for bricks the
            // client has never been sent. The sweep is bounded per pass so a
            // large world costs a predictable amount of work per iteration, and
            // it wraps, so the fill revisits everything on its second lap.
            //
            // The fill sends the *top* level: one cell for the whole brick,
            // four bytes. That is the point of the pyramid for the background
            // -- a world of 16.7M bricks at one cell each is 67 MB, which is
            // what makes "have the whole world resident" affordable. Anything
            // finer than the top level for scenery the player cannot see is
            // bandwidth spent on detail no one will look at, and it is exactly
            // what the interest window is for.
            const std::uint32_t total = m_spec.rootCount();
            // The top of the pyramid: one cell for the whole brick. Written as
            // a pyramid level directly rather than derived from a ring, because
            // the ring mapping would read "root level" as "full octree" and
            // send 16.7M exact bricks as the background fill.
            const std::uint8_t fillLevel = brickRootLevel;
            std::uint32_t examined = 0;
            while (examined < kFillSweepPerPass && fill.size() < kMaxFillPerPass) {
                const std::uint32_t index = fillCursor;
                fillCursor = (fillCursor + 1 < total) ? fillCursor + 1 : 0;
                ++examined;
                ++fillScanned;
                if (held(index, fillLevel)) continue;
                const int bx = static_cast<int>(index % dimX);
                const int by = static_cast<int>((index / dimX) % dimY);
                const int bz = static_cast<int>(index / (dimX * dimY));
                const float dx = static_cast<float>(bx - cx);
                const float dy = static_cast<float>(by - cy);
                const float dz = static_cast<float>(bz - cz);
                const std::int32_t ring =
                    std::max({std::abs(bx - cx), std::abs(by - cy),
                              std::abs(bz - cz)});
                fill.push_back(Target{index, dx * dx + dy * dy + dz * dz, ring,
                                      fillLevel});
            }
        }
        if (priority.empty() && fill.empty()) {
            publishRate();
            if (!serviced) connection.pollReadable(20);
            continue;
        }
        // Ordered by *ring*, not by Euclidean distance.
        //
        // The level of detail is a function of the ring and is monotonic in it:
        // ring 0..2 is full resolution, and every band beyond is coarser. So
        // sorting by ring sends every brick at full resolution before any brick
        // at reduced detail, and the bands follow in order -- high quality
        // first, then progressively further away and lower quality, which is
        // what the player should be seeing arrive.
        //
        // Sorting by Euclidean distance instead does not achieve that. The
        // distance shells and the LOD rings are different shapes: a brick
        // diagonally far but on a small ring shares a band with bricks right
        // next to the player, and gets interleaved with them. The result was
        // that reduced-detail bricks were arriving while the ground under the
        // player's feet was still queued.
        //
        // Distance breaks ties, so bricks within one band still arrive nearest
        // first.
        std::sort(priority.begin(), priority.end(),
                  [](const Target& a, const Target& b) {
                      if (a.ring != b.ring) return a.ring < b.ring;
                      return a.distance < b.distance;
                  });

        int sent = 0;
        // The visible neighbourhood goes first and in full; whatever bandwidth
        // is left goes to the background fill. Concatenating the tiers rather
        // than interleaving them is what keeps a fill from delaying terrain the
        // player is standing in front of.
        std::vector<const Target*> queue;
        queue.reserve(priority.size() + fill.size());
        for (const Target& target : priority) queue.push_back(&target);
        for (const Target& target : fill) queue.push_back(&target);

        for (const Target* entry : queue) {
            const Target& target = *entry;
            if (sent >= 16) break;
            // Bandwidth: charge the frame before sending it, so a frame that
            // does not fit the remaining budget waits for the next pass rather
            // than overshooting.
            if (!pacer.canSpend(nowMs(), kFrameOverheadBytes)) break;
            voxel::SvoNode root{};
            std::vector<voxel::SvoNode> nodes;
            std::vector<BrickLevel> levels;
            std::uint32_t revision = 0;
            // The target carries the level that was chosen for it when it was
            // scheduled, which is what makes the delivery record below match
            // the payload actually sent. Deriving it here instead would let a
            // brick be booked at one level and shipped at another.
            const std::uint8_t leafLevel = target.level;
            const std::uint8_t brickRootLevel = static_cast<std::uint8_t>(
                voxel::brickLodLevel(m_spec.brick));
            if (leafLevel == 0) {
                std::scoped_lock worldLock(m_worldMutex);
                m_world->snapshot(target.rootIndex, revision, root, nodes);
                if (nodes.empty()) {
                    // A completely empty brick still needs its root entry so
                    // the client can tell "resident and empty" from "unknown".
                    root.setFlags(root.flags() | voxel::kSvoFlagResident);
                    nodes.push_back(root);
                }
            } else {
                // An averaged grid, not a reduced octree.
                //
                // This is where the pyramid pays for itself. The old reduction
                // walked the exact octree and had to preserve material identity,
                // so it refused to summarise any box a surface ran through and
                // the refusal kept the subtree. Averaging has no refusal case:
                // every box collapses to one colour, so a level-k grid is
                // exactly (brick >> k)^3 cells whatever the terrain does. Level
                // 5 of a 128-cell brick is 64 cells on the wire, where the same
                // volume as a reduced octree is routinely tens of kilobytes.
                std::scoped_lock worldLock(m_worldMutex);
                voxel::BrickPyramid pyramid{};
                m_world->snapshotPyramid(target.rootIndex, pyramid);
                revision = pyramid.revision;
                const std::vector<AvgCell>& cells = pyramid.level[leafLevel];
                if (!cells.empty()) {
                    BrickLevel level{};
                    level.level = leafLevel;
                    level.edge = 1u << (brickRootLevel - leafLevel);
                    level.cells = cells;
                    levels.push_back(std::move(level));
                } else if (pyramid.uniform) {
                    // A uniform brick holds one cell, and it is the value of
                    // every level it has. Broadcast it with the single-colour
                    // fast path rather than fanning it out to edge^3 identical
                    // bytes: level 1 of a 128-cell brick would otherwise be a
                    // megabyte of one colour, and 96% of the world is exactly
                    // this case.
                    BrickLevel level{};
                    level.level = leafLevel;
                    level.edge = 0;
                    if (!pyramid.level[brickRootLevel].empty())
                        level.cells.assign(
                            1, pyramid.level[brickRootLevel].front());
                    else
                        level.cells.assign(1, pyramid.uniformCell);
                    levels.push_back(std::move(level));
                }
                if (levels.empty()) {
                    // Nowhere to put the data. Skipping rather than sending an
                    // empty frame, which the client would reject: the resend
                    // cost is one retry, the alternative is a brick that looks
                    // corrupt forever.
                    continue;
                }
            }
            BrickSnapshot snapshot{};
            snapshot.uuid = hello.uuid;
            snapshot.playerId = hello.playerId;
            snapshot.rootIndex = target.rootIndex;
            snapshot.revision = revision;
            snapshot.worldRevision = m_world->revision();
            snapshot.lodLevel = leafLevel;
            snapshot.nodes = std::move(nodes);
            snapshot.levels = std::move(levels);
            const auto frame = encode(snapshot);
            if (frame.empty()) continue;   // payload too large to encode
            // Charged by actual size rather than an estimate, so the rate is
            // the real one on the wire and not the one it was designed for.
            if (!pacer.canSpend(nowMs(), frame.size())) continue;
            if (!connection.sendFrame(frame)) {
                connection.shutdown();
                break;
            }
            pacer.charge(nowMs(), frame.size());
            windowBytes += frame.size();
            std::scoped_lock lock(stream->mutex);
            // Recorded against the level actually sent. The client
            // acknowledges the same key, and a mismatch here would book a level
            // the client never received -- the brick would be skipped forever
            // as already-delivered while the client waits for a frame that is
            // never coming.
            stream->sent[levelKey(target.rootIndex, leafLevel)] =
                StreamClient::Delivery{revision, nowMs()};
            ++sent;
        }
    }
    connection.shutdown();
}

} // namespace voxel::net

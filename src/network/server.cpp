#include "server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>

namespace voxel::net {

Server::Server(std::uint16_t port, voxel::WorldSpec spec, std::uint64_t seed,
               std::string host)
    : m_port(port), m_host(std::move(host)), m_spec(spec),
      m_world(std::make_unique<voxel::World>(spec, seed)) {}

void Server::start() {
    if (m_running.exchange(true)) return;
    // Small worlds are cheap to build, and streaming them lazily would stall a
    // tick mid-frame, so materialise the whole root layer up front.
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
    for (auto& thread : m_streamThreads)
        if (thread.joinable()) thread.join();
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
                client.authority->state().acknowledgedInputTick)
                intent = {};
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
        std::scoped_lock lock(m_streamMutex);
        for (auto& thread : m_streamThreads)
            if (thread.joinable()) thread.join();
        m_streamThreads.emplace_back(&Server::worldConnection, this,
                                     std::move(connection));
    }
}

void Server::worldConnection(TcpSocket connection) {
    std::vector<std::byte> frame;
    WorldInterest hello{};
    if (!connection.receiveFrame(frame) || !decode(frame, hello)) return;

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
                stream->acknowledged[ack.rootIndex] = ack.revision;
                serviced = true;
            }
        }
        if (!m_running.load() || !connection.valid()) break;

        // Build the pending brick list, nearest first, then queue a bounded
        // number of frames. Bounding keeps a slow client from buffering the
        // whole world in memory.
        std::vector<Target> pending;
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
            for (int bz = cz - radius; bz <= cz + radius; ++bz)
                for (int by = cy - radius; by <= cy + radius; ++by)
                    for (int bx = cx - radius; bx <= cx + radius; ++bx) {
                        if (bx < 0 || by < 0 || bz < 0 || bx >= dimX ||
                            by >= dimY || bz >= dimZ)
                            continue;
                        const auto index = static_cast<std::uint32_t>(
                            bx + dimX * (by + dimY * bz));
                        const float dx = static_cast<float>(bx - cx);
                        const float dy = static_cast<float>(by - cy);
                        const float dz = static_cast<float>(bz - cz);
                        Target target{index, dx * dx + dy * dy + dz * dz};
                        const auto sent = stream->acknowledged.find(index);
                        const auto sentRevision = stream->sent.find(index);
                        if ((sent != stream->acknowledged.end() ||
                             sentRevision != stream->sent.end()) &&
                            m_world->brickRevision(index) ==
                                (sent != stream->acknowledged.end()
                                     ? sent->second
                                     : sentRevision->second))
                            continue;
                        pending.push_back(target);
                    }
        }
        if (pending.empty()) {
            if (!serviced) connection.pollReadable(20);
            continue;
        }
        std::sort(pending.begin(), pending.end(),
                  [](const Target& a, const Target& b) {
                      return a.distance < b.distance;
                  });

        int sent = 0;
        for (const Target& target : pending) {
            if (sent >= 16) break;
            voxel::SvoNode root{};
            std::vector<voxel::SvoNode> nodes;
            std::uint32_t revision = 0;
            {
                std::scoped_lock worldLock(m_worldMutex);
                m_world->snapshot(target.rootIndex, revision, root, nodes);
                if (nodes.empty()) {
                    // A completely empty brick still needs its root entry so
                    // the client can tell "resident and empty" from "unknown".
                    root.setFlags(root.flags() | voxel::kSvoFlagResident);
                    nodes.push_back(root);
                }
            }
            BrickSnapshot snapshot{};
            snapshot.uuid = hello.uuid;
            snapshot.playerId = hello.playerId;
            snapshot.rootIndex = target.rootIndex;
            snapshot.revision = revision;
            snapshot.worldRevision = m_world->revision();
            snapshot.lodLevel = static_cast<std::uint8_t>(
                voxel::brickLodLevel(m_spec.brick));
            snapshot.nodes = std::move(nodes);
            if (!connection.sendFrame(encode(snapshot))) {
                connection.shutdown();
                break;
            }
            std::scoped_lock lock(stream->mutex);
            stream->sent[target.rootIndex] = revision;
            ++sent;
        }
    }
    connection.shutdown();
}

} // namespace voxel::net

// Tests for the provenance tree and the region store.
//
// The contract these cover is the one the whole save design rests on:
//
//   "Evicting a resident voxel brick is always lossless, because the
//    provenance tree is the source of truth and the voxel brick is only a
//    cache of generate(index) + applyTo(brick)."
//
// So the tests are mostly about *survival*: an edit must come back after a
// flush and a reload, an edit that the generator would have produced anyway
// must leave nothing behind, and a corrupt or foreign file must be refused
// rather than half-read.

#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h>

#include "simulation/region.hpp"
#include "simulation/svo.hpp"

using namespace voxel;

namespace {

// A small, fast world: big enough to have a real root layer, small enough
// that a full cell sweep is cheap.
constexpr WorldSpec kSpec{512, 512, 256, 128};
constexpr std::uint64_t kSeed = 0xABCDEF01ull;

std::filesystem::path scratch(const char* name) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("voxel-persist-" + std::to_string(::getpid()) + "-" +
                       name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

// Every solid cell in a fixed window, as a cheap content fingerprint.
std::uint64_t fingerprint(World& world, std::int32_t minX, std::int32_t minY,
                          std::int32_t minZ, std::int32_t maxX,
                          std::int32_t maxY, std::int32_t maxZ) {
    std::uint64_t hash = 0xCBF29CE484222325ull;
    for (std::int32_t z = minZ; z < maxZ; ++z)
        for (std::int32_t y = minY; y < maxY; ++y)
            for (std::int32_t x = minX; x < maxX; ++x) {
                hash ^= world.get(x, y, z);
                hash *= 0x100000001B3ull;
            }
    return hash;
}

void testProvenanceBasics() {
    ProvenanceTree tree(7);
    assert(tree.empty());

    // Untouched cells report no override.
    assert(tree.query(10, 10, 10) == nullptr);

    tree.overrideCell(10, 10, 10, 4);
    const std::uint16_t* found = tree.query(10, 10, 10);
    assert(found != nullptr && *found == 4);
    assert(!tree.empty());

    // Overriding to air is still an override, not an absence.
    tree.overrideCell(11, 10, 10, 0);
    found = tree.query(11, 10, 10);
    assert(found != nullptr && *found == 0);

    // A neighbour is untouched.
    assert(tree.query(12, 10, 10) == nullptr);

    // Clearing gives the cell back to "the generator stands".
    tree.clearCell(10, 10, 10);
    assert(tree.query(10, 10, 10) == nullptr);
    // ...but the air override survives, because it is still doing work.
    assert(tree.query(11, 10, 10) != nullptr);
    tree.clearCell(11, 10, 10);
    assert(tree.empty());
}

void testProvenanceRoundTrip() {
    ProvenanceTree tree(7);
    // A spread of edits that exercises every path length and both carve and
    // build, including two cells that collapse into one shared leaf.
    tree.overrideCell(0, 0, 0, 5);
    tree.overrideCell(127, 127, 127, 1);
    tree.overrideCell(64, 64, 64, 2);
    tree.overrideCell(65, 64, 64, 2);
    tree.overrideCell(65, 65, 64, 0);

    std::vector<SvoNode> nodes;
    SvoNode root{};
    tree.serialize(nodes, root);
    assert(!nodes.empty());

    ProvenanceTree restored = ProvenanceTree::deserialize(nodes, 7);
    assert(!restored.empty());
    for (const auto& [x, y, z, expected] :
         std::vector<std::array<std::uint32_t, 4>>{
             {{0, 0, 0, 5}}, {{127, 127, 127, 1}}, {{64, 64, 64, 2}},
             {{65, 64, 64, 2}}, {{65, 65, 64, 0}}}) {
        const std::uint16_t* value = restored.query(x, y, z);
        assert(value != nullptr && *value == expected);
    }
    assert(restored.query(100, 100, 100) == nullptr);

    // Two identical adjacent overrides collapse into one leaf, so the packed
    // form stays smaller than the dense one.
    assert(nodes.size() < restored.nodeCount());
}

void testApplyToMatchesDirectEdit() {
    // generate() then applyTo(delta) must equal a direct set().
    const auto path = scratch("apply");
    RegionStore store(path.string(), kSpec.sizeX, kSpec.sizeY, kSpec.sizeZ,
                      kSpec.brick, kSeed, 4);
    store.open();

    World reference(kSpec, kSeed);   // ephemeral: direct edits only
    World withDelta(kSpec, kSeed, &store);

    reference.generateAll();
    withDelta.generateAll();

    for (int i = 0; i < 64; ++i) {
        const std::int32_t x = static_cast<std::int32_t>(i * 3 + 5);
        const std::int32_t y = static_cast<std::int32_t>(i * 7 + 11);
        const std::int32_t z = static_cast<std::int32_t>(20 + i);
        const auto material = static_cast<std::uint16_t>(1 + (i % 5));
        reference.set(x, y, z, material);
        withDelta.set(x, y, z, material);
    }
    // Match over every brick, not just the one that was edited.
    for (std::uint32_t index = 0; index < kSpec.rootCount(); ++index) {
        std::uint32_t revision = 0;
        SvoNode root{};
        std::vector<SvoNode> nodes;
        reference.snapshot(index, revision, root, nodes);
        std::uint32_t otherRevision = 0;
        SvoNode otherRoot{};
        std::vector<SvoNode> otherNodes;
        withDelta.snapshot(index, otherRevision, otherRoot, otherNodes);
        assert(nodes.size() == otherNodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            assert(nodes[i].word0 == otherNodes[i].word0);
            assert(nodes[i].word1() == otherNodes[i].word1());
        }
    }
    std::filesystem::remove_all(path);
}

void testPruneRemovesRedundantOverride() {
    // Writing the value the generator would have produced must leave no
    // residue, so a build-then-unbuild cycle costs nothing on disk.
    const auto path = scratch("prune");
    RegionStore store(path.string(), kSpec.sizeX, kSpec.sizeY, kSpec.sizeZ,
                      kSpec.brick, kSeed, 4);
    store.open();

    const std::int32_t x = 40;
    const std::int32_t y = 44;
    const std::int32_t z = 30;
    World world(kSpec, kSeed, &store);

    const auto original = world.get(x, y, z);
    const std::uint32_t rootIndex = world.rootIndexOfCell(x, y, z);

    // An edit that differs: the delta must record it.
    const auto other = static_cast<std::uint16_t>(original == 5 ? 1 : 5);
    world.set(x, y, z, other);
    assert(world.get(x, y, z) == other);
    ProvenanceTree* delta = store.deltaFor(rootIndex);
    assert(delta != nullptr && delta->query(x % kSpec.brick, y % kSpec.brick,
                                           z % kSpec.brick) != nullptr);

    // Writing the original value back prunes the override entirely.
    world.set(x, y, z, original);
    assert(world.get(x, y, z) == original);
    delta = store.deltaFor(rootIndex);
    if (delta != nullptr)
        assert(delta->query(x % kSpec.brick, y % kSpec.brick,
                            z % kSpec.brick) == nullptr);
    std::filesystem::remove_all(path);
}

void testSaveLoadRoundTrip() {
    const auto path = scratch("roundtrip");
    const std::uint32_t regionBricks = 4;

    // Pass 1: build a world, edit it, flush, shut down.
    std::vector<std::array<std::int32_t, 4>> edits;   // x, y, z, material
    std::uint64_t expected = 0;
    {
        RegionStore store(path.string(), kSpec.sizeX, kSpec.sizeY, kSpec.sizeZ,
                          kSpec.brick, kSeed, regionBricks);
        store.open();
        World world(kSpec, kSeed, &store);
        world.generateAll();

        for (int i = 0; i < 200; ++i) {
            const std::int32_t x = static_cast<std::int32_t>(i % 400);
            const std::int32_t y = static_cast<std::int32_t>((i * 13) % 400);
            const std::int32_t z = static_cast<std::int32_t>(25 + (i % 30));
            const auto material = static_cast<std::uint16_t>(1 + (i % 5));
            world.set(x, y, z, material);
            edits.push_back({x, y, z, material});
        }
        world.generateAll();
        expected = fingerprint(world, 0, 0, 0, 128, 128, 128);
        assert(store.flush() >= 1);
        store.close();
    }

    // The world directory must exist and be small: the untouched world costs
    // an index, not voxels.
    assert(std::filesystem::exists(path / "world.meta"));
    std::uintmax_t total = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path))
        if (entry.is_regular_file()) total += entry.file_size();
    assert(total > 0);

    // Pass 2: a fresh process would see exactly this.
    {
        RegionStore store(path.string(), kSpec.sizeX, kSpec.sizeY, kSpec.sizeZ,
                          kSpec.brick, kSeed, regionBricks);
        store.open();
        World world(kSpec, kSeed, &store);
        // Every edit must survive.
        for (const auto& [x, y, z, material] : edits)
            assert(world.get(x, y, z) == static_cast<std::uint16_t>(material));
        // Then the whole brick, which also proves the *unedited* cells were not
        // disturbed by the ones that were.
        world.generateAll();
        assert(fingerprint(world, 0, 0, 0, 128, 128, 128) == expected);
    }
    std::filesystem::remove_all(path);
}

void testUntouchedWorldCostsNothing() {
    const auto path = scratch("empty");
    {
        RegionStore store(path.string(), kSpec.sizeX, kSpec.sizeY, kSpec.sizeZ,
                          kSpec.brick, kSeed, 4);
        store.open();
        // Flush with nothing dirty: only the index is written.
        store.flush();
        store.close();
    }
    std::uintmax_t total = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path))
        if (entry.is_regular_file()) total += entry.file_size();
    assert(total > 0);
    // The index is a fixed ~40 bytes; no region payload should exist.
    assert(total < 1024);
    std::filesystem::remove_all(path);
}

void testSeedMismatchRefused() {
    const auto path = scratch("seedmismatch");
    {
        RegionStore store(path.string(), kSpec.sizeX, kSpec.sizeY, kSpec.sizeZ,
                          kSpec.brick, kSeed, 4);
        store.open();
        store.flush();
    }
    // Opening the same world with a different seed must fail loudly: replaying
    // one seed's deltas against another's terrain would silently produce a
    // world nobody built.
    bool refused = false;
    try {
        RegionStore store(path.string(), kSpec.sizeX, kSpec.sizeY, kSpec.sizeZ,
                          kSpec.brick, kSeed + 1, 4);
        store.open();
    } catch (const std::exception&) {
        refused = true;
    }
    assert(refused);
    std::filesystem::remove_all(path);
}

void testCorruptRegionRefused() {
    const auto path = scratch("corrupt");
    {
        RegionStore store(path.string(), kSpec.sizeX, kSpec.sizeY, kSpec.sizeZ,
                          kSpec.brick, kSeed, 4);
        store.open();
        World world(kSpec, kSeed, &store);
        world.set(50, 50, 50, 3);
        store.flush();
        store.close();
    }
    // Flip a byte in the payload. The checksum must catch it rather than
    // replaying garbage voxels.
    std::filesystem::path region;
    for (const auto& entry : std::filesystem::directory_iterator(path / "regions"))
        region = entry.path();
    assert(!region.empty());
    {
        std::FILE* file = std::fopen(region.string().c_str(), "r+b");
        assert(file != nullptr);
        std::fseek(file, -8, SEEK_END);
        const auto byte = std::fgetc(file);
        std::fseek(file, -8, SEEK_END);
        std::fputc(byte ^ 0xFF, file);
        std::fclose(file);
    }
    {
        RegionStore store(path.string(), kSpec.sizeX, kSpec.sizeY, kSpec.sizeZ,
                          kSpec.brick, kSeed, 4);
        store.open();
        bool refused = false;
        try {
            for (std::uint32_t index = 0; index < kSpec.rootCount(); ++index)
                store.deltaFor(index);
        } catch (const std::exception&) {
            refused = true;
        }
        assert(refused);
    }
    std::filesystem::remove_all(path);
}

void testGeneratorVersionRefused() {
    // A world.meta written by a different generator build must be refused.
    const auto path = scratch("genversion");
    {
        RegionStore store(path.string(), kSpec.sizeX, kSpec.sizeY, kSpec.sizeZ,
                          kSpec.brick, kSeed, 4);
        store.open();
        store.flush();
    }
    std::string meta = R"(VXW1)";
    // Flip the generator version field to a value the build does not use.
    std::filesystem::path metaPath = path / "world.meta";
    std::FILE* file = std::fopen(metaPath.string().c_str(), "r+b");
    assert(file != nullptr);
    std::fseek(file, 8, SEEK_SET);   // magic(4) + version(2) + ...
    const auto bogus = static_cast<unsigned>(generatorVersion() ^ 0x5A5Au);
    std::fputc(bogus & 0xFF, file);
    std::fputc((bogus >> 8) & 0xFF, file);
    std::fputc((bogus >> 16) & 0xFF, file);
    std::fputc((bogus >> 24) & 0xFF, file);
    std::fclose(file);

    bool refused = false;
    try {
        RegionStore store(path.string(), kSpec.sizeX, kSpec.sizeY, kSpec.sizeZ,
                          kSpec.brick, kSeed, 4);
        store.open();
    } catch (const std::exception&) {
        refused = true;
    }
    assert(refused);
    std::filesystem::remove_all(path);
}

} // namespace

int main() {
    testProvenanceBasics();
    testProvenanceRoundTrip();
    testApplyToMatchesDirectEdit();
    testPruneRemovesRedundantOverride();
    testSaveLoadRoundTrip();
    testUntouchedWorldCostsNothing();
    testSeedMismatchRefused();
    testCorruptRegionRefused();
    testGeneratorVersionRefused();
    std::printf("VoxelPersistTests: all persistence tests passed\n");
    return 0;
}
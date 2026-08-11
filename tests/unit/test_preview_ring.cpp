// SPEC.md §15.2's shared-memory ring, on the CPU tier.
//
// The ring is a memory-ordering argument, and a memory-ordering argument that can only be
// exercised through a GPU and two processes is one that never gets exercised. So the whole
// protocol -- the header, the slot rotation, the publish, and the lap detection that
// decides whether a reader was too slow -- is driven here against a real named section,
// with no D3D, no thread and no pipe.
//
// The one thing this file deliberately does *not* claim is that the preview works. A ring
// test that drove the ring directly and then reported "preview verified" would be the
// shape docs/ACCEPTANCE.md warns about twice over. `PreviewTest` (gpu) is where the
// pipeline is asserted; this is where the transport is.

#include "core/preview/preview_ring.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

using fc::preview::PreviewGeometry;
using fc::preview::PreviewReader;
using fc::preview::PreviewRing;

/// Small, so a test can fill a slot without a two-megabyte memset per case. The protocol
/// does not care about the size; every dimension-dependent number is read from the header.
[[nodiscard]] PreviewGeometry tiny() {
    PreviewGeometry geometry;
    geometry.width = 8;
    geometry.height = 4;
    geometry.fps = 30;
    return geometry;
}

/// A distinct name per case. Sections are process-wide and the CPU tier runs cases in one
/// process, so a shared name would let one case observe another's leftovers.
[[nodiscard]] std::string unique_name(const char* suffix) {
    return fc::preview::preview_section_name(std::string{"unittest-"} + suffix);
}

void fill(std::uint8_t* slot, const PreviewGeometry& geometry, std::uint8_t value) {
    std::memset(slot, value, geometry.slot_bytes());
}

[[nodiscard]] bool all_equal(const std::uint8_t* pixels, const PreviewGeometry& geometry, std::uint8_t value) {
    for (std::size_t i = 0; i < geometry.slot_bytes(); ++i) {
        if (pixels[i] != value) {
            return false;
        }
    }
    return true;
}

TEST(PreviewRing, ASectionThatWasNeverCreatedIsNotMistakenForAnEmptyPreview) {
    // The case this magic exists for. Python's `mmap(tagname=...)` *creates* a section
    // that does not exist rather than failing, so a GUI attaching to an engine that never
    // armed the preview gets a page of zeros -- and a page of zeros must not read as a
    // valid all-black frame with sequence 0.
    PreviewReader reader;
    const auto opened = reader.open(unique_name("absent"));
    EXPECT_FALSE(opened.has_value());
}

TEST(PreviewRing, TheHeaderDescribesTheGeometryTheWriterWasGiven) {
    PreviewRing ring;
    ASSERT_TRUE(ring.create(unique_name("header"), tiny()).has_value());

    PreviewReader reader;
    ASSERT_TRUE(reader.open(unique_name("header")).has_value());

    // Read back through the header rather than from the same `PreviewGeometry` object,
    // because the header is the only thing the GUI has: a reader that inferred the layout
    // from constants of its own would agree with this one until the day it did not.
    EXPECT_EQ(reader.geometry().width, 8);
    EXPECT_EQ(reader.geometry().height, 4);
    EXPECT_EQ(reader.geometry().fps, 30);
    EXPECT_EQ(reader.geometry().slots, fc::preview::kPreviewSlots);
    EXPECT_EQ(reader.geometry().stride(), 32U);
    EXPECT_EQ(reader.geometry().slot_bytes(), 128U);
    EXPECT_EQ(reader.geometry().total_bytes(), fc::preview::kPreviewHeaderBytes + (std::size_t{128} * 3U));
}

TEST(PreviewRing, NothingIsReadableBeforeTheFirstPublish) {
    PreviewRing ring;
    ASSERT_TRUE(ring.create(unique_name("empty"), tiny()).has_value());

    PreviewReader reader;
    ASSERT_TRUE(reader.open(unique_name("empty")).has_value());

    EXPECT_EQ(reader.write_index(), 0U);
    EXPECT_FALSE(reader.latest().has_value()) << "a reader saw a frame before one was published";
}

TEST(PreviewRing, ThePublishedFrameIsTheOneTheReaderSees) {
    const PreviewGeometry geometry = tiny();
    PreviewRing ring;
    ASSERT_TRUE(ring.create(unique_name("publish"), geometry).has_value());

    PreviewReader reader;
    ASSERT_TRUE(reader.open(unique_name("publish")).has_value());

    fill(ring.next_slot(), geometry, 0x5A);
    ring.publish(123'456'789);

    const auto view = reader.latest();
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view.value().sequence, 1U);
    EXPECT_EQ(view.value().qpc_ns, 123'456'789);
    EXPECT_TRUE(all_equal(view.value().pixels, geometry, 0x5A));
}

TEST(PreviewRing, TheRingCyclesThroughItsSlotsRatherThanRewritingOne) {
    // §15.2 says "triple-buffered", and the reason is that a reader must be able to hold a
    // frame while the writer produces the next. A ring that rewrote one buffer would pass
    // every read-immediately test in this file and tear under a reader that took its time.
    const PreviewGeometry geometry = tiny();
    PreviewRing ring;
    ASSERT_TRUE(ring.create(unique_name("cycle"), geometry).has_value());

    std::vector<const std::uint8_t*> addresses;
    for (int i = 0; i < geometry.slots; ++i) {
        std::uint8_t* slot = ring.next_slot();
        addresses.push_back(slot);
        fill(slot, geometry, static_cast<std::uint8_t>(0x10 + i));
        ring.publish(i);
    }
    EXPECT_NE(addresses[0], addresses[1]);
    EXPECT_NE(addresses[1], addresses[2]);
    EXPECT_NE(addresses[0], addresses[2]);

    // And it wraps: the fourth frame lands back on the first slot.
    EXPECT_EQ(ring.next_slot(), addresses[0]);
}

TEST(PreviewRing, AReaderIsToldTheMomentTheWriterLapsOntoTheFrameItIsHolding) {
    // The assertion that catches the off-by-one. With three slots the intuitive answer is
    // "two frames of slack"; the correct one is one, because at a distance of two the
    // writer's *next* fill is already the slot the reader is looking at. Getting this wrong
    // reports a torn frame as intact, which is a defect no assertion downstream would
    // attribute to the ring.
    const PreviewGeometry geometry = tiny();
    PreviewRing ring;
    ASSERT_TRUE(ring.create(unique_name("lap"), geometry).has_value());

    PreviewReader reader;
    ASSERT_TRUE(reader.open(unique_name("lap")).has_value());

    fill(ring.next_slot(), geometry, 0x01);
    ring.publish(1);

    const auto held = reader.latest();
    ASSERT_TRUE(held.has_value());
    const std::uint64_t sequence = held.value().sequence;
    EXPECT_TRUE(reader.still_valid(sequence)) << "the frame just published was reported stale";

    // One more publication: the writer is on a different slot, so the held frame is intact.
    fill(ring.next_slot(), geometry, 0x02);
    ring.publish(2);
    EXPECT_TRUE(reader.still_valid(sequence)) << "one frame of slack is what three buffers buy";

    // A second: the writer's next fill is the held slot, so it is no longer safe to read.
    fill(ring.next_slot(), geometry, 0x03);
    ring.publish(3);
    EXPECT_FALSE(reader.still_valid(sequence)) << "the writer is about to overwrite this slot";

    // And the reader is not stuck -- re-reading gives it the current frame.
    const auto fresh = reader.latest();
    ASSERT_TRUE(fresh.has_value());
    EXPECT_EQ(fresh.value().sequence, 3U);
    EXPECT_TRUE(all_equal(fresh.value().pixels, geometry, 0x03));
    EXPECT_TRUE(reader.still_valid(fresh.value().sequence));
}

TEST(PreviewRing, DroppedPreviewFramesAreCountedWhereAReaderCanSeeThem) {
    // §15.2's droppability is only honest if the drops are visible. A preview that silently
    // shows every third frame is indistinguishable from a screen that is not changing.
    PreviewRing ring;
    ASSERT_TRUE(ring.create(unique_name("dropped"), tiny()).has_value());

    PreviewReader reader;
    ASSERT_TRUE(reader.open(unique_name("dropped")).has_value());

    EXPECT_EQ(reader.dropped(), 0U);
    ring.note_dropped();
    ring.note_dropped(4);
    EXPECT_EQ(reader.dropped(), 5U);
    EXPECT_EQ(ring.stats().dropped, 5U);
}

TEST(PreviewRing, AClosedRingCannotBeReopenedByAReaderAsIfItWereLive) {
    // What a GUI sees after the engine stops previewing. The magic is cleared before the
    // section is unmapped, so a reader attaching in the window between those two finds a
    // section it refuses rather than a stale frame it has no way to date.
    {
        PreviewRing ring;
        ASSERT_TRUE(ring.create(unique_name("closed"), tiny()).has_value());
        fill(ring.next_slot(), tiny(), 0x7F);
        ring.publish(1);
        ring.close();
        EXPECT_FALSE(ring.open());
    }

    PreviewReader reader;
    EXPECT_FALSE(reader.open(unique_name("closed")).has_value());
}

TEST(PreviewRing, TheSectionNameCarriesTheSessionSoTwoEnginesCannotCollide) {
    // SPEC.md §15.1 names the pipe per session GUID; the section follows it, in the
    // `Local\` namespace so two logged-in users get two distinct objects from one name.
    const std::string first = fc::preview::preview_section_name("aaaa");
    const std::string second = fc::preview::preview_section_name("bbbb");
    EXPECT_NE(first, second);
    EXPECT_EQ(first.rfind("Local\\", 0), 0U) << first;
    EXPECT_NE(first.find("aaaa"), std::string::npos) << first;

    // An engine started by hand has no session id and still needs a section.
    EXPECT_FALSE(fc::preview::preview_section_name("").empty());
}

TEST(PreviewRing, TheGeometryIsRefusedRatherThanTruncated) {
    PreviewRing ring;
    PreviewGeometry bad = tiny();
    bad.width = 0;
    EXPECT_FALSE(ring.create(unique_name("bad"), bad).has_value());
    EXPECT_FALSE(ring.open());

    bad = tiny();
    bad.slots = 1; // not a ring at all
    EXPECT_FALSE(ring.create(unique_name("bad2"), bad).has_value());
}

TEST(PreviewRing, TheDefaultGeometryIsTheOneSpecifiedInFifteenTwo) {
    // "default 960x540, 30 fps, BGRA". Asserted rather than assumed, because these are the
    // numbers the GUI's `QImage` is sized from and nothing else would catch a typo in them.
    const PreviewGeometry geometry;
    EXPECT_EQ(geometry.width, 960);
    EXPECT_EQ(geometry.height, 540);
    EXPECT_EQ(geometry.fps, 30);
    EXPECT_EQ(geometry.slots, 3);
    EXPECT_EQ(geometry.stride(), 960U * 4U);
    EXPECT_EQ(geometry.slot_bytes(), 960U * 540U * 4U);

    // Never full-resolution: the whole ring is smaller than one 1080p BGRA frame.
    EXPECT_LT(geometry.total_bytes(), static_cast<std::size_t>(1920) * 1080 * 4)
        << "§15.2: the engine writes a downscaled preview, never full-resolution frames";
}

} // namespace

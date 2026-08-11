#pragma once

// SPEC.md §15.2's transport: the named shared-memory ring the preview travels through.
//
//   > Named **shared memory** (`CreateFileMapping`) triple-buffered ring, one `HANDLE`-passed
//   > section, with an atomic write-index header.
//
// Nothing in this file touches D3D, a thread or a pipe. That is deliberate: the ring's
// correctness is a memory-ordering argument, and a memory-ordering argument that can only
// be exercised through a GPU and two processes is one that never gets exercised. The whole
// protocol -- publish, observe, lap-detection -- is asserted on the CPU tier by
// `PreviewRing.*` driving a `PreviewReader` over the same section.
//
// ---------------------------------------------------------------------------
// "one HANDLE-passed section", and what is actually built
// ---------------------------------------------------------------------------
// §15.2 opens with "**Named** shared memory (`CreateFileMapping`)" and then says the
// section is "HANDLE-passed". Those are two different mechanisms and only one of them can
// be the transport: a section that is passed by handle does not need a name, and a section
// that is opened by name does not need its handle duplicated into the peer.
//
// **The name is the transport here.** `start_preview` answers with the section name and the
// geometry, and the GUI calls `OpenFileMapping`/`mmap(tagname=...)` on it. The reasons, in
// order:
//
//   * the first clause of the same sentence specifies it, and it is the clause that names
//     the API;
//   * duplicating a handle into the peer requires the engine to hold a `PROCESS_DUP_HANDLE`
//     right on the GUI, which inverts SPEC.md §3.1's ownership -- the GUI is the parent;
//   * `mmap(tagname=...)` is the only form Python has without a `ctypes` shim around
//     `OpenFileMapping` + `MapViewOfFile` + `DuplicateHandle`, and SPEC.md §16.1's "the GUI
//     wraps a QImage over the shared-memory buffer and nothing else" is a budget as much as
//     a constraint.
//
// The section lives in the `Local\` namespace, which is per-logon-session, and carries the
// session GUID §15.1 already uses for the pipe -- so two engines cannot collide and another
// user's session cannot see it. **This is not the pipe's SID-restricted ACL**: the section
// gets the default DACL from the creator's token, which grants the creating user and denies
// everyone else without an explicit grant. Recorded rather than glossed, because §15.1
// states an ACL requirement for the *control* channel and §15.2 states none for this one.
//
// ---------------------------------------------------------------------------
// The ring protocol
// ---------------------------------------------------------------------------
// One writer, any number of readers, no locks, and the reader never blocks the writer --
// which is the mechanical form of §15.2's "the preview path must be independently
// droppable: if the GUI is slow, preview frames are skipped with **zero effect on the
// recording**". The writer's cost does not depend on the reader existing, being attached,
// or keeping up.
//
//   writer   fill slot `write_index % slots`, then store `write_index + 1` with release.
//   reader   load `write_index` with acquire; read slot `(write_index - 1) % slots`; load
//            `write_index` again and check the writer has not lapped onto that slot.
//
// With three slots the writer must publish three more frames before it returns to the slot
// a reader is looking at, so a reader that finishes within two publications is reading
// memory nobody is writing. `PreviewReader::still_valid` is that check, and it is a check
// rather than a lock because a lock would let a stalled GUI stall the engine -- the one
// thing §15.2 forbids by name.

#include "core/error/result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace fc::preview {

/// SPEC.md §15.2: "default 960x540, 30 fps, BGRA".
///
/// Constants rather than configuration keys. §16.4 enumerates the settings dialog's
/// sections and none of them is a preview, so a key here would be one no UI can reach --
/// and CLAUDE.md §7 is explicit that half-built structure is worse than none.
inline constexpr int kPreviewWidth = 960;
inline constexpr int kPreviewHeight = 540;
inline constexpr int kPreviewFps = 30;

/// §15.2's "triple-buffered".
inline constexpr int kPreviewSlots = 3;

/// 'FCPV' little-endian. The first thing a reader checks, because
/// `mmap(tagname=...)` *creates* a section that does not exist rather than failing --
/// so a GUI attaching to a dead engine gets a page of zeros, and zeros must not read as a
/// valid black frame.
inline constexpr std::uint32_t kPreviewMagic = 0x5650'4346U;

/// Bumped only when the layout below changes incompatibly. The reader refuses a version it
/// does not know rather than misreading it.
inline constexpr std::uint32_t kPreviewVersion = 1;

/// Fixed, so a reader can compute a slot address from the header alone.
///
/// 128 rather than the 64 the fields need: the atomic write index sits at offset 40 and the
/// pixels start here, and one cache line of separation keeps a reader polling the index off
/// the line the writer is filling.
inline constexpr std::size_t kPreviewHeaderBytes = 128;

/// Byte offsets into the header. Public because the GUI's reader unpacks the same layout
/// and a second copy of these numbers is a second thing to get wrong (BUG-043's shape).
inline constexpr std::size_t kOffsetMagic = 0;
inline constexpr std::size_t kOffsetVersion = 4;
inline constexpr std::size_t kOffsetWidth = 8;
inline constexpr std::size_t kOffsetHeight = 12;
inline constexpr std::size_t kOffsetStride = 16;
inline constexpr std::size_t kOffsetSlotBytes = 20;
inline constexpr std::size_t kOffsetSlotCount = 24;
inline constexpr std::size_t kOffsetFormat = 28;
inline constexpr std::size_t kOffsetFps = 32;
inline constexpr std::size_t kOffsetWriteIndex = 40;
inline constexpr std::size_t kOffsetDropped = 48;
inline constexpr std::size_t kOffsetLastQpcNs = 56;

/// The only pixel format §15.2 allows. B, G, R, A in memory order.
inline constexpr std::uint32_t kPreviewFormatBgra8 = 0;

struct PreviewGeometry {
    int width = kPreviewWidth;
    int height = kPreviewHeight;
    int fps = kPreviewFps;
    int slots = kPreviewSlots;

    [[nodiscard]] std::size_t stride() const noexcept {
        return static_cast<std::size_t>(width) * 4U;
    }

    [[nodiscard]] std::size_t slot_bytes() const noexcept {
        return stride() * static_cast<std::size_t>(height);
    }

    [[nodiscard]] std::size_t total_bytes() const noexcept {
        return kPreviewHeaderBytes + (slot_bytes() * static_cast<std::size_t>(slots));
    }

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 && fps > 0 && slots >= 2;
    }
};

/// `Local\framecapture-preview-<session>`, matching the pipe's session GUID (§15.1).
[[nodiscard]] std::string preview_section_name(std::string_view session_id);

/// Counters for `get_stats` and for the tests. Not in shared memory except `published`
/// and `dropped`, which a reader legitimately wants.
struct PreviewRingStats {
    std::uint64_t published = 0;
    std::uint64_t dropped = 0;
};

/// The writer half. One per engine; owns the section.
///
/// Not thread-safe, and does not need to be: exactly one thread fills and publishes
/// (SPEC.md §12's rule that every queue has one owner). `note_dropped` is the one method a
/// second thread calls, and it is a relaxed increment on a counter nothing decides
/// anything from.
class PreviewRing {
public:
    PreviewRing();
    ~PreviewRing();

    PreviewRing(const PreviewRing&) = delete;
    PreviewRing& operator=(const PreviewRing&) = delete;
    PreviewRing(PreviewRing&&) = delete;
    PreviewRing& operator=(PreviewRing&&) = delete;

    /// Creates the section and stamps the header. Fails with `IPC_SHM_CREATE_FAILED` or
    /// `IPC_SHM_MAP_FAILED`; both are documented in ERROR_CODES.md as "degrade the preview,
    /// never the recording", which is what every caller does with them.
    [[nodiscard]] Result<void> create(const std::string& name, const PreviewGeometry& geometry);

    void close();

    [[nodiscard]] bool open() const noexcept;

    /// Where the *next* frame goes. Null when the ring was never created.
    ///
    /// Valid until `publish`. Handing out the pointer rather than taking a buffer is what
    /// lets the caller `memcpy` straight from a mapped staging texture into shared memory
    /// with no intermediate copy -- the readback is already one copy and a second would
    /// double the only per-frame CPU cost this path has.
    [[nodiscard]] std::uint8_t* next_slot() noexcept;

    /// Makes the slot filled by `next_slot` the one readers see. Release-ordered.
    void publish(std::int64_t qpc_ns) noexcept;

    /// A preview frame that was never produced -- rate-limited away, no staging slot free,
    /// a readback still in flight. Surfaced so "the preview is stuttering" is a number.
    void note_dropped(std::uint64_t count = 1) noexcept;

    [[nodiscard]] PreviewRingStats stats() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] const PreviewGeometry& geometry() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// The reader half, for the C++ tests and for anything in-process that wants to look.
///
/// The GUI does not use this -- it is a separate process and reads the same layout from
/// Python. Both are held to the same header offsets above, and
/// `test_preview_reader.py::test_the_header_layout_matches_the_engines` asserts they agree.
class PreviewReader {
public:
    PreviewReader();
    ~PreviewReader();

    PreviewReader(const PreviewReader&) = delete;
    PreviewReader& operator=(const PreviewReader&) = delete;
    PreviewReader(PreviewReader&&) = delete;
    PreviewReader& operator=(PreviewReader&&) = delete;

    /// Opens an existing section. `IPC_SHM_MAP_FAILED` when there is none, and
    /// `IPC_MESSAGE_MALFORMED` when the magic or version does not match -- which is what a
    /// section belonging to a different build, or a freshly created empty one, looks like.
    [[nodiscard]] Result<void> open(const std::string& name);

    void close();

    struct View {
        /// Points into shared memory. Valid while `still_valid(sequence)` holds.
        const std::uint8_t* pixels = nullptr;
        std::uint64_t sequence = 0;
        std::int64_t qpc_ns = 0;
    };

    /// The most recently published frame, or nothing when none has been.
    [[nodiscard]] Result<View> latest() const;

    /// Whether the writer has lapped onto the slot `sequence` pointed at.
    ///
    /// **The reason this returns rather than blocks.** A reader that took a lock would let
    /// a stalled GUI stall the engine, which §15.2 forbids in the same sentence it asks for
    /// droppability. So a reader that was too slow is told it read a torn frame and drops
    /// it, and the writer never learns the reader exists.
    [[nodiscard]] bool still_valid(std::uint64_t sequence) const noexcept;

    [[nodiscard]] std::uint64_t write_index() const noexcept;
    [[nodiscard]] std::uint64_t dropped() const noexcept;
    [[nodiscard]] const PreviewGeometry& geometry() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::preview

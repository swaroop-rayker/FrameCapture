#pragma once

// SPEC.md §15.2's preview stage, wired to a capture session.
//
// One object joining the three pieces §15.2 asks for: the dispatch (`PreviewScaler`), the
// readback, and the ring (`PreviewRing`). It is the only place that knows the preview
// exists at all -- `RecordingSession` calls `offer` and nothing else.
//
// ---------------------------------------------------------------------------
// Why there is a thread here, and what SPEC.md §12 says about it
// ---------------------------------------------------------------------------
// §12's thread table has no `preview` row, so this adds one: `fc-preview`, BELOW_NORMAL,
// blocking allowed. Flagged rather than assumed, per CLAUDE.md -- SPEC.md wins, so a table
// this build extends is a divergence to record and not a reinterpretation.
//
// The division of labour across it is the whole argument for the thread existing:
//
//   capture thread   dispatch the shader off the live source texture, copy the 960x540
//                    result to a staging surface, hand over a slot index. All GPU command
//                    recording -- tens of microseconds, no map, no wait, no allocation.
//   fc-preview       `Map` the staging surface, `memcpy` two megabytes into shared memory,
//                    publish. The part with a CPU cost proportional to the picture.
//
// Doing the readback on the capture thread instead was measured and rejected: the `memcpy`
// alone is a few hundred microseconds, and CLAUDE.md hard rule 4's "never block the capture
// thread" exists precisely so that per-frame work does not accumulate there. The staging
// `Map` uses `D3D11_MAP_FLAG_DO_NOT_WAIT`, so even on `fc-preview` a readback that is not
// ready yet costs a retry rather than a GPU stall.
//
// ---------------------------------------------------------------------------
// "zero effect on the recording", stated as a mechanism
// ---------------------------------------------------------------------------
// §15.2: "The preview path must be independently droppable: if the GUI is slow, preview
// frames are skipped with **zero effect on the recording**." Three independent reasons that
// holds here, none of which is "we were careful":
//
//   1. **The ring never waits for a reader.** `PreviewRing::publish` is one release store.
//      A GUI that stops reading, or that never attached, changes nothing about what this
//      code does -- there is no acknowledgement, no back-channel and no reference count.
//   2. **The hand-off is a single slot with a drop-oldest policy.** A `fc-preview` thread
//      that falls behind loses preview frames; it cannot apply back-pressure, because the
//      capture thread's only interaction with it is "take a free slot or give up".
//   3. **Every one of those give-ups is counted**, so a degraded preview is a number in
//      `get_stats` rather than a silence.
//
// `injected_publish_stall_ns` exists to make claim 2 testable rather than argued -- see
// its note below.

#include "core/error/result.h"
#include "core/preview/preview_ring.h"

#include <cstdint>
#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace fc::preview {

struct PreviewWriterSettings {
    /// SPEC.md §15.2's 30 fps. A 60 fps capture publishes every other frame; the decimation
    /// is on the *source* timestamp, so an irregular source still produces an even cadence.
    int fps = kPreviewFps;

    bool tone_map_hdr = true;
    double sdr_white_nits = 203.0;

    /// Chaos injection: `fc-preview` sleeps this long before publishing each frame.
    ///
    /// Zero on every production path. It exists because §15.2's "zero effect on the
    /// recording" is a claim about what happens when the preview cannot keep up, and a test
    /// that only ever runs a preview that *does* keep up proves nothing about it -- the
    /// same shape as BUG-038's supplied clock with no drift loop, and BUG-042's timestamps
    /// on an exact grid. Set to a frame period or more and the single hand-off slot
    /// saturates, which is the state the recording has to be measured in.
    std::int64_t injected_publish_stall_ns = 0;
};

struct PreviewWriterStats {
    /// Frames the capture thread showed the preview, whatever became of them.
    std::uint64_t offered = 0;
    /// Skipped to hold the configured cadence. Expected to be ~half of `offered` at 60 fps
    /// capture and 30 fps preview -- this is the rate limiter working, not a fault.
    std::uint64_t rate_limited = 0;
    std::uint64_t dispatched = 0;
    std::uint64_t published = 0;

    /// No staging slot was free: `fc-preview` had not finished with any of them. The
    /// primary evidence that the preview degraded rather than the recording.
    std::uint64_t dropped_no_slot = 0;
    /// A queued frame was displaced by a newer one before `fc-preview` took it.
    std::uint64_t dropped_stale = 0;
    /// The GPU had not finished the copy within the readback's retry budget.
    std::uint64_t dropped_readback_busy = 0;
    /// A dispatch, a map or a device call failed. Non-zero means look at the log.
    std::uint64_t failures = 0;

    /// What `offer` costs the **capture thread**, in nanoseconds.
    ///
    /// The number that decides whether §15.2's "zero effect on the recording" is true in
    /// practice rather than only in structure: the capture thread is the one place a
    /// per-frame cost turns into dropped source frames.
    std::int64_t worst_offer_ns = 0;
    std::int64_t total_offer_ns = 0;
    std::uint64_t offer_samples = 0;

    [[nodiscard]] std::int64_t mean_offer_ns() const noexcept {
        return offer_samples == 0 ? 0 : total_offer_ns / static_cast<std::int64_t>(offer_samples);
    }
};

/// Turns captured frames into published preview frames.
///
/// Lifetime: `start`, any number of `offer` calls from one thread, `stop`. The ring is
/// borrowed and outlives this -- a device rebuild (SPEC.md §5.4) destroys the writer and
/// builds another on the new device, and the GUI's mapping must survive that untouched.
class PreviewWriter {
public:
    PreviewWriter();
    ~PreviewWriter();

    PreviewWriter(const PreviewWriter&) = delete;
    PreviewWriter& operator=(const PreviewWriter&) = delete;
    PreviewWriter(PreviewWriter&&) = delete;
    PreviewWriter& operator=(PreviewWriter&&) = delete;

    /// `ring` must already be created and must outlive this object.
    [[nodiscard]] Result<void> start(ID3D11Device* device, ID3D11DeviceContext* context, PreviewRing* ring,
                                     const PreviewWriterSettings& settings);

    /// Shows one captured frame to the preview. **Called from the capture thread.**
    ///
    /// Never blocks, never allocates, never fails in a way the caller has to handle: a
    /// preview that cannot take this frame simply does not, and says so in `stats()`.
    /// Returning `void` is deliberate -- a `Result` here would invite a caller to treat a
    /// preview problem as a capture problem.
    void offer(ID3D11Texture2D* source, std::int64_t qpc_ns);

    /// Joins `fc-preview` within SPEC.md §12's bounded deadline. Idempotent.
    void stop();

    [[nodiscard]] PreviewWriterStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fc::preview

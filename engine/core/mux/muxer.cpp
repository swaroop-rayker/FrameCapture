#include "core/mux/muxer.h"

#include "core/logging/logger.h"
#include "core/timing/frame_pacer.h"
#include "core/timing/qpc_clock.h"

#include <windows.h>

extern "C" {
#include <libavutil/opt.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fc::mux {
namespace {

/// SPEC.md §10.2. A cluster is the unit of recoverability in Matroska: a truncated
/// file is playable up to the last complete cluster, so a 2 s limit bounds what a
/// hard kill can cost.
constexpr const char* kClusterTimeLimitMs = "2000";
/// 4 MB. Bounds cluster size on high-bitrate content, where 2 s of video would
/// otherwise make a single cluster large enough to lose a lot of work.
constexpr const char* kClusterSizeLimitBytes = "4194304";

/// SPEC.md §10.3's fragmented-MP4 flags, used while recording. The progressive
/// form is produced by `remux_to_progressive` once the recording has ended.
constexpr const char* kFragmentedMp4Flags = "frag_keyframe+empty_moov+default_base_moof";

/// Close a fragment after this long even when no keyframe has arrived, in
/// microseconds (BUG-034).
///
/// `frag_keyframe` alone ties the fragment length to the GOP, which SPEC.md §9 fixes
/// at two seconds — so a killed recording always loses the whole open fragment, and
/// row 3's "duration ≥ 28 s of 30 s" is a promise with **121 ms** of room in it. That
/// is not a margin; it is the absence of one, and it made the row's outcome binary:
/// measured, a killed file decoded to 28.02 s or to 26.02 s and to nothing in
/// between.
///
/// Cutting on time as well decouples the loss bound from the GOP. At 500 ms the
/// worst case is a quarter of what it was, and the row's margin goes from 121 ms to
/// about 1.5 s — which is the difference between a guarantee that holds and one that
/// happens to.
///
/// **What it costs, so the trade is visible:** one extra `moof` per 500 ms instead of
/// per 2 s, about 150 bytes each — under 0.1% of a 30-second recording at the
/// bitrates measured here. Fragments no longer all begin on a keyframe, which is
/// legal fMP4 and costs seek granularity in the *intermediate* file only: a clean
/// stop losslessly remuxes to progressive (§10.3), and on the crash path more
/// fragments is strictly better than fewer.
constexpr std::int64_t kFragmentDurationUs = 500'000;

/// How the progressive form gets its `moov` atom to the front -- and why it is not
/// `faststart` (BUG-046).
///
/// The `moov` must lead the file: a reader that has to seek to the end before it can show
/// anything is what SPEC.md §10.3's "with faststart" is asking to avoid. There are two ways
/// to arrange that, and movenc implements both.
///
/// **`faststart` writes the file and then rewrites it.** Its own option text says so --
/// "Run a second pass to put the index (moov atom) at the beginning of the file" -- and
/// `ff_format_shift_data` re-opens the output for reading and copies the entire mdat forward
/// by the size of the moov. Combined with the read and write the remux already performs,
/// that is **four passes over the recording** where two are required. Measured on this rig:
/// ~190 MB/s, which for the reported 1.2 GB recording is about 6.4 s of pure I/O.
///
/// **`moov_size` reserves the space instead.** movenc skips that many bytes at the head,
/// writes the mdat after it, and at trailer time seeks back and writes the moov into the
/// reservation, padding whatever is left with a `free` atom. Same file layout, no second
/// pass. The cost is that the reservation has to be large enough, and whatever is unused
/// stays in the file as padding -- see `estimate_moov_size`.
constexpr const char* kProgressiveMoovSizeOption = "moov_size";

/// The fallback when a reservation turns out too small. Correct but slow; see above.
constexpr const char* kFaststartFlags = "faststart";

/// Bytes to reserve per media sample in the `moov` estimate.
///
/// The moov's size is dominated by the per-sample tables: `stsz` at 4 bytes a sample,
/// `stco`/`co64` at 4-8 per chunk, `ctts` at 8 when B-frames reorder (SPEC.md §9 sets
/// `max_b_frames = 2`, so they do), `stts` and `stss` run-length coded and small. That is
/// 16-20 bytes a sample in practice.
///
/// 40 is deliberately generous. Being wrong costs a whole extra remux through the fallback
/// path, and being right costs only padding: at 1080p60 with AAC, a one-hour recording
/// reserves about 15 MB against a ~36 GB file, and the reported 15-minute one about 4 MB
/// against 1.2 GB -- 0.3%, in a `free` atom every reader skips.
constexpr std::int64_t kMoovBytesPerSample = 40;

/// Fixed part of the reservation: `ftyp`, the stream descriptors, codec parameter sets, and
/// the atoms that do not scale with length.
constexpr std::int64_t kMoovFixedBytes = 256LL * 1024;

/// SPEC.md §8.6's "Max **6** tracks", counting track 0's system mix.
constexpr std::size_t kMaxAudioStreams = 6;

const char* muxer_name(config::Container container) noexcept {
    switch (container) {
    case config::Container::Mkv:
        return "matroska";
    case config::Container::Mp4:
        return "mp4";
    }
    return nullptr;
}

/// Writes the output file to disk properly rather than leaving it in the OS cache.
/// SPEC.md §10.4 calls for an fsync equivalent after closing.
void flush_to_disk(const std::filesystem::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        FC_LOG_WARN(Subsystem::Mux, "could not reopen the output to flush it", LogFields{}.add("path", path.string()));
        return;
    }
    if (FlushFileBuffers(file) == 0) {
        FC_LOG_WARN(Subsystem::Mux, "FlushFileBuffers failed on the output", LogFields{}.add("path", path.string()));
    }
    CloseHandle(file);
}

/// Rolling window of per-write latencies, for SPEC.md §13 rung 6's "disk write
/// latency P99 > 500 ms".
///
/// P99 rather than a mean, because the mean is uninformative here by construction:
/// `av_interleaved_write_frame` buffers, so the overwhelming majority of calls
/// return in microseconds without touching the disk and a handful do all the I/O.
/// A mean over that distribution stays flat while the recording stalls; the tail is
/// the signal.
///
/// Written by the `mux` thread, read by the `watchdog` thread. The lock is held only
/// to store or copy numbers -- **never across the write itself**, which is the rule
/// that matters (SPEC.md §12, CLAUDE.md hard rule 4). The mux thread is the one
/// thread permitted to block on disk, and it does so outside this lock.
class WriteLatency {
public:
    void add(std::int64_t ns) {
        const std::lock_guard lock(mutex_);
        samples_[head_] = ns;
        head_ = (head_ + 1) % kCapacity;
        if (size_ < kCapacity) {
            ++size_;
        }
        worst_ns_ = std::max(worst_ns_, ns);
    }

    /// 99th percentile over the retained window, or 0 before there is one.
    ///
    /// `kMinimumSamples` is why this returns 0 early rather than a percentile of
    /// four numbers: the 99th percentile of a handful of samples is just the maximum
    /// wearing a percentile's name, and the first write of any recording includes
    /// the container header and is legitimately slow. Reporting that as a P99 would
    /// engage rung 6 on every recording's first second.
    [[nodiscard]] std::int64_t p99_ns() const {
        std::array<std::int64_t, kCapacity> copy{};
        std::size_t count = 0;
        {
            const std::lock_guard lock(mutex_);
            count = size_;
            std::copy_n(samples_.begin(), count, copy.begin());
        }
        if (count < kMinimumSamples) {
            return 0;
        }
        // Copied to the stack and sorted here, on the watchdog thread, which SPEC.md
        // §12 permits to block and to allocate. 4 KB of stack rather than a heap
        // allocation, so this stays allocation-free on every thread.
        const std::size_t index = (count * 99) / 100;
        const auto nth = copy.begin() + static_cast<std::ptrdiff_t>(std::min(index, count - 1));
        std::nth_element(copy.begin(), nth, copy.begin() + static_cast<std::ptrdiff_t>(count));
        return *nth;
    }

    [[nodiscard]] std::int64_t worst_ns() const {
        const std::lock_guard lock(mutex_);
        return worst_ns_;
    }

private:
    /// About 4.8 s of packets at 1080p60 with stereo AAC (~107 packets/s), so the
    /// window comfortably covers SPEC.md §13's 3 s evaluation window. Fixed size --
    /// CLAUDE.md hard rule 5 applies to every buffer in the engine.
    static constexpr std::size_t kCapacity = 512;
    static constexpr std::size_t kMinimumSamples = 32;

    mutable std::mutex mutex_;
    std::array<std::int64_t, kCapacity> samples_{};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::int64_t worst_ns_ = 0;
};

} // namespace

struct Muxer::Impl {
    ff::OutputFormatContext format;
    AVStream* video_stream = nullptr;
    /// SPEC.md §8.6's audio tracks, in `EncodedPacket::audio_track` order. Empty for
    /// a video-only recording, one entry for Tier A, up to six for Tier B.
    std::vector<AVStream*> audio_streams;
    std::filesystem::path output;
    config::Container container = config::Container::Mkv;

    // The units packets *arrive* in, captured before `avformat_write_header` runs.
    // It is entitled to change a stream's timebase and matroskaenc always does --
    // it forces 1/1000 on every stream -- so reading `stream->time_base` afterwards
    // and rescaling from it is a no-op that silently reinterprets the packet's
    // timestamps as being in the muxer's units (BUG-015).
    AVRational video_source_timebase{1, static_cast<int>(timing::kVideoTimebaseDen)};
    /// One per audio stream, and per stream rather than shared: the tracks have
    /// independent encoders, and although §8.6 fixes every one of them at 48 kHz
    /// today, reading a rate off the wrong track is exactly BUG-015's shape.
    std::vector<AVRational> audio_source_timebases;

    /// SPEC.md §11: what this file's timestamps count from. 0 except for the second and
    /// subsequent segments of a split recording. See `MuxerSettings::timeline_origin_ns`.
    std::int64_t timeline_origin_ns = 0;

    bool header_written = false;
    bool finalized = false;
    std::uint64_t packets = 0;
    std::uint64_t bytes = 0;

    /// Presentation time of the last **video** packet handed to libavformat, in
    /// nanoseconds on the recording's own timeline.
    ///
    /// Read by the crash-recovery diagnostics: the difference between this and the
    /// wall clock is how far the file on disk trails the recording, which is the
    /// quantity SPEC.md §20 row 3's guarantee actually depends on and the one nothing
    /// measured before BUG-034.
    std::atomic<std::int64_t> last_video_pts_ns{0};

    WriteLatency latency;

    /// SPEC.md §20.1 chaos-tier disk-stall injection. Zero on every production path.
    std::int64_t injected_stall_ns = 0;
    int injected_stall_period = 20;
};

Muxer::Muxer() : impl_(std::make_unique<Impl>()) {}

// The only statement here is a log call, which allocates and can therefore throw
// on a genuinely exhausted heap. There is nothing useful to do at that point --
// the process is already failing and the file is already on disk in its
// last-complete-cluster state -- and catching it would need either an empty
// handler or a catch-all, both of which CLAUDE.md §4 bans. Escaping is the honest
// outcome, so the check is silenced rather than worked around.
// NOLINTNEXTLINE(bugprone-exception-escape)
Muxer::~Muxer() {
    if (impl_ && impl_->format && !impl_->finalized) {
        // Destroyed without finalize -- a bug in the caller, but the file must not
        // be left with an open handle. It stays unfinalized on disk, which for MKV
        // is still playable to the last complete cluster (SPEC.md §10.2) and is
        // exactly what the recovery path in M5 exists to repair.
        FC_LOG_WARN(Subsystem::Mux, "muxer destroyed before finalize; output left unfinalized",
                    LogFields{}.add("path", impl_->output.string()));
    }
}

Result<void> Muxer::open(const MuxerSettings& settings, const encode::IVideoEncoder& encoder,
                         const encode::AacEncoder* audio) {
    if (audio == nullptr) {
        return open(settings, encoder, std::span<const AudioStreamSpec>{});
    }
    // Unnamed: one audio stream needs no `Name` to be unambiguous, and tagging it
    // would put a title in every Tier A file's metadata that no reader wants.
    const std::array<AudioStreamSpec, 1> single{AudioStreamSpec{audio, {}}};
    return open(settings, encoder, std::span<const AudioStreamSpec>{single});
}

Result<void> Muxer::open(const MuxerSettings& settings, const encode::IVideoEncoder& encoder,
                         std::span<const AudioStreamSpec> audio) {
    if (settings.output.empty()) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    if (impl_->format) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    const AVCodecContext* codec = encoder.codec_context();
    if (codec == nullptr) {
        return FcError::INTERNAL_INVALID_STATE;
    }

    impl_->output = settings.output;
    impl_->container = settings.container;
    impl_->timeline_origin_ns = settings.timeline_origin_ns;
    impl_->injected_stall_ns = settings.injected_stall_ns;
    impl_->injected_stall_period = settings.injected_stall_period > 0 ? settings.injected_stall_period : 1;

    // SPEC.md §20 row 2: the muxer comes from the container enum, never from the
    // filename. Inferring it from an extension is how a file ends up with the
    // wrong internal format and refuses to play.
    const char* format_name = muxer_name(settings.container);
    const std::string filename = settings.output.string();
    if (const int err = impl_->format.alloc(format_name, filename.c_str()); err < 0 || !impl_->format) {
        FC_LOG_ERROR(Subsystem::Mux, "could not allocate the output context",
                     LogFields{}
                         .add("format", format_name)
                         .add("error", ff::error_text(err))
                         .add_error(FcError::MUX_OPEN_FAILED));
        return FcError::MUX_OPEN_FAILED;
    }

    AVStream* stream = avformat_new_stream(impl_->format.get(), nullptr);
    if (stream == nullptr) {
        return FcError::MUX_STREAM_ADD_FAILED;
    }
    impl_->video_stream = stream;

    if (const int err = avcodec_parameters_from_context(stream->codecpar, codec); err < 0) {
        FC_LOG_ERROR(Subsystem::Mux, "copying codec parameters failed",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::MUX_STREAM_ADD_FAILED));
        return FcError::MUX_STREAM_ADD_FAILED;
    }

    // Matroska stores SPS/PPS in CodecPrivate, and avformat_write_header fails
    // without them. Whether an encoder produces extradata by default is
    // vendor-specific, so checking here names the cause instead of letting
    // write_header return a generic error several lines later.
    if (stream->codecpar->extradata == nullptr || stream->codecpar->extradata_size == 0) {
        FC_LOG_ERROR(Subsystem::Mux, "encoder produced no extradata; the container has no parameter sets",
                     LogFields{}
                         .add("encoder", encoder.encoder_name())
                         .add("hint", "open the encoder with AV_CODEC_FLAG_GLOBAL_HEADER")
                         .add_error(FcError::MUX_STREAM_ADD_FAILED));
        return FcError::MUX_STREAM_ADD_FAILED;
    }

    // SPEC.md §6: the colour tags must land in the container as well as the SPS
    // VUI. avcodec_parameters_from_context carries them across, but only if the
    // encoder context had them -- so assert rather than assume.
    stream->codecpar->color_space = AVCOL_SPC_BT709;
    stream->codecpar->color_primaries = AVCOL_PRI_BT709;
    stream->codecpar->color_trc = AVCOL_TRC_BT709;
    stream->codecpar->color_range = settings.full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;

    stream->time_base = AVRational{1, static_cast<int>(timing::kVideoTimebaseDen)};
    stream->avg_frame_rate = codec->framerate;

    // SPEC.md §8.6 caps the file at six audio tracks. Enforced here as well as
    // where the tracks are built, because this is the layer that turns a track into
    // a stream and it is the one that must not be talked past.
    if (audio.size() > static_cast<std::size_t>(kMaxAudioStreams)) {
        FC_LOG_ERROR(Subsystem::Mux, "more audio streams were requested than SPEC.md §8.6 allows",
                     LogFields{}
                         .add("requested", static_cast<std::int64_t>(audio.size()))
                         .add("limit", kMaxAudioStreams)
                         .add_error(FcError::MULTITRACK_TRACK_LIMIT_EXCEEDED));
        return FcError::MULTITRACK_TRACK_LIMIT_EXCEEDED;
    }
    // §8.6 makes Tier B MKV-only, and §20 row 16 makes that "a hard block, not a
    // soft warning". The block is applied at `configure` where a user can be told
    // why; this is the last line, on the one component that would otherwise
    // cheerfully write six AAC streams into an MP4 that most players show one of.
    if (audio.size() > 1 && settings.container != config::Container::Mkv) {
        FC_LOG_ERROR(Subsystem::Mux, "multi-track audio was requested on a container that cannot carry it",
                     LogFields{}
                         .add("container", std::string{config::to_string(settings.container)})
                         .add("tracks", static_cast<std::int64_t>(audio.size()))
                         .add_error(FcError::MULTITRACK_REQUIRES_MKV));
        return FcError::MULTITRACK_REQUIRES_MKV;
    }

    impl_->audio_streams.reserve(audio.size());
    impl_->audio_source_timebases.reserve(audio.size());
    for (const AudioStreamSpec& spec : audio) {
        if (spec.encoder == nullptr) {
            return FcError::INTERNAL_INVALID_ARGUMENT;
        }
        const AVCodecContext* audio_codec = spec.encoder->codec_context();
        if (audio_codec == nullptr) {
            return FcError::INTERNAL_INVALID_STATE;
        }

        AVStream* audio_stream = avformat_new_stream(impl_->format.get(), nullptr);
        if (audio_stream == nullptr) {
            return FcError::MUX_STREAM_ADD_FAILED;
        }
        if (const int err = avcodec_parameters_from_context(audio_stream->codecpar, audio_codec); err < 0) {
            FC_LOG_ERROR(Subsystem::Mux, "copying audio codec parameters failed",
                         LogFields{}.add("error", ff::error_text(err)).add_error(FcError::MUX_STREAM_ADD_FAILED));
            return FcError::MUX_STREAM_ADD_FAILED;
        }

        // Signalling site 3 of 3 (SPEC.md §8.5). `avcodec_parameters_from_context`
        // carries the layout across, and the Matroska muxer writes Channels from
        // it -- but only if it survived the copy, so it is checked rather than
        // assumed. Getting this wrong is 5.1 playing as stereo (§20 row 17).
        if (audio_stream->codecpar->ch_layout.nb_channels != audio_codec->ch_layout.nb_channels) {
            FC_LOG_ERROR(Subsystem::Mux, "the audio channel layout did not survive into the container",
                         LogFields{}
                             .add("encoder_channels", audio_codec->ch_layout.nb_channels)
                             .add("container_channels", audio_stream->codecpar->ch_layout.nb_channels)
                             .add_error(FcError::AUDIO_CHANNEL_LAYOUT_UNSUPPORTED));
            return FcError::AUDIO_CHANNEL_LAYOUT_UNSUPPORTED;
        }

        // Without the AudioSpecificConfig a decoder reads the container's guess
        // instead, which is how the layout silently reverts to stereo.
        if (audio_stream->codecpar->extradata == nullptr || audio_stream->codecpar->extradata_size == 0) {
            FC_LOG_ERROR(Subsystem::Mux, "the audio stream carries no AudioSpecificConfig",
                         LogFields{}.add_error(FcError::MUX_STREAM_ADD_FAILED));
            return FcError::MUX_STREAM_ADD_FAILED;
        }

        // SPEC.md §8.6: "Every track gets a human-readable Matroska `Name` tag.
        // Untagged tracks are a UX failure." matroskaenc writes TrackEntry/Name
        // from the stream's `title` metadata, and mov writes it to a `name` atom --
        // one field, both containers, so a Tier B file that somehow reached MP4
        // would still be labelled rather than anonymous.
        //
        // Set before `avformat_write_header`: the muxer serialises track headers
        // there, and metadata added afterwards never reaches the file.
        if (!spec.name.empty()) {
            if (const int err = av_dict_set(&audio_stream->metadata, "title", spec.name.c_str(), 0); err < 0) {
                FC_LOG_ERROR(Subsystem::Mux, "naming an audio track failed",
                             LogFields{}
                                 .add("name", spec.name)
                                 .add("error", ff::error_text(err))
                                 .add_error(FcError::MUX_STREAM_ADD_FAILED));
                return FcError::MUX_STREAM_ADD_FAILED;
            }
        }

        // AAC PTS arrive as a sample count from `t0`, which is what the encoder's
        // own timebase says and what keeps every track aligned against the shared
        // epoch (SPEC.md §7.1). One epoch, N tracks -- §8.6's whole alignment
        // guarantee reduces to every one of these being the same `t0`.
        const AVRational source_timebase{1, audio_codec->sample_rate};
        audio_stream->time_base = source_timebase;
        impl_->audio_source_timebases.push_back(source_timebase);
        impl_->audio_streams.push_back(audio_stream);
    }

    if ((impl_->format->oformat->flags & AVFMT_NOFILE) == 0) {
        if (const int err = avio_open2(&impl_->format->pb, filename.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
            err < 0) {
            FC_LOG_ERROR(Subsystem::Mux, "could not open the output file",
                         LogFields{}
                             .add("path", filename)
                             .add("error", ff::error_text(err))
                             .add_error(FcError::MUX_OPEN_FAILED));
            return FcError::MUX_OPEN_FAILED;
        }
    }

    ff::Dictionary options;
    if (settings.container == config::Container::Mkv) {
        // SPEC.md §10.2.
        options.set("cluster_time_limit", kClusterTimeLimitMs);
        options.set("cluster_size_limit", kClusterSizeLimitBytes);
        options.set("write_crc32", "0"); // saves CPU; Matroska CRCs buy nothing here
        options.set("reserve_index_space", static_cast<std::int64_t>(0));
    } else {
        // SPEC.md §10.3's recipe, verbatim. Each flag earns its place:
        //
        //   frag_keyframe        close a fragment at every keyframe, so the file on
        //                        disk is complete up to the last one -- two seconds
        //                        at the GOP length SPEC.md §9 fixes.
        //   empty_moov          write the `moov` atom immediately, empty. Without
        //                        it the header lands at the *end* and a killed
        //                        process leaves a file with no header at all: the
        //                        unplayable brick of §20 row 3.
        //   default_base_moof   makes each `moof` self-describing rather than
        //                        relative to the previous one, so a reader that
        //                        starts mid-file, or one whose file simply stops,
        //                        can still resolve sample positions.
        options.set("movflags", kFragmentedMp4Flags);
        // And on time as well as on keyframes, so what a kill costs stops being a
        // function of the GOP length. See `kFragmentDurationUs`.
        options.set("frag_duration", kFragmentDurationUs);
    }

    // **When the bytes leave libavformat, not just when the container emits them**
    // (BUG-034). Both recipes above bound what a crash costs in *container* terms --
    // a closed cluster, a closed fragment -- and say nothing about the 256 KB AVIO
    // buffer those bytes land in on the way to the disk.
    //
    // Flushing on our own keyframe writes was the first answer (BUG-020) and it is
    // not sufficient, for a reason that took the mux-lag instrumentation to see:
    // `av_interleaved_write_frame` *queues* a packet until it can interleave it
    // against the other stream. So the call that hands over a keyframe is very often
    // not the call in which movenc receives it, closes the previous fragment and
    // writes the `moof`. Those bytes then sit in the AVIO buffer until the *next*
    // keyframe's write flushes them -- one whole fragment of exposure, and whether a
    // kill lands inside it is a coin toss. Measured: the muxer was only **39-44 ms**
    // behind the capture at the kill, yet the file decoded 1.98 s shorter than that,
    // and occasionally 3.98 s.
    //
    // `AVFMT_FLAG_FLUSH_PACKETS` moves the flush to where the writing actually
    // happens: libavformat flushes after every packet *it* writes, which is the same
    // call in which a fragment is closed. What remains unwritten is then only the
    // fragment still open, which is what §10.3's recipe promises and no more.
    impl_->format->flags |= AVFMT_FLAG_FLUSH_PACKETS;

    // Note on timestamp resolution, because it looks like a bug and is not.
    //
    // Matroska block timestamps are stored in units of the segment's
    // TimestampScale, which FFmpeg's matroskaenc fixes at 1 ms and does not
    // expose as an option. A 60 fps frame is 16.667 ms, so the deltas written to
    // the file alternate 17/17/16 and cannot be made identical here. (The
    // `video_track_timescale` option is not the lever: it writes the deprecated
    // per-track TrackTimestampScale, not the segment scale.)
    //
    // This does not reintroduce the judder of SPEC.md §20 row 7, because the
    // rounding does not accumulate: PTS n stays within one millisecond of
    // n * 1000/fps for the whole file, so there is no drift for a player to chase.
    // The exactness that matters is upstream, in the 1/60000 grid the pacer emits
    // on -- that is what keeps frame n at exactly n/60 s rather than at whatever
    // the wall clock said.

    if (const int err = avformat_write_header(impl_->format.get(), options.address()); err < 0) {
        FC_LOG_ERROR(Subsystem::Mux, "writing the container header failed",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::MUX_WRITE_HEADER_FAILED));
        return FcError::MUX_WRITE_HEADER_FAILED;
    }
    impl_->header_written = true;

    if (!options.empty()) {
        FC_LOG_WARN(Subsystem::Mux, "muxer ignored some options",
                    LogFields{}.add("format", format_name).add("ignored", options.remaining()));
    }

    FC_LOG_INFO(Subsystem::Mux, "container opened",
                LogFields{}
                    .add("path", filename)
                    .add("format", format_name)
                    .add("codec", avcodec_get_name(stream->codecpar->codec_id))
                    // SPEC.md §8.6's track count, on every recording. A Tier B file
                    // whose track count is not what the user asked for is answerable
                    // from the log rather than by demuxing the output.
                    .add("audio_tracks", static_cast<std::int64_t>(impl_->audio_streams.size()))
                    .add("color_range", settings.full_range ? "full" : "limited"));
    return ok();
}

Result<void> Muxer::write(encode::EncodedPacket packet) {
    if (!impl_->format || !impl_->header_written || impl_->finalized) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (!packet.packet) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    const bool is_audio = packet.audio && !impl_->audio_streams.empty();
    if (packet.audio && !is_audio) {
        // An audio packet with no audio stream to put it on. Reachable only through a
        // caller bug, and refused rather than written to the video stream -- which is
        // what indexing without the check would do.
        return FcError::INTERNAL_INVALID_STATE;
    }
    if (is_audio &&
        (packet.audio_track < 0 || static_cast<std::size_t>(packet.audio_track) >= impl_->audio_streams.size())) {
        // A track index this file has no stream for. §8.6's stream set is fixed at
        // `avformat_write_header`, so this cannot be answered by adding one; refusing
        // is the honest outcome and it names the track in the log rather than
        // silently landing the packet on the system mix.
        FC_LOG_ERROR(Subsystem::Mux, "an audio packet named a track this file does not have",
                     LogFields{}
                         .add("track", static_cast<std::int64_t>(packet.audio_track))
                         .add("streams", static_cast<std::int64_t>(impl_->audio_streams.size()))
                         .add_error(FcError::INTERNAL_INVALID_ARGUMENT));
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    const auto track = static_cast<std::size_t>(is_audio ? packet.audio_track : 0);
    const bool is_keyframe_video = !is_audio && packet.keyframe;
    const AVStream* const stream = is_audio ? impl_->audio_streams[track] : impl_->video_stream;
    packet.packet->stream_index = stream->index;

    // Video PTS arrive in 1/60000 units and audio PTS as a sample count, and
    // `avformat_write_header` has almost certainly changed both streams' timebases
    // by now -- matroskaenc forces 1/1000 on every stream. Rescaling from the units
    // the packet was *produced* in to the units the stream now uses is what keeps
    // the two tracks aligned against the shared `t0` (SPEC.md §7.1).
    //
    // Both source timebases are recorded at open time rather than read back off the
    // stream, because reading them back yields the muxer's own units and makes this
    // a no-op that reinterprets rather than converts (BUG-015).
    const AVRational source_timebase = is_audio ? impl_->audio_source_timebases[track] : impl_->video_source_timebase;

    // SPEC.md §11: "timestamps restart at 0 in each segment". Applied in the *source*
    // timebase, before the rescale, so video and audio shift by the same instant rather
    // than by the same integer -- they have different source units, and subtracting a
    // constant after the rescale would move the two tracks apart by whatever the two
    // timebases disagree about, which is §7.1's shared epoch broken at every seam.
    //
    // Zero for an unsegmented recording, where this is not reached at all.
    if (impl_->timeline_origin_ns != 0) {
        const std::int64_t origin =
            av_rescale_q(impl_->timeline_origin_ns, AVRational{1, 1'000'000'000}, source_timebase);
        if (packet.packet->pts != AV_NOPTS_VALUE) {
            packet.packet->pts -= origin;
        }
        if (packet.packet->dts != AV_NOPTS_VALUE) {
            packet.packet->dts -= origin;
        }
    }

    av_packet_rescale_ts(packet.packet.get(), source_timebase, stream->time_base);

    const auto size = static_cast<std::uint64_t>(packet.packet->size);

    // Read before the write: `av_interleaved_write_frame` takes the payload and blanks
    // the packet, so afterwards there is no PTS left to read (BUG-034's instrumentation
    // was wrong in exactly this way on the first attempt).
    const std::int64_t video_pts_ns =
        (!is_audio && packet.packet->pts != AV_NOPTS_VALUE)
            ? av_rescale_q(packet.packet->pts, stream->time_base, AVRational{1, 1'000'000'000})
            : -1;

    // Spans the write *and* the keyframe flush below, because between them they are
    // the whole of this thread's disk cost and rung 6 is about how long the disk
    // takes. Timing only the write would report microseconds on a volume whose
    // every flush takes a second.
    const std::int64_t started_ns = timing::qpc_now_ns();

    // Takes ownership of the packet's payload and handles interleaving depth.
    const int err = av_interleaved_write_frame(impl_->format.get(), packet.packet.get());
    if (err < 0) {
        FC_LOG_ERROR(Subsystem::Mux, "writing a packet failed",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::MUX_WRITE_PACKET_FAILED));
        return FcError::MUX_WRITE_PACKET_FAILED;
    }

    ++impl_->packets;
    impl_->bytes += size;

    if (!is_audio && video_pts_ns >= 0) {
        impl_->last_video_pts_ns.store(video_pts_ns, std::memory_order_relaxed);
    }

    // The AVIO buffer is flushed by `AVFMT_FLAG_FLUSH_PACKETS`, set in `open` --
    // inside the call above, at the point libavformat actually writes, which is the
    // only point that is in step with a fragment being closed. See the comment there
    // for why flushing here on our own keyframes was not enough (BUG-020, BUG-034).
    //
    // Kept as a belt-and-braces flush on the keyframe boundary because it is free
    // when the buffer is already empty, and because a future container whose muxer
    // buffers differently would otherwise silently lose the guarantee.
    if (is_keyframe_video && impl_->format->pb != nullptr) {
        avio_flush(impl_->format->pb);
    }

    // Inside the timed span, deliberately: the injected stall must be visible to
    // rung 6 through the same measurement a real slow volume would move, or the test
    // would be exercising a special case instead of the production path.
    if (impl_->injected_stall_ns > 0 &&
        impl_->packets % static_cast<std::uint64_t>(impl_->injected_stall_period) == 0) {
        std::this_thread::sleep_for(std::chrono::nanoseconds{impl_->injected_stall_ns});
    }

    impl_->latency.add(timing::qpc_now_ns() - started_ns);
    return ok();
}

std::int64_t Muxer::write_latency_p99_ns() const {
    return impl_->latency.p99_ns();
}

std::int64_t Muxer::worst_write_latency_ns() const {
    return impl_->latency.worst_ns();
}

std::uint64_t Muxer::output_volume_free_bytes() const {
    if (impl_->output.empty()) {
        return UINT64_MAX;
    }
    ULARGE_INTEGER available{};
    // The parent directory, not the file: the file may not exist yet, and
    // `GetDiskFreeSpaceExW` wants a path that does.
    const std::filesystem::path directory = impl_->output.parent_path();
    const std::wstring target = directory.empty() ? L"." : directory.wstring();
    if (GetDiskFreeSpaceExW(target.c_str(), &available, nullptr, nullptr) == 0) {
        // `UINT64_MAX` is "not measured", deliberately distinct from 0. A failed
        // query must not read as an empty volume and stop a healthy recording
        // (SPEC.md §13 rung 6).
        return UINT64_MAX;
    }
    // The caller-quota figure, not the volume total: on a quota'd volume the space
    // this process can actually use is the smaller of the two, and the larger one
    // would let the recording run into a write failure it was told would not come.
    return available.QuadPart;
}

Result<void> Muxer::finalize() {
    if (impl_->finalized) {
        return ok(); // SPEC.md §10.4: every finalization stage is idempotent.
    }
    if (!impl_->format || !impl_->header_written) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    impl_->finalized = true;

    // Writes the Cues element for MKV, which is what makes the file seekable.
    if (const int err = av_write_trailer(impl_->format.get()); err < 0) {
        FC_LOG_ERROR(Subsystem::Mux, "writing the container trailer failed",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::MUX_WRITE_TRAILER_FAILED));
        impl_->format.reset();
        return FcError::MUX_WRITE_TRAILER_FAILED;
    }

    // Order matters: close the AVIO handle first so the bytes are actually
    // committed, then flush the file, then release the context.
    impl_->format.close_io();
    flush_to_disk(impl_->output);
    impl_->format.reset();

    FC_LOG_INFO(Subsystem::Mux, "container finalized",
                LogFields{}
                    .add("path", impl_->output.string())
                    .add("packets", static_cast<std::int64_t>(impl_->packets))
                    .add("bytes", static_cast<std::int64_t>(impl_->bytes))
                    // Reported on every recording, not only when rung 6 fired. A
                    // stutter report is answerable with these two numbers and
                    // unanswerable without them.
                    .add("write_p99_us", impl_->latency.p99_ns() / 1000)
                    .add("worst_write_us", impl_->latency.worst_ns() / 1000));
    return ok();
}

bool Muxer::has_audio() const noexcept {
    return !impl_->audio_streams.empty();
}

int Muxer::audio_stream_count() const noexcept {
    return static_cast<int>(impl_->audio_streams.size());
}

bool Muxer::needs_remux() const noexcept {
    return impl_->container == config::Container::Mp4;
}

bool Muxer::open_for_writing() const noexcept {
    return impl_->format && impl_->header_written && !impl_->finalized;
}

std::int64_t Muxer::last_video_pts_ns() const noexcept {
    return impl_->last_video_pts_ns.load(std::memory_order_relaxed);
}

std::uint64_t Muxer::packets_written() const noexcept {
    return impl_->packets;
}

std::uint64_t Muxer::bytes_written() const noexcept {
    return impl_->bytes;
}

namespace {

/// Opens a decoder for `par`, or returns null with the reason in `detail`.
[[nodiscard]] ff::CodecContext open_decoder(const AVCodecParameters* par, std::string& detail) {
    ff::CodecContext context;
    const AVCodec* decoder = avcodec_find_decoder(par->codec_id);
    if (decoder == nullptr) {
        detail = std::string{"no decoder for "} + avcodec_get_name(par->codec_id);
        return context;
    }
    if (!context.alloc(decoder)) {
        detail = "could not allocate a decoder context";
        return context;
    }
    if (const int err = avcodec_parameters_to_context(context.get(), par); err < 0) {
        detail = "avcodec_parameters_to_context failed: " + ff::error_text(err);
        context.reset();
        return context;
    }
    if (const int err = avcodec_open2(context.get(), decoder, nullptr); err < 0) {
        detail = "opening the decoder failed: " + ff::error_text(err);
        context.reset();
        return context;
    }
    return context;
}

/// How far the two tracks may disagree in length before the file is called bad.
///
/// Deliberately loose. This gate answers "is the recording usable", not "is it in
/// sync" -- that is `test_av_sync`'s job against SPEC.md §20 row 4's 20 ms, with a
/// signal whose position is known. What this catches is a track that is wrong by a
/// factor rather than by a margin: a timebase misread puts the audio 48× out, and
/// a truncated file legitimately ends its two tracks a fragment apart.
constexpr double kTrackLengthToleranceSeconds = 1.0;

/// How much of the end of the file SPEC.md §10.4's "last frames decode" reads (BUG-040).
///
/// Wide enough to contain a whole GOP at either supported rate -- SPEC.md §9's default
/// keyframe interval is 2 s -- so the seek lands on a keyframe and the closing pictures
/// decode with their references present. Wide enough, too, that a fragmented MKV cluster
/// or MP4 fragment falls inside it rather than straddling the boundary.
constexpr double kTailProbeSeconds = 5.0;

} // namespace

namespace {

/// How much space to reserve at the head of the progressive file for its `moov` atom.
///
/// Counted from the source where it can be and estimated from its duration where it cannot:
/// a fragmented MP4 often reports `nb_frames` after `avformat_find_stream_info`, and when it
/// does that is the exact sample count rather than a guess. Both paths are then padded by
/// `kMoovBytesPerSample`, which is roughly twice what the tables actually need.
///
/// Returns 0 when the source says nothing useful about its own length, which the caller
/// treats as "reserve nothing and use `faststart`" -- a slow correct answer rather than a
/// fast wrong one.
[[nodiscard]] std::int64_t estimate_moov_size(const AVFormatContext* in) {
    if (in == nullptr) {
        return 0;
    }
    const double duration_seconds =
        in->duration != AV_NOPTS_VALUE ? static_cast<double>(in->duration) / AV_TIME_BASE : 0.0;

    std::int64_t samples = 0;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVStream* stream = in->streams[i];
        if (stream->nb_frames > 0) {
            samples += stream->nb_frames;
            continue;
        }
        if (duration_seconds <= 0.0) {
            continue;
        }
        const AVCodecParameters* params = stream->codecpar;
        if (params->codec_type == AVMEDIA_TYPE_VIDEO) {
            const AVRational rate = stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate : AVRational{60, 1};
            samples += static_cast<std::int64_t>(duration_seconds * av_q2d(rate));
        } else if (params->codec_type == AVMEDIA_TYPE_AUDIO && params->sample_rate > 0) {
            // AAC's frame is 1024 samples (SPEC.md §8.5), so that is the packet count.
            const int frame_size = params->frame_size > 0 ? params->frame_size : 1024;
            samples += static_cast<std::int64_t>(duration_seconds * params->sample_rate / frame_size);
        }
    }

    if (samples <= 0) {
        return 0;
    }
    return (samples * kMoovBytesPerSample) + kMoovFixedBytes;
}

/// Copies every packet from `in` to `output`, rescaling into the destination's timebases.
///
/// `on_progress`, when given, is called at `kFinalizeProgressIntervalNs` intervals with the
/// read's position in the source. Measured by *bytes read* rather than packets copied:
/// packet counts are proportional to bytes only when packets are the same size, and a
/// recording's audio packets are two orders of magnitude smaller than its keyframes, so a
/// packet-counted bar races through the audio and crawls through the video. `avio_size` is
/// the source's length and `avio_tell` the read position libavformat advances as it demuxes.
///
/// A source that will not report a size yields no progress at all rather than an invented
/// figure; the phase label still says a remux is running.
[[nodiscard]] Result<std::uint64_t> copy_packets(AVFormatContext* in, AVFormatContext* output,
                                                 const std::vector<AVRational>& source_timebases,
                                                 const FinalizeProgressFn& on_progress) {
    ff::Packet packet;
    if (!packet.alloc()) {
        return FcError::INTERNAL_OUT_OF_MEMORY;
    }

    const std::int64_t source_size = in->pb != nullptr ? avio_size(in->pb) : -1;
    const bool report = static_cast<bool>(on_progress) && source_size > 0;
    std::int64_t last_report_ns = timing::qpc_now_ns();

    std::uint64_t copied = 0;
    while (av_read_frame(in, packet.get()) >= 0) {
        // Checked per packet, fired at 10 Hz. `qpc_now_ns` is a `QueryPerformanceCounter`
        // read -- tens of nanoseconds against the ~8.8 µs per packet BUG-046 measured --
        // so the guard costs well under 1% of the loop it guards, and the alternative is
        // ~96,000 pipe writes for a 1.2 GB recording.
        if (report) {
            if (const std::int64_t now_ns = timing::qpc_now_ns();
                now_ns - last_report_ns >= kFinalizeProgressIntervalNs) {
                last_report_ns = now_ns;
                if (const std::int64_t position = avio_tell(in->pb); position > 0) {
                    const double fraction =
                        std::clamp(static_cast<double>(position) / static_cast<double>(source_size), 0.0, 1.0);
                    on_progress(FinalizeProgress{
                        FinalizePhase::Remuxing, finalize_percent(FinalizePhase::Remuxing, fraction),
                        static_cast<std::uint64_t>(position), static_cast<std::uint64_t>(source_size)});
                }
            }
        }

        const auto index = static_cast<unsigned>(packet->stream_index);
        if (index >= output->nb_streams) {
            packet.unref();
            continue;
        }
        // The destination's timebase is whatever `write_header` settled on, which
        // for MP4 is a per-stream timescale it picks itself. Rescaling from the
        // source's own units is the same discipline BUG-015 established: never read
        // the source unit back off the object that owns the destination unit.
        av_packet_rescale_ts(packet.get(), source_timebases[index], output->streams[index]->time_base);
        packet->pos = -1;

        if (const int err = av_interleaved_write_frame(output, packet.get()); err < 0) {
            FC_LOG_ERROR(Subsystem::Mux, "writing a packet during remux failed",
                         LogFields{}
                             .add("error", ff::error_text(err))
                             .add("packets_copied", static_cast<std::int64_t>(copied))
                             .add_error(FcError::MUX_REMUX_FAILED));
            return FcError::MUX_REMUX_FAILED;
        }
        ++copied;
        packet.unref();
    }
    return copied;
}

} // namespace

namespace {

/// One remux attempt.
///
/// `moov_reserve > 0` reserves that many bytes at the head of the destination for the `moov`
/// atom; 0 falls back to `faststart`, which writes the file and then rewrites it. See
/// `kProgressiveMoovSizeOption`.
///
/// `reserve_moov` asks for the reservation; the size is computed here rather than by the
/// caller, because it comes from `avformat_find_stream_info` and this function already runs
/// one. **An earlier version had the caller probe the source separately to size it, and
/// that cost more than the two passes it saved** -- measured, 171 ms against 103 ms on a
/// 15 MB file, because probing a fragmented MP4 is not free.
///
/// `reserved_out`, when given, receives the reservation actually used.
[[nodiscard]] Result<std::uint64_t> remux_once(const std::filesystem::path& source,
                                               const std::filesystem::path& destination, int audio_initial_padding,
                                               bool reserve_moov, std::int64_t* reserved_out,
                                               const FinalizeProgressFn& on_progress) {
    ff::InputFormatContext input;
    const std::string source_name = source.string();
    if (const int err = input.open(source_name.c_str()); err < 0) {
        FC_LOG_ERROR(Subsystem::Mux, "the fragmented source would not open for remux",
                     LogFields{}
                         .add("path", source_name)
                         .add("error", ff::error_text(err))
                         .add_error(FcError::MUX_REMUX_FAILED));
        return FcError::MUX_REMUX_FAILED;
    }
    AVFormatContext* in = input.get();

    if (const int err = avformat_find_stream_info(in, nullptr); err < 0) {
        FC_LOG_ERROR(Subsystem::Mux, "the fragmented source has no readable stream info",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::MUX_REMUX_FAILED));
        return FcError::MUX_REMUX_FAILED;
    }

    // Sized here, from the probe this function already had to run.
    const std::int64_t moov_reserve = reserve_moov ? estimate_moov_size(in) : 0;
    if (reserved_out != nullptr) {
        *reserved_out = moov_reserve;
    }

    ff::OutputFormatContext output;
    const std::string destination_name = destination.string();
    if (const int err = output.alloc("mp4", destination_name.c_str()); err < 0 || !output) {
        return FcError::MUX_REMUX_FAILED;
    }

    // One output stream per input stream, parameters copied across untouched. This
    // is the whole of the "lossless" claim: no codec is opened on either side.
    //
    // The mapping is identity rather than a lookup table because `mp4` accepts
    // every stream `mp4` produced, and a stream this build cannot carry is a
    // condition that should fail loudly rather than silently drop a track.
    std::vector<AVRational> source_timebases(in->nb_streams);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVStream* in_stream = in->streams[i];
        source_timebases[i] = in_stream->time_base;

        AVStream* out_stream = avformat_new_stream(output.get(), nullptr);
        if (out_stream == nullptr) {
            return FcError::MUX_STREAM_ADD_FAILED;
        }
        if (const int err = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar); err < 0) {
            FC_LOG_ERROR(Subsystem::Mux, "copying stream parameters for the remux failed",
                         LogFields{}.add("error", ff::error_text(err)).add_error(FcError::MUX_REMUX_FAILED));
            return FcError::MUX_REMUX_FAILED;
        }
        // Cleared because it describes the *source's* multiplexing, not the
        // destination's, and libavformat rejects a tag it did not choose.
        out_stream->codecpar->codec_tag = 0;
        out_stream->time_base = in_stream->time_base;
        out_stream->avg_frame_rate = in_stream->avg_frame_rate;

        // Restored from the caller rather than carried over: the fragmented source
        // does not record the encoder delay, so the copy above brought a zero
        // across (BUG-021).
        //
        // **This records the number and does not fix the symptom.** movenc derives
        // its edit list from packet timestamps, not from this field, so a reader
        // still presents the first 1024 samples of priming as content and an MP4's
        // audio still begins ~21 ms late at the head. Producing that edit list by
        // shifting the audio packets negative has now been tried **twice**, the
        // second time with `use_editlist=1` and `avoid_negative_ts=disabled` both
        // confirmed applied, and both times it slides the whole track 21 ms early
        // instead of trimming it — one bad mark traded for nineteen. Setting the
        // field is still right: it is the correct metadata and costs nothing. The
        // rest is recorded in BUG-021.
        if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_initial_padding > 0) {
            out_stream->codecpar->initial_padding = audio_initial_padding;
        }
    }

    if ((output->oformat->flags & AVFMT_NOFILE) == 0) {
        if (const int err = avio_open2(&output->pb, destination_name.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
            err < 0) {
            FC_LOG_ERROR(Subsystem::Mux, "could not open the remux destination",
                         LogFields{}
                             .add("path", destination_name)
                             .add("error", ff::error_text(err))
                             .add_error(FcError::MUX_REMUX_FAILED));
            return FcError::MUX_REMUX_FAILED;
        }
    }

    ff::Dictionary options;
    if (moov_reserve > 0) {
        options.set(kProgressiveMoovSizeOption, moov_reserve);
    } else {
        options.set("movflags", kFaststartFlags);
    }
    if (const int err = avformat_write_header(output.get(), options.address()); err < 0) {
        FC_LOG_ERROR(Subsystem::Mux, "writing the progressive header failed",
                     LogFields{}.add("error", ff::error_text(err)).add_error(FcError::MUX_REMUX_FAILED));
        return FcError::MUX_REMUX_FAILED;
    }

    // One copy loop, in `copy_packets`. It used to be written out again here, which is
    // how this file ended up with the helper defined and never called -- and would have
    // meant adding the progress hook to whichever of the two a reader found first.
    FC_TRY_ASSIGN(const std::uint64_t copied, copy_packets(in, output.get(), source_timebases, on_progress));

    // The one call that can refuse specifically because the reservation was too small.
    // movenc logs "reserved_moov_size is too small" and returns EINVAL; the caller retries
    // with `faststart` rather than failing the recording, so this is a warning and not an
    // error.
    if (const int err = av_write_trailer(output.get()); err < 0) {
        FC_LOG_WARN(Subsystem::Mux, "writing the progressive trailer failed",
                    LogFields{}
                        .add("error", ff::error_text(err))
                        .add("moov_reserved", moov_reserve)
                        .add_error(FcError::MUX_REMUX_FAILED));
        return FcError::MUX_REMUX_FAILED;
    }

    output.close_io();
    flush_to_disk(destination);
    output.reset();
    return copied;
}

} // namespace

Result<void> remux_to_progressive(const std::filesystem::path& source, const std::filesystem::path& destination,
                                  int audio_initial_padding, RemuxStats* stats, const FinalizeProgressFn& on_progress) {
    if (source.empty() || destination.empty() || source == destination) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    const std::int64_t began_ns = timing::qpc_now_ns();

    // The reserved form first, always. It is the fast path and it is the one that works for
    // every file this engine writes; the fallback exists for a source whose length cannot be
    // read, which is not a file `Muxer` produces.
    std::int64_t reserved_bytes = 0;
    bool reserved = true;
    Result<std::uint64_t> copied =
        remux_once(source, destination, audio_initial_padding, true, &reserved_bytes, on_progress);

    if (!copied.has_value() || reserved_bytes == 0) {
        // Either the reservation was too small, or the source would not say how long it is
        // and none was made. The fragmented source is untouched -- `finalize_in_place` does
        // not delete it until the result validates -- so falling back costs time and
        // nothing else.
        if (!copied.has_value()) {
            FC_LOG_WARN(Subsystem::Mux, "the reserved moov did not fit; falling back to a second-pass faststart",
                        LogFields{}.add("reserved", reserved_bytes));
            std::error_code ec;
            std::filesystem::remove(destination, ec);
            // The second attempt reports progress too, and it restarts from 0%. A bar
            // that goes backwards once is honest about a file being written twice;
            // freezing it at the first attempt's last value would not be.
            copied = remux_once(source, destination, audio_initial_padding, false, nullptr, on_progress);
        }
        reserved = false;
    }

    if (!copied.has_value()) {
        return copied.error();
    }

    if (stats != nullptr) {
        stats->packets = copied.value();
        stats->moov_reserved_bytes = reserved ? reserved_bytes : 0;
        stats->second_pass = !reserved;
        stats->elapsed_ns = timing::qpc_now_ns() - began_ns;
    }

    FC_LOG_INFO(Subsystem::Mux, "remuxed to progressive MP4",
                LogFields{}
                    .add("source", source.string())
                    .add("destination", destination.string())
                    .add("packets", static_cast<std::int64_t>(copied.value()))
                    .add("moov_reserved", reserved ? reserved_bytes : 0)
                    // True when the file was written twice (SPEC.md §10.3's `faststart`).
                    // A recording that logs this is one whose estimate was wrong.
                    .add("second_pass", !reserved)
                    .add("ms", static_cast<double>(timing::qpc_now_ns() - began_ns) / 1'000'000.0));
    return ok();
}

std::string_view to_string(FinalizePhase phase) noexcept {
    switch (phase) {
    case FinalizePhase::Flushing:
        return "flushing";
    case FinalizePhase::Remuxing:
        return "remuxing";
    case FinalizePhase::Validating:
        return "validating";
    case FinalizePhase::Swapping:
        return "swapping";
    case FinalizePhase::Done:
        return "done";
    }
    return "unknown";
}

int finalize_percent(FinalizePhase phase, double fraction) noexcept {
    // Bands, in order. `Remuxing` gets the majority because it is the phase that
    // actually scales with the file and the only one with real progress inside it;
    // `Validating` gets a fixed 18 points for BUG-046's fixed ~650-1000 ms.
    //
    // Nothing reaches 100 except `Done`, and `Done` is emitted by the caller after the
    // report says the file is good. A bar at 100% before validation has passed would be
    // claiming a recording is saved that may yet be rejected.
    struct Band {
        int start;
        int end;
    };

    const Band band = [phase]() -> Band {
        switch (phase) {
        case FinalizePhase::Flushing:
            return {0, 2};
        case FinalizePhase::Remuxing:
            return {2, 80};
        case FinalizePhase::Validating:
            return {80, 98};
        case FinalizePhase::Swapping:
            return {98, 99};
        case FinalizePhase::Done:
            return {100, 100};
        }
        return {0, 0};
    }();

    const double clamped = std::clamp(fraction, 0.0, 1.0);
    const auto span = static_cast<double>(band.end - band.start);
    return band.start + static_cast<int>(clamped * span);
}

Result<ValidationReport> finalize_in_place(const std::filesystem::path& path, const ValidationExpectation& expectation,
                                           int audio_initial_padding, const FinalizeProgressFn& on_progress) {
    // Reported before anything slow starts, so a GUI shows a phase rather than an empty
    // bar for however long opening a 1.2 GB file takes.
    const auto report_phase = [&on_progress](FinalizePhase phase) {
        if (on_progress) {
            on_progress(FinalizeProgress{phase, finalize_percent(phase, 0.0), 0, 0});
        }
    };
    report_phase(FinalizePhase::Flushing);

    // Beside the recording, not in the system temp directory: a rename across
    // volumes is a copy, and the whole point of the swap is that it is atomic.
    std::filesystem::path staged = path;
    staged += ".progressive.tmp";

    std::error_code ec;
    std::filesystem::remove(staged, ec); // a leftover from a previous killed attempt

    // SPEC.md §10.4: "Every stage of finalization is idempotent and **separately logged**."
    // The timings matter as much as the stages: §10.3 budgets the remux at "~2 s for a 1 h
    // file", and a number that is only ever measured end to end cannot say which stage
    // spent it.
    const std::uint64_t source_bytes = std::filesystem::file_size(path, ec);
    const std::int64_t began_ns = timing::qpc_now_ns();

    report_phase(FinalizePhase::Remuxing);
    if (const Result<void> remuxed = remux_to_progressive(path, staged, audio_initial_padding, nullptr, on_progress);
        !remuxed.has_value()) {
        std::filesystem::remove(staged, ec);
        return remuxed.error();
    }
    const std::int64_t remuxed_ns = timing::qpc_now_ns();

    // SPEC.md §10.3: "verify the output with a decode-probe before deleting the
    // source." The fragmented original is a working recording, and it is not given
    // up for one that has not proven itself.
    report_phase(FinalizePhase::Validating);
    FC_TRY_ASSIGN(const ValidationReport report, Muxer::validate(staged, expectation));
    const std::int64_t validated_ns = timing::qpc_now_ns();
    if (!report.valid) {
        std::filesystem::remove(staged, ec);
        FC_LOG_ERROR(Subsystem::Mux, "the remuxed file did not validate; keeping the fragmented recording",
                     LogFields{}.add("path", path.string()).add("detail", report.detail));
        return report;
    }

    // `MoveFileEx` with REPLACE_EXISTING is the atomic swap; `WRITE_THROUGH` makes
    // it durable before returning, so a power cut cannot leave the directory entry
    // updated and the data behind it not.
    report_phase(FinalizePhase::Swapping);
    if (MoveFileExW(staged.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        const DWORD error = GetLastError();
        std::filesystem::remove(staged, ec);
        FC_LOG_ERROR(Subsystem::Mux, "could not swap the progressive file in; keeping the fragmented recording",
                     LogFields{}
                         .add("path", path.string())
                         .add("gle", static_cast<std::int64_t>(error))
                         .add_error(FcError::IO_ATOMIC_REPLACE_FAILED));
        return FcError::IO_ATOMIC_REPLACE_FAILED;
    }

    const std::int64_t swapped_ns = timing::qpc_now_ns();
    const double remux_ms = static_cast<double>(remuxed_ns - began_ns) / 1'000'000.0;
    const double megabytes = static_cast<double>(source_bytes) / (1024.0 * 1024.0);

    FC_LOG_INFO(Subsystem::Mux, "MP4 finalized to progressive",
                LogFields{}
                    .add("path", path.string())
                    .add("duration_s", report.duration_seconds)
                    .add("decoded_frames", report.decoded_frames)
                    .add("size_mb", megabytes)
                    .add("remux_ms", remux_ms)
                    .add("validate_ms", static_cast<double>(validated_ns - remuxed_ns) / 1'000'000.0)
                    .add("swap_ms", static_cast<double>(swapped_ns - validated_ns) / 1'000'000.0)
                    // The number §10.3's "~2 s for a 1 h file" is a claim about. A recording
                    // finalizing at less than the disk's sequential rate is doing more
                    // passes over the file than it needs to.
                    .add("remux_mb_per_s", remux_ms > 0.0 ? megabytes / (remux_ms / 1000.0) : 0.0));
    return report;
}

Result<ValidationReport> Muxer::validate(const std::filesystem::path& path, const ValidationExpectation& expectation) {
    ValidationReport report;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) == 0) {
        report.detail = "output file is missing or empty";
        return report;
    }

    ff::InputFormatContext input;
    const std::string filename = path.string();
    if (const int err = input.open(filename.c_str()); err < 0) {
        report.detail = "avformat_open_input failed: " + ff::error_text(err);
        return report;
    }
    AVFormatContext* raw = input.get();

    if (const int err = avformat_find_stream_info(raw, nullptr); err < 0) {
        report.detail = "avformat_find_stream_info failed: " + ff::error_text(err);
        return report;
    }

    report.format_name = raw->iformat->name != nullptr ? raw->iformat->name : "";
    report.stream_count = static_cast<int>(raw->nb_streams);
    report.duration_seconds = raw->duration != AV_NOPTS_VALUE ? static_cast<double>(raw->duration) / AV_TIME_BASE : 0.0;

    int video_index = -1;
    // Every audio stream, in file order. SPEC.md §8.6 puts the system mix first and
    // the per-application tracks after it, so this vector's order is the track
    // numbering `EncodedPacket::audio_track` uses.
    std::vector<int> audio_indices;
    for (unsigned i = 0; i < raw->nb_streams; ++i) {
        const AVMediaType type = raw->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && video_index < 0) {
            video_index = static_cast<int>(i);
        } else if (type == AVMEDIA_TYPE_AUDIO) {
            audio_indices.push_back(static_cast<int>(i));
        }
    }
    const int audio_index = audio_indices.empty() ? -1 : audio_indices.front();
    report.audio_stream_count = static_cast<int>(audio_indices.size());
    if (video_index < 0) {
        report.detail = "no video stream in the output";
        return report;
    }

    // A recording that was supposed to have sound and does not is a failed
    // recording, however cleanly the video decodes.
    if (expectation.audio && audio_index < 0) {
        report.detail = "the output has no audio stream, but audio was recorded";
        return report;
    }

    // SPEC.md §8.6 forbids dropping a track from the container, so a file with
    // fewer streams than were opened is a failure and not a partial success.
    if (expectation.audio_streams > 0 && report.audio_stream_count != expectation.audio_streams) {
        report.detail = "the output carries " + std::to_string(report.audio_stream_count) + " audio tracks, not the " +
                        std::to_string(expectation.audio_streams) + " that were recorded";
        return report;
    }

    const AVCodecParameters* par = raw->streams[video_index]->codecpar;
    report.video_codec = avcodec_get_name(par->codec_id);

    // SPEC.md §10.4: confirm frames actually decode. A container that parses but
    // whose payload is garbage is exactly the failure this gate exists to catch.
    ff::CodecContext decode_ctx = open_decoder(par, report.detail);
    if (!decode_ctx) {
        return report;
    }

    // One probe per audio stream. Probe 0 is the system mix and carries every check
    // this gate has always made; the rest are SPEC.md §8.6's per-application tracks
    // and are held to the same structural checks plus agreement on length.
    struct AudioProbe {
        int index = 0;
        std::string name;
        int channels = 0;
        int sample_rate = 0;
        ff::CodecContext context;
        std::int64_t samples = 0;
        std::int64_t first_pts = AV_NOPTS_VALUE;
        std::int64_t last_end_pts = AV_NOPTS_VALUE;
        double duration_seconds = 0.0;
    };

    std::vector<AudioProbe> audio_probes;
    audio_probes.reserve(audio_indices.size());

    for (const int index : audio_indices) {
        const AVCodecParameters* audio_par = raw->streams[index]->codecpar;
        AudioProbe probe;
        probe.index = index;
        probe.channels = audio_par->ch_layout.nb_channels;
        probe.sample_rate = audio_par->sample_rate;
        if (const AVDictionaryEntry* title = av_dict_get(raw->streams[index]->metadata, "title", nullptr, 0);
            title != nullptr && title->value != nullptr) {
            probe.name = title->value;
        }

        if (index == audio_index) {
            report.audio_codec = avcodec_get_name(audio_par->codec_id);
            report.audio_channels = probe.channels;
            report.audio_sample_rate = probe.sample_rate;

            // Signalling site 3 of SPEC.md §8.5, checked on the way out rather than
            // only on the way in. 5.1 that reopens as stereo is §20 row 17's defect.
            //
            // Track 0 only: §8.6 fixes the per-application tracks at stereo whatever
            // the system mix's layout is, so holding them to the same expectation
            // would fail every 5.1 Tier B recording.
            if (expectation.audio_channels > 0 && report.audio_channels != expectation.audio_channels) {
                report.detail = "the audio stream reopened with " + std::to_string(report.audio_channels) +
                                " channels, not the " + std::to_string(expectation.audio_channels) +
                                " that were recorded";
                return report;
            }
        } else if (expectation.app_track_channels > 0 && probe.channels != expectation.app_track_channels) {
            // SPEC.md §8.5's pin, applied per stream (amended 2026-08-06). §8.6 supplies
            // the per-application format, so these are stereo however the system mix was
            // configured — and a track that came back otherwise means the supplied format
            // was not the one that reached the encoder.
            report.detail = "per-application audio track " + std::to_string(index) + " reopened with " +
                            std::to_string(probe.channels) + " channels, not the " +
                            std::to_string(expectation.app_track_channels) + " that SPEC.md §8.6 supplies";
            return report;
        }

        // Without the AudioSpecificConfig a decoder reads the container's guess.
        // Every track, because a per-application track that reopens as a guess is
        // the same defect on a stream nobody looked at.
        if (audio_par->extradata == nullptr || audio_par->extradata_size == 0) {
            report.detail = "audio stream " + std::to_string(index) + " carries no AudioSpecificConfig";
            return report;
        }

        probe.context = open_decoder(audio_par, report.detail);
        if (!probe.context) {
            return report;
        }
        audio_probes.push_back(std::move(probe));
    }

    ff::Packet packet;
    ff::Frame frame;
    if (!packet.alloc() || !frame.alloc()) {
        report.detail = "allocation failed during validation";
        return report;
    }

    std::int64_t first_video_pts = AV_NOPTS_VALUE;
    std::int64_t last_video_pts = AV_NOPTS_VALUE;
    /// The gap between the last two decoded video frames, in stream ticks. A track runs
    /// to the *end* of its last frame, so the span needs one frame added, and this is
    /// that frame measured rather than averaged.
    std::int64_t last_video_step = 0;

    const auto harvest_video = [&] {
        while (avcodec_receive_frame(decode_ctx.get(), frame.get()) >= 0) {
            ++report.decoded_frames;
            if (frame->pts != AV_NOPTS_VALUE) {
                if (first_video_pts == AV_NOPTS_VALUE) {
                    first_video_pts = frame->pts;
                }
                if (last_video_pts != AV_NOPTS_VALUE && frame->pts > last_video_pts) {
                    last_video_step = frame->pts - last_video_pts;
                }
                last_video_pts = frame->pts;
            }
            frame.unref();
        }
    };
    const auto harvest_audio = [&](AudioProbe& probe) {
        while (avcodec_receive_frame(probe.context.get(), frame.get()) >= 0) {
            probe.samples += frame->nb_samples;
            if (frame->pts != AV_NOPTS_VALUE) {
                if (probe.first_pts == AV_NOPTS_VALUE) {
                    probe.first_pts = frame->pts;
                }
                const AVRational tb = raw->streams[probe.index]->time_base;
                const std::int64_t ticks = av_rescale_q(frame->nb_samples, AVRational{1, probe.sample_rate}, tb);
                probe.last_end_pts = frame->pts + ticks;
            }
            frame.unref();
        }
    };

    /// The probe for a stream index, or null when the stream is not audio.
    ///
    /// Linear over at most six entries (SPEC.md §8.6's cap), which is cheaper than a
    /// map and is not on any hot path -- this runs once per packet of a five-second
    /// probe window, not once per packet of the recording.
    const auto probe_for = [&](int index) -> AudioProbe* {
        for (AudioProbe& probe : audio_probes) {
            if (probe.index == index) {
                return &probe;
            }
        }
        return nullptr;
    };

    /// Reads packets and decodes them until `stop()` says enough, or EOF.
    const auto pump = [&](const std::function<bool()>& stop) {
        while (!stop() && av_read_frame(raw, packet.get()) >= 0) {
            if (packet->stream_index == video_index) {
                if (avcodec_send_packet(decode_ctx.get(), packet.get()) >= 0) {
                    harvest_video();
                }
            } else if (AudioProbe* probe = probe_for(packet->stream_index); probe != nullptr) {
                if (avcodec_send_packet(probe->context.get(), packet.get()) >= 0) {
                    harvest_audio(*probe);
                }
            }
            packet.unref();
        }
    };

    // ---------------------------------------------------------------------
    // SPEC.md §10.4: "the first and last frames decode" (BUG-040)
    // ---------------------------------------------------------------------
    // Head and tail, not the whole file. This gate used to decode every packet of both
    // streams, which made its cost proportional to the recording's length: measured in
    // the field, **14.27 s to finalize a 112-second recording**, all of it on the IPC
    // thread inside `stop_record`, which starved the host heartbeat and made the engine
    // exit mid-finalize (BUG-039). At half an hour it would have been minutes.
    //
    // The checks are unchanged -- stream count, decodable content at both ends, channel
    // layout, the AudioSpecificConfig, duration within 1%, and the two tracks against
    // each other. What changes is that the evidence comes from both ends rather than
    // from every frame in between, which is what §10.4 asks for in as many words.
    //
    // **A middle that fails to decode is what this no longer catches**, so it is worth
    // being plain about why that is an acceptable trade here rather than a hole: the
    // packets in the middle were written by the same muxer, in one continuous run, with
    // `write_p99_us` reported separately -- a file that is corrupt only in the middle is
    // a disk that lied about a write, and §10.4's answer to that is the `.fcrecover`
    // sidecar and the recovery path, not a full decode on every stop.
    //
    // With SPEC.md §8.6's Tier B the head probe waits for **every** track, not just
    // the first. A per-application track is silent far more often than the system
    // mix, and silence is still samples -- the timeline emits it -- so a track that
    // decodes nothing in the head window is a track that carries nothing at all,
    // which is exactly the ragged-track defect row 14 exists to catch.
    const auto head_done = [&] {
        if (report.decoded_frames == 0) {
            return false;
        }
        return std::ranges::all_of(audio_probes, [](const AudioProbe& probe) { return probe.samples > 0; });
    };
    pump(head_done);

    const std::int64_t head_frames = report.decoded_frames;
    const std::int64_t first_video_pts_head = first_video_pts;

    // The tail. Seeking backwards from just before the end lands on the keyframe at or
    // before it, and reading to EOF from there decodes the closing GOP.
    bool tail_decoded = false;
    const double container_seconds =
        raw->duration != AV_NOPTS_VALUE ? static_cast<double>(raw->duration) / AV_TIME_BASE : 0.0;
    if (container_seconds > kTailProbeSeconds) {
        const auto target = static_cast<std::int64_t>((container_seconds - kTailProbeSeconds) * AV_TIME_BASE);
        if (av_seek_frame(raw, -1, target, AVSEEK_FLAG_BACKWARD) >= 0) {
            avcodec_flush_buffers(decode_ctx.get());
            for (const AudioProbe& probe : audio_probes) {
                avcodec_flush_buffers(probe.context.get());
            }
            pump([] { return false; }); // to EOF
            tail_decoded = true;
        }
    }

    if (!tail_decoded) {
        // Either the file is shorter than the probe window, or the seek was refused.
        // Both fall back to reading the rest of it: correctness before speed, and a
        // short file costs nothing to read in full anyway.
        pump([] { return false; });
    }

    // Drain both decoders' buffers, or the last frames go uncounted.
    if (avcodec_send_packet(decode_ctx.get(), nullptr) >= 0) {
        harvest_video();
    }
    for (AudioProbe& probe : audio_probes) {
        if (avcodec_send_packet(probe.context.get(), nullptr) >= 0) {
            harvest_audio(probe);
        }
    }

    if (report.decoded_frames == 0) {
        report.detail = "the output contains no decodable frames";
        return report;
    }
    if (head_frames == 0) {
        report.detail = "the output's first frames do not decode";
        return report;
    }
    // Nothing decoded after the head probe, on a file long enough to have a tail. Stated
    // without reference to whether the seek succeeded, because both routes reach the end
    // of the file and both must produce frames: a tail that decodes to nothing is the
    // failure this half of §10.4 exists to catch, however we got there.
    if (report.decoded_frames == head_frames && container_seconds > kTailProbeSeconds) {
        report.detail = "the output's last frames do not decode";
        return report;
    }

    // The video track's own length, from decoded timestamps rather than from the
    // container's duration -- which for Matroska is the longest stream and is the
    // *audio* track whenever audio outlives video, as it does on any clean stop.
    if (first_video_pts_head != AV_NOPTS_VALUE && last_video_pts > first_video_pts_head) {
        const double timebase = av_q2d(raw->streams[video_index]->time_base);
        const double span = static_cast<double>(last_video_pts - first_video_pts_head) * timebase;
        // Plus one frame: a track's duration runs to the end of its last frame, not to
        // the start of it. Measured from the closing pair rather than averaged over a
        // count this no longer has -- and a measured interval is the right one anyway
        // once SPEC.md §13 rung 3 can change the rate mid-recording (BUG-025).
        report.video_duration_seconds = span + (static_cast<double>(last_video_step) * timebase);
    }

    for (AudioProbe& probe : audio_probes) {
        if (probe.samples == 0) {
            report.detail = "audio track " + std::to_string(probe.index) +
                            (probe.name.empty() ? std::string{} : " (" + probe.name + ")") +
                            " contains no decodable samples";
            return report;
        }
        // The track's *span*, head to tail, rather than a sum of the samples decoded --
        // which with head-and-tail probing would only ever be the probes themselves.
        if (probe.first_pts != AV_NOPTS_VALUE && probe.last_end_pts > probe.first_pts) {
            probe.duration_seconds = static_cast<double>(probe.last_end_pts - probe.first_pts) *
                                     av_q2d(raw->streams[probe.index]->time_base);
        } else if (probe.sample_rate > 0) {
            probe.duration_seconds = static_cast<double>(probe.samples) / probe.sample_rate;
        }

        ValidationReport::AudioTrackReport entry;
        entry.name = probe.name;
        entry.channels = probe.channels;
        entry.sample_rate = probe.sample_rate;
        entry.decoded_samples = probe.samples;
        entry.duration_seconds = probe.duration_seconds;
        report.audio_tracks.push_back(std::move(entry));
    }
    if (!audio_probes.empty()) {
        report.decoded_audio_samples = audio_probes.front().samples;
        report.audio_duration_seconds = audio_probes.front().duration_seconds;
    }

    // SPEC.md §8.6: "All tracks share one timebase, one epoch (`t0`), and identical
    // duration." Checked here as well as by §20 row 14's own assertion, because a
    // ragged track is the defect this gate is in the best position to see -- it has
    // every stream open already, and a recording whose tracks disagree by more than
    // a fragment is not a file to declare good.
    //
    // Held to the same tolerance the two Tier A tracks are: this gate answers "is
    // the recording usable", and row 14's 20 ms is the tighter question asked
    // against a signal whose position is known.
    for (std::size_t i = 1; i < audio_probes.size(); ++i) {
        const double lead = audio_probes.front().duration_seconds;
        const double other = audio_probes[i].duration_seconds;
        if (lead > 0.0 && other > 0.0 && std::abs(lead - other) > kTrackLengthToleranceSeconds) {
            report.detail = "audio track " + std::to_string(audio_probes[i].index) + " decodes to " +
                            std::to_string(other) + " s against the system mix's " + std::to_string(lead) + " s";
            return report;
        }
    }

    // Against the *video* track, because that is what a pacer-derived expectation
    // describes. Comparing it against the container's duration was wrong the
    // moment audio existed: the container reports the longer of the two streams,
    // and audio legitimately outlives video by the length of the stop sequence.
    const double measured =
        report.video_duration_seconds > 0.0 ? report.video_duration_seconds : report.duration_seconds;
    if (expectation.duration_seconds > 0.0 && measured > 0.0) {
        const double drift = std::abs(measured - expectation.duration_seconds);
        if (drift > expectation.duration_seconds * 0.01) {
            report.detail = "the video track is " + std::to_string(measured) + " s against an expected " +
                            std::to_string(expectation.duration_seconds) + " s, a difference of more than 1%";
            return report;
        }
    }

    // The two tracks against each other, both measured from what actually decoded
    // rather than from the container's own metadata -- the metadata is written by
    // the same code that would have got the timestamps wrong.
    if (report.audio_duration_seconds > 0.0 && report.video_duration_seconds > 0.0 &&
        std::abs(report.audio_duration_seconds - report.video_duration_seconds) > kTrackLengthToleranceSeconds) {
        report.detail = "the audio track decodes to " + std::to_string(report.audio_duration_seconds) +
                        " s against a video track of " + std::to_string(report.video_duration_seconds) + " s";
        return report;
    }

    report.valid = true;
    report.detail = "ok";
    return report;
}

} // namespace fc::mux

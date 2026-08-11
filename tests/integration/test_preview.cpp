// SPEC.md §15.2 -- the preview channel, and M9's other half.
//
// GPU TIER.
//
//   > Named **shared memory** (`CreateFileMapping`) triple-buffered ring [...]
//   > Engine writes a **downscaled preview** (default 960x540, 30 fps, BGRA) -- **never**
//   > full-resolution frames. The preview path must be independently droppable: if the GUI
//   > is slow, preview frames are skipped with **zero effect on the recording**.
//   > Preview generation is a separate GPU shader dispatch off the same source texture,
//   > adding < 0.2 ms/frame.
//
// §20 has no row for the preview, and M9's exit criterion names only the splits -- so, as
// with the segmentation half, the assertions below are §15.2's own sentences turned one at
// a time into something the running engine can be asked. That provenance is recorded in
// docs/ACCEPTANCE.md because nothing else pins it.
//
// ---------------------------------------------------------------------------
// What each case is guarding against, since a preview test is easy to write badly
// ---------------------------------------------------------------------------
// A preview test that drove `PreviewRing` directly would prove the ring and nothing about
// the preview. So every case here goes through `RecordingSession` -- the real capture
// thread, the real dispatch off the real source texture, the real readback -- and reads
// what comes out through `PreviewReader`, which is a *different process's* view expressed
// in-process.
//
// And a "zero effect on the recording" claim asserted with a preview that kept up would
// prove nothing at all: it would measure the case where there is nothing to be robust to.
// So `ARecordingIsUnaffectedByAPreviewThatCannotKeepUp` runs the preview deliberately
// wedged, asserts that it *did* fail -- `dropped_no_slot > 0` -- and only then asserts the
// recording is intact. That control is the whole point of the case; without it, the
// comparison is between two healthy runs.

#include "core/capture/source_resolver.h"
#include "core/logging/logger.h"
#include "core/pipeline/recording_session.h"
#include "core/preview/preview_ring.h"
#include "core/preview/preview_scaler.h"
#include "core/timing/qpc_clock.h"

#include "adapter_device.h"
#include "decoded_media.h"
#include "synthetic_source.h"
#include "temp_dir.h"

#include <windows.h>
// Must follow windows.h.
#include <d3d11.h>
#include <wrl/client.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using fc::pipeline::RecordingSession;
using fc::pipeline::SessionSettings;
using fc::pipeline::SessionStats;
using fc::preview::PreviewGeometry;
using fc::preview::PreviewReader;
using fc::preview::PreviewRing;
using Microsoft::WRL::ComPtr;

/// What the synthetic source is asked to produce for one case.
///
/// Per-case rather than one set of constants, because the cases want different things and
/// one configuration cannot serve both -- see `kDecimation` and `kComparison` below.
struct SourceConfig {
    int width = 1920;
    int height = 1080;
    int fps = 60;
};

/// For the case that has to show 60 fps of capture becoming 30 fps of preview. Full
/// 1080p60, because that is the configuration §15.2's rate limiter exists for.
constexpr SourceConfig kDecimation{1920, 1080, 60};

/// For the cases that compare a recording against itself with the preview off, on, and
/// wedged.
///
/// **720p30 deliberately, and it is the fixture that decided it, not the product.** These
/// cases assert exact equality of the recording's counters across three runs, which is only
/// meaningful if the recording is comfortable in all three -- and at 1080p60 on an
/// unoptimised build it is not. Measured on the Debug preset: the same 300 frames took
/// **13.5 s instead of 5 s**, the CFR pacer duplicate-filled 400 slots, and the stall
/// detector rebuilt the capture stack **three times with the preview switched off**. That
/// is `synthetic_source.h`'s own warning about `prerendered_frames = 0` ("a 1080p CPU
/// pattern fill plus a `Map` and a row-by-row copy on every call"), which BUG-027 and
/// BUG-032 both record, arriving a third time.
///
/// `prerendered_frames` stays 0 regardless: every frame needs a unique barcode or
/// `frames_absent_from` -- the only check that can see BUG-044 -- cannot work at all. So
/// the render cost comes down instead: 720p is 44% of the pixels and 30 fps doubles the
/// budget, which is ~4.5x the headroom and the configuration `SegmentationTest` already
/// runs on every preset.
constexpr SourceConfig kComparison{1280, 720, 30};

/// Enough frames to be a recording rather than a burst, short enough that three of them fit
/// in a routine tier run: five seconds at 60 fps, ten at 30.
constexpr std::uint32_t kFrames = 300;

/// §15.2's budget for the dispatch.
constexpr double kDispatchBudgetMs = 0.2;

/// One BGRA pixel out of a preview slot. The memory order is B, G, R, A -- which is what
/// the whole channel-order argument in `downscale_preview.hlsl` is about, so it is spelled
/// out here rather than hidden behind a struct.
struct Bgra {
    int b = 0;
    int g = 0;
    int r = 0;
};

[[nodiscard]] Bgra pixel_at(const std::uint8_t* pixels, const PreviewGeometry& geometry, int x, int y) {
    const std::uint8_t* p =
        pixels + (static_cast<std::size_t>(y) * geometry.stride()) + (static_cast<std::size_t>(x) * 4);
    return Bgra{p[0], p[1], p[2]};
}

class PreviewTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        dir_ = std::make_unique<fc::test::TempDir>("preview");
        fc::log::Config config;
        config.directory = dir_->path() / "logs";
        config.session_id = "preview00001";
        config.enable_msvc_sink = false;
        ASSERT_TRUE(fc::log::init(config).has_value());
    }

    static void TearDownTestSuite() {
        fc::log::shutdown();
        dir_.reset();
    }

    /// A session fed by the synthetic source, exactly as `WatchdogRecoveryTest` does and
    /// for the same reason (CLAUDE.md §5): a preview of whatever happens to be on the
    /// developer's desktop is not something a test can assert about.
    static SessionSettings settings_for(const char* name, const SourceConfig& source_config) {
        SessionSettings settings;
        settings.output = dir_->path() / (std::string{name} + ".mkv");
        settings.video.width = source_config.width;
        settings.video.height = source_config.height;
        settings.video.fps = source_config.fps;
        settings.video.container = fc::config::Container::Mkv;

        if (const auto display = fc::capture::primary_display(); display.has_value()) {
            settings.target.monitor = display.value().monitor;
        }

        settings.capture_factory =
            [source_config](ID3D11Device* device) -> fc::Result<std::unique_ptr<fc::capture::IScreenCapture>> {
            fc::test::SyntheticSource::Settings source;
            source.width = source_config.width;
            source.height = source_config.height;
            source.fps = source_config.fps;
            source.frame_limit = kFrames;
            // Paced, because `RecordingSession`'s capture loop relies on the backend to
            // pace it -- see the note on `pace_to_real_time`.
            source.pace_to_real_time = true;
            // Deliberately 0: every frame gets a unique barcode, so "no frame was lost"
            // stays assertable on the file (SPEC.md §20 row 5).
            source.prerendered_frames = 0;

            auto created = std::make_unique<fc::test::SyntheticSource>();
            FC_TRY(created->configure(source));
            FC_TRY(created->start(device, fc::capture::CaptureTarget{}));
            return std::unique_ptr<fc::capture::IScreenCapture>{std::move(created)};
        };
        return settings;
    }

    struct Run {
        SessionStats stats;
        std::int64_t capture_elapsed_ms = 0;
        std::filesystem::path output;
        bool valid = false;
        /// Frames a reader actually observed while the recording ran.
        std::uint64_t observed_by_reader = 0;
    };

    /// Records `kFrames` and returns what happened, with the preview configured as asked.
    ///
    /// `reader` may be null, which is the "no GUI attached" case §15.2 has to survive.
    static Run record(const char* name, const SourceConfig& source_config, PreviewRing* ring, std::int64_t stall_ns,
                      PreviewReader* reader) {
        Run run;
        SessionSettings settings = settings_for(name, source_config);
        run.output = settings.output;
        if (ring != nullptr) {
            settings.preview = ring;
            settings.preview_enabled = true;
            settings.preview_settings.injected_publish_stall_ns = stall_ns;
        }

        RecordingSession session;
        const auto started = session.start(settings);
        if (!started.has_value()) {
            ADD_FAILURE() << "the session would not start: " << fc::error_name(started.error());
            return run;
        }

        const std::int64_t began_ns = fc::timing::qpc_now_ns();
        std::uint64_t last_seen = 0;
        // Wait for the source to run out, polling the reader as a GUI would.
        const std::int64_t deadline_ns = began_ns + (30LL * 1'000'000'000LL);
        while (fc::timing::qpc_now_ns() < deadline_ns) {
            if (session.stats().frames_captured >= kFrames) {
                break;
            }
            if (reader != nullptr) {
                if (const auto view = reader->latest(); view.has_value() && view.value().sequence != last_seen) {
                    last_seen = view.value().sequence;
                    ++run.observed_by_reader;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        run.capture_elapsed_ms = (fc::timing::qpc_now_ns() - began_ns) / 1'000'000;
        run.stats = session.stats();

        const auto report = session.stop();
        if (!report.has_value()) {
            ADD_FAILURE() << "finalization failed: " << fc::error_name(report.error());
            return run;
        }
        run.valid = report.value().valid;
        return run;
    }

    /// Frames whose barcode does not follow its predecessor in `path`.
    ///
    /// **This is the assertion that caught BUG-044, and it is the only one that could
    /// have.** With the preview running, the recording decoded with corrupted frames while
    /// every counter the pipeline keeps said the recording was perfect -- `frames_encoded`
    /// 300, `frames_queue_dropped` 0, `frames_paced_out` 0, `duplicates_emitted` 0,
    /// identical to the preview-off run. Nothing had been dropped. The frames had been
    /// *written wrong*, because the preview's compute dispatch and the colour converter's
    /// were interleaving on one immediate context and each was executing with the other's
    /// bindings.
    ///
    /// So the number this returns is read out of the decoded picture, not out of a
    /// counter: a barcode that jumps is a frame whose top-left corner is not the frame's
    /// own. Measured 11 and 84 across two runs before `gpu::ScopedDeviceLock`, and 0 in
    /// every configuration after. The same lesson as `SoftwareEncoderTest`'s readback
    /// check, arriving from a different direction.
    [[nodiscard]] static int frames_absent_from(const std::filesystem::path& path, const char* label) {
        fc::test::DecodedMedia media;
        fc::test::decode_media(path, fc::test::DecodeOptions{}, media);
        EXPECT_TRUE(media.opened) << label << ": " << media.detail;
        EXPECT_GT(media.video.frame_count, 0) << label << " decoded no frames";

        int missing = 0;
        std::optional<std::uint32_t> previous;
        for (const std::optional<std::uint32_t>& barcode : media.video.barcodes) {
            if (!barcode.has_value()) {
                continue;
            }
            if (previous.has_value() && *barcode > *previous + 1) {
                missing += static_cast<int>(*barcode - *previous - 1);
            }
            previous = barcode;
        }
        return missing;
    }

    static std::unique_ptr<fc::test::TempDir> dir_;
};

std::unique_ptr<fc::test::TempDir> PreviewTest::dir_;

// ---------------------------------------------------------------------------
// The channel, end to end
// ---------------------------------------------------------------------------

// Frames from a real capture session reach shared memory, downscaled, in BGRA, at the rate
// §15.2 specifies -- and are the *picture*, not a plausible-looking buffer.
//
// The content assertions are the ones that matter and the ones a weaker test would omit. A
// preview that published all-zero frames would satisfy "960x540 at 30 fps" perfectly.
TEST_F(PreviewTest, ARecordingPublishesDownscaledFramesTheGuiCanRead) {
    PreviewRing ring;
    ASSERT_TRUE(ring.create(fc::preview::preview_section_name("gputest-live"), PreviewGeometry{}).has_value());

    PreviewReader reader;
    ASSERT_TRUE(reader.open(fc::preview::preview_section_name("gputest-live")).has_value());

    const Run run = record("live", kDecimation, &ring, 0, &reader);
    ASSERT_TRUE(run.valid) << "the recording did not validate";
    ASSERT_GE(run.stats.frames_captured, kFrames);

    // --- the geometry §15.2 specifies ----------------------------------------
    EXPECT_EQ(reader.geometry().width, 960);
    EXPECT_EQ(reader.geometry().height, 540);
    EXPECT_LT(reader.geometry().slot_bytes(), static_cast<std::size_t>(kDecimation.width) * kDecimation.height * 4)
        << "§15.2: never full-resolution frames";

    // --- the rate ------------------------------------------------------------
    // 300 source frames at 60 fps is five seconds, so a 30 fps preview publishes ~150. The
    // band is wide on purpose: this is asserting that the rate limiter exists and targets
    // 30, not that the machine was idle.
    const std::uint64_t published = run.stats.preview.published;
    ASSERT_GT(published, 0U) << "no preview frame was ever published";
    EXPECT_GT(published, kFrames / 4) << "published " << published << " of ~" << kFrames / 2 << " expected";
    EXPECT_LT(published, static_cast<std::uint64_t>(kFrames) * 3 / 4)
        << "published " << published << ", which is closer to the capture rate than to §15.2's 30 fps";
    EXPECT_GT(run.stats.preview.rate_limited, 0U) << "nothing was rate-limited, so 60 fps reached the ring";

    // --- and a GUI saw them, one at a time -----------------------------------
    EXPECT_GT(run.observed_by_reader, 0U) << "the ring published but nothing was observable through a reader";

    // --- the content ---------------------------------------------------------
    const auto view = reader.latest();
    ASSERT_TRUE(view.has_value());
    ASSERT_TRUE(reader.still_valid(view.value().sequence));

    const PreviewGeometry& geometry = reader.geometry();
    const int row = geometry.height / 2; // well below the barcode, inside the SMPTE bars
    const int bar_width = geometry.width / 7;

    // Not uniform. The single assertion that a black or garbage buffer fails.
    int distinct_bars = 0;
    Bgra previous{-1, -1, -1};
    for (int bar = 0; bar < 7; ++bar) {
        const Bgra sample = pixel_at(view.value().pixels, geometry, (bar * bar_width) + (bar_width / 2), row);
        if (sample.b != previous.b || sample.g != previous.g || sample.r != previous.r) {
            ++distinct_bars;
        }
        previous = sample;
    }
    EXPECT_GE(distinct_bars, 6) << "the preview shows fewer than six distinct colour bars; it is not the picture";

    // The channel order, which nothing else in the tree would catch. SMPTE bar 5 is red
    // (r=191, g=0, b=0) and bar 6 is blue. A BGRA/RGBA swap anywhere in the shader's store,
    // the readback or the `QImage` format makes these two exchange places -- and the result
    // still looks like a colour-bar pattern, which is why it has to be asserted by name.
    const Bgra red_bar = pixel_at(view.value().pixels, geometry, (5 * bar_width) + (bar_width / 2), row);
    const Bgra blue_bar = pixel_at(view.value().pixels, geometry, (6 * bar_width) + (bar_width / 2), row);
    EXPECT_GT(red_bar.r, red_bar.b + 60) << "SMPTE bar 5 is red; it read as b=" << red_bar.b << " r=" << red_bar.r
                                         << " -- the channels are swapped";
    EXPECT_GT(blue_bar.b, blue_bar.r + 60)
        << "SMPTE bar 6 is blue; it read as b=" << blue_bar.b << " r=" << blue_bar.r << " -- the channels are swapped";

    std::printf("[M9 preview] %llu captured -> %llu published, %llu rate-limited, %llu observed by a reader; "
                "offer worst %lld us, mean %lld us\n",
                static_cast<unsigned long long>(run.stats.frames_captured), static_cast<unsigned long long>(published),
                static_cast<unsigned long long>(run.stats.preview.rate_limited),
                static_cast<unsigned long long>(run.observed_by_reader),
                static_cast<long long>(run.stats.preview.worst_offer_ns / 1000),
                static_cast<long long>(run.stats.preview.mean_offer_ns() / 1000));
}

// ---------------------------------------------------------------------------
// §15.2's measured requirement
// ---------------------------------------------------------------------------

// "Preview generation is a separate GPU shader dispatch off the same source texture, adding
// < 0.2 ms/frame."
//
// ---------------------------------------------------------------------------
// How this is measured, and the two ways the first version got it wrong
// ---------------------------------------------------------------------------
// On the GPU's own clock, with timestamp queries, not the CPU's: `Dispatch` returns as soon
// as the command is recorded, so a CPU stopwatch would report the cost of writing a command
// buffer and call it a shader.
//
// And **back to back inside one disjoint block**, not one dispatch per full CPU-GPU sync.
// The first version spun on `GetData` after every dispatch, which drains the pipeline and
// lets the GPU drop a power state between samples: it reported a median of 0.237 ms with a
// floor of 0.179 ms and a spread out to 0.45 ms, which is a measurement of a GPU
// repeatedly waking up rather than of a preview running alongside a recording. A preview
// dispatch never happens in isolation -- there is a capture, a colour conversion and an
// encode on the same queue -- so the steady-state figure is the honest one.
//
// The recording rig has two adapters, and which one this runs on is not a detail: §5.2
// encodes where the pixels already live, which on this MUX-less laptop is the iGPU, and an
// iGPU reads its source texture across the same memory bus as the CPU. Both are measured
// and both are reported.
TEST_F(PreviewTest, TheDispatchStaysInsideTheBudgetFifteenTwoStates) {
    fc::gpu::GpuTopologyService topology;
    ASSERT_TRUE(topology.refresh().has_value());

    struct Measurement {
        std::string adapter;
        bool integrated = false;
        double min_ms = 0.0;
        double median_ms = 0.0;
        double p99_ms = 0.0;
        double max_ms = 0.0;
    };

    std::vector<Measurement> measurements;

    for (const fc::gpu::AdapterInfo& adapter : topology.topology().adapters) {
        if (adapter.adapter_class == fc::gpu::AdapterClass::Software) {
            continue;
        }
        auto created = fc::test::device_for_adapter(adapter.id);
        if (!created.has_value()) {
            continue;
        }
        const fc::gpu::D3dDevice device{std::move(created).value()};

        fc::test::SyntheticSource::Settings source_settings;
        source_settings.width = kDecimation.width;
        source_settings.height = kDecimation.height;
        source_settings.fps = kDecimation.fps;
        source_settings.prerendered_frames = 8;
        fc::test::SyntheticSource source;
        ASSERT_TRUE(source.configure(source_settings).has_value());
        ASSERT_TRUE(source.start(device.device(), fc::capture::CaptureTarget{}).has_value());

        fc::preview::PreviewScaler scaler;
        ASSERT_TRUE(scaler.initialize(device.device(), fc::preview::PreviewScalerSettings{}).has_value());

        ID3D11DeviceContext* context = device.context();

        constexpr int kWarmup = 60;
        constexpr int kSamples = 200;

        // The frames are collected up front so the measured loop does nothing but dispatch.
        std::vector<ID3D11Texture2D*> textures;
        std::vector<fc::capture::CaptureFrame> frames;
        for (int i = 0; i < kWarmup + kSamples; ++i) {
            auto frame = source.acquire(std::chrono::milliseconds{1000});
            ASSERT_TRUE(frame.has_value()) << "the synthetic source stopped at " << i;
            frames.push_back(frame.value());
            textures.push_back(static_cast<ID3D11Texture2D*>(frame.value().texture));
        }

        for (int i = 0; i < kWarmup; ++i) {
            ASSERT_TRUE(scaler.dispatch(context, textures[static_cast<std::size_t>(i)]).has_value());
        }
        context->Flush();

        D3D11_QUERY_DESC disjoint_desc{};
        disjoint_desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        D3D11_QUERY_DESC stamp_desc{};
        stamp_desc.Query = D3D11_QUERY_TIMESTAMP;

        ComPtr<ID3D11Query> disjoint;
        ASSERT_EQ(device.device()->CreateQuery(&disjoint_desc, &disjoint), S_OK);
        std::vector<ComPtr<ID3D11Query>> stamps(kSamples + 1);
        for (ComPtr<ID3D11Query>& stamp : stamps) {
            ASSERT_EQ(device.device()->CreateQuery(&stamp_desc, &stamp), S_OK);
        }

        context->Begin(disjoint.Get());
        context->End(stamps[0].Get());
        for (int i = 0; i < kSamples; ++i) {
            ASSERT_TRUE(scaler.dispatch(context, textures[static_cast<std::size_t>(kWarmup + i)]).has_value());
            context->End(stamps[static_cast<std::size_t>(i) + 1].Get());
        }
        context->End(disjoint.Get());
        context->Flush();

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint_data{};
        while (context->GetData(disjoint.Get(), &disjoint_data, sizeof(disjoint_data), 0) != S_OK) {
            std::this_thread::sleep_for(std::chrono::microseconds{200});
        }
        // A disjoint interval means the GPU clock changed underneath the measurement.
        // Reported as a skip rather than averaged in: §15.2's budget deserves a real number
        // or none at all.
        if (disjoint_data.Disjoint != 0 || disjoint_data.Frequency == 0) {
            for (const fc::capture::CaptureFrame& frame : frames) {
                source.release(frame);
            }
            source.stop();
            ADD_FAILURE() << adapter.description << ": the GPU clock was disjoint across the whole run";
            continue;
        }

        std::vector<std::uint64_t> ticks(stamps.size());
        for (std::size_t i = 0; i < stamps.size(); ++i) {
            while (context->GetData(stamps[i].Get(), &ticks[i], sizeof(ticks[i]), 0) != S_OK) {
                std::this_thread::sleep_for(std::chrono::microseconds{50});
            }
        }
        for (const fc::capture::CaptureFrame& frame : frames) {
            source.release(frame);
        }
        source.stop();

        std::vector<double> milliseconds;
        milliseconds.reserve(static_cast<std::size_t>(kSamples));
        for (std::size_t i = 1; i < ticks.size(); ++i) {
            milliseconds.push_back(static_cast<double>(ticks[i] - ticks[i - 1]) * 1000.0 /
                                   static_cast<double>(disjoint_data.Frequency));
        }
        std::ranges::sort(milliseconds);

        Measurement measurement;
        measurement.adapter = adapter.description;
        measurement.integrated = adapter.adapter_class == fc::gpu::AdapterClass::Integrated;
        measurement.min_ms = milliseconds.front();
        measurement.median_ms = milliseconds[milliseconds.size() / 2];
        measurement.p99_ms = milliseconds[(milliseconds.size() * 99) / 100];
        measurement.max_ms = milliseconds.back();
        measurements.push_back(measurement);
    }

    ASSERT_FALSE(measurements.empty()) << "no adapter produced a measurement";

    for (const Measurement& measurement : measurements) {
        std::printf("[M9 preview] dispatch %dx%d -> 960x540 on %s (%s): min %.4f ms, median %.4f ms, "
                    "p99 %.4f ms, max %.4f ms (budget %.2f ms)\n",
                    kDecimation.width, kDecimation.height, measurement.adapter.c_str(),
                    measurement.integrated ? "integrated" : "discrete", measurement.min_ms, measurement.median_ms,
                    measurement.p99_ms, measurement.max_ms, kDispatchBudgetMs);
    }

    // The median of a back-to-back run, on every adapter the engine could select. Not the
    // max: a single long sample on a GPU that is also compositing a desktop is a scheduling
    // artefact, and §15.2's "adding < 0.2 ms/frame" is a statement about what a frame costs,
    // not about the worst instant the machine ever had.
    for (const Measurement& measurement : measurements) {
        EXPECT_LT(measurement.median_ms, kDispatchBudgetMs)
            << measurement.adapter << " exceeds SPEC.md §15.2's 0.2 ms preview dispatch budget";
    }
}

// ---------------------------------------------------------------------------
// "zero effect on the recording"
// ---------------------------------------------------------------------------

// Three recordings of the same 300 synthetic frames: preview off, preview on with a reader
// keeping up, and preview on deliberately wedged with no reader at all.
//
// **The control comes first.** The wedged run has to be shown to have actually failed --
// `dropped_no_slot > 0` -- before "the recording is unaffected" means anything. A version
// of this case without that line would compare three healthy runs and report success.
TEST_F(PreviewTest, ARecordingIsUnaffectedByAPreviewThatCannotKeepUp) {
    const Run without = record("preview_off", kComparison, nullptr, 0, nullptr);
    ASSERT_TRUE(without.valid);

    PreviewRing healthy_ring;
    ASSERT_TRUE(
        healthy_ring.create(fc::preview::preview_section_name("gputest-healthy"), PreviewGeometry{}).has_value());
    PreviewReader reader;
    ASSERT_TRUE(reader.open(fc::preview::preview_section_name("gputest-healthy")).has_value());
    const Run with = record("preview_on", kComparison, &healthy_ring, 0, &reader);
    ASSERT_TRUE(with.valid);

    // 120 ms per frame against a 33 ms preview period: the single hand-off slot saturates
    // within the first few frames and stays saturated. No reader is attached either, which
    // is the other half of "if the GUI is slow" -- there is not even a GUI.
    PreviewRing wedged_ring;
    ASSERT_TRUE(wedged_ring.create(fc::preview::preview_section_name("gputest-wedged"), PreviewGeometry{}).has_value());
    const Run wedged = record("preview_wedged", kComparison, &wedged_ring, 120'000'000, nullptr);
    ASSERT_TRUE(wedged.valid);

    // --- the control ---------------------------------------------------------
    ASSERT_GT(wedged.stats.preview.dropped_no_slot, 0U)
        << "the wedged preview kept up, so this case compared three healthy runs and proved nothing";
    EXPECT_LT(wedged.stats.preview.published, with.stats.preview.published)
        << "the wedged preview published as much as the healthy one";

    // --- and now the claim ---------------------------------------------------
    // Every counter that would move if the preview cost the recording anything: frames
    // that never arrived, frames the bounded queue had to discard, stages that failed, and
    // a capture stack that had to be rebuilt. All of them are exact rather than
    // approximate, because "zero effect" is a claim that admits exact answers.
    int missing[3] = {0, 0, 0};
    int index = 0;
    for (const auto& [label, run] :
         {std::pair{"preview off", &without}, std::pair{"preview on", &with}, std::pair{"preview wedged", &wedged}}) {
        EXPECT_GE(run->stats.frames_captured, kFrames) << label << " did not capture every source frame";
        EXPECT_EQ(run->stats.pipeline.frames_queue_dropped, 0U) << label << " dropped frames under queue pressure";
        EXPECT_EQ(run->stats.pipeline.convert_failures, 0U) << label;
        EXPECT_EQ(run->stats.pipeline.encode_failures, 0U) << label;
        EXPECT_EQ(run->stats.rebuilds, 0U) << label << " rebuilt the capture stack";
        missing[index] = frames_absent_from(run->output, label);
        EXPECT_EQ(missing[index], 0) << label << ": " << missing[index]
                                     << " frames in the file are not the frames that produced them (BUG-044)";
        ++index;
    }

    // The capture thread is the one place a per-frame cost turns into a slower recording,
    // so the wall clock of the same 300 paced frames is what would show it. 10% is generous
    // against a five-second run on a machine also running a desktop; the measured spread is
    // reported either way, and it is the number to compare against when this changes.
    const double drift = static_cast<double>(wedged.capture_elapsed_ms - without.capture_elapsed_ms) /
                         static_cast<double>(std::max<std::int64_t>(without.capture_elapsed_ms, 1));
    EXPECT_LT(drift, 0.10) << "the wedged preview slowed capture by " << (drift * 100.0) << "%";

    std::printf("[M9 preview] recording with preview off / on / wedged:\n"
                "             captured   %llu / %llu / %llu\n"
                "             encoded    %llu / %llu / %llu\n"
                "             qdropped   %llu / %llu / %llu\n"
                "             paced_out  %llu / %llu / %llu\n"
                "             duplicates %llu / %llu / %llu\n"
                "             absent     %d / %d / %d  (source frames merged by the CFR grid)\n"
                "             elapsed_ms %lld / %lld / %lld\n"
                "             preview published %llu / %llu / %llu, no-slot drops %llu / %llu / %llu\n"
                "             offer worst_us %lld / %lld / %lld, mean_us %lld / %lld / %lld\n",
                static_cast<unsigned long long>(without.stats.frames_captured),
                static_cast<unsigned long long>(with.stats.frames_captured),
                static_cast<unsigned long long>(wedged.stats.frames_captured),
                static_cast<unsigned long long>(without.stats.pipeline.frames_encoded),
                static_cast<unsigned long long>(with.stats.pipeline.frames_encoded),
                static_cast<unsigned long long>(wedged.stats.pipeline.frames_encoded),
                static_cast<unsigned long long>(without.stats.pipeline.frames_queue_dropped),
                static_cast<unsigned long long>(with.stats.pipeline.frames_queue_dropped),
                static_cast<unsigned long long>(wedged.stats.pipeline.frames_queue_dropped),
                static_cast<unsigned long long>(without.stats.pipeline.frames_paced_out),
                static_cast<unsigned long long>(with.stats.pipeline.frames_paced_out),
                static_cast<unsigned long long>(wedged.stats.pipeline.frames_paced_out),
                static_cast<unsigned long long>(without.stats.pipeline.duplicates_emitted),
                static_cast<unsigned long long>(with.stats.pipeline.duplicates_emitted),
                static_cast<unsigned long long>(wedged.stats.pipeline.duplicates_emitted), missing[0], missing[1],
                missing[2], static_cast<long long>(without.capture_elapsed_ms),
                static_cast<long long>(with.capture_elapsed_ms), static_cast<long long>(wedged.capture_elapsed_ms),
                static_cast<unsigned long long>(without.stats.preview.published),
                static_cast<unsigned long long>(with.stats.preview.published),
                static_cast<unsigned long long>(wedged.stats.preview.published),
                static_cast<unsigned long long>(without.stats.preview.dropped_no_slot),
                static_cast<unsigned long long>(with.stats.preview.dropped_no_slot),
                static_cast<unsigned long long>(wedged.stats.preview.dropped_no_slot),
                static_cast<long long>(without.stats.preview.worst_offer_ns / 1000),
                static_cast<long long>(with.stats.preview.worst_offer_ns / 1000),
                static_cast<long long>(wedged.stats.preview.worst_offer_ns / 1000),
                static_cast<long long>(without.stats.preview.mean_offer_ns() / 1000),
                static_cast<long long>(with.stats.preview.mean_offer_ns() / 1000),
                static_cast<long long>(wedged.stats.preview.mean_offer_ns() / 1000));
}

// ---------------------------------------------------------------------------
// The negative control for the whole feature
// ---------------------------------------------------------------------------

// A session given no ring dispatches nothing. Without this, every assertion above would
// still pass on a build that previewed unconditionally, and CLAUDE.md hard rule 7's shape
// -- a feature that runs when nobody asked for it -- would have arrived one section over.
TEST_F(PreviewTest, ASessionWithNoPreviewNeverDispatchesOne) {
    const Run run = record("no_preview", kComparison, nullptr, 0, nullptr);
    ASSERT_TRUE(run.valid);
    EXPECT_EQ(run.stats.preview.offered, 0U);
    EXPECT_EQ(run.stats.preview.dispatched, 0U);
    EXPECT_EQ(run.stats.preview.published, 0U);
}

// SPEC.md §15.1 makes `start_preview` and `start_record` independent, so a preview can be
// asked for after a recording is already running -- and withdrawn while it still is. Both
// are ordinary states, not errors, and the recording must not notice either.
TEST_F(PreviewTest, ThePreviewCanBeArmedAndDisarmedWhileTheRecordingRuns) {
    PreviewRing ring;
    ASSERT_TRUE(ring.create(fc::preview::preview_section_name("gputest-toggle"), PreviewGeometry{}).has_value());

    SessionSettings settings = settings_for("toggle", kComparison);
    settings.preview = &ring;
    settings.preview_enabled = false; // recording starts with no preview

    RecordingSession session;
    ASSERT_TRUE(session.start(settings).has_value());

    const auto wait_for_frames = [&session](std::uint64_t target) {
        const std::int64_t deadline = fc::timing::qpc_now_ns() + (15LL * 1'000'000'000LL);
        while (fc::timing::qpc_now_ns() < deadline && session.stats().frames_captured < target) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    };

    wait_for_frames(60);
    ASSERT_EQ(session.stats().preview.offered, 0U) << "a disarmed preview dispatched anyway";

    session.set_preview_enabled(true);
    wait_for_frames(180);
    const SessionStats armed = session.stats();
    EXPECT_GT(armed.preview.published, 0U) << "arming the preview mid-recording published nothing";

    session.set_preview_enabled(false);
    // One capture-loop iteration is enough for the request to be picked up; give it a
    // generous margin and then check nothing further is published.
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    const std::uint64_t after_disarm = session.stats().preview.published;
    wait_for_frames(kFrames);
    EXPECT_EQ(session.stats().preview.published, after_disarm) << "the preview kept publishing after being disarmed";

    const auto report = session.stop();
    ASSERT_TRUE(report.has_value());
    EXPECT_TRUE(report.value().valid) << report.value().detail;

    // The recording survived two mid-flight preview transitions -- each of which builds or
    // tears down a shader, two staging surfaces and a thread on the capture thread itself.
    // That it still validated and dropped nothing is the assertion; see
    // `frames_absent_from` for why the barcode sequence is reported rather than required.
    const SessionStats final_stats = session.stats();
    EXPECT_EQ(final_stats.pipeline.frames_queue_dropped, 0U);
    EXPECT_EQ(final_stats.rebuilds, 0U);
    const int missing = frames_absent_from(settings.output, "toggled preview");
    EXPECT_EQ(missing, 0) << missing << " frames in the file are not the frames that produced them (BUG-044)";
    std::printf("[M9 preview] arm/disarm mid-recording: captured %llu, encoded %llu, qdropped %llu, "
                "absent %d, preview published %llu\n",
                static_cast<unsigned long long>(final_stats.frames_captured),
                static_cast<unsigned long long>(final_stats.pipeline.frames_encoded),
                static_cast<unsigned long long>(final_stats.pipeline.frames_queue_dropped), missing,
                static_cast<unsigned long long>(final_stats.preview.published));
}

} // namespace

#include "core/preview/preview_ring.h"

#include "core/error/hresult.h"
#include "core/logging/logger.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <utility>

namespace fc::preview {
namespace {

/// A mapped section, unmapped and closed however the owner is destroyed.
///
/// Written out rather than reached for from a library because the two handles have to be
/// released in order -- the view before the section -- and a `unique_ptr` with two deleters
/// is more machinery than the four lines it replaces.
class MappedSection {
public:
    MappedSection() = default;

    ~MappedSection() {
        reset();
    }

    MappedSection(const MappedSection&) = delete;
    MappedSection& operator=(const MappedSection&) = delete;
    MappedSection(MappedSection&&) = delete;
    MappedSection& operator=(MappedSection&&) = delete;

    void reset() {
        if (base_ != nullptr) {
            ::UnmapViewOfFile(base_);
            base_ = nullptr;
        }
        if (section_ != nullptr) {
            ::CloseHandle(section_);
            section_ = nullptr;
        }
        bytes_ = 0;
    }

    void adopt(HANDLE section, void* base, std::size_t bytes) noexcept {
        section_ = section;
        base_ = base;
        bytes_ = bytes;
    }

    [[nodiscard]] std::uint8_t* base() const noexcept {
        return static_cast<std::uint8_t*>(base_);
    }

    [[nodiscard]] std::size_t bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] bool mapped() const noexcept {
        return base_ != nullptr;
    }

private:
    HANDLE section_ = nullptr;
    void* base_ = nullptr;
    std::size_t bytes_ = 0;
};

[[nodiscard]] std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

/// Little-endian load/store helpers over the mapped header.
///
/// `std::atomic_ref` for the write index specifically, and plain copies for everything
/// else: the header's descriptive fields are written once before the section is ever
/// published and never change, so an atomic on each of them would cost ordering for a value
/// that has none to establish.
void store_u32(std::uint8_t* base, std::size_t offset, std::uint32_t value) noexcept {
    std::memcpy(base + offset, &value, sizeof(value));
}

[[nodiscard]] std::uint32_t load_u32(const std::uint8_t* base, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

void store_u64(std::uint8_t* base, std::size_t offset, std::uint64_t value) noexcept {
    std::memcpy(base + offset, &value, sizeof(value));
}

[[nodiscard]] std::uint64_t load_u64(const std::uint8_t* base, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

/// The write index, as an atomic over shared memory.
///
/// `std::atomic_ref` rather than an `std::atomic<std::uint64_t>` placed into the mapping:
/// the mapping is memory two processes agree on the *layout* of, and placement-newing a
/// C++ object into it would make that layout a property of one compiler's atomic
/// representation. `atomic_ref` promises the object's representation is unchanged, which is
/// exactly the property a cross-process header needs.
[[nodiscard]] std::atomic_ref<std::uint64_t> write_index_ref(std::uint8_t* base) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a mapped section is
    // untyped bytes by construction, and `kOffsetWriteIndex` is 8-aligned within a
    // page-aligned mapping. There is no other way to name an object in shared memory.
    return std::atomic_ref<std::uint64_t>{*reinterpret_cast<std::uint64_t*>(base + kOffsetWriteIndex)};
}

[[nodiscard]] std::atomic_ref<const std::uint64_t> write_index_ref(const std::uint8_t* base) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): see above.
    return std::atomic_ref<const std::uint64_t>{*reinterpret_cast<const std::uint64_t*>(base + kOffsetWriteIndex)};
}

/// How far the write index may advance past `sequence` while a reader is still looking at
/// the frame `sequence` published.
///
/// Worked out rather than guessed, because "three buffers, so two frames of slack" is the
/// intuitive answer and it is wrong by one. A reader holding `sequence` is on slot
/// `(sequence - 1) % slots`. The writer's *next* fill goes to `now % slots`. Those collide
/// when `now ≡ sequence - 1 (mod slots)`, i.e. at `now - sequence == slots - 1` -- and at
/// that moment the slot is being written, not about to be. So the last safe distance is
/// `slots - 2`: for §15.2's three buffers, one.
///
/// The off-by-one is not academic. At `slots - 1` a reader is told a frame is intact while
/// the writer is filling it, which is a torn preview frame that no assertion downstream
/// would attribute to this function.
[[nodiscard]] std::uint64_t lap_distance(int slots) noexcept {
    return static_cast<std::uint64_t>(slots) - 2U;
}

} // namespace

std::string preview_section_name(std::string_view session_id) {
    // `Local\` is the per-logon-session object namespace: two users logged in at once get
    // two distinct sections from the same name, which is the isolation §3.1's
    // one-engine-per-user-session rule assumes everywhere else.
    std::string name = "Local\\framecapture-preview-";
    name.append(session_id.empty() ? std::string_view{"default"} : session_id);
    return name;
}

// ---------------------------------------------------------------------------
// PreviewRing -- the writer
// ---------------------------------------------------------------------------

struct PreviewRing::Impl {
    MappedSection section;
    PreviewGeometry geometry;
    std::string name;
    /// The writer's own copy of the published count. Kept alongside the shared one so
    /// `next_slot` does not have to do an atomic load per frame to find its slot.
    std::uint64_t write_index = 0;
    std::atomic<std::uint64_t> dropped{0};
};

PreviewRing::PreviewRing() : impl_(std::make_unique<Impl>()) {}

PreviewRing::~PreviewRing() = default;

Result<void> PreviewRing::create(const std::string& name, const PreviewGeometry& geometry) {
    if (!geometry.valid() || name.empty()) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }
    close();

    const std::size_t bytes = geometry.total_bytes();
    const std::wstring wide = widen(name);
    if (wide.empty()) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    // Default security descriptor: the creating user's token grants itself access and
    // nobody else is named. See the header note -- §15.2 states no ACL requirement for this
    // channel, unlike §15.1's pipe, and inventing one silently would be a decision made in
    // the wrong place.
    HANDLE section =
        ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(bytes), wide.c_str());
    if (section == nullptr) {
        FC_LOG_ERROR(Subsystem::Ipc, "the preview section could not be created",
                     LogFields{}
                         .add("name", name)
                         .add("bytes", static_cast<std::int64_t>(bytes))
                         .add("last_error", static_cast<std::int64_t>(::GetLastError()))
                         .add_error(FcError::IPC_SHM_CREATE_FAILED));
        return FcError::IPC_SHM_CREATE_FAILED;
    }

    void* base = ::MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (base == nullptr) {
        const DWORD error = ::GetLastError();
        ::CloseHandle(section);
        FC_LOG_ERROR(Subsystem::Ipc, "the preview section could not be mapped",
                     LogFields{}
                         .add("name", name)
                         .add("bytes", static_cast<std::int64_t>(bytes))
                         .add("last_error", static_cast<std::int64_t>(error))
                         .add_error(FcError::IPC_SHM_MAP_FAILED));
        return FcError::IPC_SHM_MAP_FAILED;
    }

    impl_->section.adopt(section, base, bytes);
    impl_->geometry = geometry;
    impl_->name = name;
    impl_->write_index = 0;
    impl_->dropped.store(0, std::memory_order_relaxed);

    std::uint8_t* header = impl_->section.base();
    std::memset(header, 0, kPreviewHeaderBytes);

    // Everything except the magic first, then the magic. A reader that attaches mid-stamp
    // must not see a valid magic over a half-written geometry, and the magic is the only
    // field it checks before trusting the rest.
    store_u32(header, kOffsetVersion, kPreviewVersion);
    store_u32(header, kOffsetWidth, static_cast<std::uint32_t>(geometry.width));
    store_u32(header, kOffsetHeight, static_cast<std::uint32_t>(geometry.height));
    store_u32(header, kOffsetStride, static_cast<std::uint32_t>(geometry.stride()));
    store_u32(header, kOffsetSlotBytes, static_cast<std::uint32_t>(geometry.slot_bytes()));
    store_u32(header, kOffsetSlotCount, static_cast<std::uint32_t>(geometry.slots));
    store_u32(header, kOffsetFormat, kPreviewFormatBgra8);
    store_u32(header, kOffsetFps, static_cast<std::uint32_t>(geometry.fps));
    store_u64(header, kOffsetWriteIndex, 0);
    store_u64(header, kOffsetDropped, 0);
    store_u64(header, kOffsetLastQpcNs, 0);
    std::atomic_thread_fence(std::memory_order_release);
    store_u32(header, kOffsetMagic, kPreviewMagic);

    FC_LOG_INFO(Subsystem::Ipc, "preview section created",
                LogFields{}
                    .add("name", name)
                    .add("width", geometry.width)
                    .add("height", geometry.height)
                    .add("fps", geometry.fps)
                    .add("slots", geometry.slots)
                    .add("bytes", static_cast<std::int64_t>(bytes)));
    return ok();
}

void PreviewRing::close() {
    if (impl_->section.mapped()) {
        // Cleared before unmapping so a reader still attached sees "no longer a preview"
        // rather than a stale frame it has no way to date.
        store_u32(impl_->section.base(), kOffsetMagic, 0);
    }
    impl_->section.reset();
    impl_->name.clear();
    impl_->write_index = 0;
}

bool PreviewRing::open() const noexcept {
    return impl_->section.mapped();
}

std::uint8_t* PreviewRing::next_slot() noexcept {
    if (!impl_->section.mapped()) {
        return nullptr;
    }
    const auto slots = static_cast<std::uint64_t>(impl_->geometry.slots);
    const auto slot = static_cast<std::size_t>(impl_->write_index % slots);
    return impl_->section.base() + kPreviewHeaderBytes + (slot * impl_->geometry.slot_bytes());
}

void PreviewRing::publish(std::int64_t qpc_ns) noexcept {
    if (!impl_->section.mapped()) {
        return;
    }
    std::uint8_t* header = impl_->section.base();
    // The timestamp before the index, because the index is the release that makes both
    // visible. A reader that saw the new index and the old timestamp would date the frame
    // to its predecessor.
    store_u64(header, kOffsetLastQpcNs, static_cast<std::uint64_t>(qpc_ns));
    ++impl_->write_index;
    write_index_ref(header).store(impl_->write_index, std::memory_order_release);
}

void PreviewRing::note_dropped(std::uint64_t count) noexcept {
    const std::uint64_t total = impl_->dropped.fetch_add(count, std::memory_order_relaxed) + count;
    if (impl_->section.mapped()) {
        store_u64(impl_->section.base(), kOffsetDropped, total);
    }
}

PreviewRingStats PreviewRing::stats() const noexcept {
    PreviewRingStats out;
    out.published = impl_->write_index;
    out.dropped = impl_->dropped.load(std::memory_order_relaxed);
    return out;
}

const std::string& PreviewRing::name() const noexcept {
    return impl_->name;
}

const PreviewGeometry& PreviewRing::geometry() const noexcept {
    return impl_->geometry;
}

// ---------------------------------------------------------------------------
// PreviewReader
// ---------------------------------------------------------------------------

struct PreviewReader::Impl {
    MappedSection section;
    PreviewGeometry geometry;
};

PreviewReader::PreviewReader() : impl_(std::make_unique<Impl>()) {}

PreviewReader::~PreviewReader() = default;

Result<void> PreviewReader::open(const std::string& name) {
    close();
    const std::wstring wide = widen(name);
    if (wide.empty()) {
        return FcError::INTERNAL_INVALID_ARGUMENT;
    }

    HANDLE section = ::OpenFileMappingW(FILE_MAP_READ, FALSE, wide.c_str());
    if (section == nullptr) {
        return FcError::IPC_SHM_MAP_FAILED;
    }

    // Header first, so the geometry decides how much to map rather than a number the caller
    // had to already know. A reader that trusted a caller-supplied size would map past the
    // end of a section written by a different build.
    const void* header_view = ::MapViewOfFile(section, FILE_MAP_READ, 0, 0, kPreviewHeaderBytes);
    if (header_view == nullptr) {
        ::CloseHandle(section);
        return FcError::IPC_SHM_MAP_FAILED;
    }

    const auto* header = static_cast<const std::uint8_t*>(header_view);
    PreviewGeometry geometry;
    const std::uint32_t magic = load_u32(header, kOffsetMagic);
    const std::uint32_t version = load_u32(header, kOffsetVersion);
    geometry.width = static_cast<int>(load_u32(header, kOffsetWidth));
    geometry.height = static_cast<int>(load_u32(header, kOffsetHeight));
    geometry.fps = static_cast<int>(load_u32(header, kOffsetFps));
    geometry.slots = static_cast<int>(load_u32(header, kOffsetSlotCount));
    ::UnmapViewOfFile(header_view);

    if (magic != kPreviewMagic || version != kPreviewVersion || !geometry.valid()) {
        ::CloseHandle(section);
        // Not a hard failure at the call site -- a GUI that attaches before the engine has
        // armed the preview gets this and tries again. Logged at debug for the same reason.
        FC_LOG_DEBUG(Subsystem::Ipc, "the preview section is not a preview this build understands",
                     LogFields{}
                         .add("name", name)
                         .add("magic", static_cast<std::int64_t>(magic))
                         .add("version", static_cast<std::int64_t>(version)));
        return FcError::IPC_MESSAGE_MALFORMED;
    }

    const std::size_t bytes = geometry.total_bytes();
    void* base = ::MapViewOfFile(section, FILE_MAP_READ, 0, 0, bytes);
    if (base == nullptr) {
        ::CloseHandle(section);
        return FcError::IPC_SHM_MAP_FAILED;
    }

    impl_->section.adopt(section, base, bytes);
    impl_->geometry = geometry;
    return ok();
}

void PreviewReader::close() {
    impl_->section.reset();
}

Result<PreviewReader::View> PreviewReader::latest() const {
    if (!impl_->section.mapped()) {
        return FcError::INTERNAL_INVALID_STATE;
    }
    const std::uint8_t* base = impl_->section.base();
    const std::uint64_t sequence = write_index_ref(base).load(std::memory_order_acquire);
    if (sequence == 0) {
        return FcError::INTERNAL_INVALID_STATE; // nothing has been published yet
    }

    const auto slots = static_cast<std::uint64_t>(impl_->geometry.slots);
    const auto slot = static_cast<std::size_t>((sequence - 1U) % slots);

    View view;
    view.pixels = base + kPreviewHeaderBytes + (slot * impl_->geometry.slot_bytes());
    view.sequence = sequence;
    view.qpc_ns = static_cast<std::int64_t>(load_u64(base, kOffsetLastQpcNs));
    return view;
}

bool PreviewReader::still_valid(std::uint64_t sequence) const noexcept {
    if (!impl_->section.mapped() || sequence == 0) {
        return false;
    }
    const std::uint64_t now = write_index_ref(impl_->section.base()).load(std::memory_order_acquire);
    return now >= sequence && (now - sequence) <= lap_distance(impl_->geometry.slots);
}

std::uint64_t PreviewReader::write_index() const noexcept {
    if (!impl_->section.mapped()) {
        return 0;
    }
    return write_index_ref(impl_->section.base()).load(std::memory_order_acquire);
}

std::uint64_t PreviewReader::dropped() const noexcept {
    if (!impl_->section.mapped()) {
        return 0;
    }
    return load_u64(impl_->section.base(), kOffsetDropped);
}

const PreviewGeometry& PreviewReader::geometry() const noexcept {
    return impl_->geometry;
}

} // namespace fc::preview

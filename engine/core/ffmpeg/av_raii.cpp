#include "core/ffmpeg/av_raii.h"

#include <array>

namespace fc::ff {

std::string error_text(int averror) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(averror, buffer.data(), buffer.size()) < 0) {
        return "unknown libav error " + std::to_string(averror);
    }
    return std::string{buffer.data()};
}

std::string Dictionary::remaining() const {
    if (dict_ == nullptr) {
        return {};
    }

    std::string text;
    const AVDictionaryEntry* entry = nullptr;
    while ((entry = av_dict_iterate(dict_, entry)) != nullptr) {
        if (!text.empty()) {
            text += ", ";
        }
        text += entry->key;
        text += '=';
        text += entry->value;
    }
    return text;
}

} // namespace fc::ff

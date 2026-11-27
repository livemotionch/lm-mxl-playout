#pragma once

#include "format.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace mtp {

// Direct play: an uploaded file plays as it is, without a conversion. A video (any container and
// codec FFmpeg reads, also audio only) is decoded while it plays, and conformed to the output
// format by the same filters the ProRes conversion uses (deinterlace, BT.601 to BT.709, fit,
// frame rate, interlace). Stills and animations are decoded when they are loaded.
struct DirectSpec {
    std::string path;                  // the original file
    std::string fit = "fit";           // fit, fill, center (1:1)
    std::string scan = "progressive";  // of the source: progressive, interlaced_tff, interlaced_bff
    std::string color_space;           // of the source (ffprobe), "" when not known
    int threads = 4;                   // decoder and filter threads

    // The stream key StreamDecoder opens (Media::mezz): "direct:" and the spec as JSON. A
    // different spec of the same file is another key, so the decoder opens it again.
    std::string key() const;
    static bool parse(const std::string& key, DirectSpec& out);
};

// The filter chain from a decoded source frame to the output format, yuv422p10le at the output's
// frame rate (frames, two fields each when interlaced). `fps_mode` "motion" interpolates (only for
// the conversion: it is too slow to play live).
std::string conform_filter(const std::string& fit, const std::string& fps_mode, const std::string& scan, const std::string& color_space,
                           const VideoFormat& format);

// Output frames of a video of `duration_s` seconds at `format` (at least 1).
std::int64_t direct_frames(double duration_s, const VideoFormat& format);

// Reads frame k of the output format from the original: seeks to the key frame before it when k
// is not the next frame, and holds the last frame after the end of the file. A file without
// video gives black frames (and its audio).
class DirectReader {
public:
    DirectReader();
    ~DirectReader();
    DirectReader(const DirectReader&) = delete;
    DirectReader& operator=(const DirectReader&) = delete;

    bool open(const DirectSpec& spec, const VideoFormat& format, std::string& error);
    void close();
    bool is_open() const { return impl_ != nullptr; }
    int channels() const { return channels_; }
    // One frame of v210 at the output size and `audio_count` interleaved samples (48 kHz,
    // channels()) from sample `audio_start` of the clip.
    bool read(std::int64_t frame, std::uint8_t* v210, float* audio, std::uint64_t audio_start, int audio_count, std::string& error);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    int channels_ = 0;
};

// Fills `v210` (v210_size(width, height) bytes) with black.
void fill_v210_black(std::uint8_t* v210, int width, int height);

// A still image decoded and fitted to the format: fill, key (fill_key) and v210a alpha. False with
// `error` when it cannot be decoded.
bool render_still(const std::string& path, const std::string& fit, bool premultiply, const VideoFormat& format, std::vector<std::uint8_t>& fill,
                  std::vector<std::uint8_t>& key, std::vector<std::uint8_t>& a10, std::string& error);

// An animation decoded and fitted to the format, frame by frame: the fill and the v210a alpha of
// every frame appended to `fill` and `a10`, its durations in seconds to `durations`. Refused before
// decoding when it would take more than `budget_bytes`.
bool render_animation(const std::string& path, const std::string& fit, bool premultiply, const VideoFormat& format, std::uint64_t budget_bytes,
                      std::vector<std::uint8_t>& fill, std::vector<std::uint8_t>& a10, std::vector<double>& durations, std::string& error);

}  // namespace mtp

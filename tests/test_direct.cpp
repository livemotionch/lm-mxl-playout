// Direct play: an original decoded and conformed while it plays (direct.cpp), and uploads that
// play without a conversion (media.cpp).
#include "decode.hpp"
#include "direct.hpp"
#include "format.hpp"
#include "media.hpp"
#include "v210.hpp"

#include <doctest.h>
#include <nlohmann/json.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

using namespace mtp;

namespace {

constexpr int kSrcFps = 30;
constexpr int kSrcFrames = 45;  // 1.5 s
constexpr int kSrcRate = 44100;

// Source frame i is flat grey with 8-bit luma 20 + 4 i.
int src_luma(int i) { return 20 + 4 * i; }

// A test file written with libavformat (the image build has no ffmpeg program): MPEG-4 video at
// 160x90, 30 fps, a key frame every 12 frames and B-frames (so a seek decodes forward to its
// frame), and 44.1 kHz stereo PCM, the left channel at 0.25, the right at -0.5.
bool write_clip(const std::string& path, bool video, bool audio) {
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "matroska", path.c_str()) < 0) return false;
    AVCodecContext* venc = nullptr;
    AVCodecContext* aenc = nullptr;
    AVStream* vs = nullptr;
    AVStream* as = nullptr;
    if (video) {
        const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
        if (!c) return false;
        vs = avformat_new_stream(oc, nullptr);
        venc = avcodec_alloc_context3(c);
        venc->width = 160;
        venc->height = 90;
        venc->pix_fmt = AV_PIX_FMT_YUV420P;
        venc->time_base = AVRational{1, kSrcFps};
        venc->framerate = AVRational{kSrcFps, 1};
        venc->gop_size = 12;
        venc->max_b_frames = 2;
        venc->bit_rate = 400000;
        if (oc->oformat->flags & AVFMT_GLOBALHEADER) venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(venc, c, nullptr) < 0) return false;
        avcodec_parameters_from_context(vs->codecpar, venc);
        vs->time_base = venc->time_base;
    }
    if (audio) {
        const AVCodec* c = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE);
        as = avformat_new_stream(oc, nullptr);
        aenc = avcodec_alloc_context3(c);
        aenc->sample_fmt = AV_SAMPLE_FMT_S16;
        aenc->sample_rate = kSrcRate;
        av_channel_layout_default(&aenc->ch_layout, 2);
        aenc->time_base = AVRational{1, kSrcRate};
        if (avcodec_open2(aenc, c, nullptr) < 0) return false;
        avcodec_parameters_from_context(as->codecpar, aenc);
        as->time_base = aenc->time_base;
    }
    if (avio_open(&oc->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) return false;
    if (avformat_write_header(oc, nullptr) < 0) return false;
    AVPacket* pkt = av_packet_alloc();
    auto drain = [&](AVCodecContext* enc, AVStream* st) {
        while (avcodec_receive_packet(enc, pkt) == 0) {
            av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(oc, pkt);
        }
    };
    if (video) {
        AVFrame* f = av_frame_alloc();
        f->format = AV_PIX_FMT_YUV420P;
        f->width = 160;
        f->height = 90;
        av_frame_get_buffer(f, 0);
        for (int i = 0; i < kSrcFrames; ++i) {
            av_frame_make_writable(f);
            for (int y = 0; y < 90; ++y) std::memset(f->data[0] + y * f->linesize[0], src_luma(i), 160);
            for (int y = 0; y < 45; ++y) {
                std::memset(f->data[1] + y * f->linesize[1], 128, 80);
                std::memset(f->data[2] + y * f->linesize[2], 128, 80);
            }
            f->pts = i;
            avcodec_send_frame(venc, f);
            drain(venc, vs);
        }
        avcodec_send_frame(venc, nullptr);
        drain(venc, vs);
        av_frame_free(&f);
    }
    if (audio) {
        AVFrame* f = av_frame_alloc();
        f->format = AV_SAMPLE_FMT_S16;
        f->sample_rate = kSrcRate;
        av_channel_layout_default(&f->ch_layout, 2);
        f->nb_samples = 1024;
        av_frame_get_buffer(f, 0);
        const int total = kSrcRate * kSrcFrames / kSrcFps;
        for (int s = 0; s < total; s += 1024) {
            av_frame_make_writable(f);
            f->nb_samples = std::min(1024, total - s);
            auto* d = reinterpret_cast<std::int16_t*>(f->data[0]);
            for (int i = 0; i < f->nb_samples; ++i) {
                d[2 * i] = 8192;
                d[2 * i + 1] = -16384;
            }
            f->pts = s;
            avcodec_send_frame(aenc, f);
            drain(aenc, as);
        }
        avcodec_send_frame(aenc, nullptr);
        drain(aenc, as);
        av_frame_free(&f);
    }
    av_write_trailer(oc);
    av_packet_free(&pkt);
    if (venc) avcodec_free_context(&venc);
    if (aenc) avcodec_free_context(&aenc);
    avio_closep(&oc->pb);
    avformat_free_context(oc);
    return true;
}

int center_luma(const std::vector<std::uint8_t>& v210, const VideoFormat& fmt) {
    std::vector<std::uint16_t> y(static_cast<std::size_t>(fmt.width)), cb(static_cast<std::size_t>(fmt.width / 2)), cr(cb.size());
    unpack_v210_line(v210.data() + static_cast<std::size_t>(fmt.height / 2) * v210_line_stride(fmt.width), fmt.width, y.data(), cb.data(), cr.data());
    return y[static_cast<std::size_t>(fmt.width / 2)];
}

// The source frame a 10-bit luma came from.
int source_frame(int luma10) { return static_cast<int>(std::lround((luma10 / 4.0 - 20) / 4.0)); }

std::filesystem::path temp_dir(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / (std::string(name) + "-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST_CASE("direct spec keys round-trip, and a mezzanine path is not one") {
    DirectSpec s;
    s.path = "/data/library/x/original.mp4";
    s.fit = "fill";
    s.scan = "interlaced_tff";
    s.color_space = "bt470bg";
    s.threads = 3;
    DirectSpec back;
    REQUIRE(DirectSpec::parse(s.key(), back));
    CHECK(back.path == s.path);
    CHECK(back.fit == "fill");
    CHECK(back.scan == "interlaced_tff");
    CHECK(back.color_space == "bt470bg");
    CHECK(back.threads == 3);
    CHECK_FALSE(DirectSpec::parse("/data/library/x/mezz-1080p50.mov", back));
    CHECK(direct_frames(1.5, *parse_format("720p25")) == 38);
    CHECK(direct_frames(0, *parse_format("720p25")) == 1);
    // The conversion and direct play conform with the same filters.
    const auto i50 = conform_filter("fit", "drop", "interlaced_tff", "smpte170m", *parse_format("1080i25"));
    CHECK(i50.find("bwdif") != std::string::npos);
    CHECK(i50.find("colorspace=all=bt709") != std::string::npos);
    CHECK(i50.find("fps=50/1") != std::string::npos);
    CHECK(i50.find("tinterlace=mode=interleave_top") != std::string::npos);
    CHECK(conform_filter("fill", "drop", "progressive", "", *parse_format("720p25")).find("force_original_aspect_ratio=increase") != std::string::npos);
}

TEST_CASE("a direct video plays any frame: frame-rate conformed, seek to the same picture, the last frame held, its audio at 48 kHz") {
    const auto dir = temp_dir("mtp-direct");
    const auto path = (dir / "clip.mkv").string();
    REQUIRE(write_clip(path, true, true));
    const VideoFormat fmt = *parse_format("720p25");
    DirectSpec spec;
    spec.path = path;
    DirectReader reader;
    std::string err;
    REQUIRE(reader.open(spec, fmt, err));
    CHECK(reader.channels() == 2);
    const std::int64_t frames = direct_frames(1.5, fmt);  // 38 output frames
    std::vector<std::uint8_t> v210(v210_size(fmt.width, fmt.height));
    std::vector<int> seq;
    int prev = -1;
    for (std::int64_t k = 0; k < frames; ++k) {
        REQUIRE_MESSAGE(reader.read(k, v210.data(), nullptr, 0, 0, err), err);
        const int luma = center_luma(v210, fmt);
        seq.push_back(luma);
        const int src = source_frame(luma);
        INFO("frame " << k << " luma " << luma << " source frame " << src);
        // 30 to 25 fps: output frame k shows the source frame at its time (k * 1.2), one either side.
        CHECK(std::abs(src - static_cast<int>(std::lround(k * 1.2))) <= 1);
        CHECK(src >= prev);
        prev = src;
    }
    // A seek (backwards and forwards, into a GOP) gives the frame a sequential read gave.
    for (std::int64_t k : {10, 3, 31, 17, 0, 37}) {
        REQUIRE_MESSAGE(reader.read(k, v210.data(), nullptr, 0, 0, err), err);
        INFO("seek to frame " << k);
        CHECK(center_luma(v210, fmt) == seq[static_cast<std::size_t>(k)]);
    }
    // After the end of the file the last frame is held.
    REQUIRE(reader.read(frames + 5, v210.data(), nullptr, 0, 0, err));
    CHECK(center_luma(v210, fmt) == seq.back());
    // Audio of frame 12 (samples from 23040 on), resampled to 48 kHz: left 0.25, right -0.5.
    const int count = static_cast<int>(samples_in_grain(12, fmt.frame_rate));
    std::vector<float> audio(static_cast<std::size_t>(count) * 2);
    REQUIRE(reader.read(12, v210.data(), audio.data(), samples_until_grain(12, fmt.frame_rate), count, err));
    CHECK(count == 1920);
    CHECK(audio[static_cast<std::size_t>(count)] == doctest::Approx(0.25).epsilon(0.01));
    CHECK(audio[static_cast<std::size_t>(count) + 1] == doctest::Approx(-0.5).epsilon(0.01));
    CHECK(center_luma(v210, fmt) == seq[12]);
    // And sequentially on, without a gap in the samples.
    REQUIRE(reader.read(13, v210.data(), audio.data(), samples_until_grain(13, fmt.frame_rate), count, err));
    CHECK(audio[0] == doctest::Approx(0.25).epsilon(0.01));
    CHECK(audio[2 * static_cast<std::size_t>(count) - 1] == doctest::Approx(-0.5).epsilon(0.01));
    std::filesystem::remove_all(dir);
}

TEST_CASE("a direct file of audio only plays black with its audio") {
    const auto dir = temp_dir("mtp-direct-audio");
    const auto path = (dir / "sound.mkv").string();
    REQUIRE(write_clip(path, false, true));
    const VideoFormat fmt = *parse_format("720p25");
    DirectSpec spec;
    spec.path = path;
    DirectReader reader;
    std::string err;
    REQUIRE(reader.open(spec, fmt, err));
    CHECK(reader.channels() == 2);
    std::vector<std::uint8_t> v210(v210_size(fmt.width, fmt.height));
    std::vector<float> audio(1920 * 2);
    REQUIRE(reader.read(5, v210.data(), audio.data(), samples_until_grain(5, fmt.frame_rate), 1920, err));
    CHECK(center_luma(v210, fmt) == kYBlack);
    CHECK(audio[1920] == doctest::Approx(0.25).epsilon(0.01));
    std::filesystem::remove_all(dir);
}

TEST_CASE("an upload plays as it is: no conversion job, a direct entry per format; convert or an option that needs it converts") {
    const auto dir = temp_dir("mtp-direct-library");
    const auto clip = (dir / "clip.mkv").string();
    REQUIRE(write_clip(clip, true, true));
    Config cfg;
    cfg.library_dir = (dir / "library").string();
    const VideoFormat fmt = *parse_format("720p25");
    Library library(cfg);  // not started: a conversion would stay queued
    const auto item = library.ingest_file(clip, "clip.mkv", nlohmann::json::object(), fmt);
    CHECK(item.direct);
    CHECK(item.type == ItemType::Video);
    const auto conv = library.get(item.id).conversions.at(fmt.name);
    CHECK(conv.at("status") == "ready");
    CHECK(conv.at("direct") == true);
    CHECK(conv.at("frames") == 38);
    CHECK(conv.at("audio_channels") == 2);
    CHECK(library.jobs().empty());
    CHECK(std::filesystem::exists(std::filesystem::path(library.item_dir(item.id)) / "thumb.jpg"));
    // Another output format: an entry with its frame count, still no job.
    library.ensure_format(item.id, *parse_format("1080p50"));
    CHECK(library.get(item.id).conversions.at("1080p50").at("frames") == 75);
    CHECK(library.jobs().empty());
    // To a conversion and back to direct.
    std::string err;
    REQUIRE(library.reconvert(item.id, {{"direct", false}}, fmt, err));
    CHECK_FALSE(library.get(item.id).direct);
    CHECK(library.jobs().size() == 1);
    CHECK_FALSE(library.reconvert(item.id, {{"direct", true}}, fmt, err));  // its conversion is queued
    // An option only a conversion applies converts.
    std::filesystem::copy_file(clip, dir / "loud.mkv");
    const auto loud = library.ingest_file((dir / "loud.mkv").string(), "loud", {{"loudness", true}}, fmt);
    CHECK_FALSE(loud.direct);
    CHECK(library.get(loud.id).conversions.at(fmt.name).at("status") == "queued");
    // Not a media file: refused with a reason, not "ready".
    std::ofstream(dir / "junk.bin") << "not media";
    const auto junk = library.ingest_file((dir / "junk.bin").string(), "junk", nlohmann::json::object(), fmt);
    CHECK(library.get(junk.id).conversions.at(fmt.name).at("status") == "failed");
    std::filesystem::remove_all(dir);
}

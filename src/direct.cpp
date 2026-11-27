#include "direct.hpp"

#include "decode.hpp"
#include "v210.hpp"

#include <nlohmann/json.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>

namespace mtp {

namespace {
constexpr const char* kPrefix = "direct:";
}

std::string DirectSpec::key() const {
    return kPrefix + nlohmann::json{{"path", path}, {"fit", fit}, {"scan", scan}, {"color_space", color_space}, {"threads", threads}}.dump();
}

bool DirectSpec::parse(const std::string& key, DirectSpec& out) {
    if (key.rfind(kPrefix, 0) != 0) return false;
    try {
        const auto j = nlohmann::json::parse(key.substr(std::strlen(kPrefix)));
        out.path = j.value("path", "");
        out.fit = j.value("fit", "fit");
        out.scan = j.value("scan", "progressive");
        out.color_space = j.value("color_space", "");
        out.threads = std::clamp(j.value("threads", 4), 1, 64);
        return !out.path.empty();
    } catch (...) {
        return false;
    }
}

std::string conform_filter(const std::string& fit, const std::string& fps_mode, const std::string& scan, const std::string& color_space,
                           const VideoFormat& format) {
    const bool dst_i = format.interlaced();
    const auto prog_num = dst_i ? format.frame_rate.num * 2 : format.frame_rate.num;
    const std::string W = std::to_string(format.width), H = std::to_string(format.height);
    std::string vf;
    if (scan.find("interlace") != std::string::npos) vf += "bwdif=mode=send_frame,";
    if (color_space == "smpte170m" || color_space == "bt470bg" || color_space == "smpte240m") vf += "colorspace=all=bt709:iall=bt601,";
    if (fit == "fill") {
        vf += "scale=" + W + ":" + H + ":force_original_aspect_ratio=increase,crop=" + W + ":" + H + ",";
    } else if (fit == "center" || fit == "1:1") {
        vf += "pad=" + W + ":" + H + ":(ow-iw)/2:(oh-ih)/2:black,crop=" + W + ":" + H + ",";
    } else {
        vf += "scale=" + W + ":" + H + ":force_original_aspect_ratio=decrease,pad=" + W + ":" + H + ":(ow-iw)/2:(oh-ih)/2:black,";
    }
    const std::string fps = std::to_string(prog_num) + "/" + std::to_string(format.frame_rate.den);
    if (fps_mode == "motion") vf += "minterpolate=fps=" + fps + ":mi_mode=mci,";
    else vf += "fps=" + fps + ",";
    // Two progressive frames at the field rate make one frame; the earlier one goes into the
    // first field's lines (top for tff, bottom for bff).
    if (dst_i) vf += format.bottom_field_first() ? "tinterlace=mode=interleave_bottom," : "tinterlace=mode=interleave_top,";
    vf += "format=yuv422p10le";
    return vf;
}

std::int64_t direct_frames(double duration_s, const VideoFormat& format) {
    const double fps = format.frame_rate.to_double();
    if (!(duration_s > 0) || !(fps > 0)) return 1;
    return std::max<std::int64_t>(1, std::llround(duration_s * fps));
}

void fill_v210_black(std::uint8_t* v210, int width, int height) {
    std::vector<std::uint16_t> y(static_cast<std::size_t>(width), kYBlack), c(static_cast<std::size_t>(width / 2 + 1), kCMid);
    const std::uint32_t stride = v210_line_stride(width);
    pack_v210_line(y.data(), c.data(), c.data(), width, v210);
    for (int row = 1; row < height; ++row) std::memcpy(v210 + static_cast<std::size_t>(row) * stride, v210, stride);
}

struct DirectReader::Impl {
    DirectSpec spec;
    VideoFormat fmt;
    AVFormatContext* in = nullptr;
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    SwrContext* swr = nullptr;
    int vindex = -1;
    int aindex = -1;
    int channels = 0;
    std::int64_t start_us = 0;  // the file's start time: time 0 of the clip

    // The filter graph, made from the first frame after an open or a seek (its size and pixel
    // format are known then).
    AVFilterGraph* graph = nullptr;
    AVFilterContext* src = nullptr;
    AVFilterContext* sink = nullptr;
    bool flushed = false;   // the end of the file went into the graph
    bool video_eof = false; // the demuxer has no more packets
    std::int64_t skip_before = std::numeric_limits<std::int64_t>::min();  // decoded frames that end before this (stream tb) are not filtered
    std::int64_t next_out = 0;  // the output frame a sequential read gives next
    AVFrame* last = nullptr;    // the latest output frame (held after the end)
    std::deque<AVPacket*> pending;  // video packets read while the audio was looked for
    std::vector<float> audio;       // decoded audio, interleaved, from sample audio_pos on
    std::int64_t audio_pos = -1;

    ~Impl() {
        for (auto* p : pending) av_packet_free(&p);
        if (last) av_frame_free(&last);
        if (graph) avfilter_graph_free(&graph);
        if (swr) swr_free(&swr);
        if (vdec) avcodec_free_context(&vdec);
        if (adec) avcodec_free_context(&adec);
        if (in) avformat_close_input(&in);
    }

    AVStream* vs() const { return in->streams[vindex]; }
    std::int64_t start_in(AVRational tb) const { return av_rescale_q(start_us, AV_TIME_BASE_Q, tb); }
    AVRational out_rate() const { return AVRational{static_cast<int>(fmt.frame_rate.num), static_cast<int>(fmt.frame_rate.den)}; }

    bool make_graph(const AVFrame* f, std::string& error) {
        if (graph) avfilter_graph_free(&graph);
        graph = avfilter_graph_alloc();
        if (!graph) {
            error = "filter graph alloc failed";
            return false;
        }
        graph->nb_threads = spec.threads;
        AVStream* s = vs();
        const AVRational sar = f->sample_aspect_ratio.num ? f->sample_aspect_ratio : AVRational{1, 1};
        const AVRational fr = s->avg_frame_rate.num ? s->avg_frame_rate : s->r_frame_rate;
        char args[512];
        std::snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d%s", f->width, f->height, f->format,
                      s->time_base.num, s->time_base.den, sar.num, sar.den,
                      fr.num > 0 ? (":frame_rate=" + std::to_string(fr.num) + "/" + std::to_string(fr.den)).c_str() : "");
        if (avfilter_graph_create_filter(&src, avfilter_get_by_name("buffer"), "in", args, nullptr, graph) < 0 ||
            avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph) < 0) {
            error = "filter graph: buffer failed";
            return false;
        }
        AVFilterInOut* outputs = avfilter_inout_alloc();
        AVFilterInOut* inputs = avfilter_inout_alloc();
        outputs->name = av_strdup("in");
        outputs->filter_ctx = src;
        outputs->pad_idx = 0;
        outputs->next = nullptr;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink;
        inputs->pad_idx = 0;
        inputs->next = nullptr;
        const auto chain = conform_filter(spec.fit, "drop", spec.scan, spec.color_space, fmt);
        const int r = avfilter_graph_parse_ptr(graph, chain.c_str(), &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (r < 0 || avfilter_graph_config(graph, nullptr) < 0) {
            error = "filter graph failed: " + chain;
            avfilter_graph_free(&graph);
            return false;
        }
        flushed = false;
        return true;
    }

    // A decoded source frame into the graph (or dropped, before a seek's target).
    bool push_video(AVFrame* f, std::string& error) {
        std::int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        if (pts != AV_NOPTS_VALUE) {
            const std::int64_t dur = f->duration > 0 ? f->duration : 0;
            if (pts + dur < skip_before) return true;
            pts -= start_in(vs()->time_base);
        }
        f->pts = pts;
        if (!graph && !make_graph(f, error)) return false;
        if (av_buffersrc_add_frame_flags(src, f, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
            error = "filter graph: add frame failed";
            return false;
        }
        return true;
    }

    bool decode_video(AVPacket* p, std::string& error) {
        if (avcodec_send_packet(vdec, p) < 0 && p) return true;  // a damaged packet is skipped
        AVFrame* f = av_frame_alloc();
        bool ok = true;
        while (ok && avcodec_receive_frame(vdec, f) == 0) {
            ok = push_video(f, error);
            av_frame_unref(f);
        }
        av_frame_free(&f);
        return ok;
    }

    void decode_audio(AVPacket* p) {
        if (avcodec_send_packet(adec, p) < 0 && p) return;
        AVFrame* f = av_frame_alloc();
        const auto ch = static_cast<std::size_t>(channels);
        while (avcodec_receive_frame(adec, f) == 0) {
            AVStream* as = in->streams[aindex];
            if (audio_pos < 0 && f->pts != AV_NOPTS_VALUE) {
                audio_pos = av_rescale_q(f->pts - start_in(as->time_base), as->time_base, AVRational{1, 48000});
            }
            const int dst_nb = swr_get_out_samples(swr, f->nb_samples) + 32;
            std::vector<float> tmp(static_cast<std::size_t>(std::max(0, dst_nb)) * ch);
            uint8_t* planes[1] = {reinterpret_cast<uint8_t*>(tmp.data())};
            const int n = swr_convert(swr, planes, dst_nb, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
            if (n > 0 && audio_pos >= 0) audio.insert(audio.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(n) * static_cast<std::ptrdiff_t>(ch));
            av_frame_unref(f);
        }
        av_frame_free(&f);
    }

    void seek(std::int64_t k) {
        const int index = vindex >= 0 ? vindex : aindex;
        if (index >= 0) {
            AVStream* s = in->streams[index];
            const std::int64_t ts = av_rescale_q(k, av_inv_q(out_rate()), s->time_base) + start_in(s->time_base);
            if (av_seek_frame(in, index, ts, AVSEEK_FLAG_BACKWARD) < 0) av_seek_frame(in, index, start_in(s->time_base), AVSEEK_FLAG_BACKWARD);
            // Frames that end more than an output frame before frame k are decoded, not filtered.
            if (vindex >= 0) skip_before = ts - av_rescale_q(1, av_inv_q(out_rate()), s->time_base);
        }
        if (vdec) avcodec_flush_buffers(vdec);
        if (adec) avcodec_flush_buffers(adec);
        if (swr) swr_init(swr);
        if (graph) avfilter_graph_free(&graph);
        for (auto* p : pending) av_packet_free(&p);
        pending.clear();
        audio.clear();
        audio_pos = -1;
        flushed = false;
        video_eof = false;
        next_out = k;
    }
};

DirectReader::DirectReader() = default;
DirectReader::~DirectReader() { close(); }

void DirectReader::close() {
    delete impl_;
    impl_ = nullptr;
    channels_ = 0;
}

bool DirectReader::open(const DirectSpec& spec, const VideoFormat& format, std::string& error) {
    close();
    impl_ = new Impl();
    impl_->spec = spec;
    impl_->fmt = format;
    if (avformat_open_input(&impl_->in, spec.path.c_str(), nullptr, nullptr) < 0) {
        error = "cannot open " + spec.path;
        close();
        return false;
    }
    avformat_find_stream_info(impl_->in, nullptr);
    impl_->start_us = impl_->in->start_time != AV_NOPTS_VALUE ? impl_->in->start_time : 0;
    for (unsigned i = 0; i < impl_->in->nb_streams; ++i) {
        AVStream* s = impl_->in->streams[i];
        if (s->disposition & AV_DISPOSITION_ATTACHED_PIC) s->discard = AVDISCARD_ALL;  // cover art of an audio file
    }
    impl_->vindex = av_find_best_stream(impl_->in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (impl_->vindex >= 0 && (impl_->in->streams[impl_->vindex]->disposition & AV_DISPOSITION_ATTACHED_PIC)) impl_->vindex = -1;
    impl_->aindex = av_find_best_stream(impl_->in, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (impl_->vindex < 0 && impl_->aindex < 0) {
        error = "the file has no video or audio";
        close();
        return false;
    }
    for (unsigned i = 0; i < impl_->in->nb_streams; ++i) {
        if (static_cast<int>(i) != impl_->vindex && static_cast<int>(i) != impl_->aindex) impl_->in->streams[i]->discard = AVDISCARD_ALL;
    }
    if (impl_->vindex >= 0) {
        AVStream* vs = impl_->in->streams[impl_->vindex];
        const AVCodec* vc = avcodec_find_decoder(vs->codecpar->codec_id);
        impl_->vdec = vc ? avcodec_alloc_context3(vc) : nullptr;
        if (!impl_->vdec) {
            error = "no decoder for the video";
            close();
            return false;
        }
        avcodec_parameters_to_context(impl_->vdec, vs->codecpar);
        impl_->vdec->pkt_timebase = vs->time_base;
        impl_->vdec->thread_count = spec.threads;
        impl_->vdec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (avcodec_open2(impl_->vdec, vc, nullptr) < 0) {
            error = "video decoder failed";
            close();
            return false;
        }
    }
    if (impl_->aindex >= 0) {
        AVStream* as = impl_->in->streams[impl_->aindex];
        const AVCodec* ac = avcodec_find_decoder(as->codecpar->codec_id);
        impl_->adec = ac ? avcodec_alloc_context3(ac) : nullptr;
        if (impl_->adec) {
            avcodec_parameters_to_context(impl_->adec, as->codecpar);
            impl_->adec->pkt_timebase = as->time_base;
            if (avcodec_open2(impl_->adec, ac, nullptr) < 0) avcodec_free_context(&impl_->adec);
        }
        if (impl_->adec) {
            impl_->channels = std::clamp(impl_->adec->ch_layout.nb_channels, 1, 64);
            AVChannelLayout out_layout;
            av_channel_layout_default(&out_layout, impl_->channels);
            AVChannelLayout in_layout;
            if (impl_->adec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&in_layout, impl_->adec->ch_layout.nb_channels);
            else av_channel_layout_copy(&in_layout, &impl_->adec->ch_layout);
            // Each channel to its own output channel, no mix: 2..64 channels kept as they are.
            const int r = swr_alloc_set_opts2(&impl_->swr, &out_layout, AV_SAMPLE_FMT_FLT, 48000, &in_layout, impl_->adec->sample_fmt,
                                              impl_->adec->sample_rate, 0, nullptr);
            av_channel_layout_uninit(&out_layout);
            av_channel_layout_uninit(&in_layout);
            if (r < 0 || swr_init(impl_->swr) < 0) {
                if (impl_->swr) swr_free(&impl_->swr);
                avcodec_free_context(&impl_->adec);
                impl_->channels = 0;
            }
        }
        if (!impl_->adec) impl_->aindex = -1;
    }
    channels_ = impl_->channels;
    if (impl_->vindex < 0 && impl_->aindex < 0) {
        error = "the file has no video and its audio cannot be decoded";
        close();
        return false;
    }
    return true;
}

bool DirectReader::read(std::int64_t k, std::uint8_t* v210, float* audio, std::uint64_t audio_start, int audio_count, std::string& error) {
    if (!impl_) {
        error = "reader closed";
        return false;
    }
    Impl& d = *impl_;
    if (k != d.next_out) d.seek(k);
    const bool want_audio = audio && audio_count > 0 && d.adec && d.channels > 0;
    const auto ch = static_cast<std::size_t>(std::max(1, d.channels));
    const std::int64_t audio_end = static_cast<std::int64_t>(audio_start) + audio_count;
    bool audio_eof = false;
    auto audio_ready = [&] {
        return !want_audio || audio_eof || (d.audio_pos >= 0 && d.audio_pos + static_cast<std::int64_t>(d.audio.size() / ch) >= audio_end);
    };
    bool got = d.vindex < 0;
    if (got) fill_v210_black(v210, d.fmt.width, d.fmt.height);
    const AVRational rate = d.out_rate();
    AVFrame* out = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    bool failed = false;
    // The graph's frames before k are dropped; frame k (or a later one, when the source has a
    // gap) is the frame.
    auto drain = [&] {
        while (!got && d.graph && av_buffersink_get_frame(d.sink, out) >= 0) {
            const AVRational tb = av_buffersink_get_time_base(d.sink);
            const std::int64_t idx = out->pts == AV_NOPTS_VALUE ? k : av_rescale_q_rnd(out->pts, tb, av_inv_q(rate), static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            if (!d.last) d.last = av_frame_alloc();
            av_frame_unref(d.last);
            av_frame_move_ref(d.last, out);
            if (idx >= k) {
                got = frame_to_v210(d.last, v210, error);
                if (!got) failed = true;
            }
        }
    };
    while (!failed) {
        drain();
        if (got && audio_ready()) break;
        if (!got && d.flushed) {
            // The end of the file: the last frame is held.
            if (d.last) got = frame_to_v210(d.last, v210, error);
            else fill_v210_black(v210, d.fmt.width, d.fmt.height), got = true;
            if (!got) failed = true;
            continue;
        }
        if (!got && !d.pending.empty()) {
            AVPacket* p = d.pending.front();
            d.pending.pop_front();
            if (!d.decode_video(p, error)) failed = true;
            av_packet_free(&p);
            continue;
        }
        if (d.video_eof) {
            if (!got) {
                // Flush the decoder, then the graph.
                if (d.vdec && !d.decode_video(nullptr, error)) failed = true;
                if (d.graph) av_buffersrc_add_frame_flags(d.src, nullptr, 0);
                d.flushed = true;
                continue;
            }
            audio_eof = true;
            break;
        }
        const int r = av_read_frame(d.in, pkt);
        if (r < 0) {
            d.video_eof = true;
            if (d.adec) d.decode_audio(nullptr);
            continue;
        }
        if (pkt->stream_index == d.vindex) {
            if (got) d.pending.push_back(av_packet_clone(pkt));
            else if (!d.decode_video(pkt, error)) failed = true;
        } else if (pkt->stream_index == d.aindex && d.adec) {
            d.decode_audio(pkt);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    av_frame_free(&out);
    if (failed || !got) {
        if (error.empty()) error = "frame not available";
        return false;
    }
    d.next_out = k + 1;
    if (audio && audio_count > 0) {
        std::fill(audio, audio + static_cast<std::size_t>(audio_count) * ch, 0.f);
        if (want_audio && d.audio_pos >= 0) {
            const std::int64_t have = static_cast<std::int64_t>(d.audio.size() / ch);
            const std::int64_t from = std::max<std::int64_t>(d.audio_pos, static_cast<std::int64_t>(audio_start));
            const std::int64_t to = std::min<std::int64_t>(d.audio_pos + have, audio_end);
            if (to > from) {
                std::memcpy(audio + static_cast<std::size_t>(from - static_cast<std::int64_t>(audio_start)) * ch,
                            d.audio.data() + static_cast<std::size_t>(from - d.audio_pos) * ch, static_cast<std::size_t>(to - from) * ch * sizeof(float));
            }
            const std::int64_t drop = std::clamp<std::int64_t>(audio_end - d.audio_pos, 0, have);
            d.audio.erase(d.audio.begin(), d.audio.begin() + static_cast<std::ptrdiff_t>(drop * static_cast<std::int64_t>(ch)));
            d.audio_pos += drop;
        }
    }
    return true;
}

bool render_still(const std::string& path, const std::string& fit, bool premultiply, const VideoFormat& format, std::vector<std::uint8_t>& fill,
                  std::vector<std::uint8_t>& key, std::vector<std::uint8_t>& a10, std::string& error) {
    const ScaleMode mode = fit == "fill" ? ScaleMode::Fill : fit == "center" || fit == "1:1" ? ScaleMode::Center : ScaleMode::Fit;
    const std::size_t fill_bytes = v210_size(format.width, format.height);
    fill.assign(fill_bytes, 0);
    key.assign(fill_bytes, 0);
    a10.assign(alpha10_size(format.width, format.height), 0);
    RgbaFitter fitter;
    RgbaBuffer canvas;
    bool any = false;
    const bool ok = decode_visual_frames(path, [&](const RgbaBuffer& f, double) {
        fitter.fit(f, mode, format.width, format.height, canvas);
        rgba_to_fill_key(canvas.px.data(), canvas.w, canvas.h, premultiply, fill.data(), key.data(), a10.data(), kYBlack, kYWhite);
        any = true;
        return false;  // the first frame
    }, error);
    if (!ok || !any) {
        if (error.empty()) error = "the image has no frame";
        return false;
    }
    return true;
}

bool render_animation(const std::string& path, const std::string& fit, bool premultiply, const VideoFormat& format, std::uint64_t budget_bytes,
                      std::vector<std::uint8_t>& fill, std::vector<std::uint8_t>& a10, std::vector<double>& durations, std::string& error) {
    VisualInfo info;
    if (!probe_visual(path, info, error)) return false;
    if (info.frames > kMaxAnimationFrames) {
        error = "animation has " + std::to_string(info.frames) + " frames, more than " + std::to_string(kMaxAnimationFrames);
        return false;
    }
    const std::size_t fill_bytes = v210_size(format.width, format.height);
    const std::size_t a10_bytes = alpha10_size(format.width, format.height);
    const auto n = static_cast<std::uint64_t>(std::max(1, info.frames));
    // At play the clip takes the fill and the alpha plane (v210a) or the fill and the key
    // (fill_key, the larger): that must fit the budget.
    if (n * std::max(fill_bytes + a10_bytes, 2 * fill_bytes) > budget_bytes) {
        error = "animation of " + std::to_string(info.frames) + " frames needs more than PLAYER_RAM_BUDGET_MB at " + format.name;
        return false;
    }
    const ScaleMode mode = fit == "fill" ? ScaleMode::Fill : fit == "center" || fit == "1:1" ? ScaleMode::Center : ScaleMode::Fit;
    fill.clear();
    a10.clear();
    durations.clear();
    fill.reserve(n * fill_bytes);
    a10.reserve(n * a10_bytes);
    RgbaFitter fitter;
    RgbaBuffer canvas;
    const bool ok = decode_visual_frames(path, [&](const RgbaBuffer& f, double dur) {
        if (durations.size() >= n) return false;
        fitter.fit(f, mode, format.width, format.height, canvas);
        const std::size_t fo = fill.size(), ao = a10.size();
        fill.resize(fo + fill_bytes);
        a10.resize(ao + a10_bytes);
        rgba_to_fill_key(canvas.px.data(), canvas.w, canvas.h, premultiply, fill.data() + fo, nullptr, a10.data() + ao, kYBlack, kYWhite);
        durations.push_back(dur);
        return true;
    }, error);
    if (!ok || durations.empty()) {
        if (error.empty()) error = "the animation has no frame";
        return false;
    }
    return true;
}

}  // namespace mtp

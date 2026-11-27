#include "widgets.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <sstream>
#include <strings.h>

namespace mtp {

namespace {

using json = nlohmann::json;

// True when the WIDGET_FRAME_ANCESTORS source list names this Origin (or holds `*`).
bool listed_origin(const std::string& sources, std::string origin) {
    auto normal = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        while (!s.empty() && s.back() == '/') s.pop_back();
        return s;
    };
    origin = normal(origin);
    if (origin.empty() || origin == "null") return false;
    std::istringstream tokens(sources);
    std::string token;
    while (tokens >> token)
        if (token == "*" || normal(token) == origin) return true;
    return false;
}

}  // namespace

bool widget_path(const std::string& path) { return path == "/widgets" || path.rfind("/widget/", 0) == 0; }

void widget_response(const Config& cfg, const HttpRequest& req, const std::string& page, HttpResponse& res) {
    res.cors = false;  // not the API's Access-Control-Allow-Origin: *
    auto error = [&](int status, const std::string& message) {
        res.status = status;
        res.set_text(json{{"error", message}}.dump());
    };
    if (req.method != "GET") {
        res.headers["Allow"] = "GET";
        error(405, "widgets answer GET only");
        return;
    }
    // transport: on air, next and the take (a test source: its transport); shotbox: clips and
    // playlists to cue; rundown: the playlist on air (or `playlist`) to cue from.
    struct Widget {
        const char* id;
        const char* title;
        int w, h;
        bool playlist;
    };
    static const Widget widgets[] = {
        {"transport", "Playout transport", 420, 220, false},
        {"shotbox", "Shotbox", 400, 300, false},
        {"rundown", "Rundown", 420, 320, true},
    };
    if (req.path == "/widgets") {
        const json output = {{"type", "integer"},
                             {"minimum", 1},
                             {"maximum", cfg.outputs},
                             {"title", "Output"},
                             {"description", "1 is the first output (Out 1, API index 0)"}};
        json list = json::array();
        for (const auto& w : widgets) {
            json props = {{"output", output}};
            if (w.playlist) props["playlist"] = {{"type", "string"}, {"title", "Playlist"}, {"description", "a playlist id; without it the playlist on air"}};
            list.push_back(json{{"id", w.id},
                                {"title", w.title},
                                {"params", {{"type", "object"}, {"properties", props}, {"required", json::array({"output"})}}},
                                {"min_size", {{"w", w.w}, {"h", w.h}}},
                                {"version", PLAYER_VERSION}});
        }
        res.set_text(list.dump());
        for (const auto& [key, value] : req.headers)
            if (strcasecmp(key.c_str(), "Origin") == 0 && listed_origin(cfg.widget_frame_ancestors, value))
                res.headers["Access-Control-Allow-Origin"] = value;
        res.headers["Vary"] = "Origin";
        return;
    }
    bool known = false;
    for (const auto& w : widgets)
        if (req.path == std::string("/widget/") + w.id) known = true;
    if (!known) {
        error(404, "widget not found");
        return;
    }
    const auto output = query_get(req.query, "output");
    if (output.empty() || output.size() > 2 || output.find_first_not_of("0123456789") != std::string::npos || std::stoi(output) < 1 ||
        std::stoi(output) > cfg.outputs) {
        error(400, "output must be an output 1.." + std::to_string(cfg.outputs));
        return;
    }
    const auto theme = query_get(req.query, "theme");
    if (!theme.empty() && theme != "dark" && theme != "light" && theme != "transparent") {
        error(400, "theme must be dark, light or transparent");
        return;
    }
    // The page picks the widget from its URL. Only these routes may be framed.
    res.status = 200;
    res.set_text(page, "text/html; charset=utf-8");
    res.headers["Content-Security-Policy"] = "frame-ancestors " + cfg.widget_frame_ancestors;
}

}  // namespace mtp

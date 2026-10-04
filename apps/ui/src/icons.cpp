#include "icons.hpp"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "icons_data.hpp"

namespace procyon::ui::icons {
namespace {

// ---- SVG path data ----

class PathData {
public:
    explicit PathData(std::string_view d) : d_(d) {}

    void parse(Path &outline, Path &closed) {
        char command = 0;
        Point current{}, start{}, last_control{};
        char last_command = 0;
        // The figure being built, replayed into `closed` when it ends with Z.
        std::vector<Path::Command> figure;
        bool figure_closed = false;
        auto flush = [&] {
            if (figure_closed)
                for (const Path::Command &c : figure) replay(closed, c);
            figure.clear();
            figure_closed = false;
        };
        auto emit = [&](Path::Command c) {
            replay(outline, c);
            figure.push_back(c);
        };
        skip_separators();
        while (!at_end()) {
            const char c = peek();
            if (std::isalpha(static_cast<unsigned char>(c))) {
                command = c;
                ++pos_;
            } else if (command == 0) {
                break;
            } else if (command == 'M') {
                command = 'L';  // extra pairs after M are line-tos
            } else if (command == 'm') {
                command = 'l';
            }
            const bool relative = std::islower(static_cast<unsigned char>(command)) != 0;
            const Point base = relative ? current : Point{};
            switch (std::toupper(static_cast<unsigned char>(command))) {
                case 'M': {
                    flush();
                    const Point p = add(base, point());
                    emit({Path::Op::Move, p, {}, {}});
                    current = start = p;
                    break;
                }
                case 'L': {
                    const Point p = add(base, point());
                    emit({Path::Op::Line, p, {}, {}});
                    current = p;
                    break;
                }
                case 'H': {
                    const Point p{(relative ? current.x : 0) + number(), current.y};
                    emit({Path::Op::Line, p, {}, {}});
                    current = p;
                    break;
                }
                case 'V': {
                    const Point p{current.x, (relative ? current.y : 0) + number()};
                    emit({Path::Op::Line, p, {}, {}});
                    current = p;
                    break;
                }
                case 'C': {
                    const Point c1 = add(base, point()), c2 = add(base, point()), p = add(base, point());
                    emit({Path::Op::Cubic, p, c1, c2});
                    last_control = c2;
                    current = p;
                    break;
                }
                case 'S': {
                    const bool smooth = last_command == 'C' || last_command == 'S';
                    const Point c1 = smooth ? reflect(last_control, current) : current;
                    const Point c2 = add(base, point()), p = add(base, point());
                    emit({Path::Op::Cubic, p, c1, c2});
                    last_control = c2;
                    current = p;
                    break;
                }
                case 'Q': {
                    const Point c1 = add(base, point()), p = add(base, point());
                    emit({Path::Op::Quad, p, c1, {}});
                    last_control = c1;
                    current = p;
                    break;
                }
                case 'T': {
                    const bool smooth = last_command == 'Q' || last_command == 'T';
                    const Point c1 = smooth ? reflect(last_control, current) : current;
                    const Point p = add(base, point());
                    emit({Path::Op::Quad, p, c1, {}});
                    last_control = c1;
                    current = p;
                    break;
                }
                case 'A': {
                    const float rx = number(), ry = number(), rotation = number();
                    const bool large = flag(), sweep = flag();
                    const Point p = add(base, point());
                    Path::Command arc{Path::Op::Arc, p, {}, {}};
                    arc.rx = std::fabs(rx);
                    arc.ry = std::fabs(ry);
                    arc.rotation = rotation;
                    arc.large = large;
                    arc.sweep = sweep;
                    emit(arc);
                    current = p;
                    break;
                }
                case 'Z': {
                    emit({Path::Op::Close, {}, {}, {}});
                    figure_closed = true;
                    current = start;
                    break;
                }
                default: return;
            }
            last_command = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
            skip_separators();
        }
        flush();
    }

private:
    static void replay(Path &path, const Path::Command &c) {
        switch (c.op) {
            case Path::Op::Move: path.move_to(c.p); break;
            case Path::Op::Line: path.line_to(c.p); break;
            case Path::Op::Quad: path.quad_to(c.c1, c.p); break;
            case Path::Op::Cubic: path.cubic_to(c.c1, c.c2, c.p); break;
            case Path::Op::Arc: path.arc_to(c.p, c.rx, c.ry, c.rotation, c.large, c.sweep); break;
            case Path::Op::Close: path.close(); break;
        }
    }
    static Point add(Point a, Point b) { return {a.x + b.x, a.y + b.y}; }
    static Point reflect(Point control, Point about) { return {2 * about.x - control.x, 2 * about.y - control.y}; }

    bool at_end() const { return pos_ >= d_.size(); }
    char peek() const { return d_[pos_]; }
    void skip_separators() {
        while (!at_end() && (std::isspace(static_cast<unsigned char>(peek())) || peek() == ',')) ++pos_;
    }
    float number() {
        skip_separators();
        const char *begin = d_.data() + pos_;
        char *end = nullptr;
        const float value = std::strtof(begin, &end);
        if (end == begin) {
            pos_ = d_.size();  // malformed: stop
            return 0;
        }
        pos_ += static_cast<size_t>(end - begin);
        return value;
    }
    bool flag() {
        skip_separators();
        if (at_end()) return false;
        const bool value = peek() == '1';
        ++pos_;
        return value;
    }
    Point point() {
        const float x = number();
        const float y = number();
        return {x, y};
    }

    std::string_view d_;
    size_t pos_ = 0;
};

// ---- elements ----

struct Element {
    std::string_view tag;
    std::map<std::string_view, std::string_view> attributes;

    float number(const char *name, float fallback = 0) const {
        auto it = attributes.find(name);
        if (it == attributes.end()) return fallback;
        return std::strtof(std::string(it->second).c_str(), nullptr);
    }
    bool has(const char *name) const { return attributes.count(name) != 0; }
};

std::vector<Element> elements(std::string_view svg) {
    std::vector<Element> out;
    size_t pos = 0;
    while ((pos = svg.find('<', pos)) != std::string_view::npos) {
        const size_t end = svg.find('>', pos);
        if (end == std::string_view::npos) break;
        std::string_view body = svg.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        if (body.empty() || body[0] == '/' || body[0] == '!' || body[0] == '?') continue;
        Element e;
        size_t i = 0;
        while (i < body.size() && !std::isspace(static_cast<unsigned char>(body[i])) && body[i] != '/') ++i;
        e.tag = body.substr(0, i);
        while (i < body.size()) {
            while (i < body.size() && (std::isspace(static_cast<unsigned char>(body[i])) || body[i] == '/')) ++i;
            const size_t name_start = i;
            while (i < body.size() && body[i] != '=' && !std::isspace(static_cast<unsigned char>(body[i]))) ++i;
            if (i >= body.size()) break;
            const std::string_view name = body.substr(name_start, i - name_start);
            while (i < body.size() && body[i] != '"') ++i;
            if (i >= body.size()) break;
            const size_t value_start = ++i;
            while (i < body.size() && body[i] != '"') ++i;
            e.attributes[name] = body.substr(value_start, i - value_start);
            if (i < body.size()) ++i;
        }
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<Point> points_list(std::string_view text) {
    std::vector<Point> out;
    std::string buffer(text);
    const char *p = buffer.c_str();
    while (*p) {
        char *end = nullptr;
        const float x = std::strtof(p, &end);
        if (end == p) break;
        p = end;
        while (*p == ',' || std::isspace(static_cast<unsigned char>(*p))) ++p;
        const float y = std::strtof(p, &end);
        if (end == p) break;
        p = end;
        while (*p == ',' || std::isspace(static_cast<unsigned char>(*p))) ++p;
        out.push_back({x, y});
    }
    return out;
}

}  // namespace

Icon parse_svg(std::string_view svg) {
    Icon icon;
    for (const Element &e : elements(svg)) {
        if (e.tag == "path") {
            auto it = e.attributes.find("d");
            if (it != e.attributes.end()) PathData(it->second).parse(icon.outline, icon.closed);
        } else if (e.tag == "rect") {
            const Rect r{e.number("x"), e.number("y"), e.number("width"), e.number("height")};
            const float rx = e.has("rx") ? e.number("rx") : e.number("ry");
            for (Path *path : {&icon.outline, &icon.closed}) path->add_round_rect(r, rx);
        } else if (e.tag == "circle") {
            const Point c{e.number("cx"), e.number("cy")};
            for (Path *path : {&icon.outline, &icon.closed}) path->add_circle(c, e.number("r"));
        } else if (e.tag == "ellipse") {
            const Point c{e.number("cx"), e.number("cy")};
            for (Path *path : {&icon.outline, &icon.closed}) path->add_ellipse(c, e.number("rx"), e.number("ry"));
        } else if (e.tag == "line") {
            icon.outline.move_to({e.number("x1"), e.number("y1")});
            icon.outline.line_to({e.number("x2"), e.number("y2")});
        } else if (e.tag == "polyline" || e.tag == "polygon") {
            auto it = e.attributes.find("points");
            if (it == e.attributes.end()) continue;
            const std::vector<Point> pts = points_list(it->second);
            if (pts.empty()) continue;
            const bool polygon = e.tag == "polygon";
            for (Path *path : {&icon.outline, polygon ? &icon.closed : nullptr}) {
                if (!path) continue;
                path->move_to(pts[0]);
                for (size_t i = 1; i < pts.size(); ++i) path->line_to(pts[i]);
                if (polygon) path->close();
            }
        }
    }
    return icon;
}

const Icon *lucide(std::string_view name) {
    static std::map<std::string, Icon, std::less<>> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return &it->second;
    for (const IconSvg &entry : kLucide) {
        if (name != entry.name) continue;
        return &cache.emplace(std::string(name), parse_svg(entry.svg)).first->second;
    }
    return nullptr;
}

}  // namespace procyon::ui::icons

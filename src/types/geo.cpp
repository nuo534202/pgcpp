// geo.cpp — geometric types implementations.
//
// Mirrors PostgreSQL's utils/adt/geo_ops.c with simplified storage.

#include "mytoydb/types/geo.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "mytoydb/common/error/elog.hpp"
#include "mytoydb/common/memory/memory_context.hpp"

namespace mytoydb::types {

using mytoydb::error::LogLevel;
using mytoydb::memory::MemoryContext;
using mytoydb::memory::palloc;

namespace {

char* PallocCString(std::string_view s) {
    char* buf = static_cast<char*>(palloc(s.size() + 1));
    if (!s.empty()) {
        std::memcpy(buf, s.data(), s.size());
    }
    buf[s.size()] = '\0';
    return buf;
}

void SkipWs(std::string_view s, std::size_t& it) {
    while (it < s.size() && std::isspace(static_cast<unsigned char>(s[it]))) {
        ++it;
    }
}

bool ParseChar(std::string_view s, std::size_t& it, char expected) {
    SkipWs(s, it);
    if (it < s.size() && s[it] == expected) {
        ++it;
        return true;
    }
    return false;
}

bool ParseDouble(std::string_view s, std::size_t& it, double& out) {
    SkipWs(s, it);
    if (it >= s.size()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    std::string buf(s);
    double val = std::strtod(buf.c_str() + it, &end);
    if (end == buf.c_str() + it) {
        return false;
    }
    out = val;
    it = static_cast<std::size_t>(end - buf.c_str());
    return true;
}

bool ParsePoint(std::string_view s, std::size_t& it, Point& out) {
    if (!ParseChar(s, it, '(')) {
        return false;
    }
    if (!ParseDouble(s, it, out.x)) {
        return false;
    }
    if (!ParseChar(s, it, ',')) {
        return false;
    }
    if (!ParseDouble(s, it, out.y)) {
        return false;
    }
    if (!ParseChar(s, it, ')')) {
        return false;
    }
    return true;
}

std::string FormatPoint(const Point& p) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%g,%g)", p.x, p.y);
    return buf;
}

}  // namespace

Datum MakePointDatum(const Point& p) {
    auto* v = static_cast<Point*>(palloc(sizeof(Point)));
    *v = p;
    return reinterpret_cast<Datum>(v);
}

Datum MakeBoxDatum(const Box& b) {
    auto* v = static_cast<Box*>(palloc(sizeof(Box)));
    *v = b;
    return reinterpret_cast<Datum>(v);
}

Datum MakeCircleDatum(const Circle& c) {
    auto* v = static_cast<Circle*>(palloc(sizeof(Circle)));
    *v = c;
    return reinterpret_cast<Datum>(v);
}

Datum point_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type point: NULL");
    }
    std::string_view s(str);
    std::size_t it = 0;
    Point p{};
    if (!ParsePoint(s, it, p) || it != s.size()) {
        ereport(LogLevel::kError,
                "invalid input syntax for type point: \"" + std::string(str) + "\"");
    }
    return MakePointDatum(p);
}

char* point_out(Datum value) {
    const auto* p = DatumGetPoint(value);
    return PallocCString(FormatPoint(*p));
}

Datum make_point(Datum x, Datum y) {
    Point p{DatumGetFloat8(x), DatumGetFloat8(y)};
    return MakePointDatum(p);
}

int point_cmp(Datum a, Datum b) {
    const auto* x = DatumGetPoint(a);
    const auto* y = DatumGetPoint(b);
    if (x->x != y->x) {
        return (x->x < y->x) ? -1 : 1;
    }
    if (x->y != y->y) {
        return (x->y < y->y) ? -1 : 1;
    }
    return 0;
}

Datum point_eq(Datum a, Datum b) {
    return BoolGetDatum(point_cmp(a, b) == 0);
}

Datum point_distance(Datum a, Datum b) {
    const auto* x = DatumGetPoint(a);
    const auto* y = DatumGetPoint(b);
    double dx = x->x - y->x;
    double dy = x->y - y->y;
    return Float8GetDatum(std::sqrt(dx * dx + dy * dy));
}

Datum lseg_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type lseg: NULL");
    }
    std::string_view s(str);
    std::size_t it = 0;
    SkipWs(s, it);
    bool had_bracket = ParseChar(s, it, '[');
    Lseg lseg{};
    if (!ParsePoint(s, it, lseg.p[0]) || !ParseChar(s, it, ',') || !ParsePoint(s, it, lseg.p[1])) {
        ereport(LogLevel::kError,
                "invalid input syntax for type lseg: \"" + std::string(str) + "\"");
    }
    if (had_bracket && !ParseChar(s, it, ']')) {
        ereport(LogLevel::kError, "expected ']' in lseg literal");
    }
    SkipWs(s, it);
    if (it != s.size()) {
        ereport(LogLevel::kError, "trailing garbage in lseg literal");
    }
    auto* v = static_cast<Lseg*>(palloc(sizeof(Lseg)));
    *v = lseg;
    return reinterpret_cast<Datum>(v);
}

char* lseg_out(Datum value) {
    const auto* l = reinterpret_cast<Lseg*>(value);
    std::string out = "[";
    out += FormatPoint(l->p[0]);
    out += ",";
    out += FormatPoint(l->p[1]);
    out += "]";
    return PallocCString(out);
}

Datum lseg_length(Datum value) {
    const auto* l = reinterpret_cast<Lseg*>(value);
    double dx = l->p[1].x - l->p[0].x;
    double dy = l->p[1].y - l->p[0].y;
    return Float8GetDatum(std::sqrt(dx * dx + dy * dy));
}

Datum box_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type box: NULL");
    }
    std::string_view s(str);
    std::size_t it = 0;
    SkipWs(s, it);
    bool had_bracket = ParseChar(s, it, '(');
    if (had_bracket) {
        SkipWs(s, it);
        // In PG box has the form "((x1,y1),(x2,y2))".
    }
    Point a{};
    Point b{};
    if (!ParsePoint(s, it, a) || !ParseChar(s, it, ',') || !ParsePoint(s, it, b)) {
        ereport(LogLevel::kError,
                "invalid input syntax for type box: \"" + std::string(str) + "\"");
    }
    if (had_bracket && !ParseChar(s, it, ')')) {
        ereport(LogLevel::kError, "expected ')' in box literal");
    }
    SkipWs(s, it);
    if (it != s.size()) {
        ereport(LogLevel::kError, "trailing garbage in box literal");
    }
    Box box{};
    box.high.x = (a.x > b.x) ? a.x : b.x;
    box.high.y = (a.y > b.y) ? a.y : b.y;
    box.low.x = (a.x < b.x) ? a.x : b.x;
    box.low.y = (a.y < b.y) ? a.y : b.y;
    return MakeBoxDatum(box);
}

char* box_out(Datum value) {
    const auto* b = DatumGetBox(value);
    std::string out = "(";
    out += FormatPoint(b->high);
    out += ",";
    out += FormatPoint(b->low);
    out += ")";
    return PallocCString(out);
}

int box_cmp(Datum a, Datum b) {
    const auto* x = DatumGetBox(a);
    const auto* y = DatumGetBox(b);
    int c = point_cmp(reinterpret_cast<Datum>(const_cast<Point*>(&x->high)),
                      reinterpret_cast<Datum>(const_cast<Point*>(&y->high)));
    if (c != 0) {
        return c;
    }
    return point_cmp(reinterpret_cast<Datum>(const_cast<Point*>(&x->low)),
                     reinterpret_cast<Datum>(const_cast<Point*>(&y->low)));
}

Datum box_area(Datum value) {
    const auto* b = DatumGetBox(value);
    double w = b->high.x - b->low.x;
    double h = b->high.y - b->low.y;
    return Float8GetDatum(w * h);
}

Datum box_width(Datum value) {
    const auto* b = DatumGetBox(value);
    return Float8GetDatum(b->high.x - b->low.x);
}

Datum box_height(Datum value) {
    const auto* b = DatumGetBox(value);
    return Float8GetDatum(b->high.y - b->low.y);
}

Datum line_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type line: NULL");
    }
    std::string_view s(str);
    std::size_t it = 0;
    SkipWs(s, it);
    bool had_brace = ParseChar(s, it, '{');
    Line line{};
    if (!ParseDouble(s, it, line.a) || !ParseChar(s, it, ',') || !ParseDouble(s, it, line.b) ||
        !ParseChar(s, it, ',') || !ParseDouble(s, it, line.c)) {
        ereport(LogLevel::kError,
                "invalid input syntax for type line: \"" + std::string(str) + "\"");
    }
    if (had_brace && !ParseChar(s, it, '}')) {
        ereport(LogLevel::kError, "expected '}' in line literal");
    }
    SkipWs(s, it);
    if (it != s.size()) {
        ereport(LogLevel::kError, "trailing garbage in line literal");
    }
    auto* v = static_cast<Line*>(palloc(sizeof(Line)));
    *v = line;
    return reinterpret_cast<Datum>(v);
}

char* line_out(Datum value) {
    const auto* l = reinterpret_cast<Line*>(value);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "{%g,%g,%g}", l->a, l->b, l->c);
    return PallocCString(buf);
}

Datum path_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type path: NULL");
    }
    std::string_view s(str);
    std::size_t it = 0;
    SkipWs(s, it);
    if (it >= s.size()) {
        ereport(LogLevel::kError, "invalid input syntax for type path: empty");
    }
    char open = s[it];
    bool closed = (open == '(');
    if (open != '[' && open != '(') {
        ereport(LogLevel::kError, "invalid input syntax for type path: expected '(' or '['");
    }
    ++it;
    auto* path = static_cast<Path*>(palloc(sizeof(Path)));
    new (path) Path();
    MemoryContext* ctx = mytoydb::memory::GetCurrentMemoryContext();
    if (ctx != nullptr) {
        ctx->RegisterDestructor(path, [](void* o) { static_cast<Path*>(o)->~Path(); });
    }
    path->closed = closed;
    while (true) {
        SkipWs(s, it);
        if (it < s.size() && (s[it] == ')' || s[it] == ']')) {
            ++it;
            break;
        }
        Point p{};
        if (!ParsePoint(s, it, p)) {
            ereport(LogLevel::kError, "invalid point in path literal");
        }
        path->pts.push_back(p);
        SkipWs(s, it);
        if (it < s.size() && s[it] == ',') {
            ++it;
            continue;
        }
        if (it < s.size() && (s[it] == ')' || s[it] == ']')) {
            ++it;
            break;
        }
        ereport(LogLevel::kError, "expected ',' or close in path literal");
    }
    return reinterpret_cast<Datum>(path);
}

char* path_out(Datum value) {
    const auto* p = reinterpret_cast<Path*>(value);
    std::string out;
    out.push_back(p->closed ? '(' : '[');
    for (std::size_t i = 0; i < p->pts.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        out += FormatPoint(p->pts[i]);
    }
    out.push_back(p->closed ? ')' : ']');
    return PallocCString(out);
}

Datum path_length(Datum value) {
    const auto* p = reinterpret_cast<Path*>(value);
    double total = 0.0;
    std::size_t count = p->pts.size();
    if (count < 2) {
        return Float8GetDatum(0.0);
    }
    std::size_t limit = p->closed ? count : count - 1;
    for (std::size_t i = 0; i < limit; ++i) {
        const auto& a = p->pts[i];
        const auto& b = p->pts[(i + 1) % count];
        double dx = a.x - b.x;
        double dy = a.y - b.y;
        total += std::sqrt(dx * dx + dy * dy);
    }
    return Float8GetDatum(total);
}

Datum path_npoints(Datum value) {
    const auto* p = reinterpret_cast<Path*>(value);
    return Int32GetDatum(static_cast<int32_t>(p->pts.size()));
}

Datum polygon_in(const char* str) {
    // A polygon is the same literal form as a closed path; we reuse path_in
    // and ignore the closed flag.
    Datum d = path_in(str);
    return d;
}

char* polygon_out(Datum value) {
    return path_out(value);
}

Datum polygon_npoints(Datum value) {
    return path_npoints(value);
}

Datum circle_in(const char* str) {
    if (str == nullptr) {
        ereport(LogLevel::kError, "invalid input syntax for type circle: NULL");
    }
    std::string_view s(str);
    std::size_t it = 0;
    SkipWs(s, it);
    bool had_bracket = ParseChar(s, it, '<');
    Circle c{};
    if (!ParsePoint(s, it, c.center) || !ParseChar(s, it, ',') || !ParseDouble(s, it, c.radius)) {
        ereport(LogLevel::kError,
                "invalid input syntax for type circle: \"" + std::string(str) + "\"");
    }
    if (had_bracket && !ParseChar(s, it, '>')) {
        ereport(LogLevel::kError, "expected '>' in circle literal");
    }
    SkipWs(s, it);
    if (it != s.size()) {
        ereport(LogLevel::kError, "trailing garbage in circle literal");
    }
    return MakeCircleDatum(c);
}

char* circle_out(Datum value) {
    const auto* c = DatumGetCircle(value);
    std::string out = "<";
    out += FormatPoint(c->center);
    char buf[32];
    std::snprintf(buf, sizeof(buf), ",%g>", c->radius);
    out += buf;
    return PallocCString(out);
}

Datum circle_area(Datum value) {
    const auto* c = DatumGetCircle(value);
    return Float8GetDatum(M_PI * c->radius * c->radius);
}

Datum circle_radius(Datum value) {
    const auto* c = DatumGetCircle(value);
    return Float8GetDatum(c->radius);
}

}  // namespace mytoydb::types

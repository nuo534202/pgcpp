#pragma once

#include <cstdint>
#include <vector>

#include "types/datum.hpp"

namespace pgcpp::types {

// ---------------------------------------------------------------------------
// Geometric types (PostgreSQL utils/adt/geo_ops.c).
//
//   point   (OID 600) : (x, y)
//   line    (OID 628) : Ax + By + C = 0  (stored as {A, B, C})
//   lseg    (OID 601) : two endpoints (p1, p2)
//   box     (OID 603) : corners (high, low)
//   path    (OID 602) : ordered list of points; open or closed
//   polygon (OID 604) : closed path; treated as polygon
//   circle  (OID 718) : (center, radius)
//
// Storage: a palloc'd struct; Datum is a pointer.
// Display forms match PostgreSQL: "(x,y)" / "[(x1,y1),(x2,y2)]" / etc.
// ---------------------------------------------------------------------------

struct Point {
    double x;
    double y;
};

struct Lseg {
    Point p[2];
};

struct Box {
    Point high;
    Point low;
};

struct Line {
    double a;
    double b;
    double c;
};

struct Path {
    bool closed;
    std::vector<Point> pts;
};

struct Circle {
    Point center;
    double radius;
};

// --- point ---
Datum point_in(const char* str);
char* point_out(Datum value);
Datum make_point(Datum x, Datum y);
int point_cmp(Datum a, Datum b);
Datum point_eq(Datum a, Datum b);
Datum point_distance(Datum a, Datum b);

// --- lseg ---
Datum lseg_in(const char* str);
char* lseg_out(Datum value);
Datum lseg_length(Datum value);

// --- box ---
Datum box_in(const char* str);
char* box_out(Datum value);
int box_cmp(Datum a, Datum b);
Datum box_area(Datum value);
Datum box_width(Datum value);
Datum box_height(Datum value);

// --- line ---
Datum line_in(const char* str);
char* line_out(Datum value);

// --- path ---
Datum path_in(const char* str);
char* path_out(Datum value);
Datum path_length(Datum value);
Datum path_npoints(Datum value);

// --- polygon ---
Datum polygon_in(const char* str);
char* polygon_out(Datum value);
Datum polygon_npoints(Datum value);

// --- circle ---
Datum circle_in(const char* str);
char* circle_out(Datum value);
Datum circle_area(Datum value);
Datum circle_radius(Datum value);

// Helpers.
Datum MakePointDatum(const Point& p);
Datum MakeBoxDatum(const Box& b);
Datum MakeCircleDatum(const Circle& c);
inline Point* DatumGetPoint(Datum x) {
    return reinterpret_cast<Point*>(x);
}
inline Box* DatumGetBox(Datum x) {
    return reinterpret_cast<Box*>(x);
}
inline Circle* DatumGetCircle(Datum x) {
    return reinterpret_cast<Circle*>(x);
}

}  // namespace pgcpp::types

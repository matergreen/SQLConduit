#ifndef SQLCONDUIT_COMMON_PG_TYPES_H
#define SQLCONDUIT_COMMON_PG_TYPES_H

#include "sqlconduit/common/types.h"

#include <optional>
#include <string>
#include <vector>

namespace sqlconduit::common {
    struct PgPoint {
        double x = 0.0;
        double y = 0.0;
    };

    struct PgLine {
        double a = 0.0;
        double b = 0.0;
        double c = 0.0;
    };

    struct PgLseg {
        PgPoint p1;
        PgPoint p2;
    };

    struct PgBox {
        PgPoint high;
        PgPoint low;
    };

    struct PgPath {
        bool closed = false;
        std::vector<PgPoint> points;
    };

    struct PgPolygon {
        std::vector<PgPoint> points;
    };

    struct PgCircle {
        PgPoint center;
        double radius = 0.0;
    };

    inline bool operator==(const PgPoint &a, const PgPoint &b) { return a.x == b.x && a.y == b.y; }
    inline bool operator!=(const PgPoint &a, const PgPoint &b) { return !(a == b); }

    inline bool operator==(const PgLine &a, const PgLine &b) {
        return a.a == b.a && a.b == b.b && a.c == b.c;
    }

    inline bool operator!=(const PgLine &a, const PgLine &b) { return !(a == b); }
    inline bool operator==(const PgLseg &a, const PgLseg &b) { return a.p1 == b.p1 && a.p2 == b.p2; }
    inline bool operator!=(const PgLseg &a, const PgLseg &b) { return !(a == b); }
    inline bool operator==(const PgBox &a, const PgBox &b) { return a.high == b.high && a.low == b.low; }
    inline bool operator!=(const PgBox &a, const PgBox &b) { return !(a == b); }

    inline bool operator==(const PgPath &a, const PgPath &b) {
        return a.closed == b.closed && a.points == b.points;
    }

    inline bool operator!=(const PgPath &a, const PgPath &b) { return !(a == b); }
    inline bool operator==(const PgPolygon &a, const PgPolygon &b) { return a.points == b.points; }
    inline bool operator!=(const PgPolygon &a, const PgPolygon &b) { return !(a == b); }

    inline bool operator==(const PgCircle &a, const PgCircle &b) {
        return a.center == b.center && a.radius == b.radius;
    }

    inline bool operator!=(const PgCircle &a, const PgCircle &b) { return !(a == b); }

    bool pgParseDouble(const std::string &text, double &out);

    std::string pgFormatDouble(double v);

    bool pgParsePoint(const std::string &text, PgPoint &out);

    bool pgParseLine(const std::string &text, PgLine &out);

    bool pgParseLseg(const std::string &text, PgLseg &out);

    bool pgParseBox(const std::string &text, PgBox &out);

    bool pgParsePath(const std::string &text, PgPath &out);

    bool pgParsePolygon(const std::string &text, PgPolygon &out);

    bool pgParseCircle(const std::string &text, PgCircle &out);

    std::string pgFormatPoint(const PgPoint &p);

    std::string pgFormatLine(const PgLine &l);

    std::string pgFormatLseg(const PgLseg &s);

    std::string pgFormatBox(const PgBox &b);

    std::string pgFormatPath(const PgPath &p);

    std::string pgFormatPolygon(const PgPolygon &p);

    std::string pgFormatCircle(const PgCircle &c);

    std::string pgGeometryToJson(const PgPoint &p);

    std::string pgGeometryToJson(const PgLine &l);

    std::string pgGeometryToJson(const PgLseg &s);

    std::string pgGeometryToJson(const PgBox &b);

    std::string pgGeometryToJson(const PgPath &p);

    std::string pgGeometryToJson(const PgPolygon &p);

    std::string pgGeometryToJson(const PgCircle &c);

    bool pgParseArray(const std::string &text, std::vector<std::optional<std::string> > &out);

    bool pgParseComposite(const std::string &text, std::vector<std::optional<std::string> > &out);

    std::string pgQuoteArrayElement(const std::string &raw);

    std::string pgFormatArray(const std::vector<std::optional<std::string> > &elements);

    std::string pgFormatComposite(const std::vector<std::optional<std::string> > &elements);

    bool pgIsGeometryOid(std::uint32_t oid);

    const char *pgGeometryTypeName(std::uint32_t oid);
}

#endif

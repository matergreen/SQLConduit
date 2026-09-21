#include "sqlconduit/common/pg_types.h"

#include <cmath>
#include <string>
#include <vector>

namespace sqlconduit::common
{
    namespace
    {
        struct Scanner
        {
            const std::string& s;
            std::size_t i = 0;

            explicit Scanner(const std::string& text) : s(text)
            {
            }

            void ws()
            {
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
                    ++i;
            }

            bool lit(const char c)
            {
                ws();
                if (i < s.size() && s[i] == c)
                {
                    ++i;
                    return true;
                }
                return false;
            }

            bool eof()
            {
                ws();
                return i >= s.size();
            }

            bool number(double& out)
            {
                ws();
                const std::size_t start = i;
                bool negative = false;
                if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                {
                    negative = s[i] == '-';
                    ++i;
                }
                double whole = 0;
                bool digit = false;
                while (i < s.size() && s[i] >= '0' && s[i] <= '9')
                {
                    whole = whole * 10 + (s[i] - '0');
                    ++i;
                    digit = true;
                }
                double frac = 0;
                double scale = 1;
                if (i < s.size() && s[i] == '.')
                {
                    ++i;
                    while (i < s.size() && s[i] >= '0' && s[i] <= '9')
                    {
                        frac = frac * 10 + (s[i] - '0');
                        scale *= 10;
                        ++i;
                        digit = true;
                    }
                }
                if (!digit)
                {
                    i = start;
                    return false;
                }
                int exp = 0;
                if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
                {
                    ++i;
                    bool negExp = false;
                    if (i < s.size() && (s[i] == '+' || s[i] == '-'))
                    {
                        negExp = s[i] == '-';
                        ++i;
                    }
                    int e = 0;
                    bool expDigit = false;
                    while (i < s.size() && s[i] >= '0' && s[i] <= '9')
                    {
                        if (e < 100000) e = e * 10 + (s[i] - '0');
                        ++i;
                        expDigit = true;
                    }
                    if (!expDigit)
                    {
                        i = start;
                        return false;
                    }
                    exp = negExp ? -e : e;
                }
                double v = (whole + frac / scale) * std::pow(10.0, exp);
                out = negative ? -v : v;
                return true;
            }

            bool point(PgPoint& out)
            {
                if (!lit('(')) return false;
                if (!number(out.x)) return false;
                if (!lit(',')) return false;
                if (!number(out.y)) return false;
                return lit(')');
            }

            bool points(const char open, const char close, std::vector<PgPoint>& out)
            {
                if (!lit(open)) return false;
                if (lit(close)) return true;
                for (;;)
                {
                    PgPoint p;
                    if (!point(p)) return false;
                    out.push_back(p);
                    if (lit(',')) continue;
                    return lit(close);
                }
            }
        };

        std::string num(const double v) { return pgFormatDouble(v); }

        std::string jsonPoint(const PgPoint& p)
        {
            return std::string("{\"x\":") + num(p.x) + ",\"y\":" + num(p.y) + "}";
        }

        std::string jsonPoints(const std::vector<PgPoint>& pts)
        {
            std::string s = "[";
            for (std::size_t i = 0; i < pts.size(); ++i)
            {
                if (i) s += ',';
                s += jsonPoint(pts[i]);
            }
            s += ']';
            return s;
        }

        bool needsQuoting(const std::string& raw)
        {
            if (raw.empty()) return true;
            if (raw.size() == 4 && (raw[0] == 'N' || raw[0] == 'n') &&
                (raw[1] == 'U' || raw[1] == 'u') && (raw[2] == 'L' || raw[2] == 'l') &&
                (raw[3] == 'L' || raw[3] == 'l'))
                return true;
            for (const char c : raw)
            {
                if (c == '{' || c == '}' || c == ',' || c == '"' || c == '\\') return true;
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return true;
            }
            return false;
        }

        bool splitElements(const std::string& text, const char open, const char close,
                           const bool emptyIsNull, std::vector<std::optional<std::string>>& out)
        {
            if (text.size() < 2 || text.front() != open || text.back() != close) return false;
            if (text.size() == 2)
            {
                out.clear();
                return true;
            }
            out.clear();
            std::string raw;
            bool quoted = false;
            bool quotedAny = false;
            int depth = 0;

            const auto flush = [&]()
            {
                if (!quotedAny && raw == "NULL")
                {
                    out.emplace_back(std::nullopt);
                }
                else if (!quotedAny && raw.empty() && emptyIsNull)
                {
                    out.emplace_back(std::nullopt);
                }
                else
                {
                    out.emplace_back(raw);
                }
                raw.clear();
                quotedAny = false;
            };

            for (std::size_t i = 1; i + 1 < text.size();)
            {
                const char c = text[i];
                if (quoted)
                {
                    if (c == '\\' && i + 1 < text.size() - 1)
                    {
                        raw.push_back(text[i + 1]);
                        i += 2;
                        continue;
                    }
                    if (c == '"')
                    {
                        quoted = false;
                        ++i;
                        continue;
                    }
                    raw.push_back(c);
                    ++i;
                    continue;
                }
                if (c == '"')
                {
                    quoted = true;
                    quotedAny = true;
                    ++i;
                    continue;
                }
                if (c == open)
                {
                    ++depth;
                    raw.push_back(c);
                    ++i;
                    continue;
                }
                if (c == close)
                {
                    --depth;
                    if (depth < 0) return false;
                    raw.push_back(c);
                    ++i;
                    continue;
                }
                if (c == ',' && depth == 0)
                {
                    flush();
                    ++i;
                    continue;
                }
                raw.push_back(c);
                ++i;
            }
            if (quoted || depth != 0) return false;
            flush();
            return true;
        }
    }

    bool pgParseDouble(const std::string& text, double& out)
    {
        Scanner sc{text};
        if (!sc.number(out)) return false;
        return sc.eof();
    }

    std::string pgFormatDouble(const double v)
    {
        if (std::isnan(v)) return "NaN";
        if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
        char buf[64];
        for (int precision = 15; precision <= 17; ++precision)
        {
            std::snprintf(buf, sizeof(buf), "%.*g", precision, v);
            double back = 0;
            if (pgParseDouble(std::string(buf), back) && back == v) break;
        }
        std::string out(buf);
        for (char& c : out)
            if (c == ',') c = '.';
        return out;
    }

    bool pgParsePoint(const std::string& text, PgPoint& out)
    {
        Scanner sc{text};
        return sc.point(out) && sc.eof();
    }

    bool pgParseLine(const std::string& text, PgLine& out)
    {
        Scanner sc{text};
        if (!sc.lit('{')) return false;
        if (!sc.number(out.a)) return false;
        if (!sc.lit(',')) return false;
        if (!sc.number(out.b)) return false;
        if (!sc.lit(',')) return false;
        if (!sc.number(out.c)) return false;
        return sc.lit('}') && sc.eof();
    }

    bool pgParseLseg(const std::string& text, PgLseg& out)
    {
        Scanner sc{text};
        if (!sc.lit('[')) return false;
        if (!sc.point(out.p1)) return false;
        if (!sc.lit(',')) return false;
        if (!sc.point(out.p2)) return false;
        return sc.lit(']') && sc.eof();
    }

    bool pgParseBox(const std::string& text, PgBox& out)
    {
        Scanner sc{text};
        if (!sc.point(out.high)) return false;
        if (!sc.lit(',')) return false;
        if (!sc.point(out.low)) return false;
        return sc.eof();
    }

    bool pgParsePath(const std::string& text, PgPath& out)
    {
        Scanner sc{text};
        sc.ws();
        const bool closed = sc.i < sc.s.size() && sc.s[sc.i] == '(';
        out.points.clear();
        out.closed = closed;
        if (closed) return sc.points('(', ')', out.points) && sc.eof();
        return sc.points('[', ']', out.points) && sc.eof();
    }

    bool pgParsePolygon(const std::string& text, PgPolygon& out)
    {
        Scanner sc{text};
        out.points.clear();
        return sc.points('(', ')', out.points) && sc.eof();
    }

    bool pgParseCircle(const std::string& text, PgCircle& out)
    {
        Scanner sc{text};
        if (!sc.lit('<')) return false;
        if (!sc.point(out.center)) return false;
        if (!sc.lit(',')) return false;
        if (!sc.number(out.radius)) return false;
        return sc.lit('>') && sc.eof();
    }

    std::string pgFormatPoint(const PgPoint& p)
    {
        return std::string("(") + num(p.x) + "," + num(p.y) + ")";
    }

    std::string pgFormatLine(const PgLine& l)
    {
        return std::string("{") + num(l.a) + "," + num(l.b) + "," + num(l.c) + "}";
    }

    std::string pgFormatLseg(const PgLseg& s)
    {
        return std::string("[") + pgFormatPoint(s.p1) + "," + pgFormatPoint(s.p2) + "]";
    }

    std::string pgFormatBox(const PgBox& b)
    {
        return pgFormatPoint(b.high) + "," + pgFormatPoint(b.low);
    }

    std::string pgFormatPath(const PgPath& p)
    {
        std::string s;
        s += p.closed ? '(' : '[';
        for (std::size_t i = 0; i < p.points.size(); ++i)
        {
            if (i) s += ',';
            s += pgFormatPoint(p.points[i]);
        }
        s += p.closed ? ')' : ']';
        return s;
    }

    std::string pgFormatPolygon(const PgPolygon& p)
    {
        std::string s = "(";
        for (std::size_t i = 0; i < p.points.size(); ++i)
        {
            if (i) s += ',';
            s += pgFormatPoint(p.points[i]);
        }
        s += ')';
        return s;
    }

    std::string pgFormatCircle(const PgCircle& c)
    {
        return std::string("<") + pgFormatPoint(c.center) + "," + num(c.radius) + ">";
    }

    std::string pgGeometryToJson(const PgPoint& p) { return jsonPoint(p); }

    std::string pgGeometryToJson(const PgLine& l)
    {
        return std::string("{\"a\":") + num(l.a) + ",\"b\":" + num(l.b) + ",\"c\":" + num(l.c) + "}";
    }

    std::string pgGeometryToJson(const PgLseg& s)
    {
        return std::string("{\"p1\":") + jsonPoint(s.p1) + ",\"p2\":" + jsonPoint(s.p2) + "}";
    }

    std::string pgGeometryToJson(const PgBox& b)
    {
        return std::string("{\"high\":") + jsonPoint(b.high) + ",\"low\":" + jsonPoint(b.low) + "}";
    }

    std::string pgGeometryToJson(const PgPath& p)
    {
        return std::string("{\"closed\":") + (p.closed ? "true" : "false")
            + ",\"points\":" + jsonPoints(p.points) + "}";
    }

    std::string pgGeometryToJson(const PgPolygon& p)
    {
        return std::string("{\"points\":") + jsonPoints(p.points) + "}";
    }

    std::string pgGeometryToJson(const PgCircle& c)
    {
        return std::string("{\"center\":") + jsonPoint(c.center) + ",\"radius\":" + num(c.radius) + "}";
    }

    bool pgParseArray(const std::string& text, std::vector<std::optional<std::string>>& out)
    {
        return splitElements(text, '{', '}', false, out);
    }

    bool pgParseComposite(const std::string& text, std::vector<std::optional<std::string>>& out)
    {
        return splitElements(text, '(', ')', true, out);
    }

    std::string pgQuoteArrayElement(const std::string& raw)
    {
        if (!needsQuoting(raw)) return raw;
        std::string s = "\"";
        for (const char c : raw)
        {
            if (c == '"' || c == '\\') s.push_back('\\');
            s.push_back(c);
        }
        s.push_back('"');
        return s;
    }

    std::string pgFormatArray(const std::vector<std::optional<std::string>>& elements)
    {
        std::string s = "{";
        for (std::size_t i = 0; i < elements.size(); ++i)
        {
            if (i) s += ',';
            if (elements[i]) s += pgQuoteArrayElement(*elements[i]);
            else s += "NULL";
        }
        s += '}';
        return s;
    }

    std::string pgFormatComposite(const std::vector<std::optional<std::string>>& elements)
    {
        std::string s = "(";
        for (std::size_t i = 0; i < elements.size(); ++i)
        {
            if (i) s += ',';
            if (elements[i]) s += pgQuoteArrayElement(*elements[i]);
        }
        s += ')';
        return s;
    }

    bool pgIsGeometryOid(const std::uint32_t oid)
    {
        return pgGeometryTypeName(oid) != nullptr;
    }

    const char* pgGeometryTypeName(const std::uint32_t oid)
    {
        switch (oid)
        {
        case 600: return "point";
        case 601: return "lseg";
        case 602: return "path";
        case 603: return "box";
        case 604: return "polygon";
        case 628: return "line";
        case 718: return "circle";
        default: return nullptr;
        }
    }
}

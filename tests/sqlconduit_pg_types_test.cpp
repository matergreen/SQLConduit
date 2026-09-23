#include "sqlconduit/common/pg_types.h"
#include "sqlconduit/common/types.h"
#include "sqlconduit/mapping.h"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace sqlconduit;

static int g_failed = 0;
static int g_passed = 0;

static void check(const bool cond, const std::string &name) {
    if (cond) {
        ++g_passed;
        std::cout << "  [PASS] " << name << "\n";
    } else {
        ++g_failed;
        std::cout << "  [FAIL] " << name << "\n";
    }
}

static std::string joined(const std::vector<std::optional<std::string> > &parts) {
    std::string s;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) s += '|';
        s += parts[i] ? *parts[i] : std::string("<NULL>");
    }
    return s;
}

int main() {
    std::cout << "== PG 数组文本：解析 ==\n";
    {
        std::vector<std::optional<std::string> > parts;
        check(common::pgParseArray("{1,2,3}", parts) && parts.size() == 3 &&
              joined(parts) == "1|2|3", "int4[] 文本 {1,2,3} 拆成 3 个元素");

        check(common::pgParseArray("{}", parts) && parts.empty(), "空数组 {} 得到 0 个元素");

        check(common::pgParseArray("{NULL,a}", parts) && parts.size() == 2 &&
              !parts[0] && parts[1] && *parts[1] == "a", "未加引号的 NULL 解析为 nullopt");

        check(common::pgParseArray(R"({"a,b",c})", parts) && parts.size() == 2 &&
              parts[0] && *parts[0] == "a,b", "带引号元素里的逗号不当分隔符");

        check(common::pgParseArray(R"({"a\"b"})", parts) && parts.size() == 1 &&
              parts[0] && *parts[0] == "a\"b", "引号内的 \\\" 转义为双引号");

        check(common::pgParseArray(R"({"a\\b"})", parts) && parts.size() == 1 &&
              parts[0] && *parts[0] == "a\\b", "引号内的 \\\\ 转义为单反斜杠");

        check(common::pgParseArray(R"({""})", parts) && parts.size() == 1 &&
              parts[0] && parts[0]->empty(), "引号空串解析为空字符串而非 NULL");

        check(common::pgParseArray("{{1,2},{3,4}}", parts) && parts.size() == 2 &&
              parts[0] && *parts[0] == "{1,2}" && parts[1] && *parts[1] == "{3,4}",
              "二维数组按顶层拆成两个嵌套文本");

        check(!common::pgParseArray("{1,2", parts), "缺少右花括号时解析失败");
        check(!common::pgParseArray("1,2", parts), "非数组文本解析失败");
    }

    std::cout << "\n== PG 数组文本：序列化 ==\n";
    {
        check(common::pgFormatArray({}) == "{}", "空数组序列化为 {}");
        check(common::pgFormatArray({"1", "2"}) == "{1,2}", "普通元素不引号");
        check(common::pgFormatArray({"a,b"}) == R"({"a,b"})", "含逗号的元素自动引号");
        check(common::pgFormatArray({"NULL"}) == R"({"NULL"})", "字面 NULL 必须引号以区别于 SQL NULL");
        check(common::pgFormatArray({std::optional<std::string>{}}) == "{NULL}",
              "nullopt 序列化为未加引号的 NULL");
        check(common::pgFormatArray({std::optional<std::string>{std::string()}}) == R"({""})",
              "空字符串序列化为 \"\"");
        check(common::pgFormatArray({"{1,2}"}) == R"({"{1,2}"})", "嵌套数组文本作为元素时引号包裹");

        std::vector<std::optional<std::string> > round;
        const std::string text = common::pgFormatArray({"a", std::optional<std::string>{}, "b,c"});
        check(common::pgParseArray(text, round) && joined(round) == "a|<NULL>|b,c",
              "序列化后再解析可无损还原");
    }

    std::cout << "\n== PG 复合类型文本 ==\n";
    {
        std::vector<std::optional<std::string> > parts;
        check(common::pgParseComposite("(a,b)", parts) && parts.size() == 2 &&
              joined(parts) == "a|b", "复合文本 (a,b) 拆成 2 个字段");

        check(common::pgParseComposite("(,b)", parts) && parts.size() == 2 &&
              !parts[0] && parts[1] && *parts[1] == "b", "未加引号的空字段是 NULL");

        check(common::pgParseComposite(R"((,""))", parts) && parts.size() == 2 &&
              !parts[0] && parts[1] && parts[1]->empty(), "\"\" 是空字符串不是 NULL");

        check(common::pgParseComposite("((1,2),3)", parts) && parts.size() == 2 &&
              parts[0] && *parts[0] == "(1,2)", "字段里的括号嵌套不破坏分隔");

        check(common::pgFormatComposite({"a", std::optional<std::string>{}, "b"}) == "(a,,b)",
              "NULL 字段序列化为空");
        check(common::pgFormatComposite({}) == "()", "空复合值序列化为 ()");
    }

    std::cout << "\n== 几何类型：文本解析与序列化 ==\n";
    {
        common::PgPoint p;
        check(common::pgParsePoint("(1,2)", p) && p.x == 1 && p.y == 2, "point (1,2) 解析");
        check(common::pgParsePoint("(1.5,-2.25)", p) && p.x == 1.5 && p.y == -2.25,
              "point 支持小数与负号");
        check(!common::pgParsePoint("(1,2", p), "point 缺右括号解析失败");
        check(common::pgFormatPoint(common::PgPoint{1, 2}) == "(1,2)", "point 序列化为 (1,2)");
        check(common::pgFormatPoint(common::PgPoint{1.5, -2}) == "(1.5,-2)", "point 小数紧凑输出");
        check(common::pgFormatDouble(0.1) == "0.1", "0.1 走最短往返表示而非 17 位尾数");
        check(common::pgFormatDouble(1.0) == "1", "整数值不带小数尾巴");

        common::PgLine l;
        check(common::pgParseLine("{1,2,3}", l) && l.a == 1 && l.b == 2 && l.c == 3, "line {a,b,c} 解析");
        check(common::pgFormatLine(common::PgLine{1, 2, 3}) == "{1,2,3}", "line 序列化");

        common::PgLseg s;
        check(common::pgParseLseg("[(1,2),(3,4)]", s) && s.p1.x == 1 && s.p2.y == 4, "lseg 解析");
        check(common::pgFormatLseg(common::PgLseg{{1, 2}, {3, 4}}) == "[(1,2),(3,4)]", "lseg 序列化");

        common::PgBox b;
        check(common::pgParseBox("(3,4),(1,2)", b) && b.high.x == 3 && b.high.y == 4 &&
              b.low.x == 1 && b.low.y == 2, "box 解析为 high/low 两点");
        check(common::pgFormatBox(common::PgBox{{3, 4}, {1, 2}}) == "(3,4),(1,2)", "box 序列化");

        common::PgPath path;
        check(common::pgParsePath("((1,2),(3,4))", path) && path.closed &&
              path.points.size() == 2, "闭合 path 解析");
        check(common::pgParsePath("[(1,2),(3,4)]", path) && !path.closed &&
              path.points.size() == 2, "开放 path 解析");
        check(common::pgFormatPath(common::PgPath{true, {{1, 2}}}) == "((1,2))", "闭合 path 序列化");
        check(common::pgFormatPath(common::PgPath{false, {{1, 2}}}) == "[(1,2)]", "开放 path 序列化");

        common::PgPolygon poly;
        check(common::pgParsePolygon("((1,2),(3,4),(5,6))", poly) && poly.points.size() == 3,
              "polygon 解析");
        check(common::pgFormatPolygon(common::PgPolygon{{{1, 2}, {3, 4}}}) == "((1,2),(3,4))",
              "polygon 序列化");

        common::PgCircle c;
        check(common::pgParseCircle("<(1,2),3>", c) && c.center.x == 1 && c.radius == 3,
              "circle 解析");
        check(common::pgFormatCircle(common::PgCircle{{1, 2}, 3}) == "<(1,2),3>", "circle 序列化");

        check(std::string(common::pgGeometryTypeName(600)) == "point" &&
              std::string(common::pgGeometryTypeName(718)) == "circle" &&
              common::pgGeometryTypeName(23) == nullptr, "几何 OID 名称映射");
        check(common::pgIsGeometryOid(603) && !common::pgIsGeometryOid(23), "几何 OID 判定");

        check(common::pgGeometryToJson(common::PgPoint{1, 2}) == "{\"x\":1,\"y\":2}",
              "point 的 JSON 表示");
        check(common::pgGeometryToJson(common::PgCircle{{1, 2}, 3}) ==
              "{\"center\":{\"x\":1,\"y\":2},\"radius\":3}", "circle 的 JSON 表示");
    }

    std::cout << "\n== common::Value：Array / Composite ==\n";
    {
        common::Array arr;
        arr.items.push_back(common::Value{std::int64_t{1}});
        arr.items.push_back(common::Value{std::string("x")});
        common::Value av{arr};
        check(std::holds_alternative<common::Array>(av), "Value 可承载 Array");
        check(std::get_if<common::Array>(&av)->items.size() == 2, "get_if<Array> 可用");

        common::Array nested;
        nested.items.push_back(av);
        common::Value nv{nested};
        check(std::get_if<common::Array>(&nv)->items.size() == 1 &&
              std::holds_alternative<common::Array>(std::get_if<common::Array>(&nv)->items[0]),
              "Array 可嵌套 Array");

        common::Composite comp;
        comp.fields.emplace_back("id", common::Value{std::int64_t{7}});
        common::Value cv{comp};
        check(std::get_if<common::Composite>(&cv)->find("id") != nullptr, "Composite::find 命中");
        check(std::get_if<common::Composite>(&cv)->find("nope") == nullptr,
              "Composite::find 未命中返回 nullptr");

        check(common::valueToString(av) == "[1, x]", "valueToString 渲染 Array");
        check(common::valueToString(cv) == "(id=7)", "valueToString 渲染 Composite");
        check(common::escapeLiteralGeneric(av) == "ARRAY[1, 'x']", "escapeLiteralGeneric 渲染 Array");
        check(common::escapeLiteralGeneric(cv) == "ROW(7)", "escapeLiteralGeneric 渲染 Composite");
        check(common::paramTypeSignature({av, cv}) == "AC", "paramTypeSignature 新增 A/C");

        common::Value copy = av;
        check(copy == av, "Array 深比较相等");
        copy = common::Value{common::Array{}};
        check(!(copy == av), "元素不同的 Array 比较不等");

        int visited = 0;
        common::visitValue([&visited](const auto &) { ++visited; }, av);
        check(visited == 1, "visitValue 可访问 Value");
    }

    std::cout << "\n== mapping：std::vector / 几何类型绑定 ==\n";
    {
        using namespace sqlconduit::mapping;

        common::Array arr;
        arr.items.push_back(common::Value{std::int64_t{1}});
        arr.items.push_back(common::Value{std::int64_t{2}});
        arr.items.push_back(common::Value{std::int64_t{3}});
        const common::Value av{arr};

        std::vector<int> ints;
        const auto s1 = ValueConverter<std::vector<int> >::fromValue(av, ints, FieldFlags::None);
        check(s1.ok() && ints.size() == 3 && ints[0] == 1 && ints[2] == 3,
              "Array -> std::vector<int>");

        std::vector<std::string> strs;
        common::Array sarr;
        sarr.items.push_back(common::Value{std::string("a")});
        sarr.items.push_back(common::Value{std::string("b")});
        const auto s2 = ValueConverter<std::vector<std::string> >::fromValue(
            common::Value{sarr}, strs, FieldFlags::None);
        check(s2.ok() && strs.size() == 2 && strs[1] == "b", "Array -> std::vector<std::string>");

        const common::Value back = ValueConverter<std::vector<int> >::toValue(ints);
        check(std::holds_alternative<common::Array>(back) &&
              std::get_if<common::Array>(&back)->items.size() == 3,
              "std::vector<int> -> Array");

        std::vector<double> bad;
        const auto s3 = ValueConverter<std::vector<double> >::fromValue(av, bad, FieldFlags::Lossy);
        check(s3.ok() && bad.size() == 3 && bad[0] == 1.0, "int 数组在 Lossy 下可转 double 数组");

        std::vector<int> mismatch;
        const auto s4 = ValueConverter<std::vector<int> >::fromValue(
            common::Value{std::string("nope")}, mismatch, FieldFlags::None);
        check(!s4.ok() && s4.code == common::ErrorCode::MappingError,
              "非 Array 值转 vector 返回 MappingError");

        common::PgPoint p{1, 2};
        const common::Value pv = ValueConverter<common::PgPoint>::toValue(p);
        check(std::holds_alternative<common::Json>(pv) &&
              std::get_if<common::Json>(&pv)->value == "(1,2)", "PgPoint -> Json(PG 文本)");

        common::PgPoint out;
        const auto s5 = ValueConverter<common::PgPoint>::fromValue(pv, out, FieldFlags::None);
        check(s5.ok() && out == p, "Json(PG 文本) -> PgPoint 往返一致");

        common::PgCircle circle{{0, 0}, 5};
        const common::Value cvv = ValueConverter<common::PgCircle>::toValue(circle);
        common::PgCircle cout;
        check(ValueConverter<common::PgCircle>::fromValue(cvv, cout, FieldFlags::None).ok() &&
              cout == circle, "PgCircle 往返一致");

        common::PgPath path{true, {{1, 2}, {3, 4}}};
        const common::Value pthv = ValueConverter<common::PgPath>::toValue(path);
        common::PgPath pout;
        check(ValueConverter<common::PgPath>::fromValue(pthv, pout, FieldFlags::None).ok() &&
              pout == path, "PgPath 往返一致（含 closed 标志）");

        common::PgPoint junk;
        check(!ValueConverter<common::PgPoint>::fromValue(
                  common::Value{common::Json{"not-a-point"}}, junk, FieldFlags::None).ok(),
              "非法几何文本返回错误");
    }

    std::cout << "\n----------------------------------------\n";
    std::cout << "通过 " << g_passed << " 项，失败 " << g_failed << " 项\n";
    return g_failed == 0 ? 0 : 1;
}

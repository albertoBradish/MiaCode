#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/chart/transform/ChartBatchTransform.h"
#include "core/chart/transform/BasicHandOrderLayout.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include <QCoreApplication>
#include <QTextStream>
#include <algorithm>

using namespace miacode::hand_order;
using namespace miacode::chart_transform;

namespace {
struct Expectation { int marker; bool track; HandOrderHand hand; };
struct Case { QString name, chart; QVector<Expectation> expected; };
constexpr auto left = HandOrderHand::Left, right = HandOrderHand::Right;

QString apply(QString text, const BasicResult& result)
{
    for (int i = result.patches.size() - 1; i >= 0; --i) {
        const auto& patch = result.patches.at(i);
        text.replace(patch.start, patch.end - patch.start, patch.text);
    }
    return text;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QVector<Case> cases{
        {"hold-ended sweeps", "{24}3,,,4,,,2h[4:1]/5,,,,,,1,8,7h[24:7],,,,,,,8,1,2h[24:7],,,,,,,",
            {{4,false,left},{5,false,left},{6,false,left},{7,false,right},{8,false,right},{9,false,right}}},
        {"shared arcs after one stroke", "{8}7>2[16:3],1>4[16:3],3<6[16:3],5<8[16:3],,,,1<5[8:1]*>5[8:1],",
            {{4,true,left},{5,true,right}}},
        {"one-stroke entry tap", "{8},2V46[4:1],4,6V82[4:1],8-4[8:1],8,,,",
            {{1,false,left},{0,true,right},{2,true,right}}},
        {"shared lines after hold", "{8}5,6h[4:1],,8-6[8:1],6-2[8:1]*-4[8:1],,,,",
            {{3,true,right},{4,true,left}}},
        {"shallow Each reach", "{8}2/6h[8:1],2,1h[8:1]/5,5,4/8,4/8,1/5,1/5,",
            {{6,false,left},{7,false,right},{8,false,left},{9,false,right},{11,false,left},{13,false,left}}},
        {"same-head coordination and following sweep", "{8}6-4[8:1],4V68[4:1],6,1,2,2-6[8:1],,2>5[8:1]/6,{24},,,1/2,8,7,6,,,,,,1/8,,,1/8,,,2/7-5[8:1],,,,,",
            {{5,true,right},{6,true,right},{8,false,left},{9,false,right},{10,false,left},{11,false,left},{12,false,left}}},
        {"DX Touch at one-stroke entry", "{8},2V46[4:1],A4,6V82[4:1],8-4[8:1],8,,,",
            {{1,false,left}}},
        {"moderate same-side sweep after slide", "(135){16}1q5[16:3],,,,,,,,8,7,6,5,8,1,2,3,",
            {{1,false,left},{2,false,left},{3,false,left},{4,false,left}}}
    };
    QVector<PlayerProfile> profiles(4);
    profiles[1] = {8.5,5.5,1.4,3.0,0.8,3.0};
    profiles[2] = {6.5,7.5,1.1,2.5,0.25,5.0};
    profiles[3] = {5.0,8.0,1.0,2.0,-0.9,3.0};
    bool ok = true;
    const QString shallowManual = "(180){8}4~l/8~r,4~l/8~r,1~r/5~l,";
    const auto retained = basicHandOrderLayoutInSelection(shallowManual, 0, 0);
    const bool keepsShallow = retained.edit.replacement == shallowManual;
    QTextStream(keepsShallow ? stdout : stderr) << (keepsShallow ? "PASS: " : "FAIL: ")
        << "basic layout retains explicitly chosen shallow Each reaches\n";
    ok &= keepsShallow;
    for (const auto& fixture : cases) for (int profile = 0; profile < profiles.size(); ++profile)
        for (int mirrored = 0; mirrored < 2; ++mirrored)
        for (int modified = 0; modified < (fixture.name.startsWith("DX") ? 1 : 2); ++modified) {
            QString chart = (fixture.chart.startsWith('(') ? QString{} : QStringLiteral("(180)")) + fixture.chart;
            if (mirrored) chart = transformChartText(chart, ChartTransformOp::MirrorLeftRight);
            if (modified) chart = toggleExForSelection(toggleBreakForSelection(chart));
            BasicParameters p; p.player = profiles.at(profile); p.counterfactualBudget = 0;
            const auto result = inferBasicHandOrder(chart, SimaiParser::validateSyntax(chart), 0, chart.size(), p);
            const auto output = SimaiParser::validateSyntax(apply(chart, result));
            const auto native = SimaiParser::validateSyntax(chart);
            bool pass = result.error.isEmpty() && !result.workLimited && output.ok
                && output.annotationErrors.isEmpty() && output.noteMarkers.size() == native.noteMarkers.size()
                && stripAnnotations(apply(chart, result)) == chart
                && std::all_of(result.rows.cbegin(), result.rows.cend(), [](const auto& row) { return row.fullReplayVerified; });
            for (const auto& expected : fixture.expected) {
                const auto found = std::find_if(output.noteMarkers.cbegin(), output.noteMarkers.cend(), [&](const auto& m) {
                    return m.parseOrder == expected.marker;
                });
                if (found == output.noteMarkers.cend()) { pass = false; continue; }
                const auto& marker = *found;
                const auto actual = expected.track ? (marker.handOrderTrackStops.isEmpty()
                    ? HandOrderHand::Unspecified : marker.handOrderTrackStops.first().hand) : marker.handOrderHead;
                const auto hand = mirrored ? (expected.hand == left ? right : left) : expected.hand;
                pass &= actual == hand;
            }
            QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << fixture.name
                << " profile=" << profile << " mirrored=" << mirrored << " EX/Break=" << modified << '\n';
            if (!pass) {
                QTextStream(stderr) << apply(chart, result) << " error=" << result.error << " limited=" << result.workLimited << '\n';
                for (const auto& row : result.rows) if (!row.fullReplayVerified)
                    QTextStream(stderr) << row.start << ' ' << row.status << ' ' << row.reason << '\n';
                for (const auto& expected : fixture.expected) for (const auto& m : output.noteMarkers)
                    if (m.parseOrder == expected.marker) {
                    QTextStream(stderr) << expected.marker << " lane=" << m.lane << " head=" << int(m.handOrderHead)
                        << " track=" << (m.handOrderTrackStops.isEmpty() ? -1 : int(m.handOrderTrackStops.first().hand)) << '\n';
                }
            }
            ok &= pass;
        }
    return ok ? 0 : 1;
}

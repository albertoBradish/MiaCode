#include "core/analysis/hand_order/HandOrderHoldCandidates.h"
#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/chart/parser/SimaiParser.h"
#include <QCoreApplication>
#include <QTextStream>

using namespace miacode::hand_order;
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    bool ok = true;
    const auto expect = [&](bool pass, const char* message) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << message << '\n';
        ok &= pass;
    };
    for (const auto& text : {QStringLiteral("(120){4}1h[2:1],"), QStringLiteral("(156){8}1h[8:1],"),
                             QStringLiteral("(120){4}Ch[2:1],")}) {
        const auto parsed = SimaiParser::validateSyntax(text);
        expect(parsed.ok && parsed.noteMarkers.size() == 1, "hold fixture parses as one native object");
        if (!parsed.ok || parsed.noteMarkers.size() != 1) continue;
        const auto marker = parsed.noteMarkers.first();
        const bool button = marker.type == "hold";
        ContactAction full;
        full.kind = button ? QStringLiteral("button") : QStringLiteral("touch_hold");
        full.hand = 1; full.start = marker.second; full.end = marker.endSecond;
        full.points = {button ? QPointF(0.3826834324, -0.9238795325) : touchPadPoint(marker.touchPad)};
        full.times = {full.start};
        const auto choices = holdReleaseCandidates(full, marker, 0.025);
        expect(!choices.isEmpty() && choices.first().end == full.end,
               "the full-duration physical hold remains the first option");
        for (const auto& action : choices) {
            const auto replay = replayHandOrderChart({marker}, {{action, button, marker.lane}});
            expect(replay.valid && replay.meetsTarget && replay.judgements.first().highQuality,
                   "every early release candidate independently satisfies the unchanged full replay");
        }
        if (marker.endSecond - marker.second > 0.9) {
            expect(choices.size() == 3, "long stationary hold exposes two distinct guarded releases");
            auto unsafe = full; unsafe.end = full.start + 0.7;
            const auto replay = replayHandOrderChart({marker}, {{unsafe, button, marker.lane}});
            expect(!replay.meetsTarget, "releasing inside the required hold body still fails the target");
        }
    }
    const QString chain = QStringLiteral("(130){8}6,5,4,3,{16}2h[16:3],,,3h[16:3],,,4,,");
    BasicParameters parameters; parameters.counterfactualBudget = 0;
    const auto result = inferBasicHandOrder(chain, SimaiParser::validateSyntax(chain), 0, chain.size(), parameters);
    expect(!result.rows.isEmpty() && std::all_of(result.rows.cbegin(), result.rows.cend(), [](const auto& row) {
        return row.fullReplayVerified;
    }) && result.rows.last().peakSpeed < 8.0,
        "successive holds retain a comfortable verified approach to the following adjacent tap");
    return ok ? 0 : 1;
}

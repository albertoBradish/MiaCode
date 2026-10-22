#include "core/chart/transform/BasicHandOrderLayout.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"

#include <QCoreApplication>
#include <QTextStream>
#include <limits>

namespace {
bool sameNative(const QString& a, const QString& b)
{
    const auto first = SimaiParser::parseForTimeline(a);
    const auto second = SimaiParser::parseForTimeline(b);
    if (!first.ok || !second.ok || first.noteMarkers.size() != second.noteMarkers.size()
        || first.durationSeconds != second.durationSeconds) return false;
    for (int index = 0; index < first.noteMarkers.size(); ++index) {
        const auto& x = first.noteMarkers.at(index);
        const auto& y = second.noteMarkers.at(index);
        if (x.type != y.type || x.lane != y.lane || x.endLane != y.endLane
            || x.second != y.second || x.endSecond != y.endSecond || x.slideTraceSecond != y.slideTraceSecond
            || x.slideSegmentDurations != y.slideSegmentDurations || x.slideSegmentKeys != y.slideSegmentKeys
            || x.slideSegmentPoints != y.slideSegmentPoints || x.wifiLanePoints != y.wifiLanePoints
            || x.wifiTrackAreaPoints != y.wifiTrackAreaPoints || x.touchPoint != y.touchPoint
            || x.isEach != y.isEach || x.eachGroupId != y.eachGroupId || x.isBreak != y.isBreak
            || x.isEx != y.isEx || x.isMine != y.isMine || x.headMine != y.headMine || x.trackMine != y.trackMine
            || x.headBreak != y.headBreak || x.trackBreak != y.trackBreak || x.headEx != y.headEx
            || x.hasHeadStar != y.hasHeadStar || x.headlessImmediate != y.headlessImmediate
            || x.hsMultiplier != y.hsMultiplier || x.onSlide != y.onSlide) return false;
    }
    return true;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace miacode::chart_transform;
    bool ok = true;
    const auto expect = [&](bool pass, const char* label) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << label << '\n';
        ok &= pass;
    };
    const QString buttons = "(120){4}12345678,1h[4:1],5bh[4:1],2xm,6m,";
    const auto base = basicHandOrderLayoutInSelection(buttons, 5, 5);
    expect(base.edit.diagnostics.isEmpty() && base.edit.replacement.contains("1~r/2~r/3~r/4~r/5~l/6~l/7~l/8~l")
        && base.edit.replacement.contains("1h[4:1]~r") && base.edit.replacement.contains("5bh[4:1]~l"),
        "empty selection lays out the whole difficulty, right 1-4 and left 5-8, including holds");
    expect(sameNative(buttons, base.edit.replacement), "digit expansion preserves original geometry, timing, Each and modifiers");
    expect(basicHandOrderLayoutInSelection(base.edit.replacement, 0, 0).edit.changed == 0,
        "repeating basic layout preserves every generated annotation without an empty undo edit");

    const QString slides = "(120){4}1-5[4:1],5-1[4:1],1-3-7[1:1],5?-1[4:1],";
    const auto laidSlides = basicHandOrderLayoutInSelection(slides, 0, 0);
    const auto parsedSlides = SimaiParser::validateSyntax(laidSlides.edit.replacement);
    expect(laidSlides.edit.diagnostics.isEmpty() && parsedSlides.annotationErrors.isEmpty() && parsedSlides.noteMarkers.size() == 4,
        "ordinary, chained and headless slides accept the deterministic basis");
    for (const auto& marker : parsedSlides.noteMarkers) {
        expect(marker.handOrderTrackStops.size() == 1
            && marker.handOrderTrackStops.constFirst().hand == basicLayoutTrackHand(marker),
            "the entire cross-screen slide keeps its actual motion-start hand with no invented handoff");
    }
    expect(sameNative(slides, laidSlides.edit.replacement), "slide layout changes only hand-order annotation");

    const QString touches = "(120){4}A1,B8,C,D1,D3,D5,D7,E1,";
    const auto laidTouches = basicHandOrderLayoutInSelection(touches, 0, 0);
    expect(laidTouches.edit.diagnostics.isEmpty() && laidTouches.edit.replacement.contains("A1~r")
        && laidTouches.edit.replacement.contains("B8~l") && laidTouches.edit.replacement.contains("C~m")
        && laidTouches.edit.replacement.contains("D1~m") && laidTouches.edit.replacement.contains("D3~r")
        && laidTouches.edit.replacement.contains("D5~m") && laidTouches.edit.replacement.contains("D7~l")
        && laidTouches.edit.replacement.contains("E1~m"), "Touch uses actual pad geometry and centre-axis Either, not pad digits");
    expect(sameNative(touches, laidTouches.edit.replacement), "Touch positions and timing remain unchanged");
    expect(basicLayoutHandForPoint({0.0, 10.0}) == HandOrderHand::Either
        && basicLayoutHandForPoint({-1.0, 0.0}) == HandOrderHand::Left
        && basicLayoutHandForPoint({1.0, 0.0}) == HandOrderHand::Right
        && basicLayoutHandForPoint({std::numeric_limits<double>::quiet_NaN(), 0.0}) == HandOrderHand::Unspecified,
        "shared geometry distinguishes a genuine centre preference from invalid input");

    const QString wifi = "(120){4}1w5[4:1],1w5[4:1]*-7[4:1],";
    const auto laidWifi = basicHandOrderLayoutInSelection(wifi, 0, 0);
    const auto parsedWifi = SimaiParser::validateSyntax(laidWifi.edit.replacement);
    expect(laidWifi.edit.diagnostics.isEmpty() && parsedWifi.annotationErrors.isEmpty()
        && laidWifi.edit.replacement.contains("track=split:") && laidWifi.edit.replacement.contains("branch0=split:")
        && laidWifi.edit.replacement.contains("branch1=0:r"), "Wifi emits explicit native sectors and preserves source branch numbering");
    for (const auto& marker : parsedWifi.noteMarkers) {
        if (marker.type != QStringLiteral("wifi")) continue;
        expect(marker.handOrderWifiMode == HandOrderWifiMode::Split && marker.handOrderTrackStops.isEmpty()
            && marker.handOrderWifiSectorStops.size() == 2
            && marker.handOrderWifiSectorStops.at(0).first().hand != marker.handOrderWifiSectorStops.at(1).first().hand,
            "unannotated Wifi gets two different hands rather than an incorrect ordinary one-color track");
    }
    expect(sameNative(wifi, laidWifi.edit.replacement), "Wifi native fan geometry, counts and timing remain unchanged");
    TimelineNoteMarker sameHalf;
    sameHalf.type = QStringLiteral("wifi");
    sameHalf.wifiLanePoints = {{QPointF(0, 0), QPointF(100, 10)}, {QPointF(0, 0), QPointF(150, 0)},
        {QPointF(0, 0), QPointF(200, -10)}};
    expect(basicLayoutWifiSectorHand(sameHalf, 0) == HandOrderHand::Left
        && basicLayoutWifiSectorHand(sameHalf, 1) == HandOrderHand::Right,
        "a Wifi fan wholly on one half still receives two opposite hands by relative native sector x");

    const QString locked = "(120){4}1~l,5~m,1w5[4:1]~r,2w6[4:1]~[track=split:0:r&0:l],"
        "3w7[4:1]~[track=single:0:m],8~z,2,";
    const auto protectedLayout = basicHandOrderLayoutInSelection(locked, 0, 0);
    expect(protectedLayout.preserved == 6 && protectedLayout.edit.changed == 1
        && protectedLayout.edit.replacement == locked.left(locked.size() - 2) + "2~r,"
        && protectedLayout.edit.diagnostics.size() == 1,
        "existing shorthand, Wifi modes, Either and invalid suffixes retain exact original text");

    const QString partial = "(120){4}1-5[4:1]/8,2,";
    const int begin = partial.indexOf("-5");
    const int end = partial.indexOf(",2");
    const auto selection = basicHandOrderLayoutInSelection(partial, begin, end);
    expect(selection.edit.partial == 1 && selection.edit.changed == 1 && selection.edit.replacement.endsWith("/8~l"),
        "selection boundaries protect an incomplete slide token while assigning complete neighbours");
    expect(sameNative(partial, partial.left(begin) + selection.edit.replacement + partial.mid(end)),
        "partial selection retains the native chart projection");
    const QString unicode = QStringLiteral("|| 中文 😀\r\n(120){4}8/1-5[4:1],\r\nE");
    const auto ordered = basicHandOrderLayoutInSelection(unicode, 0, 0);
    expect(ordered.edit.diagnostics.isEmpty() && ordered.edit.replacement.startsWith(QStringLiteral("|| 中文 😀\r\n"))
        && ordered.edit.replacement.contains("8~l/1-5[4:1]~[head=r;track=0:r]")
        && sameNative(unicode, ordered.edit.replacement), "UTF-16, CRLF, comments and parser sort order do not alter source identity");
    const QString badTiming = "(wrong){4}1-5[4:1],";
    const auto invalid = basicHandOrderLayoutInSelection(badTiming, 0, 0);
    expect(invalid.edit.changed == 0 && invalid.edit.replacement == badTiming && !invalid.edit.diagnostics.isEmpty(),
        "invalid native timing cannot produce a silently partial layout");
    return ok ? 0 : 1;
}

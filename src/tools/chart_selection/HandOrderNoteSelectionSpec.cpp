#include "app/ui/editor/EditorController.h"
#include "app/ui/editor/NoteSelectionEdit.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/parser/SimaiParser.h"
#include "core/chart/transform/ChartNoteSelection.h"
#include "timeline/TimelineQuickModel.h"

#include <QCoreApplication>
#include <QTextStream>
#include <cmath>

namespace {
bool sameTiming(const TimelineNoteMarker& left, const TimelineNoteMarker& right)
{
    const auto close = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    return left.type == right.type && left.lane == right.lane && left.endLane == right.endLane
        && close(left.second, right.second) && close(left.endSecond, right.endSecond)
        && close(left.slideTraceSecond, right.slideTraceSecond);
}
bool sameNativeTiming(const QString& left, const QString& right)
{
    const auto a = SimaiParser::parseForTimeline(left), b = SimaiParser::parseForTimeline(right);
    if (!a.ok || !b.ok || a.noteMarkers.size() != b.noteMarkers.size()) return false;
    for (int index = 0; index < a.noteMarkers.size(); ++index)
        if (!sameTiming(a.noteMarkers[index], b.noteMarkers[index])) return false;
    return true;
}
QString applied(const QString& original, const miacode::editor::SimaiTextEditResult& edit)
{
    if (!edit.transaction.hasEdit) return original;
    auto text = original;
    const auto& tx = edit.transaction;
    text.replace(tx.replacementStart, tx.replacementEnd - tx.replacementStart, tx.replacementText);
    return text;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto expect = [&](bool condition, const char* message) {
        QTextStream(condition ? stdout : stderr) << (condition ? "PASS: " : "FAIL: ") << message << '\n';
        if (!condition) ++failures;
    };
    using namespace miacode::chart_selection;
    // Synthetic forms follow HAND_ORDER_ANNOTATION_SPEC_ZH and HandOrderEditSpec.
    const QString source = QStringLiteral("(120){4}1~l/5~r,2~m,(180){8}3h[4:1]~m,1-3-7[1:1]~l,4~r,E");
    TimelineQuickModel model;
    model.rebuildFromText(source, 0.0);
    const auto all = model.noteSourceRanges();
    const auto span = [&](const QString& token) {
        const int start = source.indexOf(token);
        return NoteRange{start, start + int(token.size())};
    };
    const auto tap = span(QStringLiteral("1~l"));
    const auto hold = span(QStringLiteral("3h[4:1]~m"));
    const auto chain = span(QStringLiteral("1-3-7[1:1]~l"));
    const auto last = span(QStringLiteral("4~r"));
    const auto native = SimaiParser::parseForTimeline(source);
    expect(native.ok && native.annotationErrors.isEmpty() && !native.sourceTokens.isEmpty(),
        "annotated fixture retains native sourceTokens and separate annotation diagnostics");
    bool suffixToken = false;
    for (const auto& token : native.sourceTokens)
        suffixToken |= source.mid(token.col - 1, token.length) == QStringLiteral("1~l");
    expect(suffixToken, "sourceTokens include the complete annotation suffix");
    const auto copiedTap = copySelectedNotes(source, {{tap.start + 1, tap.start + 2}}, all);
    const auto tapParse = SimaiParser::parseForTimeline(copiedTap + QStringLiteral("E"));
    expect(copiedTap.contains(QStringLiteral("1~l")) && !copiedTap.contains(QStringLiteral("5~r"))
        && tapParse.ok && tapParse.annotationErrors.isEmpty(), "a suffix hit selects one complete simultaneous note");
    const NoteRanges selected{tap, hold, chain};
    const auto copied = copySelectedNotes(source, selected, all);
    const auto copiedParse = SimaiParser::parseForTimeline(copied + QStringLiteral("E"));
    expect(copiedParse.ok && copiedParse.annotationErrors.isEmpty() && copiedParse.noteMarkers.size() == 3
        && copied.contains(QStringLiteral("3h[4:1]~m")) && copied.contains(QStringLiteral("1-3-7[1:1]~l")),
        "copy owns Hold and chained Slide suffixes without partial annotation fragments");
    bool copyTimes = copiedParse.noteMarkers.size() == 3;
    for (const auto& note : copiedParse.noteMarkers) {
        bool found = false;
        for (const auto& original : native.noteMarkers) found |= sameTiming(note, original);
        copyTimes &= found;
    }
    expect(copyTimes, "copied note timing survives intervening unselected notes and BPM changes");
    const auto deleted = deleteSelectedNotes(source, selected, all);
    const auto deletedParse = SimaiParser::parseForTimeline(deleted);
    bool survivors = deletedParse.ok && deletedParse.annotationErrors.isEmpty();
    for (const auto& note : deletedParse.noteMarkers) {
        bool found = false;
        for (const auto& original : native.noteMarkers) found |= sameTiming(note, original);
        survivors &= found;
    }
    expect(survivors && deleted.contains(QStringLiteral("5~r")) && deleted.contains(QStringLiteral("2~m"))
        && deleted.contains(QStringLiteral("4~r")) && deleted.count(',') == source.count(','),
        "deletion keeps legal same-beat suffixes, timing commas and surviving note times");

    miacode::ui::EditorController controller;
    controller.activateHandOrderDocument(QStringLiteral("synthetic:selection"), source);
    const auto command = [&](const QString& text, const NoteRanges& ranges, const QString& operation) {
        return miacode::ui::editSelectedNoteRanges(text, ranges, operation,
            [&](const QString& current, int start, int end) {
                return controller.handOrderCommand(current, start, end, operation);
            });
    };
    const auto set = command(source, {tap, last}, QStringLiteral("hand_order.set_left"));
    const auto assigned = applied(source, set);
    expect(set.consumed && set.transaction.hasEdit && set.transaction.replacementStart == 0
        && set.transaction.replacementEnd == source.size() && assigned.contains(QStringLiteral("4~l"))
        && assigned.contains(QStringLiteral("5~r")) && assigned.contains(QStringLiteral("2~m"))
        && sameNativeTiming(source, assigned), "discontiguous set-hand collects one transaction without editing intervening notes");
    const auto swap = command(source, {tap, chain}, QStringLiteral("hand_order.swap_hands"));
    const auto swapped = applied(source, swap);
    expect(swap.consumed && swapped.contains(QStringLiteral("1~r/5~r"))
        && swapped.contains(QStringLiteral("1-3-7[1:1]~r")) && swapped.contains(QStringLiteral("2~m"))
        && sameNativeTiming(source, swapped), "multi-range swap changes every selected suffix and preserves native timing");
    const QString slides = QStringLiteral("(120){4}1-3-7[1:1]~l,2~m,1-5[4:1]~r,E");
    TimelineQuickModel slideModel;
    slideModel.rebuildFromText(slides, 0.0);
    const auto slideRanges = slideModel.noteSourceRanges();
    const auto bisect = command(slides, {slideRanges.first(), slideRanges.last()}, QStringLiteral("hand_order.bisect_annotation"));
    const auto bisected = applied(slides, bisect);
    const auto bisectedParse = SimaiParser::parseForTimeline(bisected);
    expect(bisect.consumed && bisect.transaction.hasEdit && bisected.count(QStringLiteral("0.5:r")) == 2
        && bisected.contains(QStringLiteral("2~m")) && bisectedParse.annotationErrors.isEmpty()
        && sameNativeTiming(slides, bisected), "multi-range bisection updates both complete Slide objects in one transaction");
    for (const auto& operation : {QStringLiteral("hand_order.split_annotation"),
        QStringLiteral("hand_order.split_native_slide")}) {
        bool called = false;
        const auto blocked = miacode::ui::editSelectedNoteRanges(source, {tap, last}, operation,
            [&](const QString&, int, int) { called = true; return miacode::editor::SimaiTextEditResult{}; });
        expect(!called && !blocked.transaction.hasEdit
            && blocked.error == QStringLiteral("hand_order.object_selection_requires_contiguous"),
            "contextual hand commands explicitly reject discontiguous selections before any edit");
    }
    controller.handOrderCommand(source, tap.start, tap.end,
        QStringLiteral("hand_order.object_selection_requires_contiguous"));
    expect(!controller.handOrderFeedback().isEmpty() && !controller.handOrderSplitAvailable(),
        "rejected contextual selection announces feedback and leaves no interval draft active");

    const QString invalid = QStringLiteral("(120){4}1~[(999){32}],2~l,E");
    model.rebuildFromText(invalid, 0.0);
    const int second = invalid.indexOf(QStringLiteral("2~l"));
    const auto isolated = copySelectedNotes(invalid, {{second, second + 1}}, model.noteSourceRanges());
    expect(isolated.startsWith(QStringLiteral("|| 4/4\n(120){4}<HS*1>2~l")),
        "invalid annotation contents never masquerade as BPM or subdivision context");
    const auto invalidParse = SimaiParser::parseForTimeline(invalid);
    expect(invalidParse.ok && !invalidParse.annotationErrors.isEmpty() && invalidParse.sourceTokens.size() == 2,
        "invalid annotations keep native tokens and separate diagnostics for recovery");
    const QString cluster = QStringLiteral("(120){4}12~l/3~r,4~l,E");
    model.rebuildFromText(cluster, 0.0);
    const int clusterStart = cluster.indexOf(QStringLiteral("12~l"));
    const auto recovered = deleteSelectedNotes(cluster, {{clusterStart, clusterStart + 1}}, model.noteSourceRanges());
    expect(recovered.contains(QStringLiteral("3~r,4~l")) && recovered.count(',') == cluster.count(',')
        && SimaiParser::parseForTimeline(recovered).annotationErrors.isEmpty(),
        "source-length recovery never deletes a legal neighbour's suffix or beat separator");

    const QString token = QStringLiteral("1bx-3[4:1]*w5[8:1]~[head=l;branch0=0:r|0.25:l;branch1=split:0:l|0.5:r&0:r]");
    const QString branches = QStringLiteral("(120){4}") + token + QStringLiteral("/2~m,\nE");
    model.rebuildFromText(branches, 0.0);
    const auto objects = model.noteObjects();
    const auto branchNotes = model.noteSourceRanges();
    const auto parsedBranches = SimaiParser::parseForTimeline(branches);
    expect(objects.size() == 4 && parsedBranches.noteMarkers.size() == 3
        && parsedBranches.annotationErrors.isEmpty(), "annotated branches expose separate head and track objects");
    if (objects.size() == 4 && parsedBranches.noteMarkers.size() == 3) {
        const auto& slide = parsedBranches.noteMarkers[0];
        const auto& wifi = parsedBranches.noteMarkers[1];
        expect(objects[0].component == NoteComponent::SlideHead
            && objects[1].trackIndex == 0 && objects[2].trackIndex == 1
            && objects[1].location == sourceLocationId(slide.sourceLine, slide.slideTrackSourceCol)
            && objects[2].location == sourceLocationId(wifi.sourceLine, wifi.slideTrackSourceCol)
            && objects[2].highlight.end == wifi.handOrderAnnotationCol - 1,
            "preview and timeline agree on track identity and highlight stops before the suffix");

        const NoteObjects head{objects[0]}, firstTrack{objects[1]}, wifiTrack{objects[2]};
        const auto assignment = transformSlideComponents(token, wifiTrack, u"hand_order.set_either");
        expect(assignment.supported && assignment.text.contains(QStringLiteral(
            "~[head=l;branch0=0:r|0.25:l;branch1=split:0:m|0.5:m&0:m]")),
            "assigning a hand to one track leaves the head, sibling and Wifi handoff positions intact");
        const auto swappedWifi = transformSlideComponents(token, wifiTrack, u"hand_order.swap_hands");
        expect(swappedWifi.supported && swappedWifi.text.contains(QStringLiteral(
            "~[head=l;branch0=0:r|0.25:l;branch1=split:0:r|0.5:l&0:l]")),
            "swap affects only the selected track and keeps its sectors");
        const auto brokenWifi = transformSlideComponents(token, wifiTrack, u"transform.toggle_break");
        expect(brokenWifi.supported && brokenWifi.text == QStringLiteral("1bx-3[4:1]*w5b[8:1]")
            + token.mid(token.indexOf(QLatin1Char('~'))), "changing a property leaves the complete suffix untouched");

        const auto removedSlide = deleteSelectedNoteObjects(branches, firstTrack, branchNotes);
        const auto removedHead = deleteSelectedNoteObjects(branches, head, branchNotes);
        const auto copiedHead = copySelectedNoteObjects(branches, head, branchNotes);
        const auto copiedWifi = copySelectedNoteObjects(branches, wifiTrack, branchNotes);
        expect(removedSlide.supported && removedSlide.text.contains(QStringLiteral("1bxw5[8:1]~[head=l;track=split:0:l|0.5:r&0:r]"))
            && SimaiParser::validateSyntax(removedSlide.text).annotationErrors.isEmpty(),
            "deleting one branch remaps the surviving Wifi clause to track");
        expect(removedHead.supported && removedHead.text.contains(QStringLiteral("1?-3[4:1]*w5[8:1]~[branch0=0:r|0.25:l;branch1=split:0:l|0.5:r&0:r]")),
            "deleting the shared head retains both track annotations without a head clause");
        expect(copiedHead.supported && copiedHead.text.contains(QStringLiteral("1bx$$~l,"))
            && copiedWifi.supported && copiedWifi.text.contains(QStringLiteral("1?w5[8:1]~[track=split:0:l|0.5:r&0:r],"))
            && SimaiParser::validateSyntax(copiedWifi.text + QStringLiteral("E")).annotationErrors.isEmpty(),
            "copy isolates the chosen component and retains its hand annotation");

        const QString unannotated = QStringLiteral("1bx-3[4:1]*w5[8:1]");
        const auto unsupported = transformSlideComponents(unannotated, wifiTrack, u"hand_order.set_left");
        expect(!unsupported.supported && unsupported.text == unannotated,
            "partial assignment refuses to assign an unselected branch implicitly");
    }
    return failures ? 1 : 0;
}

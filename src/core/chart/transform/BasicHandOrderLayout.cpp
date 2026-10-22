#include "core/chart/transform/BasicHandOrderLayout.h"

#include "core/chart/parser/HandOrderAnnotationSyntax.h"

#include <cmath>
#include <limits>

namespace miacode::chart_transform {
namespace {
constexpr double kCentreTolerance = 1e-6;
constexpr double kTouchCanvasCentre = 270.0;

HandOrderHand laneHand(int lane)
{
    if (lane < 1 || lane > 8) return HandOrderHand::Unspecified;
    return lane <= 4 ? HandOrderHand::Right : HandOrderHand::Left;
}

QString glyph(HandOrderHand hand)
{
    switch (hand) {
    case HandOrderHand::Left: return QStringLiteral("l");
    case HandOrderHand::Right: return QStringLiteral("r");
    case HandOrderHand::Either: return QStringLiteral("m");
    default: return {};
    }
}
} // namespace

HandOrderHand basicLayoutHandForPoint(const QPointF& point)
{
    if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return HandOrderHand::Unspecified;
    if (point.x() < -kCentreTolerance) return HandOrderHand::Left;
    if (point.x() > kCentreTolerance) return HandOrderHand::Right;
    return HandOrderHand::Either;
}

HandOrderHand basicLayoutHeadHand(const TimelineNoteMarker& marker)
{
    if (marker.type == QStringLiteral("touch") || marker.type == QStringLiteral("touch_hold")) {
        // The parser's Touch points use a legacy 540-square canvas origin.
        return basicLayoutHandForPoint(marker.touchPoint - QPointF(kTouchCanvasCentre, kTouchCanvasCentre));
    }
    return laneHand(marker.lane);
}

QPointF basicLayoutTrackStartPoint(const TimelineNoteMarker& marker)
{
    if (marker.type == QStringLiteral("slide") && !marker.slideSegmentPoints.isEmpty()
        && !marker.slideSegmentPoints.constFirst().isEmpty()) return marker.slideSegmentPoints.constFirst().constFirst();
    if (marker.type == QStringLiteral("wifi") && !marker.wifiLanePoints.isEmpty()
        && !marker.wifiLanePoints.constFirst().isEmpty()) return marker.wifiLanePoints.constFirst().constFirst();
    const double invalid = std::numeric_limits<double>::quiet_NaN();
    return {invalid, invalid};
}

HandOrderHand basicLayoutTrackHand(const TimelineNoteMarker& marker)
{
    return basicLayoutHandForPoint(basicLayoutTrackStartPoint(marker));
}

HandOrderHand basicLayoutWifiSectorHand(const TimelineNoteMarker& marker, int sector)
{
    if (sector < 0 || sector > 1 || marker.type != QStringLiteral("wifi") || marker.wifiLanePoints.size() < 3
        || marker.wifiLanePoints.at(0).isEmpty() || marker.wifiLanePoints.at(2).isEmpty()) return HandOrderHand::Unspecified;
    const auto first = marker.wifiLanePoints.at(0).constLast();
    const auto second = marker.wifiLanePoints.at(2).constLast();
    if (!std::isfinite(first.x()) || !std::isfinite(first.y())
        || !std::isfinite(second.x()) || !std::isfinite(second.y())) return HandOrderHand::Unspecified;
    // A Wifi basis always uses two hands, including fans on one half-screen.
    // Native sector order is the deterministic tie-break for equal x.
    const int leftSector = first.x() <= second.x() ? 0 : 1;
    return sector == leftSector ? HandOrderHand::Left : HandOrderHand::Right;
}

BasicHandOrderLayoutPlan basicHandOrderLayoutInSelection(const QString& text, int begin, int end,
    const miacode::simai::SimaiTimingMetadata& metadata)
{
    BasicHandOrderLayoutPlan result;
    auto& plan = result.edit;
    const int first = qBound(0, qMin(begin, end), int(text.size()));
    const int last = qBound(first, qMax(begin, end), int(text.size()));
    begin = first == last ? 0 : first;
    end = first == last ? int(text.size()) : last;
    plan.replacement = text.mid(begin, end - begin);
    const auto parsed = SimaiParser::validateSyntax(text, metadata);
    if (!parsed.errors.isEmpty()) {
        plan.diagnostics = parsed.errors;
        return result;
    }
    QVector<const TimelineNoteMarker*> byOrder(parsed.noteMarkers.size(), nullptr);
    for (const auto& marker : parsed.noteMarkers)
        if (marker.parseOrder >= 0 && marker.parseOrder < byOrder.size()) byOrder[marker.parseOrder] = &marker;
    QVector<int> lineStarts{0};
    for (int index = 0; index < text.size(); ++index)
        if (text.at(index) == QLatin1Char('\n')) lineStarts.append(index + 1);
    int delta = 0;
    for (const auto& source : parsed.sourceTokens) {
        if (source.line < 1 || source.line > lineStarts.size()) continue;
        const int start = lineStarts.at(source.line - 1) + source.col - 1;
        const int finish = start + source.length;
        if (start >= end || finish <= begin || source.markerCount == 0) continue;
        if (start < begin || finish > end) { ++plan.partial; continue; }
        const QString original = text.mid(start, source.length);
        const auto parts = hand_order::splitToken(original);
        if (!parts.suffix.isEmpty()) {
            ++result.preserved;
            for (const auto& issue : parsed.annotationErrors)
                if (issue.line == source.line && issue.col >= source.col && issue.col < source.col + source.length)
                    plan.diagnostics.append(issue);
            continue;
        }
        if (!source.nativeValid) { ++plan.unsupported; continue; }
        QString replacement;
        const auto* firstMarker = byOrder.value(source.firstParseOrder);
        if (!firstMarker) { ++plan.unsupported; continue; }
        const bool slide = firstMarker->type == QStringLiteral("slide") || firstMarker->type == QStringLiteral("wifi");
        if (slide) {
            HandOrderTokenEdit token;
            token.baseToken = parts.baseToken;
            token.head = basicLayoutHeadHand(*firstMarker);
            bool supported = true;
            for (int index = 0; index < source.markerCount; ++index) {
                const auto* marker = byOrder.value(source.firstParseOrder + index);
                if (!marker || marker->slideBranchIndex != token.tracks.size()) { supported = false; break; }
                token.hasHead |= marker->hasHeadStar;
                const bool wifi = marker->type == QStringLiteral("wifi");
                if (!wifi && marker->type != QStringLiteral("slide")) { supported = false; break; }
                token.wifiBranches.append(wifi);
                token.wifiModes.append(wifi ? HandOrderWifiMode::Split : HandOrderWifiMode::Legacy);
                if (wifi) {
                    token.tracks.append(QVector<HandOrderTrackStop>{});
                    token.wifiSectors.append(QVector<QVector<HandOrderTrackStop>>{
                        QVector<HandOrderTrackStop>{{0.0, basicLayoutWifiSectorHand(*marker, 0)}},
                        QVector<HandOrderTrackStop>{{0.0, basicLayoutWifiSectorHand(*marker, 1)}}});
                } else {
                    token.tracks.append(QVector<HandOrderTrackStop>{{0.0, basicLayoutTrackHand(*marker)}});
                    token.wifiSectors.append(QVector<QVector<HandOrderTrackStop>>{});
                }
            }
            const QString suffix = supported ? handOrderSuffix(token) : QString();
            if (suffix.isEmpty()) { ++plan.unsupported; continue; }
            replacement = parts.baseToken + suffix;
        } else {
            bool cluster = source.markerCount > 1 && parts.baseToken.size() == source.markerCount;
            for (const auto ch : parts.baseToken) cluster &= ch >= QLatin1Char('1') && ch <= QLatin1Char('8');
            if (cluster) {
                QStringList notes;
                for (const auto ch : parts.baseToken) notes.append(QString(ch) + QLatin1Char('~') + glyph(laneHand(ch.digitValue())));
                replacement = notes.join(QLatin1Char('/'));
            } else {
                const auto hand = basicLayoutHeadHand(*firstMarker);
                if (source.markerCount != 1 || hand == HandOrderHand::Unspecified) { ++plan.unsupported; continue; }
                replacement = parts.baseToken + QLatin1Char('~') + glyph(hand);
            }
        }
        plan.replacement.replace(start - begin + delta, source.length, replacement);
        plan.patches.append({start, finish, replacement});
        delta += replacement.size() - source.length;
        ++plan.changed;
    }
    const auto checked = SimaiParser::validateSyntax(text.left(begin) + plan.replacement + text.mid(end), metadata);
    if (!checked.errors.isEmpty() || checked.annotationErrors.size() > parsed.annotationErrors.size()) {
        plan.replacement = text.mid(begin, end - begin);
        plan.patches.clear();
        plan.changed = 0;
        plan.diagnostics += checked.errors;
        plan.diagnostics += checked.annotationErrors;
    }
    return result;
}
} // namespace miacode::chart_transform

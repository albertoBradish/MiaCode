#include "core/chart/transform/BasicHandOrderLayout.h"

#include "core/chart/parser/HandOrderAnnotationSyntax.h"

#include <cmath>
#include <limits>
#include <algorithm>
#include <QHash>
#include <QLineF>

namespace miacode::chart_transform {
namespace {
constexpr double kCentreTolerance = 1e-6;
constexpr double kTouchCanvasCentre = 270.0;
constexpr double kPositionMemorySeconds = 0.4;

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

QPointF lanePoint(int lane)
{
    if (lane < 1 || lane > 8) return {};
    const double angle = (lane - 0.5) * 3.14159265358979323846 / 4.0;
    return {std::sin(angle), -std::cos(angle)};
}

QPointF headPoint(const TimelineNoteMarker& marker)
{
    if (marker.type == QStringLiteral("touch") || marker.type == QStringLiteral("touch_hold"))
        return (marker.touchPoint - QPointF(kTouchCanvasCentre, kTouchCanvasCentre)) / 240.0;
    return lanePoint(marker.lane);
}

struct LayoutHead {
    const TimelineNoteMarker* marker = nullptr;
    int token = -1;
    QPointF point;
    HandOrderHand hand = HandOrderHand::Unspecified;
    bool fixed = false;
};

struct LayoutSustain {
    const TimelineNoteMarker* marker = nullptr;
    int token = -1;
    int head = -1;
    double end = 0.0;
    HandOrderHand hand = HandOrderHand::Unspecified;
    bool fixed = false;
};

int handIndex(HandOrderHand hand)
{
    return hand == HandOrderHand::Left ? 0 : hand == HandOrderHand::Right ? 1 : -1;
}

HandOrderHand handAt(const LayoutSustain& sustain, double second)
{
    if (!sustain.fixed || sustain.marker->handOrderTrackStops.size() <= 1) return sustain.hand;
    const auto& marker = *sustain.marker;
    const double duration = marker.endSecond - marker.slideTraceSecond;
    if (duration <= 0.0) return sustain.hand;
    const double progress = qBound(0.0, (second - marker.slideTraceSecond) / duration, 1.0);
    HandOrderHand hand = sustain.hand;
    for (const auto& stop : marker.handOrderTrackStops) {
        if (stop.progress > progress + 1e-9) break;
        hand = stop.hand;
    }
    return hand;
}

// This is a local scheduling constraint, not a judgement or playability proof.
void assignLocalHands(QVector<LayoutHead>* heads, QVector<LayoutSustain>* sustains)
{
    QVector<int> order;
    order.reserve(heads->size());
    for (int i = 0; i < heads->size(); ++i) order.append(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const auto& left = heads->at(a);
        const auto& right = heads->at(b);
        if (left.marker->second != right.marker->second) return left.marker->second < right.marker->second;
        return left.marker->parseOrder < right.marker->parseOrder;
    });
    QPointF position[2] = {lanePoint(6), lanePoint(3)};
    double positionTime[2] = {-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    QVector<bool> ended(sustains->size(), false);
    const auto busy = [&](int hand, double second, int token) {
        for (const auto& sustain : *sustains) {
            if (sustain.token == token || sustain.marker->second > second + 1e-9
                || sustain.end + 1e-9 < second) continue;
            if (handIndex(handAt(sustain, second)) == hand) return true;
        }
        return false;
    };
    const auto headCost = [&](const LayoutHead& head, int hand) {
        const auto preferred = basicLayoutHeadHand(*head.marker);
        const double sideCost = handIndex(preferred) >= 0 && handIndex(preferred) != hand ? 0.25 : 0.0;
        const QPointF origin = head.marker->second - positionTime[hand] > kPositionMemorySeconds
            ? lanePoint(hand == 0 ? 6 : 3) : position[hand];
        return QLineF(origin, head.point).length() + sideCost
            + (busy(hand, head.marker->second, head.token) ? 100.0 : 0.0);
    };
    const auto commitHead = [&](int index, int hand) {
        auto& head = (*heads)[index];
        if (!head.fixed) head.hand = hand < 0 ? HandOrderHand::Either
            : hand == 0 ? HandOrderHand::Left : HandOrderHand::Right;
        const int actual = handIndex(head.hand);
        if (actual >= 0) {
            position[actual] = head.point;
            positionTime[actual] = head.marker->second;
        }
    };
    for (int first = 0; first < order.size();) {
        const auto& anchor = heads->at(order.at(first));
        for (int index = 0; index < sustains->size(); ++index) {
            const auto& sustain = sustains->at(index);
            if (ended.at(index) || sustain.end > anchor.marker->second + 1e-9) continue;
            ended[index] = true;
            const int hand = handIndex(handAt(sustain, sustain.end));
            if (hand < 0 || sustain.end < positionTime[hand]) continue;
            position[hand] = sustain.marker->type == QStringLiteral("slide")
                ? lanePoint(sustain.marker->endLane) : headPoint(*sustain.marker);
            positionTime[hand] = sustain.end;
        }
        int last = first + 1;
        while (last < order.size()) {
            const auto& next = heads->at(order.at(last));
            const double threshold = 240.0 / (64.0 * qMax(anchor.marker->tempoBpm, next.marker->tempoBpm));
            if (next.marker->second - anchor.marker->second >= threshold - 1e-9) break;
            ++last;
        }
        if (last - first == 2) {
            auto& a = (*heads)[order.at(first)];
            auto& b = (*heads)[order.at(first + 1)];
            double best = std::numeric_limits<double>::infinity();
            int firstHand = -1;
            for (int candidate = 0; candidate < 2; ++candidate) {
                if (a.fixed && handIndex(a.hand) >= 0 && handIndex(a.hand) != candidate) continue;
                if (b.fixed && handIndex(b.hand) >= 0 && handIndex(b.hand) == candidate) continue;
                if ((!a.fixed && busy(candidate, a.marker->second, a.token))
                    || (!b.fixed && busy(1 - candidate, b.marker->second, b.token))) continue;
                double cost = headCost(a, candidate) + headCost(b, 1 - candidate);
                if ((a.point.x() < b.point.x() && candidate != 0)
                    || (a.point.x() > b.point.x() && candidate != 1)) cost += 2.0;
                if (cost < best) { best = cost; firstHand = candidate; }
            }
            if (firstHand >= 0) {
                commitHead(order.at(first), firstHand);
                commitHead(order.at(first + 1), 1 - firstHand);
            }
        } else if (last - first == 1) {
            const int index = order.at(first);
            const auto& head = heads->at(index);
            const int fixed = handIndex(head.hand);
            const auto preferred = basicLayoutHeadHand(*head.marker);
            if (!head.fixed && busy(0, head.marker->second, head.token)
                && busy(1, head.marker->second, head.token)) {
                // No legal hand is available; the token stays unassigned.
            } else if (!head.fixed && preferred == HandOrderHand::Either
                && !busy(0, head.marker->second, head.token)
                && !busy(1, head.marker->second, head.token)) commitHead(index, -1);
            else if (!head.fixed && handIndex(preferred) >= 0
                && head.marker->second - positionTime[0] > kPositionMemorySeconds
                && head.marker->second - positionTime[1] > kPositionMemorySeconds
                && !busy(handIndex(preferred), head.marker->second, head.token))
                commitHead(index, handIndex(preferred));
            else commitHead(index, fixed >= 0 ? fixed : headCost(head, 0) <= headCost(head, 1) ? 0 : 1);
        } else {
            // More than two independent heads cannot be assigned to two hands.
            for (int i = first; i < last; ++i) {
                const int index = order.at(i);
                if (heads->at(index).fixed) commitHead(index, handIndex(heads->at(index).hand));
            }
        }
        for (auto& sustain : *sustains) {
            if (sustain.head < 0) continue;
            const auto& head = heads->at(sustain.head);
            if (head.marker->second < anchor.marker->second - 1e-9
                || head.marker->second >= heads->at(order.at(last - 1)).marker->second + 1e-9) continue;
            if (sustain.fixed || sustain.hand != HandOrderHand::Unspecified) continue;
            if (sustain.marker->type == QStringLiteral("hold")
                || sustain.marker->type == QStringLiteral("touch_hold")) {
                sustain.hand = head.hand;
                continue;
            }
            const auto preferred = basicLayoutTrackHand(*sustain.marker);
            double best = std::numeric_limits<double>::infinity();
            for (int candidate = 0; candidate < 2; ++candidate) {
                double cost = handIndex(preferred) >= 0 && handIndex(preferred) != candidate ? 1.2 : 0.0;
                if (handIndex(head.hand) >= 0 && handIndex(head.hand) != candidate) cost += 0.8;
                if (busy(candidate, head.marker->second, sustain.token)) cost += 100.0;
                for (const auto& future : *heads) {
                    if (future.token == sustain.token || future.marker->second <= head.marker->second + 1e-9
                        || future.marker->second > sustain.end + 1e-9) continue;
                    const int futureHand = handIndex(future.fixed ? future.hand : basicLayoutHeadHand(*future.marker));
                    if (futureHand == candidate) cost += 1.0;
                }
                if (cost < best) { best = cost; sustain.hand = candidate == 0 ? HandOrderHand::Left : HandOrderHand::Right; }
            }
        }
        first = last;
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
    QVector<LayoutHead> heads;
    QVector<LayoutSustain> sustains;
    QHash<int, int> headByOrder;
    QHash<int, int> sustainByOrder;
    for (int tokenIndex = 0; tokenIndex < parsed.sourceTokens.size(); ++tokenIndex) {
        const auto& source = parsed.sourceTokens.at(tokenIndex);
        if (!source.nativeValid || source.markerCount == 0 || source.line < 1 || source.line > lineStarts.size()) continue;
        const int start = lineStarts.at(source.line - 1) + source.col - 1;
        const bool fixed = !hand_order::splitToken(text.mid(start, source.length)).suffix.isEmpty();
        int sharedSlideHead = -1;
        for (int index = 0; index < source.markerCount; ++index) {
            const auto* marker = byOrder.value(source.firstParseOrder + index);
            if (!marker || marker->isMine || marker->headMine || marker->trackMine) continue;
            const bool slide = marker->type == QStringLiteral("slide") || marker->type == QStringLiteral("wifi");
            int headIndex = -1;
            if (!slide || marker->hasHeadStar) {
                if (!slide || sharedSlideHead < 0) {
                    headIndex = heads.size();
                    heads.append({marker, tokenIndex, headPoint(*marker),
                        fixed ? marker->handOrderHead : HandOrderHand::Unspecified, fixed});
                    if (slide) sharedSlideHead = headIndex;
                } else headIndex = sharedSlideHead;
                headByOrder.insert(marker->parseOrder, headIndex);
            }
            if (slide && marker->type == QStringLiteral("slide")) {
                sustainByOrder.insert(marker->parseOrder, sustains.size());
                sustains.append({marker, tokenIndex, headIndex, marker->endSecond,
                    fixed && !marker->handOrderTrackStops.isEmpty()
                        ? marker->handOrderTrackStops.constFirst().hand
                        : !marker->hasHeadStar ? basicLayoutTrackHand(*marker) : HandOrderHand::Unspecified, fixed});
            } else if (marker->type == QStringLiteral("hold") || marker->type == QStringLiteral("touch_hold")) {
                sustainByOrder.insert(marker->parseOrder, sustains.size());
                sustains.append({marker, tokenIndex, headIndex, marker->endSecond,
                    fixed ? marker->handOrderHead : HandOrderHand::Unspecified, fixed});
            }
        }
    }
    assignLocalHands(&heads, &sustains);
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
            const int headIndex = headByOrder.value(firstMarker->parseOrder, -1);
            token.head = headIndex >= 0 ? heads.at(headIndex).hand : HandOrderHand::Unspecified;
            if (firstMarker->hasHeadStar && token.head == HandOrderHand::Unspecified) {
                ++plan.unsupported;
                continue;
            }
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
                    const int sustainIndex = sustainByOrder.value(marker->parseOrder, -1);
                    const auto assigned = sustainIndex >= 0 ? sustains.at(sustainIndex).hand : HandOrderHand::Unspecified;
                    token.tracks.append(QVector<HandOrderTrackStop>{{0.0,
                        assigned == HandOrderHand::Unspecified ? basicLayoutTrackHand(*marker) : assigned}});
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
                for (int index = 0; index < source.markerCount; ++index) {
                    const auto* marker = byOrder.value(source.firstParseOrder + index);
                    const int headIndex = marker ? headByOrder.value(marker->parseOrder, -1) : -1;
                    if (headIndex < 0 || heads.at(headIndex).hand == HandOrderHand::Unspecified) break;
                    notes.append(QString(parts.baseToken.at(index)) + QLatin1Char('~') + glyph(heads.at(headIndex).hand));
                }
                if (notes.size() != source.markerCount) { ++plan.unsupported; continue; }
                replacement = notes.join(QLatin1Char('/'));
            } else {
                const int headIndex = headByOrder.value(firstMarker->parseOrder, -1);
                const auto hand = headIndex >= 0 ? heads.at(headIndex).hand : HandOrderHand::Unspecified;
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

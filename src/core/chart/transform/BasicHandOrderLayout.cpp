#include "core/chart/transform/BasicHandOrderLayout.h"

#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/chart/model/TouchPadAdjacency.h"

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
constexpr double kFastRepeatedTapGapSeconds = 0.13;
constexpr double kWindingPairGapSeconds = 0.4;
constexpr double kTouchSweepGapSeconds = 0.18;

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
    int patternHand = -1;
    bool patternNeedsBothFree = false;
    int touchPairWith = -1;
};

struct LayoutSustain {
    const TimelineNoteMarker* marker = nullptr;
    int token = -1;
    int head = -1;
    double end = 0.0;
    HandOrderHand hand = HandOrderHand::Unspecified;
    bool fixed = false;
    int patternHand = -1;
};

int handIndex(HandOrderHand hand)
{
    return hand == HandOrderHand::Left ? 0 : hand == HandOrderHand::Right ? 1 : -1;
}

QVector<int> orderedHeads(const QVector<LayoutHead>& heads)
{
    QVector<int> order;
    order.reserve(heads.size());
    for (int i = 0; i < heads.size(); ++i) order.append(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const auto& left = heads.at(a);
        const auto& right = heads.at(b);
        if (left.marker->second != right.marker->second) return left.marker->second < right.marker->second;
        return left.marker->parseOrder < right.marker->parseOrder;
    });
    return order;
}

bool simpleLayoutSuffix(const QString& suffix, const TimelineNoteMarker& marker, int markerCount)
{
    if (suffix.isEmpty()) return true;
    if (markerCount != 1) return false;
    if (marker.type == QStringLiteral("slide")) {
        for (const auto head : {'l', 'r'})
            for (const auto track : {'l', 'r'})
                if (suffix == QStringLiteral("~[head=%1;track=0:%2]").arg(QChar(head)).arg(QChar(track))) return true;
        return false;
    }
    return suffix == QStringLiteral("~l") || suffix == QStringLiteral("~r");
}

bool fastTapGap(const LayoutHead& first, const LayoutHead& second)
{
    if (first.marker->type != QStringLiteral("tap") || second.marker->type != QStringLiteral("tap")) return false;
    const double gap = second.marker->second - first.marker->second;
    const double pseudoEach = 240.0 / (64.0 * qMax(first.marker->tempoBpm, second.marker->tempoBpm));
    return gap >= pseudoEach - 1e-9 && gap <= kFastRepeatedTapGapSeconds + 1e-9;
}

int adjacentLaneStep(int first, int second)
{
    if (first < 1 || first > 8 || second < 1 || second > 8) return 0;
    if (second == first % 8 + 1) return 1;
    if (second == (first + 6) % 8 + 1) return -1;
    return 0;
}

int touchLaneDistance(const QString& first, const QString& second)
{
    if (first == QStringLiteral("C") || second == QStringLiteral("C")) return 0;
    if (first.size() != 2 || second.size() != 2) return 8;
    const int distance = std::abs(first.at(1).digitValue() - second.at(1).digitValue());
    return qMin(distance, 8 - distance);
}

bool nearbyTouches(const LayoutHead& first, const LayoutHead& second)
{
    return first.marker->type == QStringLiteral("touch")
        && second.marker->type == QStringLiteral("touch")
        && chart::touchPadsAreAdjacent(first.marker->touchPad, second.marker->touchPad)
        && touchLaneDistance(first.marker->touchPad, second.marker->touchPad) <= 2;
}

void identifyLocalPatterns(QVector<LayoutHead>* heads, QVector<LayoutSustain>* sustains,
    const QVector<bool>& canRewrite, QVector<bool>* rewriteTokens)
{
    const auto order = orderedHeads(*heads);
    QVector<QVector<int>> headsByToken(canRewrite.size());
    QVector<QVector<int>> sustainsByToken(canRewrite.size());
    QVector<QVector<int>> sustainsByHead(heads->size());
    for (int i = 0; i < heads->size(); ++i) headsByToken[heads->at(i).token].append(i);
    for (int i = 0; i < sustains->size(); ++i) {
        const auto& sustain = sustains->at(i);
        sustainsByToken[sustain.token].append(i);
        if (sustain.head >= 0) sustainsByHead[sustain.head].append(i);
    }
    const auto eligible = [&](int first, int last) {
        for (int i = first; i < last; ++i)
            if (!canRewrite.value(heads->at(order.at(i)).token)) return false;
        return true;
    };
    const auto allowRecalculation = [&](int token) {
        if (rewriteTokens->at(token)) return;
        (*rewriteTokens)[token] = true;
        for (int index : headsByToken.at(token)) {
            auto& head = (*heads)[index];
            head.fixed = false;
            head.hand = HandOrderHand::Unspecified;
        }
        for (int index : sustainsByToken.at(token)) {
            auto& sustain = (*sustains)[index];
            sustain.fixed = false;
            sustain.hand = HandOrderHand::Unspecified;
        }
    };

    // Winding rebounds keep each fast pair on one hand even when the jump to
    // the next lane contains an empty subdivision.
    const auto fastPairAt = [&](int first) {
        if (first + 1 >= order.size() || !eligible(first, first + 2)) return false;
        const auto& a = heads->at(order.at(first));
        const auto& b = heads->at(order.at(first + 1));
        return a.patternHand < 0 && b.patternHand < 0
            && a.marker->lane == b.marker->lane && fastTapGap(a, b);
    };
    for (int first = 0; first < order.size();) {
        int last = first;
        int step = 0;
        while (fastPairAt(last)) {
            if (last > first) {
                const int previousLane = heads->at(order.at(last - 2)).marker->lane;
                const int lane = heads->at(order.at(last)).marker->lane;
                const int nextStep = adjacentLaneStep(previousLane, lane);
                const double jump = heads->at(order.at(last)).marker->second
                    - heads->at(order.at(last - 1)).marker->second;
                if (!nextStep || (step && step != nextStep)
                    || jump <= 1e-9 || jump > kWindingPairGapSeconds + 1e-9) break;
                step = nextStep;
            }
            last += 2;
        }
        if (last - first >= 6) {
            const int initial = handIndex(laneHand(heads->at(order.at(first)).marker->lane));
            for (int i = first; i < last; ++i) {
                auto& head = (*heads)[order.at(i)];
                allowRecalculation(head.token);
                head.patternHand = initial ^ ((i - first) / 2 % 2);
                head.patternNeedsBothFree = true;
            }
            first = last;
        } else ++first;
    }

    // Other fast same-lane runs alternate individual taps when both hands are free.
    for (int first = 0; first < order.size();) {
        const auto& initial = heads->at(order.at(first));
        if (initial.marker->type != QStringLiteral("tap") || initial.patternHand >= 0) { ++first; continue; }
        int last = first + 1;
        while (last < order.size() && heads->at(order.at(last)).patternHand < 0
            && fastTapGap(heads->at(order.at(last - 1)), heads->at(order.at(last)))) ++last;
        for (int start = first; start < last;) {
            int finish = start + 1;
            const int lane = heads->at(order.at(start)).marker->lane;
            while (finish < last && heads->at(order.at(finish)).marker->lane == lane) ++finish;
            if (finish - start >= 2 && eligible(start, finish)) {
                const int initialHand = handIndex(laneHand(lane));
                for (int i = start; i < finish; ++i) {
                    auto& head = (*heads)[order.at(i)];
                    allowRecalculation(head.token);
                    head.patternHand = initialHand ^ ((i - start) % 2);
                    head.patternNeedsBothFree = true;
                }
            }
            start = finish;
        }
        first = last;
    }

    // Repeated pairs at a relaxed cadence have no rebound pressure that
    // justifies swapping hands inside either pair.
    for (int first = 0; first + 3 < order.size();) {
        const auto& a = heads->at(order.at(first));
        const auto& b = heads->at(order.at(first + 1));
        const auto& c = heads->at(order.at(first + 2));
        const auto& d = heads->at(order.at(first + 3));
        const int firstLane = a.marker->lane;
        const int secondLane = c.marker->lane;
        const bool pairShape = a.marker->type == QStringLiteral("tap")
            && b.marker->type == QStringLiteral("tap")
            && c.marker->type == QStringLiteral("tap")
            && d.marker->type == QStringLiteral("tap")
            && firstLane == b.marker->lane && secondLane == d.marker->lane
            && adjacentLaneStep(firstLane, secondLane) != 0;
        const auto relaxedGap = [](const LayoutHead& left, const LayoutHead& right) {
            const double gap = right.marker->second - left.marker->second;
            return gap > kFastRepeatedTapGapSeconds + 1e-9 && gap <= 0.3 + 1e-9;
        };
        if (pairShape && eligible(first, first + 4)
            && relaxedGap(a, b) && relaxedGap(b, c) && relaxedGap(c, d)) {
            for (int i = first; i < first + 4; ++i) {
                auto& head = (*heads)[order.at(i)];
                allowRecalculation(head.token);
                head.patternHand = handIndex(laneHand(i < first + 2 ? firstLane : secondLane));
                head.patternNeedsBothFree = true;
            }
            first += 4;
        } else ++first;
    }

    for (int i = 0; i + 1 < order.size(); ++i) {
        auto& first = (*heads)[order.at(i)];
        auto& second = (*heads)[order.at(i + 1)];
        if (!nearbyTouches(first, second) || !eligible(i, i + 2)) continue;
        const double gap = second.marker->second - first.marker->second;
        const double pseudoEach = 240.0 / (64.0 * qMax(first.marker->tempoBpm, second.marker->tempoBpm));
        if (gap < -1e-9) continue;
        if (gap < 1e-9 && first.marker->eachGroupId >= 0
            && first.marker->eachGroupId == second.marker->eachGroupId) {
            allowRecalculation(first.token);
            allowRecalculation(second.token);
            first.touchPairWith = order.at(i + 1);
            second.touchPairWith = order.at(i);
            ++i;
        } else if (gap >= pseudoEach - 1e-9 && gap <= kTouchSweepGapSeconds + 1e-9) {
            const auto preferred = basicLayoutHandForPoint((first.point + second.point) / 2.0);
            const int hand = handIndex(preferred);
            if (hand < 0) continue;
            allowRecalculation(first.token);
            allowRecalculation(second.token);
            first.patternHand = hand;
            second.patternHand = hand;
            ++i;
        }
    }

    // Overlapping stars whose heads alternate across the screen reserve the
    // corresponding hand for the whole track, including old simple labels.
    for (int first = 0; first < order.size();) {
        const auto& initial = heads->at(order.at(first));
        if (initial.marker->type != QStringLiteral("slide") || !initial.marker->hasHeadStar) { ++first; continue; }
        int last = first + 1;
        while (last < order.size()) {
            const auto& previous = heads->at(order.at(last - 1));
            const auto& next = heads->at(order.at(last));
            if (next.marker->type != QStringLiteral("slide") || !next.marker->hasHeadStar
                || previous.marker->endSecond < next.marker->second - 1e-9
                || next.marker->second <= previous.marker->second + 1e-9
                || handIndex(laneHand(previous.marker->lane)) == handIndex(laneHand(next.marker->lane))) break;
            ++last;
        }
        if (last - first >= 3 && eligible(first, last)) {
            for (int i = first; i < last; ++i) {
                auto& head = (*heads)[order.at(i)];
                const int hand = handIndex(laneHand(head.marker->lane));
                allowRecalculation(head.token);
                head.patternHand = hand;
                for (int index : sustainsByHead.at(order.at(i))) (*sustains)[index].patternHand = hand;
            }
        }
        first = last;
    }
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
    const auto order = orderedHeads(*heads);
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
            if (a.touchPairWith == order.at(first + 1) && b.touchPairWith == order.at(first)) {
                double best = std::numeric_limits<double>::infinity();
                int sharedHand = -1;
                for (int candidate = 0; candidate < 2; ++candidate) {
                    if (busy(candidate, a.marker->second, a.token)
                        || busy(candidate, b.marker->second, b.token)) continue;
                    const double cost = headCost(a, candidate) + headCost(b, candidate);
                    if (cost < best) { best = cost; sharedHand = candidate; }
                }
                if (sharedHand >= 0) {
                    commitHead(order.at(first), sharedHand);
                    commitHead(order.at(first + 1), sharedHand);
                }
                first = last;
                continue;
            }
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
            } else if (!head.fixed && head.patternHand >= 0
                && !busy(head.patternHand, head.marker->second, head.token)
                && (!head.patternNeedsBothFree
                    || !busy(1 - head.patternHand, head.marker->second, head.token)))
                commitHead(index, head.patternHand);
            else if (!head.fixed && preferred == HandOrderHand::Either
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
            if (sustain.patternHand >= 0 && handIndex(head.hand) == sustain.patternHand
                && !busy(sustain.patternHand, head.marker->second, sustain.token)) {
                sustain.hand = sustain.patternHand == 0 ? HandOrderHand::Left : HandOrderHand::Right;
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
    QVector<bool> canRewrite(parsed.sourceTokens.size(), false);
    QVector<bool> rewriteTokens(parsed.sourceTokens.size(), false);
    QHash<int, int> headByOrder;
    QHash<int, int> sustainByOrder;
    for (int tokenIndex = 0; tokenIndex < parsed.sourceTokens.size(); ++tokenIndex) {
        const auto& source = parsed.sourceTokens.at(tokenIndex);
        if (!source.nativeValid || source.markerCount == 0 || source.line < 1 || source.line > lineStarts.size()) continue;
        const int start = lineStarts.at(source.line - 1) + source.col - 1;
        const auto parts = hand_order::splitToken(text.mid(start, source.length));
        const bool fixed = !parts.suffix.isEmpty();
        const bool completeTarget = start >= begin && start + source.length <= end;
        const auto* firstMarker = byOrder.value(source.firstParseOrder);
        bool annotationValid = true;
        for (const auto& issue : parsed.annotationErrors)
            if (issue.line == source.line && issue.col >= source.col && issue.col < source.col + source.length)
                annotationValid = false;
        canRewrite[tokenIndex] = firstMarker && annotationValid && (!fixed || completeTarget)
            && !(firstMarker->type == QStringLiteral("slide") && source.markerCount != 1)
            && simpleLayoutSuffix(parts.suffix, *firstMarker, source.markerCount);
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
    identifyLocalPatterns(&heads, &sustains, canRewrite, &rewriteTokens);
    assignLocalHands(&heads, &sustains);
    int delta = 0;
    for (int tokenIndex = 0; tokenIndex < parsed.sourceTokens.size(); ++tokenIndex) {
        const auto& source = parsed.sourceTokens.at(tokenIndex);
        if (source.line < 1 || source.line > lineStarts.size()) continue;
        const int start = lineStarts.at(source.line - 1) + source.col - 1;
        const int finish = start + source.length;
        if (start >= end || finish <= begin || source.markerCount == 0) continue;
        if (start < begin || finish > end) { ++plan.partial; continue; }
        const QString original = text.mid(start, source.length);
        const auto parts = hand_order::splitToken(original);
        if (!parts.suffix.isEmpty() && !rewriteTokens.at(tokenIndex)) {
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
        if (replacement != original) {
            plan.replacement.replace(start - begin + delta, source.length, replacement);
            plan.patches.append({start, finish, replacement});
            delta += replacement.size() - source.length;
            ++plan.changed;
        } else if (!parts.suffix.isEmpty()) ++result.preserved;
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

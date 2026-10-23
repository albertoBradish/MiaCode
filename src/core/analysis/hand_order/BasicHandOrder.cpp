#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/analysis/hand_order/HandOrderJudge.h"
#include "core/analysis/hand_order/HandOrderFullReplay.h"
#include "core/analysis/hand_order/BasicHandOrderActions.h"
#include "core/analysis/hand_order/BasicHandOrderWifi.h"
#include "core/chart/SlideReferenceData.h"
#include "core/chart/transform/BasicHandOrderLayout.h"
#include "core/chart/model/HandOrderRestPosition.h"
#include <QJsonArray>
#include <QJsonObject>
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include "core/analysis/MuriConfig.h"
#include "common/TaskCancellation.h"
#include <QLineF>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <tuple>

namespace miacode::hand_order {
namespace {
constexpr double epsilon = 1e-8;
constexpr double referenceRadius = 240.0;
constexpr double handoffOverlap = 2.0 / TouchJudgeProfile::ticksPerSecond;
constexpr double minimumStableSeconds = 0.1;
struct Primitive {
    ContactAction action;
    int token = -1, lane = 0, lock = -1, basis = -1, layoutBasis = -1;
    int interactionPrevious = -1;
    int crossPeer = -1;
    int longitudinalPrevious = -1;
    int sweepPrevious = -1;
    bool button = true;
    TimelineNoteMarker marker;
};
struct Track {
    int token = -1, branch = 0;
    int parseOrder = -1, oneStrokePrevious = -1;
    int headLane = 0;
    double headSecond = 0;
    bool hasHeadStar = false;
    int basis = -1, layoutBasis = -1;
    ContactAction action;
    QVector<HandOrderTrackStop> locks;
    QVector<double> boundaries;
    bool wifi = false, legacyAnnotated = false;
    int sector = -1, peer = -1;
    HandOrderWifiMode wifiMode = HandOrderWifiMode::Legacy;
    QVector<ContactAction> singleActions;
    QVector<HandOrderTrackStop> singleLocks;
    double judgeAvailable = 0;
};
struct Token {
    int start = 0, end = 0, line = 1, col = 1, dependencyUnit = -1;
    QString base, suffix, issue;
    QString layoutFallback;
    bool target = false, partial = false, slide = false, protectedText = false;
    double beginTime = 0, endTime = 0;
    double nominal = 0;
    QVector<int> primitives, tracks, touches;
    QVector<TimelineNoteMarker> markers;
};
using Scheduled = ScheduledAction;
struct Decision {
    enum Kind { Slide, Fixed, Touches } kind = Fixed;
    int index = -1;
    QVector<int> members;
    double second = 0, end = 0;
    int lane = 0;
};
struct SearchConstraint {
    int primitive = -1, track = -1, touch = -1, hand = -1;
    double progress = 0;
};
struct PlayerPathState {
    PlayerEffortState effort;
    QPointF point;
    double free = -std::numeric_limits<double>::infinity();
    int source = -1;
    bool known = false, screen = false;
};
struct State {
    int inputPreference = 0;
    double returnSpeed = chart::handOrderDefaultReturnSpeed;
    PlayerProfile player;
    std::array<PlayerPathState, 2> effortOrigins, effortEnds;
    QHash<int, int> sweepPreviousSources;
    QSet<int> holdSources;
    bool ordinaryOnly = false;
    double scoreFrom = -std::numeric_limits<double>::infinity();
    double focusSecond = 0;
    int focusOrdinal = 0;
    QSet<int> focusSources;
    struct ButtonHand { double free = -1e9; QPointF point; int lane = 0; bool known = false; int ordinal = -1; double pressed = 0; };
    std::array<ButtonHand, 2> buttonHands;
    std::array<ButtonHand, 2> origins;
    QVector<Scheduled> actions;
    QVector<Scheduled> physical;
    QVector<bool> wifiSingle;
    FullReplay fullReplay;
    QVector<int> primitiveHands;
    QVector<QVector<HandOrderTrackStop>> tracks;
    QVector<BasicHandoffEvidence> transferEvidence;
    double cost = 0, peak = 0, focusPeak = 0, risk = 0;
    double playerCost = 0, peakTapRate = 0, peakLoad = 0;
    int ergonomicBreaks = 0;
    TouchReplay replay;
};
struct Search { bool found = false, truncated = false, exhausted = false, sideEffect = false; State state; };
bool spend(qint64& remaining, qint64 units = 1)
{
    task::CancellationScope::checkpoint(); remaining -= units; return remaining >= 0;
}
int fixedHand(HandOrderHand hand) { return hand == HandOrderHand::Left ? 0 : hand == HandOrderHand::Right ? 1 : -1; }
HandOrderHand namedHand(int hand) { return hand == 0 ? HandOrderHand::Left : hand == 1 ? HandOrderHand::Right : HandOrderHand::Unspecified; }
int maskHand(int mask) { return mask == 1 ? 0 : mask == 2 ? 1 : -1; }
QChar glyph(int hand) { return hand == 0 ? QLatin1Char('l') : QLatin1Char('r'); }
int peakTier(double peak) { return int(std::floor(qMin(peak, 100000.0) / 4.0)); }
int switches(const QVector<HandOrderTrackStop>& stops) { return qMax(0, int(stops.size()) - 1); }
bool better(const State& a, const State& b)
{
    const int riskA = int(std::ceil(a.risk / 0.025 - epsilon)), riskB = int(std::ceil(b.risk / 0.025 - epsilon));
    if (riskA != riskB) return riskA < riskB;
    if (a.ergonomicBreaks != b.ergonomicBreaks)
        return a.ergonomicBreaks < b.ergonomicBreaks;
    // Speed and fatigue are soft player-relative costs. A fixed peak-speed
    // tier must not override the configured ability or technique preference.
    if (std::abs(a.risk - b.risk) > epsilon) return a.risk < b.risk;
    if (std::abs(a.cost - b.cost) > epsilon) return a.cost < b.cost;
    return std::lexicographical_compare(a.primitiveHands.begin(), a.primitiveHands.end(), b.primitiveHands.begin(), b.primitiveHands.end());
}
QPointF lanePoint(int lane)
{
    const double angle = qDegreesToRadians(muri::kPadAngleBaseDegrees + (lane - 1) * muri::kPadAngleStepDegrees);
    return {std::cos(angle), std::sin(angle)};
}
bool extremeCrossedEach(const Primitive& a, int handA, const Primitive& b, int handB)
{
    const double xA = lanePoint(a.lane).x(), xB = lanePoint(b.lane).x();
    return (xA < -0.25 && xB > 0.25 && handA == 1 && handB == 0)
        || (xB < -0.25 && xA > 0.25 && handB == 1 && handA == 0);
}
ContactAction slice(const ContactAction& source, double first, double last, int hand)
{
    ContactAction action = source;
    action.start = first; action.end = last; action.hand = hand;
    action.points = {contactPosition(source, first)}; action.times = {first};
    for (int i = 0; i < source.times.size(); ++i) if (source.times.at(i) > first + epsilon && source.times.at(i) < last - epsilon) {
        action.points.append(source.points.at(i)); action.times.append(source.times.at(i));
    }
    action.points.append(contactPosition(source, last)); action.times.append(last);
    return action;
}
QVector<ContactAction> contacts(const State& state)
{
    QVector<ContactAction> result;
    for (const auto& action : state.physical) if (!action.button) result.append(action.action);
    return result;
}

void setPlayerActionKinds(State& state, const QVector<Primitive>& primitives,
                          const QVector<TouchTarget>& touches)
{
    for (const auto& primitive : primitives) {
        if (primitive.sweepPrevious >= 0)
            state.sweepPreviousSources.insert(primitive.action.tokenStart,
                primitives.at(primitive.sweepPrevious).action.tokenStart);
        if (primitive.marker.type == "hold" || primitive.marker.type == "touch_hold")
            state.holdSources.insert(primitive.action.tokenStart);
    }
    for (const auto& touch : touches) if (touch.sweepPrevious >= 0)
        state.sweepPreviousSources.insert(touch.tokenStart, touches.at(touch.sweepPrevious).tokenStart);
    // Physical sweep effort follows elapsed time, not the spelling of the
    // rhythmic grid: {48} with three commas is still a regular 16th-note sweep.
    // Rim buttons crossed by one continuous wrist stroke are movement work,
    // not repeated lifts at the configured single-key tapping rate.
    QVector<int> taps;
    for (int i = 0; i < primitives.size(); ++i)
        if (primitives.at(i).marker.type == "tap") taps.append(i);
    std::stable_sort(taps.begin(), taps.end(), [&](int a, int b) {
        return primitives.at(a).action.start < primitives.at(b).action.start;
    });
    const auto direction = [&](int a, int b) {
        const int step = (primitives.at(b).lane - primitives.at(a).lane + 8) % 8;
        return step == 1 ? 1 : step == 7 ? -1 : 0;
    };
    for (int first = 0; first + 3 < taps.size();) {
        const int step = direction(taps.at(first), taps.at(first + 1));
        const double interval = primitives.at(taps.at(first + 1)).action.start
            - primitives.at(taps.at(first)).action.start;
        int last = first + 1;
        while (step && interval > epsilon && interval <= 0.18 && last < taps.size()) {
            const auto& a = primitives.at(taps.at(last - 1));
            const auto& b = primitives.at(taps.at(last));
            const double gap = b.action.start - a.action.start;
            if (direction(taps.at(last - 1), taps.at(last)) != step
                || gap < interval * 0.9 || gap > interval * 1.1
                || a.interactionPrevious >= 0 || b.interactionPrevious >= 0) break;
            ++last;
        }
        if (last - first >= 4) {
            for (int i = first + 1; i < last; ++i)
                state.sweepPreviousSources.insert(primitives.at(taps.at(i)).action.tokenStart,
                    primitives.at(taps.at(i - 1)).action.tokenStart);
            first = last;
        } else ++first;
    }
}

// Recompute from a fixed window-entry history, never from the last candidate's
// tail. Logical screen paths already share one resolved physical centre.
bool scorePlayerEffort(State& state, qint64& work,
                       double through = std::numeric_limits<double>::infinity())
{
    state.playerCost = state.peakTapRate = state.peakLoad = 0;
    state.effortEnds = state.effortOrigins;
    for (int hand = 0; hand < 2; ++hand) {
        auto& path = state.effortEnds[hand];
        QVector<const Scheduled*> ordered;
        QVector<double> times;
        if (!spend(work, state.physical.size() + 1)) return false;
        for (const auto& action : state.physical) if (action.action.hand == hand) {
            const double begin = qMax(action.action.start, state.scoreFrom);
            const double end = qMin(action.action.end, through);
            if (end <= begin + epsilon) continue;
            if (!spend(work, action.action.times.size() + 2)) return false;
            ordered.append(&action); times.append(begin); times.append(end);
            for (double time : action.action.times) if (time > begin && time < end) times.append(time);
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
            return a->action.start < b->action.start;
        });
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end(), [](double a, double b) {
            return std::abs(a - b) < epsilon;
        }), times.end());
        QVector<const Scheduled*> active;
        int next = 0;
        const auto add = [&](const PlayerEffortDelta& delta, bool charged) {
            if (!delta.valid) return false;
            if (charged) {
                state.playerCost += delta.cost;
                state.peakTapRate = qMax(state.peakTapRate, delta.peakTapRate);
                state.peakLoad = qMax(state.peakLoad, delta.peakLoad);
            }
            return true;
        };
        for (int i = 0; i + 1 < times.size(); ++i) {
            if (!spend(work, active.size() + 1)) return false;
            const double first = times.at(i), last = times.at(i + 1);
            while (next < ordered.size() && ordered.at(next)->action.start <= first + epsilon)
                active.append(ordered.at(next++));
            active.erase(std::remove_if(active.begin(), active.end(), [&](const auto* action) {
                return action->action.end <= first + epsilon;
            }), active.end());
            if (active.isEmpty()) continue;
            const auto* selected = active.first();
            const auto from = contactPosition(selected->action, first);
            const auto to = contactPosition(selected->action, last);
            bool charged = false, screen = false, sustained = false;
            bool buttonDown = false, screenDown = false, chargedDown = false;
            for (const auto* action : active) {
                charged |= !action->committed;
                screen |= !action->button;
                sustained |= state.holdSources.contains(action->action.tokenStart);
                if (std::abs(action->action.start - first) < epsilon && !action->motion) {
                    buttonDown |= action->button;
                    screenDown |= !action->button;
                    chargedDown |= !action->committed;
                }
            }
            const bool joinedScreen = path.known && path.screen && screen && first <= path.free + epsilon;
            const bool joinedSweep = path.known && path.screen == screen
                && state.sweepPreviousSources.value(selected->action.tokenStart, -2) == path.source
                && first - path.free <= 0.35;
            if (!path.known || first > path.free + epsilon) {
                const QPointF rested = path.known
                    ? chart::handOrderRestedPoint(path.point, path.free, first, hand, state.returnSpeed)
                    : chart::handOrderNaturalRestPoint(hand);
                const double begin = path.known ? path.free : first - 1.0;
                // Idle recovery is lazy. The approach ending at this window's
                // first note has not already been included in committed history.
                if (!add(playerMovement(path.effort, begin, first,
                        QLineF(rested, from).length(), state.player), charged)) return false;
            }
            if (((buttonDown && !joinedSweep) || (screenDown && !joinedScreen && !joinedSweep))
                && first > path.effort.lastStrike + epsilon)
                if (!add(playerStrike(path.effort, first, state.player), chargedDown)) return false;
            if (!add(playerMovement(path.effort, first, last,
                    QLineF(from, to).length(), state.player, sustained), charged)) return false;
            path.point = to; path.free = last; path.known = true; path.screen = screen;
            path.source = selected->action.tokenStart;
        }
    }
    return true;
}
// Score and replay one realizable centre path per physical hand.
bool score(State& state, const QVector<Track>& tracks, const QVector<Primitive>& primitives,
           const QVector<TouchTarget>& touches,
           qint64& work, bool* sideEffect = nullptr)
{
    Q_UNUSED(sideEffect);
    state.cost = state.peak = state.focusPeak = 0;
    state.ergonomicBreaks = 0;
    if (!resolveHandOrderActions(state.actions, &state.physical, work)) return false;
    for (const auto& action : state.actions) {
        if (action.committed) continue;
        if (state.inputPreference == 1 && action.button) state.cost += 0.12;
        if (state.inputPreference == 0 && action.action.kind == "screen_button") state.cost += 0.12;
        if (action.ordinary) state.cost += action.softWeight * 2.0 * qMax(0.0, action.action.hand == 0
            ? action.action.points.first().x() : -action.action.points.first().x());
    }
    for (int id = 0; id < primitives.size(); ++id) {
        const auto& primitive = primitives.at(id);
        const int assigned = state.primitiveHands.value(id, -1);
        if (assigned < 0) continue;
        if (primitive.layoutBasis >= 0 && assigned != primitive.layoutBasis)
            state.cost += 0.75;
        if (primitive.crossPeer >= 0 && state.primitiveHands.value(primitive.crossPeer, -1) >= 0
            && extremeCrossedEach(primitive, assigned, primitives.at(primitive.crossPeer),
                state.primitiveHands.at(primitive.crossPeer))) return false;
        for (int previous : {primitive.interactionPrevious, primitive.longitudinalPrevious})
            if (previous >= 0 && assigned == state.primitiveHands.value(previous, -1))
                state.cost += 3.0;
        if (primitive.sweepPrevious >= 0
            && state.primitiveHands.value(primitive.sweepPrevious, -1) >= 0
            && assigned != state.primitiveHands.value(primitive.sweepPrevious)) {
            state.cost += 12.0;
            ++state.ergonomicBreaks;
        }
    }
    QHash<int, int> directTouchHands;
    for (const auto& action : state.actions)
        if (action.action.kind == QStringLiteral("touch"))
            directTouchHands.insert(action.action.tokenStart, action.action.hand);
    for (const auto& touch : touches) {
        const int directHand = directTouchHands.value(touch.tokenStart, -1);
        if (touch.layoutBasis >= 0 && directHand >= 0 && directHand != touch.layoutBasis)
            state.cost += 0.75;
        if (touch.sweepPrevious < 0) continue;
        const int currentHand = directTouchHands.value(touch.tokenStart, -1);
        const int previousHand = directTouchHands.value(touches.at(touch.sweepPrevious).tokenStart, -1);
        if (currentHand >= 0 && previousHand >= 0 && currentHand != previousHand) {
            state.cost += 12.0;
            ++state.ergonomicBreaks;
        }
    }
    for (int first = 0; first < tracks.size(); ++first) {
        const auto& a = tracks.at(first);
        if (a.oneStrokePrevious >= 0
            && !state.tracks.value(first).isEmpty()
            && !state.tracks.value(a.oneStrokePrevious).isEmpty()
            && state.tracks.at(first).constFirst().hand
                != state.tracks.at(a.oneStrokePrevious).constLast().hand) {
            state.cost += 12.0;
            ++state.ergonomicBreaks;
        }
        if (a.layoutBasis >= 0 && !state.tracks.value(first).isEmpty()
            && fixedHand(state.tracks.at(first).constFirst().hand) != a.layoutBasis)
            state.cost += 0.75;
        if (a.wifi) {
            if (a.sector == 0 && a.hasHeadStar && !a.legacyAnnotated
                && a.wifiMode != HandOrderWifiMode::Single
                && state.wifiSingle.value(first, false)) {
                state.cost += 12.0;
                ++state.ergonomicBreaks;
            }
            continue;
        }
        if (!a.hasHeadStar || state.tracks.value(first).isEmpty()) continue;
        for (int second = first + 1; second < tracks.size(); ++second) {
            const auto& b = tracks.at(second);
            if (b.wifi || !b.hasHeadStar || state.tracks.value(second).isEmpty()
                || a.headLane != b.headLane
                || std::abs(a.headSecond - b.headSecond) > epsilon
                || std::abs(a.action.start - b.action.start) > epsilon) continue;
            const double end = qMin(a.action.end, b.action.end);
            if (end - a.action.start < 0.05) continue;
            bool divergent = false;
            for (double fraction : {0.25, 0.5, 0.75}) {
                const double at = a.action.start + (end - a.action.start) * fraction;
                divergent |= QLineF(contactPosition(a.action, at),
                    contactPosition(b.action, at)).length() > 0.32;
            }
            if (divergent && state.tracks.at(first).constFirst().hand
                == state.tracks.at(second).constFirst().hand) {
                state.cost += 12.0;
                ++state.ergonomicBreaks;
            }
        }
    }
    for (int hand = 0; hand < 2; ++hand) {
        QVector<const Scheduled*> active;
        for (const auto& action : state.physical) if (action.action.hand == hand) active.append(&action);
        std::stable_sort(active.begin(), active.end(), [](const auto* a, const auto* b) { return a->action.start < b->action.start; });
        QVector<double> times;
        for (const auto* action : active) { times.append(action->action.start); times.append(action->action.end); times += action->action.times; }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end(), [](double a, double b) { return std::abs(a - b) < epsilon; }), times.end());
        const auto& origin = state.origins[hand];
        bool known = origin.known, wasActive = false;
        int previousOrdinal = origin.ordinal;
        double pressed = origin.pressed;
        double free = origin.free; QPointF position = known ? origin.point : chart::handOrderNaturalRestPoint(hand);
        for (int i = 1; i < times.size(); ++i) {
            if (!spend(work, active.size() + 1)) return false;
            const double rawFirst = times.at(i - 1), last = times.at(i);
            if (last <= state.scoreFrom + epsilon) continue;
            const double first = qMax(rawFirst, state.scoreFrom), middle = (first + last) / 2;
            const Scheduled* selected = nullptr;
            bool focused = false;
            for (const auto* action : active) if (middle >= action->action.start && middle < action->action.end)
                focused |= state.focusSources.contains(action->action.tokenStart);
            for (const auto* action : active) if (middle >= action->action.start && middle < action->action.end) { selected = action; break; }
            if (!selected) { wasActive = false; continue; }
            const auto from = contactPosition(selected->action, first), to = contactPosition(selected->action, last);
            if (rawFirst <= state.scoreFrom + epsilon && selected->committed && selected->action.start <= first + epsilon) {
                // Historical contacts remain in replay to preserve DOWN/head
                // consumption, but the moving hand enters scoring at its actual
                // horizon position, not at an invented new press or rest pose.
                position = from; free = first; known = wasActive = true;
                pressed = selected->action.start; previousOrdinal = selected->ordinal;
            }
            if (!wasActive) {
                const QPointF rested = known ? chart::handOrderRestedPoint(position, free, first,
                    hand, state.returnSpeed) : position;
                const double distance = QLineF(rested, from).length(), gap = known ? first - free : 1.0;
                if (known && gap <= epsilon && distance > 1e-6) return false;
                const double prep = gap > epsilon ? gap : 1.0;
                const double weight = selected->ordinary && known ? basicOrdinaryAssociationWeight(first - pressed,
                    qMax(0, selected->ordinal - previousOrdinal - 1)) : 1.0;
                const double velocityWeight = selected->ordinary ? 0.01 : 0.1;
                if (!selected->committed) state.cost += selected->softWeight * weight * (distance + velocityWeight * distance * distance / prep);
                state.peak = qMax(state.peak, distance / prep);
                if (focused) state.focusPeak = qMax(state.focusPeak, distance / prep);
                if (!selected->committed && !selected->motion && !selected->ordinary) state.cost += 0.16 * qMax(0.0, hand == 0 ? from.x() : -from.x());
                pressed = first; previousOrdinal = selected->ordinal;
            } else if (QLineF(position, from).length() > 1e-6) return false;
            const double dt = last - first, distance = QLineF(from, to).length(), speed = distance / dt;
            if (!selected->committed) state.cost += distance + 0.1 * speed * speed * dt;
            if (selected->motion && !selected->committed) state.cost += 0.16 * qMax(0.0, hand == 0 ? (from.x() + to.x()) / 2 : -(from.x() + to.x()) / 2) * dt;
            state.peak = qMax(state.peak, speed);
            if (focused) state.focusPeak = qMax(state.focusPeak, speed);
            position = to; free = last; known = wasActive = true;
        }
    }
    for (int i = 0; i < state.actions.size(); ++i) if (state.actions.at(i).button)
        for (int j = i + 1; j < state.actions.size(); ++j) if (state.actions.at(j).button && state.actions.at(j).lane == state.actions.at(i).lane) {
            if (!spend(work)) return false;
            const auto& a = state.actions.at(i).action; const auto& b = state.actions.at(j).action;
            if (qMax(a.start, b.start) < qMin(a.end, b.end) - epsilon) return false;
        }
    QVector<PhysicalHandTransfer> transfers;
    for (int id = 0; id < tracks.size(); ++id) {
        const auto& stops = state.tracks.at(id); const auto& track = tracks.at(id);
        bool proposed = false;
        for (const auto& action : state.actions) proposed |= !action.committed && action.motion && action.action.tokenStart == track.action.tokenStart;
        if (!proposed) continue;
        for (int i = 1; i < stops.size(); ++i) {
            const double second = track.action.start + stops.at(i).progress * (track.action.end - track.action.start);
            const auto proposal = contactPosition(track.action, second); auto point = proposal; double nearest = std::numeric_limits<double>::infinity();
            for (const auto& physical : state.physical) if (physical.motion && physical.action.tokenStart == track.action.tokenStart
                && physical.action.branch == track.branch && second >= physical.action.start - epsilon && second <= physical.action.end + epsilon) {
                const auto actual = contactPosition(physical.action, second);
                const double distance = QLineF(proposal, actual).length();
                if (distance < nearest) { nearest = distance; point = actual; }
            }
            transfers.append({second, point, fixedHand(stops.at(i - 1).hand), fixedHand(stops.at(i).hand), i});
        }
    }
    state.cost += physicalHandTransferCharge(transfers) * (1.0 + 0.35 * state.player.techniquePreference);
    if (!scorePlayerEffort(state, work)) return false;
    state.cost += state.playerCost;
    return true;
}
void addTrack(State& state, const Track& track, const QVector<HandOrderTrackStop>& stops)
{
    for (int i = 0; i < stops.size(); ++i) {
        const double duration = track.action.end - track.action.start;
        const double first = track.action.start + stops.at(i).progress * duration;
        const double last = i + 1 < stops.size() ? track.action.start + stops.at(i + 1).progress * duration : track.action.end;
        const double occupiedFirst = i > 0 ? qMax(track.action.start, first - handoffOverlap / 2) : first;
        const double occupiedLast = i + 1 < stops.size() ? qMin(track.action.end, last + handoffOverlap / 2) : last;
        state.actions.append({slice(track.action, occupiedFirst, occupiedLast, fixedHand(stops.at(i).hand)), false, true, 0});
    }
}
QVector<QVector<HandOrderTrackStop>> trackOptions(const Track& track, int cap, qint64& work)
{
    QVector<QVector<HandOrderTrackStop>> options;
    const double duration = track.action.end - track.action.start;
    if (!track.locks.isEmpty()) {
        std::function<void(int, QVector<HandOrderTrackStop>)> visit = [&](int index, QVector<HandOrderTrackStop> stops) {
            if (!spend(work)) return;
            if (index == track.locks.size()) {
                if (options.size() >= 512) { work = -1; return; }
                options.append(stops); return;
            }
            const auto& lock = track.locks.at(index);
            for (int hand = 0; hand < 2; ++hand) if (fixedHand(lock.hand) < 0 || fixedHand(lock.hand) == hand) {
                auto candidate = stops;
                if (candidate.isEmpty() || candidate.last().hand != namedHand(hand)) candidate.append({lock.progress, namedHand(hand)});
                visit(index + 1, candidate);
            }
        };
        visit(0, {}); return options;
    }
    std::function<void(int, QVector<HandOrderTrackStop>)> visit = [&](int next, QVector<HandOrderTrackStop> stops) {
        if (!spend(work)) return; options.append(stops);
        if (switches(stops) >= cap) return;
        for (int i = next; i < track.boundaries.size(); ++i) {
            const double progress = track.boundaries.at(i);
            if ((progress - stops.last().progress) * duration < minimumStableSeconds - epsilon
                || (1.0 - progress) * duration < minimumStableSeconds - epsilon) continue;
            auto candidate = stops; candidate.append({progress, namedHand(1 - fixedHand(stops.last().hand))}); visit(i + 1, candidate);
        }
    };
    const int basis = track.basis >= 0 ? track.basis : 0;
    visit(0, {{0, namedHand(basis)}}); visit(0, {{0, namedHand(1 - basis)}}); return options;
}
bool locksSatisfied(const QVector<TouchTarget>& targets, const TouchReplay& replay, double through, bool requireAll)
{
    if (replay.exhausted) return false;
    for (int i = 0; i < targets.size(); ++i) {
        const auto& target = targets.at(i); const auto& judgement = replay.judgements.at(i);
        if (target.second + TouchJudgeProfile::availableSeconds > through + epsilon && !requireAll) continue;
        if (!judgement.judged || !judgement.highQuality) return false;
        if (!target.hold && target.lock >= 0 && judgement.handMask != (1 << target.lock)) return false;
        // Hold head/body responsibility is checked by the complete replay's
        // per-hand sufficient mask. A source ordering or redundant slide
        // contributor cannot decide the sustained owner's identity here.

    }
    return true;
}
Search search(const QVector<Decision>& decisions, const QVector<Primitive>& primitives, const QVector<Track>& tracks,
    const QVector<TouchTarget>& originalTouches, const QVector<TimelineNoteMarker>& markers, const BasicParameters& p, const QVector<int>& caps, qint64& work,
    const SearchConstraint& constraint = {}, const State* seed = nullptr, int breadth = 2)
{
    Search result; State initial = seed ? *seed : State{};
    initial.fullReplay = {}; initial.replay = {}; initial.risk = 0; initial.inputPreference = p.inputPreference;
    initial.returnSpeed = p.returnSpeed;
    initial.player = p.player;
    if (!seed) setPlayerActionKinds(initial, primitives, originalTouches);
    auto touches = originalTouches;
    if (constraint.touch >= 0) {
        if (constraint.touch >= touches.size() || (touches.at(constraint.touch).lock >= 0
            && touches.at(constraint.touch).lock != constraint.hand)) return result;
        touches[constraint.touch].lock = constraint.hand;
    }
    const auto trackMatches = [&](const QVector<HandOrderTrackStop>& stops, int id) {
        if (constraint.track != id) return true;
        int hand = -1;
        for (const auto& stop : stops) if (stop.progress <= constraint.progress + epsilon) hand = fixedHand(stop.hand);
        return hand == constraint.hand;
    };
    initial.buttonHands = initial.origins;
    for (int hand = 0; hand < 2; ++hand) if (!initial.buttonHands[hand].known)
        initial.buttonHands[hand].point = chart::handOrderNaturalRestPoint(hand);
    for (const auto& action : initial.actions) {
        auto& hand = initial.buttonHands[action.action.hand];
        if (action.action.end >= hand.free) hand = {action.action.end, contactPosition(action.action, action.action.end),
            action.lane, true, action.ordinal, action.action.start};
    }
    initial.ordinaryOnly = touches.isEmpty() && std::all_of(decisions.begin(), decisions.end(), [&](const Decision& d) {
        return d.kind == Decision::Fixed && primitives.at(d.index).marker.type == "tap";
    }) && std::all_of(initial.actions.begin(), initial.actions.end(), [](const Scheduled& action) { return action.ordinary; });
    bool buttonOnly = p.inputPreference != 2 && initial.ordinaryOnly && breadth == 0;
    if (buttonOnly) for (int i = 0; i + 2 < decisions.size(); ++i)
        if (std::abs(decisions.at(i + 2).second - decisions.at(i).second) < epsilon) buttonOnly = false;
    if (buttonOnly) for (int i = 0; i < decisions.size(); ++i) for (int j = i + 1; j < decisions.size(); ++j) {
        const auto& a = primitives.at(decisions.at(i).index); const auto& b = primitives.at(decisions.at(j).index);
        if (a.lane == b.lane && qMax(a.action.start, b.action.start) < qMin(a.action.end, b.action.end) - epsilon)
            buttonOnly = false;
    }
    if (buttonOnly) for (const auto& scheduled : initial.actions) for (const auto& d : decisions) {
        const auto& primitive = primitives.at(d.index);
        if (scheduled.lane == primitive.lane && qMax(scheduled.action.start, primitive.action.start)
            < qMin(scheduled.action.end, primitive.action.end) - epsilon) buttonOnly = false;
    }
    if (!seed) {
        initial.primitiveHands.fill(-1, primitives.size()); initial.tracks.resize(tracks.size()); initial.wifiSingle.fill(false, tracks.size());
    }
    if (buttonOnly) {
        initial.cost = initial.peak = initial.focusPeak = 0;
        initial.playerCost = initial.peakTapRate = initial.peakLoad = 0;
        initial.ergonomicBreaks = 0;
    }
    else if (!score(initial, tracks, primitives, touches, work)) { result.exhausted = work < 0; return result; }
    QVector<State> beam{initial};
    // Locked sustained actions are known constraints on the whole dependency
    // unit. Rank partial paths against their real button/screen templates
    // before pruning, so cheap constant-hand prefixes cannot erase a required
    // later transfer. This is a bounded forecast, never an autoplay input.
    const auto rankWithFutureHolds = [&](State& state) {
        if (buttonOnly) return true;
        QVector<State> forecasts{state};
        for (const auto& decision : decisions) {
            if (!spend(work)) return false;
            if (decision.kind != Decision::Fixed || state.primitiveHands.at(decision.index) >= 0) continue;
            const auto& primitive = primitives.at(decision.index);
            if (primitive.lock < 0 || (primitive.marker.type != "hold" && primitive.marker.type != "touch_hold")) continue;
            QVector<Scheduled> templates;
            auto action = primitive.action; action.hand = primitive.lock;
            templates.append({action, primitive.button, false, primitive.lane});
            if (primitive.button) {
                action.kind = QStringLiteral("screen_button");
                action.points = {touchPadPoint(QStringLiteral("A%1").arg(primitive.lane))};
                templates.append({action, false, false, primitive.lane});
            }
            if (breadth == 0 && templates.size() > 1) templates = {templates.at(p.inputPreference == 1 ? 1 : 0)};
            QVector<State> next;
            for (const auto& forecast : forecasts) for (const auto& physical : templates) {
                State candidate = forecast; candidate.actions.append(physical);
                if (score(candidate, tracks, primitives, touches, work)) next.append(std::move(candidate));
            }
            if (next.isEmpty()) return false;
            std::stable_sort(next.begin(), next.end(), better);
            if (next.size() > 4) { next.resize(4); result.truncated = true; }
            forecasts = std::move(next);
        }
        if (!forecasts.isEmpty()) {
            state.cost = forecasts.first().cost;
            state.peak = forecasts.first().peak;
            state.ergonomicBreaks = forecasts.first().ergonomicBreaks;
            state.playerCost = forecasts.first().playerCost;
            state.peakTapRate = forecasts.first().peakTapRate;
            state.peakLoad = forecasts.first().peakLoad;
        }
        return work >= 0;
    };
    for (int event = 0; event < decisions.size(); ++event) {
        const auto& decision = decisions.at(event);
        int fixedFinish = event + 1;
        if (decision.kind == Decision::Fixed) while (fixedFinish < decisions.size()
            && decisions.at(fixedFinish).kind == Decision::Fixed
            && std::abs(decisions.at(fixedFinish).second - decision.second) < epsilon) ++fixedFinish;
        task::CancellationScope::check(); QVector<State> next;
        const auto options = decision.kind == Decision::Slide ? trackOptions(tracks.at(decision.index), tracks.at(decision.index).wifi ? qMin(caps.at(decision.index), 1) : caps.at(decision.index), work)
            : QVector<QVector<HandOrderTrackStop>>{};
        for (const auto& state : beam) {
            if (!spend(work)) { result.exhausted = true; return result; }
            if (decision.kind == Decision::Fixed) {
                std::function<void(int, State)> assign = [&](int index, State candidate) {
                    if (!spend(work)) return;
                    if (index == fixedFinish) { if (rankWithFutureHolds(candidate)) next.append(std::move(candidate)); return; }
                    const int primitiveId = decisions.at(index).index; const auto& primitive = primitives.at(primitiveId);
                    int inheritedMask = 0; bool inheritedQuality = false;
                    if (!buttonOnly && primitive.button && !candidate.physical.isEmpty()) {
                        QVector<ChartInputAction> inputs;
                        for (const auto& scheduled : candidate.physical) inputs.append({scheduled.action, scheduled.button, scheduled.lane});
                        FullReplayOptions prefix; prefix.throughSecond = primitive.action.start + FullReplayProfile::tapCriticalSeconds;
                        auto head = primitive.marker;
                        if (head.type == "slide" || head.type == "wifi") { head.type = QStringLiteral("tap"); head.isEx = head.headEx; }
                        if (head.type == "hold") head.type = QStringLiteral("tap");
                        const auto existing = replayHandOrderChart({head}, inputs, prefix, &work);
                        if (existing.exhausted) { result.exhausted = true; return; }
                        if (!existing.judgements.isEmpty()) {
                            const auto& judged = existing.judgements.first();
                            for (const auto& cause : (judged.headRequired ? judged.headCauses : judged.causes)) inheritedMask |= cause.handMask;
                            inheritedQuality = judged.headRequired ? judged.headHighQuality : judged.highQuality;
                        }
                    }
                    for (int choiceIndex = 0; choiceIndex < 2; ++choiceIndex) {
                        const int hand = primitive.basis >= 0 ? (choiceIndex == 0 ? primitive.basis : 1 - primitive.basis) : choiceIndex;
                        if ((primitive.lock >= 0 && primitive.lock != hand) || (primitiveId == constraint.primitive && hand != constraint.hand)) continue;
                        auto action = primitive.action; action.hand = hand;
                        QVector<Scheduled> choices{{action, primitive.button, false, primitive.lane}};
                        if (primitive.button && p.inputPreference == 1) {
                            choices[0].button = false; choices[0].action.kind = QStringLiteral("screen_button");
                            choices[0].action.points = {touchPadPoint(QStringLiteral("A%1").arg(primitive.lane))};
                        }
                        if (!buttonOnly && primitive.button) {
                            auto screen = action; screen.kind = QStringLiteral("screen_button");
                            screen.points = {touchPadPoint(QStringLiteral("A%1").arg(primitive.lane))};
                            if (breadth >= 1) choices.append(p.inputPreference == 1 ? Scheduled{action, true, false, primitive.lane}
                                : Scheduled{screen, false, false, primitive.lane});
                            if (inheritedQuality && inheritedMask == (1 << hand)) {
                                State inherited = candidate; inherited.primitiveHands[primitiveId] = hand;
                                assign(index + 1, std::move(inherited));
                            }
                        }
                        bool nearContact = !touches.isEmpty() || fixedFinish - event > 2;
                        for (const auto& scheduled : candidate.physical) nearContact |= scheduled.action.start <= action.end + FullReplayProfile::tapCriticalSeconds
                            && scheduled.action.end >= action.start - FullReplayProfile::tapCriticalSeconds;
                        if (breadth >= 2 && !buttonOnly && nearContact && primitive.button && primitive.marker.type != "hold") {
                            const auto nominal = choices;
                            for (double delta : {-FullReplayProfile::tapCriticalSeconds, -1.0 / FullReplayProfile::ticksPerSecond,
                                    1.0 / FullReplayProfile::ticksPerSecond, FullReplayProfile::tapCriticalSeconds}) {
                                for (auto shifted : nominal) {
                                    shifted.action.start += delta; shifted.action.end += delta;
                                    shifted.action.times = {shifted.action.start}; choices.append(shifted);
                                }
                            }
                        }
                        for (const auto& choice : choices) {
                        State branch = candidate; action = choice.action;
                        auto scheduled = choice; scheduled.ordinary = primitive.marker.type == "tap"; scheduled.ordinal = primitive.token;
                        if (scheduled.ordinary && action.start > branch.focusSecond + epsilon)
                            scheduled.softWeight = basicOrdinaryAssociationWeight(action.start - branch.focusSecond,
                                qMax(0, primitive.token - branch.focusOrdinal - 1));
                        branch.actions.append(scheduled); branch.primitiveHands[primitiveId] = hand;
                        bool valid = primitive.crossPeer < 0
                            || branch.primitiveHands.at(primitive.crossPeer) < 0
                            || !extremeCrossedEach(primitive, hand,
                                primitives.at(primitive.crossPeer),
                                branch.primitiveHands.at(primitive.crossPeer));
                        if (valid && buttonOnly) {
                            const auto& previous = candidate.buttonHands[hand]; const auto& other = candidate.buttonHands[1 - hand];
                            const QPointF rested = chart::handOrderRestedPoint(previous.point, previous.free,
                                action.start, hand, p.returnSpeed);
                            const double distance = QLineF(rested, action.points.first()).length();
                            const double gap = previous.known ? action.start - previous.free : 1.0;
                            valid = previous.free <= action.start + epsilon
                                && !(other.free > action.start + epsilon && other.lane == primitive.lane)
                                && !(previous.known && gap <= epsilon && distance > 1e-6);
                            if (valid) {
                                const double prep = gap > epsilon ? gap : 1.0;
                                const double weight = previous.known ? basicOrdinaryAssociationWeight(action.start - previous.pressed,
                                    qMax(0, primitive.token - previous.ordinal - 1)) : 1.0;
                                branch.cost += scheduled.softWeight * (weight * (distance + 0.01 * distance * distance / prep)
                                    + 2.0 * qMax(0.0, hand == 0 ? action.points.first().x() : -action.points.first().x()));
                                if (primitive.layoutBasis >= 0 && hand != primitive.layoutBasis)
                                    branch.cost += 0.75;
                                for (int previousId : {primitive.interactionPrevious,
                                                       primitive.longitudinalPrevious})
                                    if (previousId >= 0 && branch.primitiveHands.at(previousId) == hand)
                                        branch.cost += 3.0;
                                if (primitive.sweepPrevious >= 0
                                    && branch.primitiveHands.at(primitive.sweepPrevious) >= 0
                                    && branch.primitiveHands.at(primitive.sweepPrevious) != hand) {
                                    branch.cost += 12.0;
                                    ++branch.ergonomicBreaks;
                                }
                                branch.peak = qMax(branch.peak, distance / prep);
                                if (branch.focusSources.contains(action.tokenStart)) branch.focusPeak = qMax(branch.focusPeak, distance / prep);
                                branch.buttonHands[hand] = {action.end, action.points.first(), primitive.lane, true, primitive.token, action.start};
                                branch.physical = branch.actions;
                                branch.cost -= branch.playerCost;
                                valid = scorePlayerEffort(branch, work);
                                branch.cost += branch.playerCost;
                            }
                        } else if (valid) valid = score(branch, tracks, primitives, touches, work, &result.sideEffect);
                        if (valid) {
                            if (primitive.button) {
                                const double margin = FullReplayProfile::tapCriticalSeconds - std::abs(action.start - primitive.marker.second);
                                branch.risk += qMax(0.0, 0.025 - margin);
                            }
                            assign(index + 1, std::move(branch));
                        }
                        }
                    }
                };
                assign(event, state);
            } else if (decision.kind == Decision::Slide) {
                const auto& track = tracks.at(decision.index);
                if (!track.wifi) {
                    for (const auto& option : options) {
                        if (!trackMatches(option, decision.index)) continue;
                        State candidate = state; candidate.tracks[decision.index] = option; addTrack(candidate, track, option);
                        if (score(candidate, tracks, primitives, touches, work, &result.sideEffect) && rankWithFutureHolds(candidate)) next.append(std::move(candidate));
                    }
                } else {
                    const auto& peer = tracks.at(track.peer);
                    if (track.wifiMode != HandOrderWifiMode::Single && !track.legacyAnnotated) {
                        const auto peerOptions = trackOptions(peer, qMin(caps.at(track.peer), 1), work);
                        for (const auto& option : options) for (const auto& other : peerOptions) {
                            if (!spend(work)) break;
                            if (!trackMatches(option, decision.index) || !trackMatches(other, track.peer)) continue;
                            // Standard two-palm Wifi keeps the two real hands
                            // distinct; shared centre coverage is replayed.
                            bool opposite = true;
                            QVector<double> boundaries{0.0};
                            for (const auto& stop : option) boundaries.append(stop.progress);
                            for (const auto& stop : other) boundaries.append(stop.progress);
                            for (double at : boundaries) {
                                int a = 0, b = 0;
                                for (const auto& stop : option) if (stop.progress <= at + epsilon) a = fixedHand(stop.hand);
                                for (const auto& stop : other) if (stop.progress <= at + epsilon) b = fixedHand(stop.hand);
                                opposite &= a != b;
                            }
                            if (!opposite) continue;
                            State candidate = state; candidate.tracks[decision.index] = option; candidate.tracks[track.peer] = other;
                            addTrack(candidate, track, option); addTrack(candidate, peer, other);
                            if (score(candidate, tracks, primitives, touches, work, &result.sideEffect) && rankWithFutureHolds(candidate)) next.append(std::move(candidate));
                        }
                    }
                    if (track.wifiMode != HandOrderWifiMode::Split) {
                        QVector<QVector<HandOrderTrackStop>> singleOptions;
                        if (track.legacyAnnotated) {
                            auto legacy = track; legacy.locks = track.singleLocks;
                            singleOptions = trackOptions(legacy, caps.at(decision.index), work);
                        } else for (int hand = 0; hand < 2; ++hand) {
                            bool allowed = true;
                            for (const auto& lock : track.singleLocks) allowed &= fixedHand(lock.hand) < 0 || fixedHand(lock.hand) == hand;
                            if (allowed) singleOptions.append({{0.0, namedHand(hand)}});
                        }
                        for (const auto& stops : singleOptions) {
                            if (!trackMatches(stops, decision.index) || !trackMatches(stops, track.peer)) continue;
                            for (const auto& action : track.singleActions) {
                                State candidate = state;
                                candidate.wifiSingle[decision.index] = candidate.wifiSingle[track.peer] = true;
                                candidate.tracks[decision.index] = stops;
                                auto path = track; path.action = action;
                                addTrack(candidate, path, stops);
                                if (score(candidate, tracks, primitives, touches, work, &result.sideEffect) && rankWithFutureHolds(candidate)) next.append(std::move(candidate));
                            }
                        }
                    }
                }
            } else {
                QVector<State> groupBeam{state};
                const auto progress = [&](const State& candidate) {
                    int count = 0;
                    for (int id : decision.members) if (candidate.replay.judgements.at(id).highQuality) ++count;
                    return count;
                };
                for (int index : decision.members) {
                    QVector<State> layer;
                    for (auto candidate : groupBeam) {
                        if (!spend(work)) break;
                        candidate.replay = replayHandOrderTouches(touches, contacts(candidate), decision.second, &work);
                        if (candidate.replay.exhausted) break;
                        const auto& target = touches.at(index); const auto judgement = candidate.replay.judgements.at(index);
                        layer.append(candidate);
                        if (judgement.highQuality && judgement.margin >= 0.025 && (target.lock < 0 || judgement.handMask == (1 << target.lock))) continue;
                        QVector<double> moments{target.second,
                            target.second - TouchJudgeProfile::criticalSeconds + 3.0 / TouchJudgeProfile::ticksPerSecond,
                            target.second + TouchJudgeProfile::criticalSeconds - 3.0 / TouchJudgeProfile::ticksPerSecond};
                        for (const auto& cause : judgement.causes) moments.append(cause.second);
                        for (double moment : moments) for (int hand = 0; hand < 2; ++hand) {
                            if (!spend(work)) break;
                            if (target.lock >= 0 && target.lock != hand) continue;
                            State branch = candidate; ContactAction press;
                            press.tokenStart = target.tokenStart; press.kind = "touch"; press.hand = hand;
                            press.start = std::ceil(moment * TouchJudgeProfile::ticksPerSecond - epsilon) / TouchJudgeProfile::ticksPerSecond;
                            press.end = press.start + p.pressSeconds; press.points = {touchPadPoint(target.pad)}; press.times = {press.start};
                            branch.actions.append({press, false, false, 0});
                            if (!score(branch, tracks, primitives, touches, work, &result.sideEffect)) continue;
                            branch.replay = replayHandOrderTouches(touches, contacts(branch), decision.second, &work);
                            if (!branch.replay.exhausted) layer.append(std::move(branch));
                        }
                    }
                    std::stable_sort(layer.begin(), layer.end(), [&](const State& a, const State& b) {
                        if (progress(a) != progress(b)) return progress(a) > progress(b);
                        return better(a, b);
                    });
                    if (layer.size() > p.beamWidth) { layer.resize(p.beamWidth); result.truncated = true; }
                    groupBeam = std::move(layer);
                }
                for (auto& candidate : groupBeam) {
                    bool complete = !candidate.replay.exhausted;
                    for (int id : decision.members) {
                        const auto& judged = candidate.replay.judgements.at(id);
                        complete &= judged.highQuality && (touches.at(id).lock < 0 || judged.handMask == (1 << touches.at(id).lock));
                    }
                    if (complete) {
                        candidate.risk = 0;
                        for (const auto& judged : candidate.replay.judgements) if (judged.judged) candidate.risk += qMax(0.0, 0.025 - judged.margin);
                        if (rankWithFutureHolds(candidate)) next.append(std::move(candidate));
                    }
                }
            }
        }
        if (work < 0 || result.exhausted) { result.exhausted = true; return result; }
        if (next.isEmpty()) return result;
        std::stable_sort(next.begin(), next.end(), better);
        if (next.size() > p.beamWidth) { next.resize(p.beamWidth); result.truncated = true; } beam = std::move(next);
        if (decision.kind == Decision::Fixed) event = fixedFinish - 1;
    }
    QVector<State> finished;
    for (auto& candidate : beam) {
        if (work < 0) { result.exhausted = result.truncated = true; break; }
        if (!buttonOnly && !score(candidate, tracks, primitives, touches, work)) continue;
        candidate.replay = replayHandOrderTouches(touches, contacts(candidate), std::numeric_limits<double>::infinity(), &work);
        if (!locksSatisfied(touches, candidate.replay, std::numeric_limits<double>::infinity(), true)) continue;
        QVector<ChartInputAction> inputs;
        const auto& physical = candidate.physical.isEmpty() ? candidate.actions : candidate.physical;
        for (const auto& action : physical) inputs.append({action.action, action.button, action.lane});
        candidate.fullReplay = replayHandOrderChart(markers, inputs, {}, &work);
        if (candidate.fullReplay.exhausted) { result.exhausted = result.truncated = true; break; }
        if (!candidate.fullReplay.valid || !candidate.fullReplay.meetsTarget) { result.sideEffect = true; continue; }
        bool fixedResponsibilities = true;
        for (int primitiveId = 0; primitiveId < primitives.size(); ++primitiveId) if (candidate.primitiveHands.at(primitiveId) >= 0) {
            const auto& primitive = primitives.at(primitiveId);
            for (const auto& full : candidate.fullReplay.judgements) if (full.parseOrder == primitive.marker.parseOrder) {
                const int assignedMask = 1 << candidate.primitiveHands.at(primitiveId);
                const bool hold = primitive.marker.type == "hold" || primitive.marker.type == "touch_hold";
                if (hold) fixedResponsibilities &= (full.singleHandCompletionMask & assignedMask) != 0;
                else {
                    const bool star = primitive.marker.type == "slide" || primitive.marker.type == "wifi";
                    int mask = 0; for (const auto& cause : (star ? full.headCauses : full.causes)) mask |= cause.handMask;
                    fixedResponsibilities &= mask == assignedMask;
                }
            }
        }
        if (!fixedResponsibilities) continue;
        // The lightweight Touch replay proposes actions. The complete replay
        // is authoritative once button, hold and slide event competition is known.
        for (int i = 0; i < touches.size(); ++i) for (const auto& full : candidate.fullReplay.judgements) {
            if (full.parseOrder != touches.at(i).parseOrder) continue;
            auto& judged = candidate.replay.judgements[i];
            judged.judged = full.completed; judged.highQuality = full.highQuality;
            judged.handMask = full.handMask; judged.second = full.second; judged.margin = full.margin;
            judged.quality = full.quality; judged.causes = full.causes;
            bool slide = false; for (const auto& cause : full.causes) slide |= cause.kind == "slide" || cause.kind == "wifi";
            judged.kind = touches.at(i).hold ? QStringLiteral("touch_hold")
                : full.kind == "group_carry" ? QStringLiteral("group_carry")
                : slide ? QStringLiteral("slide_cover") : QStringLiteral("direct_touch");
        }
        if (!locksSatisfied(touches, candidate.replay, std::numeric_limits<double>::infinity(), true)) continue;
        candidate.risk = 0;
        for (const auto& judged : candidate.fullReplay.judgements)
            candidate.risk += qMax(0.0, 0.025 - judged.margin);
        finished.append(std::move(candidate));
        // Button-only costs are already exact and their event margins do not
        // depend on hand identity. Avoid replaying the same 100-tap schedule
        // for every equivalent terminal hand assignment.
        if (buttonOnly) break;
    }
    for (const auto& candidate : finished) if (!result.found || better(candidate, result.state)) {
        result.found = true; result.state = candidate;
    }
    // A later terminal candidate cannot erase an already replayed solution.
    // Carry attribution still needs comparison evidence, so close it when the
    // remaining terminal alternatives could not be checked.
    if (result.found && (result.exhausted || work < 0)) for (auto& judged : result.state.replay.judgements)
        if (judged.kind == "group_carry") judged.stableResponsibility = false;
    if (result.found) for (const auto& candidate : finished) {
        if (peakTier(candidate.peak) != peakTier(result.state.peak) || std::abs(candidate.risk - result.state.risk) > epsilon
            || candidate.cost > result.state.cost + 0.25) continue;
        for (int i = 0; i < touches.size(); ++i) {
            auto& judged = result.state.replay.judgements[i];
            if (judged.kind == "group_carry" && judged.handMask != candidate.replay.judgements.at(i).handMask)
                judged.stableResponsibility = false;
        }
    }
    result.exhausted |= work < 0; return result;
}
}

double basicOrdinaryAssociationWeight(double seconds, int intermediateNotes)
{
    return std::exp(-qMax(0.0, seconds) / 0.35) * std::pow(0.5, qMax(0, intermediateNotes));
}

double basicMotionCost(const QVector<QPointF>& points, const QVector<double>& seconds, double* peakSpeed)
{
    double cost = 0, peak = 0;
    if (points.size() != seconds.size() + 1) return std::numeric_limits<double>::quiet_NaN();
    for (int i = 0; i < seconds.size(); ++i) {
        task::CancellationScope::checkpoint();
        if (!std::isfinite(seconds.at(i)) || seconds.at(i) <= 0) return std::numeric_limits<double>::quiet_NaN();
        const double distance = QLineF(points.at(i), points.at(i + 1)).length(), speed = distance / seconds.at(i);
        cost += distance + 0.1 * speed * speed * seconds.at(i); peak = qMax(peak, speed);
    }
    if (peakSpeed) *peakSpeed = peak; return cost;
}

BasicResult inferBasicHandOrder(const QString& text, const SimaiParseResult& parsed, int begin, int end,
    const BasicParameters& p, const miacode::simai::SimaiTimingMetadata& metadata)
{
    BasicResult result;
    begin = qBound(0, begin, int(text.size())); end = qBound(begin, end, int(text.size()));
    if (p.version != basicAlgorithmVersion || p.judgeProfileRevision != basicJudgeProfileRevision
        || p.beamWidth < 1 || p.beamWidth > 128 || p.maxTasks < 1 || p.maxTasks > 2048
        || p.counterfactualBudget < 0 || p.counterfactualBudget > 128
        || !std::isfinite(p.contextSeconds) || p.contextSeconds < 0 || p.contextSeconds > 10
        || !std::isfinite(p.pressSeconds) || p.pressSeconds <= 0 || p.pressSeconds > 1
        || !std::isfinite(p.returnSpeed) || p.returnSpeed < BasicParameters::minReturnSpeed
        || p.returnSpeed > BasicParameters::maxReturnSpeed
        || !validPlayerProfile(p.player)
        || p.maxWorkUnits < 1000 || p.maxWorkUnits > 100000000 || p.inputPreference < 0 || p.inputPreference > 2) {
        result.error = "invalid_parameters"; return result;
    }
    QVector<int> lines{0}; for (int i = 0; i < text.size(); ++i) if (text.at(i) == '\n') lines.append(i + 1);
    const auto layout = chart_transform::basicHandOrderLayoutInSelection(
        text, begin, end, metadata, p.returnSpeed);
    QString layoutText = text;
    for (int i = layout.edit.patches.size() - 1; i >= 0; --i) {
        const auto& patch = layout.edit.patches.at(i);
        layoutText.replace(patch.start, patch.end - patch.start, patch.text);
    }
    const auto layoutParsed = SimaiParser::validateSyntax(layoutText, metadata);
    QVector<QString> layoutFallback(parsed.sourceTokens.size());
    QVector<const TimelineNoteMarker*> layoutByOrder;
    bool layoutCompatible = layoutParsed.errors.isEmpty()
        && layoutParsed.noteMarkers.size() == parsed.noteMarkers.size()
        && layoutParsed.sourceTokens.size() == parsed.sourceTokens.size();
    for (int i = 0; layoutCompatible && i < parsed.noteMarkers.size(); ++i) {
        const auto& original = parsed.noteMarkers.at(i);
        const auto& suggested = layoutParsed.noteMarkers.at(i);
        layoutCompatible = original.parseOrder == suggested.parseOrder
            && original.type == suggested.type && original.lane == suggested.lane
            && original.endLane == suggested.endLane
            && std::abs(original.second - suggested.second) < epsilon
            && std::abs(original.endSecond - suggested.endSecond) < epsilon;
    }
    QVector<int> layoutLines{0};
    for (int i = 0; i < layoutText.size(); ++i)
        if (layoutText.at(i) == QLatin1Char('\n')) layoutLines.append(i + 1);
    for (int i = 0; layoutCompatible && i < parsed.sourceTokens.size(); ++i) {
        const auto& source = parsed.sourceTokens.at(i);
        const auto& suggested = layoutParsed.sourceTokens.at(i);
        if (source.line < 1 || source.line > lines.size()
            || suggested.line < 1 || suggested.line > layoutLines.size()
            || source.markerCount != suggested.markerCount) { layoutCompatible = false; break; }
        const auto originalParts = splitToken(text.mid(lines.at(source.line - 1) + source.col - 1,
            source.length));
        const QString suggestedToken = layoutText.mid(
            layoutLines.at(suggested.line - 1) + suggested.col - 1, suggested.length);
        const auto suggestedParts = splitToken(suggestedToken);
        if (originalParts.baseToken != suggestedParts.baseToken) { layoutCompatible = false; break; }
        if (!suggestedParts.suffix.isEmpty()) layoutFallback[i] = suggestedToken;
    }
    if (layoutCompatible) {
        layoutByOrder.fill(nullptr, parsed.noteMarkers.size());
        for (const auto& marker : layoutParsed.noteMarkers)
            if (marker.parseOrder >= 0 && marker.parseOrder < layoutByOrder.size())
                layoutByOrder[marker.parseOrder] = &marker;
    } else layoutFallback.fill({});
    QVector<const TimelineNoteMarker*> byOrder(parsed.noteMarkers.size(), nullptr);
    for (const auto& marker : parsed.noteMarkers) if (marker.parseOrder >= 0 && marker.parseOrder < byOrder.size()) byOrder[marker.parseOrder] = &marker;
    QVector<Token> tokens; QVector<Primitive> primitives; QVector<Track> tracks; QVector<TouchTarget> touches;
    struct Window { double first, last; }; QVector<Window> risks;
    for (const auto& source : parsed.sourceTokens) {
        task::CancellationScope::checkpoint(); Token token;
        token.start = lines.at(source.line - 1) + source.col - 1; token.end = token.start + source.length;
        token.line = source.line; token.col = source.col;
        const auto parts = splitToken(text.mid(token.start, source.length)); token.base = parts.baseToken; token.suffix = parts.suffix;
        token.protectedText = !parts.suffix.isEmpty(); token.target = token.start >= begin && token.end <= end;
        token.partial = token.start < end && token.end > begin && !token.target;
        if (p.overwriteExisting && token.target) token.protectedText = false;
        if (!source.nativeValid || source.markerCount == 0 || !parsed.errors.isEmpty()) token.issue = "invalid_native";
        for (const auto& error : parsed.annotationErrors) if (error.line == source.line && error.col >= source.col && error.col < source.col + source.length) token.issue = "invalid_annotation";
        token.beginTime = token.nominal = std::numeric_limits<double>::infinity(); token.endTime = -std::numeric_limits<double>::infinity();
        const int tokenIndex = tokens.size(); bool headCreated = false;
        token.layoutFallback = layoutFallback.value(tokenIndex);
        for (int i = 0; i < source.markerCount; ++i) {
            const auto* pointer = byOrder.value(source.firstParseOrder + i); if (!pointer) { token.issue = "invalid_native"; continue; }
            auto marker = *pointer;
            const auto* suggested = layoutByOrder.value(marker.parseOrder, nullptr);
            if (p.overwriteExisting && token.target) {
                marker.handOrderHead = HandOrderHand::Unspecified; marker.handOrderTrackStops.clear();
                marker.handOrderWifiSectorStops.clear(); marker.handOrderWifiMode = HandOrderWifiMode::Legacy;
            }
            if (!std::isfinite(marker.second) || !std::isfinite(marker.endSecond)
                || !std::isfinite(marker.slideTraceSecond) || std::abs(marker.second) > 1e10
                || std::abs(marker.endSecond) > 1e10 || std::abs(marker.slideTraceSecond) > 1e10) {
                token.issue = "resource_limit"; continue;
            }
            const bool screenTouch = marker.type == "touch" || marker.type == "touch_hold";
            token.nominal = qMin(token.nominal, marker.second);
            const double available = screenTouch ? FullReplayProfile::touchAvailableSeconds : FullReplayProfile::tapAvailableSeconds;
            token.beginTime = qMin(token.beginTime, marker.second - available);
            token.endTime = qMax(token.endTime, qMax(marker.second + p.pressSeconds, marker.endSecond));
            token.endTime = qMax(token.endTime, marker.second + available);
            if (marker.type == "slide" || marker.type == "wifi")
                token.endTime = qMax(token.endTime, marker.endSecond + FullReplayProfile::slideAvailableSeconds);
            const bool slide = marker.type == "slide" || marker.type == "wifi", touch = marker.type == "touch", touchHold = marker.type == "touch_hold";
            token.slide |= slide;
            if (marker.type != "tap" && marker.type != "hold" && !slide && !touch && !touchHold) token.issue = "unsupported_object";
            if (marker.isMine || marker.headMine || marker.trackMine) token.issue = "mine_unsupported";
            token.markers.append(marker);
            if (touch || touchHold) {
                if (!muri::padTokenIsValid(marker.touchPad)) token.issue = "geometry_unsupported";
                token.touches.append(touches.size());
                touches.append({token.start, marker.parseOrder, marker.eachGroupId, marker.touchPad,
                    marker.second, touchHold, fixedHand(marker.handOrderHead), -1,
                    marker.tempoBpm, marker.subdivision,
                    suggested ? fixedHand(suggested->handOrderHead) : -1});
            }
            if (!touch && (!slide || (marker.hasHeadStar && !headCreated))) {
                Primitive primitive; primitive.token = tokenIndex; primitive.lane = marker.lane; primitive.marker = marker;
                primitive.action.tokenStart = token.start; primitive.action.kind = touchHold ? "touch_hold" : "button";
                primitive.action.start = marker.second;
                primitive.action.end = (marker.type == "hold" || touchHold) ? qMax(marker.endSecond, marker.second + p.pressSeconds) : marker.second + p.pressSeconds;
                primitive.action.points = {touchHold ? touchPadPoint(marker.touchPad) : lanePoint(marker.lane)};
                primitive.action.times = {primitive.action.start}; primitive.button = !touchHold;
                primitive.lock = fixedHand(marker.handOrderHead);
                primitive.basis = fixedHand(chart_transform::basicLayoutHeadHand(marker));
                primitive.layoutBasis = suggested ? fixedHand(suggested->handOrderHead) : -1;
                if (primitive.layoutBasis >= 0) primitive.basis = primitive.layoutBasis;
                token.primitives.append(primitives.size()); primitives.append(primitive); headCreated |= slide;
            }
            if (slide) {
                Track track; track.token = tokenIndex; track.branch = marker.slideBranchIndex;
                track.parseOrder = marker.parseOrder;
                track.headLane = marker.lane; track.headSecond = marker.second;
                track.hasHeadStar = marker.hasHeadStar; track.locks = marker.handOrderTrackStops;
                track.basis = fixedHand(chart_transform::basicLayoutTrackHand(marker));
                track.layoutBasis = suggested && !suggested->handOrderTrackStops.isEmpty()
                    ? fixedHand(suggested->handOrderTrackStops.constFirst().hand) : -1;
                if (track.layoutBasis >= 0) track.basis = track.layoutBasis;
                track.action.tokenStart = token.start; track.action.branch = track.branch; track.action.kind = "slide";
                track.action.start = marker.slideTraceSecond; track.action.end = marker.endSecond;
                track.judgeAvailable = marker.second - muri::kSlideLeadingSeconds;
                track.action.motionStart = track.action.start; track.action.motionEnd = track.action.end;
                if ((marker.type != "wifi" && marker.slideSegmentPoints.isEmpty()) || track.action.end <= track.action.start) token.issue = "geometry_unsupported";
                for (int segment = 0; marker.type != "wifi" && segment < marker.slideSegmentPoints.size(); ++segment) {
                    const auto& points = marker.slideSegmentPoints.at(segment);
                    if (points.size() < 2 || segment >= marker.slideSegmentShootSeconds.size()) { token.issue = "geometry_unsupported"; break; }
                    const double first = marker.slideSegmentShootSeconds.at(segment);
                    const double last = segment + 1 < marker.slideSegmentShootSeconds.size() ? marker.slideSegmentShootSeconds.at(segment + 1) : marker.endSecond;
                    if (last <= first || !std::isfinite(first) || !std::isfinite(last)) { token.issue = "geometry_unsupported"; break; }
                    for (int point = 0; point < points.size(); ++point) {
                        const double second = first + (last - first) * point / (points.size() - 1);
                        const auto position = points.at(point) / referenceRadius;
                        if (!std::isfinite(position.x()) || !std::isfinite(position.y())) { token.issue = "geometry_unsupported"; break; }
                        if (!track.action.times.isEmpty() && second <= track.action.times.last() + epsilon) {
                            if (QLineF(position, track.action.points.last()).length() > 1e-6) token.issue = "geometry_unsupported";
                            continue;
                        }
                        track.action.points.append(position); track.action.times.append(second);
                    }
                }
                compactContactAction(track.action);
                if (track.locks.size() > 14) token.issue = "resource_limit";
                if (marker.type == "wifi") {
                    QVector<ContactAction> sectors, single;
                    if (!buildWifiHandOrderActions(marker, &sectors, &single)) { token.issue = "geometry_unsupported"; continue; }
                    const int primary = tracks.size();
                    for (int sector = 0; sector < 2; ++sector) {
                        Track wing = track; wing.wifi = true; wing.sector = sector;
                        wing.basis = fixedHand(chart_transform::basicLayoutWifiSectorHand(marker, sector));
                        wing.layoutBasis = suggested && suggested->handOrderWifiSectorStops.size() > sector
                            && !suggested->handOrderWifiSectorStops.at(sector).isEmpty()
                            ? fixedHand(suggested->handOrderWifiSectorStops.at(sector).constFirst().hand) : -1;
                        if (wing.layoutBasis >= 0) wing.basis = wing.layoutBasis;
                        wing.peer = primary + 1 - sector; wing.wifiMode = marker.handOrderWifiMode;
                        wing.action = sectors.at(sector); wing.action.tokenStart = token.start;
                        wing.singleActions = single;
                        for (auto& action : wing.singleActions) action.tokenStart = token.start;
                        wing.singleLocks = marker.handOrderTrackStops;
                        wing.locks = marker.handOrderWifiMode == HandOrderWifiMode::Split
                            ? marker.handOrderWifiSectorStops.value(sector) : QVector<HandOrderTrackStop>{};
                        // Legacy l/r/m remains a whole-track label; an existing
                        // label constrains the corresponding real single hand.
                        wing.legacyAnnotated = marker.handOrderWifiMode == HandOrderWifiMode::Legacy && !marker.handOrderTrackStops.isEmpty();
                        token.tracks.append(tracks.size()); tracks.append(wing);
                    }
                } else { token.tracks.append(tracks.size()); tracks.append(track); }
            }
        }
        if (headCreated) for (int trackId : token.tracks) tracks[trackId].hasHeadStar = true;
        if (!token.issue.isEmpty() && std::isfinite(token.beginTime)) risks.append({token.beginTime - p.contextSeconds, token.endTime + p.contextSeconds});
        tokens.append(token);
    }
    if (layoutCompatible) for (const auto& link : layout.oneStrokeLinks) {
        int previous = -1, next = -1;
        for (int id = 0; id < tracks.size(); ++id) if (!tracks.at(id).wifi) {
            if (tracks.at(id).parseOrder == link.first) previous = id;
            if (tracks.at(id).parseOrder == link.second) next = id;
        }
        if (previous >= 0 && next >= 0) tracks[next].oneStrokePrevious = previous;
    }
    for (int first = 0; first < primitives.size(); ++first) {
        const auto& a = primitives.at(first);
        if (a.marker.type != QStringLiteral("tap") || a.marker.eachGroupId < 0) continue;
        for (int second = first + 1; second < primitives.size(); ++second) {
            const auto& b = primitives.at(second);
            if (b.marker.type != QStringLiteral("tap")
                || b.marker.eachGroupId != a.marker.eachGroupId
                || std::abs(a.action.start - b.action.start) > epsilon) continue;
            const double xA = lanePoint(a.lane).x(), xB = lanePoint(b.lane).x();
            if (!((xA < -0.25 && xB > 0.25) || (xB < -0.25 && xA > 0.25))) continue;
            primitives[first].crossPeer = second;
            primitives[second].crossPeer = first;
        }
    }
    // Correct only an unmistakably reversed, simple label in a selected
    // opposite-side Each. Other existing annotations remain hard locks.
    for (int id = 0; id < primitives.size(); ++id) {
        auto& primitive = primitives[id];
        if (primitive.crossPeer < 0 || !tokens.at(primitive.token).target
            || !tokens.at(primitives.at(primitive.crossPeer).token).target) continue;
        auto& token = tokens[primitive.token];
        if (token.suffix != QStringLiteral("~l") && token.suffix != QStringLiteral("~r")) continue;
        const double x = lanePoint(primitive.lane).x();
        if (!((x < -0.25 && primitive.lock == 1)
            || (x > 0.25 && primitive.lock == 0))) continue;
        token.protectedText = false;
        primitive.lock = -1;
        primitive.marker.handOrderHead = HandOrderHand::Unspecified;
        for (auto& marker : token.markers) marker.handOrderHead = HandOrderHand::Unspecified;
    }
    // Four or more regular neighbouring-lane ABAB taps are normally played by
    // alternating hands. Keep this as a cost so occupied hands can still win.
    QVector<int> tapOrder;
    for (int id = 0; id < primitives.size(); ++id)
        if (primitives.at(id).marker.type == QStringLiteral("tap")) tapOrder.append(id);
    std::stable_sort(tapOrder.begin(), tapOrder.end(), [&](int a, int b) {
        return primitives.at(a).action.start < primitives.at(b).action.start;
    });
    const auto regular = [&](int a, int b) {
        const auto& first = primitives.at(a).marker;
        const auto& second = primitives.at(b).marker;
        if (first.subdivision < 16 || second.subdivision < 16 || first.tempoBpm <= 0) return false;
        const double grid = 240.0 / (first.tempoBpm * first.subdivision);
        const double gap = second.second - first.second;
        return gap >= grid * 0.9 - epsilon && gap <= grid * 1.1 + epsilon;
    };
    const auto adjacentStep = [&](int a, int b) {
        const int first = primitives.at(a).lane, second = primitives.at(b).lane;
        if (second == first % 8 + 1) return 1;
        if (second == (first + 6) % 8 + 1) return -1;
        return 0;
    };
    for (int first = 0; first + 3 < tapOrder.size();) {
        const int a = tapOrder.at(first), b = tapOrder.at(first + 1);
        const int delta = std::abs(primitives.at(a).lane - primitives.at(b).lane);
        if (delta != 1 && delta != 7) { ++first; continue; }
        int last = first + 2;
        while (last < tapOrder.size()) {
            const int previous = tapOrder.at(last - 1), current = tapOrder.at(last);
            const int expected = primitives.at(tapOrder.at(first + (last - first) % 2)).lane;
            if (primitives.at(current).lane != expected || !regular(previous, current)) break;
            ++last;
        }
        if (last - first >= 4 && regular(a, b)) {
            for (int i = first + 1; i < last; ++i)
                primitives[tapOrder.at(i)].interactionPrevious = tapOrder.at(i - 1);
            first = last;
        } else ++first;
    }
    // A-B-A-C-A uses the centre hand on beats 1/3/5 and the other hand
    // on the two opposite neighbours. This remains a soft preference.
    for (int first = 0; first + 4 < tapOrder.size(); ++first) {
        const int centre = tapOrder.at(first);
        if (primitives.at(tapOrder.at(first + 2)).lane != primitives.at(centre).lane
            || primitives.at(tapOrder.at(first + 4)).lane != primitives.at(centre).lane
            || adjacentStep(centre, tapOrder.at(first + 1)) == 0
            || adjacentStep(centre, tapOrder.at(first + 1))
                != -adjacentStep(centre, tapOrder.at(first + 3))) continue;
        bool regularRun = true;
        for (int i = first + 1; i <= first + 4; ++i)
            regularRun &= regular(tapOrder.at(i - 1), tapOrder.at(i));
        if (!regularRun) continue;
        for (int i = first + 1; i <= first + 4; ++i)
            primitives[tapOrder.at(i)].interactionPrevious = tapOrder.at(i - 1);
    }
    // Three or more rapid identical keys need alternating DOWN actions.
    // Two-key pairs remain available to the winding and rebound patterns.
    for (int first = 0; first < tapOrder.size();) {
        int last = first + 1;
        while (last < tapOrder.size()
            && primitives.at(tapOrder.at(last)).lane == primitives.at(tapOrder.at(first)).lane
            && regular(tapOrder.at(last - 1), tapOrder.at(last))) ++last;
        if (last - first >= 3)
            for (int i = first + 1; i < last; ++i)
                primitives[tapOrder.at(i)].longitudinalPrevious = tapOrder.at(i - 1);
        first = last;
    }
    // A rapid monotonic key sweep keeps one hand through the centre and the
    // 8/1 seam; charging each handoff avoids a last-moment return jump.
    for (int first = 0; first + 3 < tapOrder.size();) {
        const int direction = adjacentStep(tapOrder.at(first), tapOrder.at(first + 1));
        int last = first + 1;
        while (direction && last < tapOrder.size() && last - first < 8) {
            const auto& previous = primitives.at(tapOrder.at(last - 1)).marker;
            const auto& current = primitives.at(tapOrder.at(last)).marker;
            if (previous.subdivision < 24 || current.subdivision < 24
                || adjacentStep(tapOrder.at(last - 1), tapOrder.at(last)) != direction
                || !regular(tapOrder.at(last - 1), tapOrder.at(last))) break;
            ++last;
        }
        // Four adjacent 24th-note keys already form one wrist sweep. Keep its
        // continuity in the search, not only in the effort discount; otherwise
        // a short release gap can make a zigzag hand assignment look cheaper.
        if (last - first >= 4) {
            for (int i = first + 1; i < last; ++i)
                primitives[tapOrder.at(i)].sweepPrevious = tapOrder.at(i - 1);
            first = last;
        } else ++first;
    }
    QVector<int> touchOrder;
    for (int id = 0; id < touches.size(); ++id)
        if (!touches.at(id).hold) touchOrder.append(id);
    std::stable_sort(touchOrder.begin(), touchOrder.end(), [&](int a, int b) {
        return touches.at(a).second < touches.at(b).second;
    });
    const auto adjacentE = [&](int a, int b) {
        const auto& first = touches.at(a);
        const auto& second = touches.at(b);
        if (first.pad.size() != 2 || second.pad.size() != 2
            || first.pad.at(0) != QLatin1Char('E') || second.pad.at(0) != QLatin1Char('E')
            || first.tempoBpm <= 0 || first.subdivision <= 0) return false;
        const int laneA = first.pad.at(1).digitValue(), laneB = second.pad.at(1).digitValue();
        const int distance = std::abs(laneA - laneB);
        if (qMin(distance, 8 - distance) != 1) return false;
        const double grid = 240.0 / (first.tempoBpm * first.subdivision);
        const double gap = second.second - first.second;
        return gap >= grid * 0.9 - epsilon && gap <= qMin(grid * 1.1, 0.18) + epsilon;
    };
    for (int first = 0; first + 2 < touchOrder.size();) {
        int last = first + 1;
        while (last < touchOrder.size()
            && adjacentE(touchOrder.at(last - 1), touchOrder.at(last))) ++last;
        if (last - first >= 3) {
            for (int i = first + 1; i < last; ++i)
                touches[touchOrder.at(i)].sweepPrevious = touchOrder.at(i - 1);
            first = last;
        } else ++first;
    }
    for (auto& track : tracks) {
        const double duration = track.action.end - track.action.start; if (duration <= 0) continue;
        track.boundaries = {0.25, 0.5, 0.75};
        for (const auto& primitive : primitives) for (double second : {primitive.action.start - handoffOverlap, primitive.action.end + handoffOverlap}) {
            const double progress = (second - track.action.start) / duration;
            if (progress > 0.02 && progress < 0.98) track.boundaries.append(progress);
        }
        std::sort(track.boundaries.begin(), track.boundaries.end());
        track.boundaries.erase(std::unique(track.boundaries.begin(), track.boundaries.end(), [&](double a, double b) { return (b - a) * duration < 0.025; }), track.boundaries.end());
        if (track.boundaries.size() > 8) {
            QVector<double> bounded;
            for (int i = 0; i < 8; ++i) bounded.append(track.boundaries.at(i * (track.boundaries.size() - 1) / 7));
            track.boundaries = bounded;
        }
    }
    // Ordinary proximity is a non-transitive lookaround, never a component edge.
    for (auto& token : tokens) if (token.issue.isEmpty()) for (const auto& risk : risks)
        if (token.beginTime <= risk.last && token.endTime >= risk.first) { token.issue = "unsupported_context"; break; }
    const auto sustained = [&](int id) {
        if (!tokens.at(id).tracks.isEmpty()) return true;
        for (int primitive : tokens.at(id).primitives) if (primitives.at(primitive).marker.type == "hold"
            || primitives.at(primitive).marker.type == "touch_hold") return true;
        return false;
    };
    QVector<bool> included(tokens.size(), false);
    const double softHorizon = qMax(0.35, p.contextSeconds);
    for (int i = 0; i < tokens.size(); ++i) if (tokens.at(i).target || tokens.at(i).partial) {
        for (int j = qMax(0, i - 1); j <= qMin(int(tokens.size()) - 1, i + 2); ++j)
            if (tokens.at(j).issue.isEmpty() && std::abs(tokens.at(j).nominal - tokens.at(i).nominal) <= softHorizon) included[j] = true;
        // Import an ongoing input/queue directly affecting this target, without
        // recursively importing every object whose broad time window overlaps it.
        for (int j = 0; j < tokens.size(); ++j) if (tokens.at(j).issue.isEmpty() && sustained(j)
            && tokens.at(j).beginTime <= tokens.at(i).endTime && tokens.at(j).endTime >= tokens.at(i).beginTime)
            included[j] = true;
    }
    // A visible sustained assumption must respect every explicit lock it spans.
    // Such locks do not grow a new soft neighbourhood of their own.
    const auto imported = included;
    for (int i = 0; i < tokens.size(); ++i) if (imported.at(i) && sustained(i))
        for (int j = 0; j < tokens.size(); ++j) if (tokens.at(j).issue.isEmpty() && tokens.at(j).protectedText
            && tokens.at(j).nominal <= tokens.at(i).endTime && tokens.at(j).endTime >= tokens.at(i).beginTime)
            included[j] = true;
    const auto groups = buildHandOrderTouchGroups(touches);
    for (const auto& group : groups) {
        bool any = false;
        for (int id : group) for (int i = 0; i < tokens.size(); ++i)
            if (tokens.at(i).touches.contains(id)) any |= included.at(i);
        if (any) for (int id : group) for (int i = 0; i < tokens.size(); ++i)
            if (tokens.at(i).touches.contains(id) && tokens.at(i).issue.isEmpty()) included[i] = true;
    }
    // Complete every nominally simultaneous set, including ordinary slash
    // tokens outside the one-before/two-after soft-neighbour count.
    for (int i = 0; i < tokens.size(); ++i) if (included.at(i))
        for (int j = 0; j < tokens.size(); ++j) if (tokens.at(j).issue.isEmpty()
            && std::abs(tokens.at(i).nominal - tokens.at(j).nominal) < epsilon) included[j] = true;
    // A later DOWN is not free to ignore an earlier waiting head on the same
    // channel. Import those real consumers as read-only hard context, then
    // execute them through the same bounded rolling windows (never one beam).
    QVector<QSet<QString>> emitted(tokens.size());
    QMap<QString, QVector<int>> consumers;
    for (int id = 0; id < tokens.size(); ++id) for (const auto& marker : tokens.at(id).markers) {
        const bool touch = marker.type == "touch" || marker.type == "touch_hold";
        const bool head = marker.type == "tap" || marker.type == "hold"
            || ((marker.type == "slide" || marker.type == "wifi") && marker.hasHeadStar);
        if (!touch && !head) continue;
        const QString pad = touch ? marker.touchPad : QStringLiteral("A%1").arg(marker.lane);
        consumers[pad].append(id);
        if (head) {
            const QString button = QStringLiteral("Button%1").arg(marker.lane);
            consumers[button].append(id); emitted[id].insert(button);
        }
        const auto point = touchPadPoint(pad);
        for (const auto& ring : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("D"), QStringLiteral("E")})
            for (int lane = 1; lane <= (ring == "C" ? 1 : 8); ++lane) {
                const QString reached = ring == "C" ? ring : ring + QString::number(lane);
                if (QLineF(point, touchPadPoint(reached)).length() <= TouchJudgeProfile::handRadius + muri::padRadius(reached) / referenceRadius + epsilon)
                    emitted[id].insert(reached);
            }
    }
    QVector<int> queue;
    for (int id = 0; id < tokens.size(); ++id) if (included.at(id)) queue.append(id);
    const auto includeConsumer = [&](int id) {
        if (included.at(id) || !tokens.at(id).issue.isEmpty()) return;
        included[id] = true; queue.append(id);
    };
    for (int at = 0; at < queue.size(); ++at) {
        task::CancellationScope::checkpoint(); const int id = queue.at(at);
        for (int other = 0; other < tokens.size(); ++other)
            if (std::abs(tokens.at(other).nominal - tokens.at(id).nominal) < epsilon) includeConsumer(other);
        for (const auto& group : groups) {
            bool member = false;
            for (int touch : group) member |= tokens.at(id).touches.contains(touch);
            if (member) for (int touch : group) for (int other = 0; other < tokens.size(); ++other)
                if (tokens.at(other).touches.contains(touch)) includeConsumer(other);
        }
        for (int touch : tokens.at(id).touches) {
            if (touches.at(touch).sweepPrevious >= 0)
                for (int other = 0; other < tokens.size(); ++other)
                    if (tokens.at(other).touches.contains(touches.at(touch).sweepPrevious))
                        includeConsumer(other);
            for (int next = 0; next < touches.size(); ++next)
                if (touches.at(next).sweepPrevious == touch)
                    for (int other = 0; other < tokens.size(); ++other)
                        if (tokens.at(other).touches.contains(next)) includeConsumer(other);
        }
        if (p.maxTasks >= 16) for (int primitive : tokens.at(id).primitives) {
            const int previous = primitives.at(primitive).sweepPrevious;
            if (previous >= 0) includeConsumer(primitives.at(previous).token);
            for (int next = 0; next < primitives.size(); ++next)
                if (primitives.at(next).sweepPrevious == primitive)
                    includeConsumer(primitives.at(next).token);
        }
        const double early = tokens.at(id).touches.isEmpty() ? FullReplayProfile::tapCriticalSeconds : TouchJudgeProfile::criticalSeconds;
        for (const auto& channel : emitted.at(id)) for (int other : consumers.value(channel))
            if (tokens.at(other).nominal < tokens.at(id).nominal - epsilon
                && tokens.at(other).nominal + FullReplayProfile::tapAvailableSeconds >= tokens.at(id).nominal - early - epsilon) includeConsumer(other);
    }
    for (bool value : included) result.includedObjects += value;
    QVector<Decision> decisions;
    for (int i = 0; i < primitives.size(); ++i) if (included.at(primitives.at(i).token)) decisions.append({Decision::Fixed, i, {}, primitives.at(i).action.start, primitives.at(i).action.end, primitives.at(i).lane});
    for (int i = 0; i < tracks.size(); ++i) if (included.at(tracks.at(i).token) && tracks.at(i).sector != 1) decisions.append({Decision::Slide, i, {}, tracks.at(i).action.start,
        tracks.at(i).action.end + FullReplayProfile::slideAvailableSeconds, 0});
    for (const auto& group : groups) {
        QVector<int> members; double last = -1e9;
        for (int id : group) for (int i = 0; i < tokens.size(); ++i) if (included.at(i) && tokens.at(i).touches.contains(id)) {
            members.append(id); last = qMax(last, touches.at(id).second + TouchJudgeProfile::availableSeconds);
        }
        if (!members.isEmpty()) decisions.append({Decision::Touches, -1, members, last, last, 0});
    }
    std::stable_sort(decisions.begin(), decisions.end(), [](const auto& a, const auto& b) { return std::tie(a.second, a.kind, a.lane, a.end) < std::tie(b.second, b.kind, b.lane, b.end); });
    const qint64 auditReserve = qMin(p.maxWorkUnits / 10, qMax(qint64(1000), qint64(parsed.noteMarkers.size()) * 3000));
    qint64 work = p.maxWorkUnits - auditReserve;
    QVector<int> assigned(primitives.size(), -1); QVector<QVector<HandOrderTrackStop>> planned(tracks.size());
    QVector<bool> plannedWifiSingle(tracks.size(), false);
    QVector<FullReplayJudgement> fullJudged(parsed.noteMarkers.size());
    QVector<TouchJudgement> judged(touches.size());
    QVector<double> costs(tokens.size(), 0), peaks(tokens.size(), 0), gaps(tokens.size(), 0);
    QVector<double> playerCosts(tokens.size(), 0), tapRates(tokens.size(), 0), loads(tokens.size(), 0);
    QVector<int> tierGaps(tokens.size(), 0); QVector<bool> counter(tokens.size(), false), participated(tokens.size(), false);
    QVector<HandOrderHand> counterHands(tokens.size(), HandOrderHand::Unspecified);
    QVector<int> counterLanes(tokens.size(), 0);
    QVector<BasicHandoffEvidence> transferEvidence(tracks.size());
    QVector<QVector<BasicCounterfactualEvidence>> confidenceEvidence(tokens.size());
    QVector<int> parent(tokens.size());
    for (int i = 0; i < parent.size(); ++i) parent[i] = i;
    const auto root = [&](int id) { while (parent.at(id) != id) id = parent.at(id); return id; };
    const auto join = [&](int a, int b) { parent[root(b)] = root(a); };
    for (int i = 0; i < tokens.size(); ++i) if (included.at(i))
        for (int j = i + 1; j < tokens.size(); ++j) if (included.at(j)
            && std::abs(tokens.at(i).nominal - tokens.at(j).nominal) < epsilon) join(i, j);
    for (const auto& group : groups) {
        int first = -1;
        for (int touch : group) for (int id = 0; id < tokens.size(); ++id)
            if (included.at(id) && tokens.at(id).touches.contains(touch)) {
                if (first >= 0) join(first, id); else first = id;
            }
    }
    for (int touch = 0; touch < touches.size(); ++touch) {
        const int previous = touches.at(touch).sweepPrevious;
        if (previous < 0) continue;
        for (int a = 0; a < tokens.size(); ++a) if (included.at(a) && tokens.at(a).touches.contains(previous))
            for (int b = 0; b < tokens.size(); ++b) if (included.at(b) && tokens.at(b).touches.contains(touch))
                join(a, b);
    }
    if (p.maxTasks >= 16) for (int primitive = 0; primitive < primitives.size(); ++primitive) {
        const int previous = primitives.at(primitive).sweepPrevious;
        if (previous >= 0 && included.at(primitives.at(previous).token)
            && included.at(primitives.at(primitive).token))
            join(primitives.at(previous).token, primitives.at(primitive).token);
    }
    QMap<int, QSet<int>> hardGroups;
    for (int i = 0; i < tokens.size(); ++i) if (included.at(i)) hardGroups[root(i)].insert(i);
    QVector<QSet<int>> cores;
    for (const auto& group : hardGroups) cores.append(group);
    std::stable_sort(cores.begin(), cores.end(), [&](const auto& a, const auto& b) {
        double firstA = std::numeric_limits<double>::infinity(), firstB = firstA;
        for (int id : a) firstA = qMin(firstA, tokens.at(id).nominal);
        for (int id : b) firstB = qMin(firstB, tokens.at(id).nominal);
        return firstA < firstB;
    });
    State committed; committed.inputPreference = p.inputPreference; committed.returnSpeed = p.returnSpeed;
    committed.player = p.player;
    setPlayerActionKinds(committed, primitives, touches);
    committed.primitiveHands.fill(-1, primitives.size()); committed.tracks.resize(tracks.size()); committed.wifiSingle.fill(false, tracks.size());
    QSet<int> committedTokens;
    QVector<TimelineNoteMarker> committedMarkers;
    double historyThrough = -std::numeric_limits<double>::infinity();
    std::array<PlayerPathState, 2> historyEnds;
    QVector<Scheduled> pendingHistory;
    int historyActionCount = 0;
    // Optional evidence must not consume the inexpensive ordinary main route
    // that can already fit the request. Keep a conservative work floor for
    // the remaining rolling solve and its eventual combined audit.
    qint64 optionalRemaining = qMin(p.maxWorkUnits / 3,
        qMax(qint64(0), p.maxWorkUnits - qint64(result.includedObjects) * 1000));
    int counterfactualRemaining = p.counterfactualBudget;
    int dependencyUnit = 0;
    for (int coreIndex = 0; coreIndex < cores.size(); ++coreIndex) {
        auto core = cores.at(coreIndex);
        for (int id : committedTokens) core.remove(id);
        bool relevant = false;
        for (int id : core) relevant |= tokens.at(id).issue.isEmpty() && !committedTokens.contains(id);
        if (!relevant) continue;
        ++dependencyUnit; ++result.windowCount;
        task::CancellationScope::check();
        QSet<int> windowTokens = core;
        bool ordinaryCore = true;
        for (int id : core) ordinaryCore &= !sustained(id) && tokens.at(id).touches.isEmpty();
        for (int next = coreIndex + 1; next <= qMin(coreIndex + 2, int(cores.size()) - 1); ++next) {
            bool ordinaryNext = true;
            for (int id : cores.at(next)) ordinaryNext &= !sustained(id) && tokens.at(id).touches.isEmpty();
            bool immediateJudgementCoupling = false;
            for (int id : core) for (int other : cores.at(next))
                immediateJudgementCoupling |= tokens.at(other).beginTime <= tokens.at(id).nominal + p.pressSeconds
                    && tokens.at(other).nominal >= tokens.at(id).nominal;
            if ((!ordinaryCore || ordinaryNext || immediateJudgementCoupling)
                && tokens.at(*cores.at(next).cbegin()).nominal - tokens.at(*core.cbegin()).nominal <= softHorizon)
                windowTokens.unite(cores.at(next));
        }
        double horizon = std::numeric_limits<double>::infinity(), last = -horizon;
        for (int id : core) { horizon = qMin(horizon, tokens.at(id).beginTime); last = qMax(last, tokens.at(id).endTime); }
        for (int id = 0; id < tokens.size(); ++id) if (included.at(id) && !committedTokens.contains(id)
            && tokens.at(id).protectedText && tokens.at(id).beginTime <= last && tokens.at(id).endTime >= horizon)
            windowTokens.insert(id);
        for (int id : windowTokens) horizon = qMin(horizon, tokens.at(id).beginTime);
        State seed = committed; seed.actions.clear(); seed.physical.clear(); seed.fullReplay = {}; seed.replay = {};
        seed.cost = seed.peak = seed.risk = 0; seed.origins = {};
        seed.scoreFrom = horizon; seed.focusSources.clear();
        seed.focusSecond = -std::numeric_limits<double>::infinity(); seed.focusOrdinal = 0;
        for (int id : core) {
            seed.focusSources.insert(tokens.at(id).start);
            seed.focusSecond = qMax(seed.focusSecond, tokens.at(id).nominal); seed.focusOrdinal = qMax(seed.focusOrdinal, id);
        }
        QSet<int> retainedSources;
        for (int id : committedTokens) if (tokens.at(id).endTime >= horizon - epsilon) {
            retainedSources.insert(tokens.at(id).start);
            for (const auto& marker : tokens.at(id).markers) {
                const auto& full = fullJudged.value(marker.parseOrder);
                auto causes = full.causes; causes += full.headCauses;
                for (const auto& cause : causes) retainedSources.insert(cause.tokenStart);
            }
        }
        for (const auto& action : committed.actions) if (action.action.end >= horizon - epsilon || retainedSources.contains(action.action.tokenStart)) {
            seed.actions.append(action);
        }
        for (const auto& action : committed.actions) if (action.action.end < horizon - epsilon) {
            auto& origin = seed.origins[action.action.hand];
            if (!origin.known || action.action.end > origin.free) origin = {action.action.end,
                contactPosition(action.action, action.action.end), action.lane, true, action.ordinal, action.action.start};
        }
        QSet<int> blockTokens = windowTokens, touchIds;
        for (int id : committedTokens) if (tokens.at(id).endTime >= horizon - epsilon) blockTokens.insert(id);
        for (const auto& action : seed.actions) for (int id : committedTokens)
            if (tokens.at(id).start == action.action.tokenStart) blockTokens.insert(id);
        QVector<Decision> block;
        const auto decisionInWindow = [&](const Decision& d) {
            if (d.kind == Decision::Fixed) return windowTokens.contains(primitives.at(d.index).token)
                && !committedTokens.contains(primitives.at(d.index).token);
            if (d.kind == Decision::Slide) return windowTokens.contains(tracks.at(d.index).token)
                && !committedTokens.contains(tracks.at(d.index).token);
            for (int touch : d.members) for (int id : windowTokens)
                if (!committedTokens.contains(id) && tokens.at(id).touches.contains(touch)) return true;
            return false;
        };
        for (const auto& decision : decisions) if (decisionInWindow(decision)) block.append(decision);
        for (int id : blockTokens) for (int touch : tokens.at(id).touches) touchIds.insert(touch);
        for (int id : core) tokens[id].dependencyUnit = dependencyUnit;
        QVector<TouchTarget> localTouches; QMap<int, int> localIndex;
        QList<int> orderedIds = touchIds.values(); std::sort(orderedIds.begin(), orderedIds.end());
        for (int id : orderedIds) {
            localIndex[id] = localTouches.size(); auto target = touches.at(id);
            for (int owner : committedTokens) if (tokens.at(owner).touches.contains(id) && !target.hold)
                target.lock = maskHand(judged.at(id).handMask);
            localTouches.append(target);
        }
        for (auto& d : block) if (d.kind == Decision::Touches) for (int& id : d.members) id = localIndex.value(id);
        for (int id : blockTokens) for (int touch : tokens.at(id).touches) if (touches.at(touch).hold && !localIndex.contains(touch)) {
            localIndex[touch] = localTouches.size(); localTouches.append(touches.at(touch));
        }
        for (auto& touch : localTouches)
            if (touch.sweepPrevious >= 0)
                touch.sweepPrevious = localIndex.value(touch.sweepPrevious, -1);
        QVector<TimelineNoteMarker> blockMarkers;
        for (int id : blockTokens) blockMarkers += tokens.at(id).markers;
        QString failure;
        const int tasks = block.size() + localTouches.size() + seed.actions.size();
        result.maxWindowTasks = qMax(result.maxWindowTasks, tasks);
        if (tasks > p.maxTasks) { failure = "task_limit"; result.taskLimited = true; }
        const qint64 beforeMain = qMax(qint64(0), work);
        const int remainingWindows = qMax(1, int(cores.size()) - coreIndex);
        const qint64 mainGrant = qMin(beforeMain, remainingWindows <= 16 ? beforeMain
            : qMax(qint64(12000), beforeMain * 4 / remainingWindows));
        qint64 mainReserved = beforeMain - mainGrant;
        work = mainGrant;
        State history = committed;
        // Only accepted physical history contributes to the next entry state;
        // the candidate's uncommitted lookahead must never accumulate fatigue.
        bool reuseHistory = std::isfinite(historyThrough) && horizon >= historyThrough - epsilon;
        for (int i = historyActionCount; reuseHistory && i < committed.actions.size(); ++i)
            reuseHistory &= committed.actions.at(i).action.start >= historyThrough - epsilon;
        if (!reuseHistory) {
            pendingHistory = committed.actions;
            historyEnds = {};
            historyThrough = -std::numeric_limits<double>::infinity();
        } else {
            for (int i = historyActionCount; i < committed.actions.size(); ++i)
                pendingHistory.append(committed.actions.at(i));
        }
        history.physical = pendingHistory;
        history.scoreFrom = historyThrough;
        history.effortOrigins = historyEnds;
        const bool historyReady = scorePlayerEffort(history, work, horizon);
        if (!historyReady && failure.isEmpty())
            failure = work < 0 ? QStringLiteral("work_limit") : QStringLiteral("player_history_invalid");
        seed.effortOrigins = history.effortEnds;
        if (historyReady) {
            historyThrough = horizon;
            historyEnds = history.effortEnds;
            historyActionCount = committed.actions.size();
            pendingHistory.erase(std::remove_if(pendingHistory.begin(), pendingHistory.end(), [&](const auto& action) {
                return action.action.end <= horizon + epsilon;
            }), pendingHistory.end());
        } else {
            historyThrough = -std::numeric_limits<double>::infinity();
        }
        bool carryResponsibility = false;
        for (const auto& decision : block) carryResponsibility |= decision.kind == Decision::Touches && decision.members.size() >= 3;
        const auto runSearch = [&](const QVector<int>& limits, const SearchConstraint& constraint = SearchConstraint{}, bool evidence = false) {
            Search combined;
            auto narrow = p;
            if (!evidence && !carryResponsibility) narrow.beamWidth = qMin(8, p.beamWidth);
            for (int pass = 0; pass < (narrow.beamWidth < p.beamWidth ? 2 : 1); ++pass) {
            for (int breadth = 0; breadth < 3; ++breadth) {
                // Try alternative physical channels before spending the whole
                // window on a wider beam of the same impossible channel.
                auto attempt = search(block, primitives, tracks, localTouches, blockMarkers,
                    pass == 0 ? narrow : p, limits, work, constraint, &seed, breadth);
                attempt.sideEffect |= combined.sideEffect;
                combined = std::move(attempt);
                if (combined.found || combined.exhausted) return combined;
            }
            }
            return combined;
        };
        QVector<int> caps(tracks.size(), 0);
        auto best = failure.isEmpty() ? runSearch(caps) : Search{};
        if (failure.isEmpty() && !best.found && !best.exhausted) {
            caps.fill(1); best = runSearch(caps);
        }
        if (failure.isEmpty() && !best.found && !best.exhausted) {
            caps.fill(3); best = runSearch(caps);
        }
        if (failure.isEmpty() && !best.found && best.exhausted) {
            const qint64 futureFloor = qint64(qMax(0, remainingWindows - 1)) * 1000;
            const qint64 extra = qMin(qMax(qint64(0), mainReserved - futureFloor),
                qMax(qint64(240000), mainGrant * 4));
            if (extra > mainGrant) {
                mainReserved -= extra; work = extra;
                best = runSearch(caps);
                if (!best.found && !best.exhausted) {
                    caps.fill(1); best = runSearch(caps);
                }
            }
        }
        bool pressureLimited = false, hasTrack = false;
        for (const auto& decision : block) hasTrack |= decision.kind == Decision::Slide && tracks.at(decision.index).locks.isEmpty()
            && !tracks.at(decision.index).legacyAnnotated;
        if (best.found && best.state.focusPeak >= 12.0 && hasTrack && caps.value(0) == 0 && work > 0) {
            auto expanded = caps; expanded.fill(1);
            const qint64 grant = qMin(work, qMax(qint64(1000), mainGrant / 4));
            const qint64 reserved = work - grant; work = grant;
            auto improved = runSearch(expanded);
            work = qMax(qint64(0), work) + reserved;
            pressureLimited = improved.exhausted;
            result.truncated |= improved.truncated || improved.exhausted;
            if (improved.found && better(improved.state, best.state)) { best = std::move(improved); caps = expanded; }
        }
        result.truncated |= best.truncated;
        const auto admitTransfers = [&](Search candidate, const SearchConstraint& required = SearchConstraint{}, const QVector<int>* candidateCaps = nullptr) {
            auto localCaps = candidateCaps ? *candidateCaps : caps;
            bool retry = candidate.found;
            while (retry) {
                retry = false;
                candidate.state.transferEvidence.fill({}, tracks.size());
                QVector<int> trackIds;
                for (const auto& d : block) if (d.kind == Decision::Slide) {
                    trackIds.append(d.index);
                    if (tracks.at(d.index).wifi && !candidate.state.wifiSingle.at(d.index)) trackIds.append(tracks.at(d.index).peer);
                }
                for (int id : trackIds) {
                    auto& proof = candidate.state.transferEvidence[id];
                    proof.branch = tracks.at(id).branch; proof.sector = tracks.at(id).sector;
                    proof.handoffCount = switches(candidate.state.tracks.at(id));
                    if (proof.handoffCount > 0) proof.reason = tracks.at(id).locks.isEmpty()
                        ? QStringLiteral("single_handoff_occupation") : QStringLiteral("existing_handoff_constraint");
                }
                for (int id : trackIds) if (tracks.at(id).locks.isEmpty() && switches(candidate.state.tracks.at(id)) > 1) {
                    auto limited = localCaps; limited[id] = 1;
                    const auto baseline = runSearch(limited, required, true);
                    result.truncated |= baseline.truncated;
                    if (baseline.exhausted) { candidate.exhausted = true; candidate.found = false; return candidate; }
                    const double costGain = baseline.found ? baseline.state.cost - candidate.state.cost : 0;
                    const double peakGain = baseline.found ? baseline.state.peak - candidate.state.peak : 0;
                    const bool clearPeak = baseline.found && peakGain >= 4.0 && peakGain >= baseline.state.peak * 0.2;
                    const bool clearCost = baseline.found && costGain >= 2.0 && costGain >= baseline.state.cost * 0.2 && candidate.state.peak <= baseline.state.peak + epsilon;
                    const bool resolves = !baseline.found && !baseline.truncated;
                    auto& proof = candidate.state.transferEvidence[id];
                    proof.baselineFound = baseline.found;
                    proof.baselineCost = baseline.found ? baseline.state.cost : 0; proof.baselinePeak = baseline.found ? baseline.state.peak : 0;
                    if (baseline.found && better(baseline.state, candidate.state)) {
                        candidate = baseline; retry = true; break;
                    }
                    if (resolves || clearPeak || clearCost) proof.reason = resolves ? "one_handoff_candidate_not_found" : "significant_context_improvement";
                    else {
                        localCaps[id] = 1; candidate = baseline; retry = candidate.found; break;
                    }
                }
            }
            return candidate;
        };
        best = admitTransfers(std::move(best));
        if (failure.isEmpty() && (best.exhausted || work < 0)) {
            result.workLimited = true;
            if (!best.found) failure = mainReserved > 0 ? "window_work_limit" : "work_limit";
            else { pressureLimited = true; result.evidenceTruncated = result.truncated = true; }
        }
        work = qMax(qint64(0), work) + mainReserved;
        if (failure.isEmpty() && !best.found) failure = best.sideEffect ? "full_replay_target_failed" : "candidate_search_failed";
        if (failure.isEmpty()) {
            const qint64 futureMainFloor = qint64(qMax(0, remainingWindows - 1)) * 1000;
            const qint64 evidenceGrant = qMin(qMin(qMax(qint64(0), work - futureMainFloor), optionalRemaining),
                qMax(qint64(1000), mainGrant / 2));
            const qint64 reserved = work - evidenceGrant;
            work -= reserved;
            struct Component { int token = -1; QString name; SearchConstraint opposite; };
            const auto componentsFor = [&](const State& state) {
                QVector<Component> components;
                for (const auto& d : block) {
                    if (d.kind == Decision::Fixed) {
                        const auto& primitive = primitives.at(d.index); const auto& token = tokens.at(primitive.token);
                        if (!core.contains(primitive.token) || !token.target || token.protectedText || primitive.lock >= 0) continue;
                        const QString name = token.slide ? QStringLiteral("head") : QStringLiteral("note%1").arg(primitive.lane);
                        components.append({primitive.token, name, {d.index, -1, -1, 1 - state.primitiveHands.at(d.index), 0}});
                    } else if (d.kind == Decision::Slide) {
                        QVector<int> ids{d.index}; const auto& track = tracks.at(d.index);
                        if (track.wifi && !state.wifiSingle.at(d.index)) ids.append(track.peer);
                        for (int id : ids) {
                            const auto& path = tracks.at(id); const auto& token = tokens.at(path.token);
                            if (!core.contains(path.token) || !token.target || token.protectedText || !path.locks.isEmpty()) continue;
                            const auto& stops = state.tracks.at(id);
                            for (int i = 0; i < stops.size(); ++i) {
                                const double last = i + 1 < stops.size() ? stops.at(i + 1).progress : 1.0;
                                const double progress = (stops.at(i).progress + last) / 2;
                                const QString name = QStringLiteral("track:%1:sector:%2").arg(path.branch).arg(path.sector);
                                components.append({path.token, name, {-1, id, -1, 1 - fixedHand(stops.at(i).hand), progress}});
                            }
                        }
                    } else for (int id : d.members) {
                        const auto& touch = localTouches.at(id); if (touch.hold || touch.lock >= 0) continue;
                        int tokenId = -1;
                        for (int token : blockTokens) if (tokens.at(token).start == touch.tokenStart) tokenId = token;
                        if (tokenId < 0 || !core.contains(tokenId) || !tokens.at(tokenId).target || tokens.at(tokenId).protectedText) continue;
                        const int hand = maskHand(state.replay.judgements.at(id).handMask);
                        components.append({tokenId, QStringLiteral("touch"), {-1, -1, id, hand < 0 ? -1 : 1 - hand, 0}});
                    }
                }
                return components;
            };
            bool evidenceLimited = pressureLimited, settled = false;
            for (int pass = 0; pass < 3 && !settled && !evidenceLimited; ++pass) {
                for (int token : core) { confidenceEvidence[token].clear(); counter[token] = false; }
                const auto components = componentsFor(best.state);
                bool changed = false; int count = 0;
                for (const auto& component : components) {
                    BasicCounterfactualEvidence proof; proof.component = component.name;
                    proof.progress = component.opposite.progress; proof.alternativeHand = namedHand(component.opposite.hand);
                    if (component.opposite.hand < 0 || counterfactualRemaining <= 0 || work <= 0) {
                        proof.limited = true; evidenceLimited |= component.opposite.hand >= 0;
                        confidenceEvidence[component.token].append(proof); continue;
                    }
                    ++count; --counterfactualRemaining; proof.searched = true;
                    auto alternativeCaps = caps;
                    for (int& limit : alternativeCaps) limit = qMax(1, limit);
                    auto alternative = runSearch(alternativeCaps, component.opposite, true);
                    if (!alternative.found && !alternative.exhausted) {
                        alternativeCaps.fill(3); alternative = runSearch(alternativeCaps, component.opposite, true);
                    }
                    if (alternative.found) alternative = admitTransfers(std::move(alternative), component.opposite, &alternativeCaps);
                    result.truncated |= alternative.truncated;
                    proof.limited = best.truncated || alternative.truncated || alternative.exhausted;
                    if (alternative.exhausted) evidenceLimited = true;
                    proof.found = alternative.found;
                    if (alternative.found) {
                        if (better(alternative.state, best.state)) { best = std::move(alternative); changed = true; break; }
                        proof.signedCostGap = alternative.state.cost - best.state.cost;
                        proof.signedPeakTierGap = peakTier(alternative.state.focusPeak) - peakTier(best.state.focusPeak);
                        const auto riskTier = [](double risk) { return int(std::ceil(risk / 0.025 - epsilon)); };
                        proof.signedRiskTierGap = riskTier(alternative.state.risk) - riskTier(best.state.risk);
                        const double separation = qMax(0.0, proof.signedCostGap)
                            + (best.state.ordinaryOnly ? 0.0 : 2.0 * qMax(0, proof.signedPeakTierGap)) + 3.0 * qMax(0, proof.signedRiskTierGap);
                        proof.score = 1.0 - std::exp(-separation / 1.5);
                        if (proof.limited) proof.score = qMin(proof.score, 0.74);
                        counter[component.token] = true; gaps[component.token] = proof.signedCostGap;
                        tierGaps[component.token] = proof.signedPeakTierGap; counterHands[component.token] = proof.alternativeHand;
                        if (component.opposite.primitive >= 0) counterLanes[component.token] = primitives.at(component.opposite.primitive).lane;
                        if (proof.signedPeakTierGap == 0 && proof.signedRiskTierGap == 0 && proof.signedCostGap <= 0.25) {
                            for (int i = 0; i < localTouches.size(); ++i) {
                                auto& judged = best.state.replay.judgements[i];
                                if (judged.kind == "group_carry" && judged.handMask != alternative.state.replay.judgements.at(i).handMask)
                                    judged.stableResponsibility = false;
                            }
                        }
                    } else proof.score = proof.limited ? 0 : 0.95;
                    confidenceEvidence[component.token].append(proof);
                }
                settled = !changed;
                if (changed && pass == 2) failure = "counterfactual_unstable";
            }
            if (evidenceLimited) {
                result.evidenceTruncated = result.truncated = true;
                for (int token : core) {
                    counter[token] = false;
                    for (auto& proof : confidenceEvidence[token]) { proof.limited = true; proof.score = qMin(proof.score, 0.39); }
                }
            }
            optionalRemaining -= evidenceGrant - qMax(qint64(0), work);
            work = qMax(qint64(0), work) + reserved;
        }
        if (!failure.isEmpty()) for (int id : core) tokens[id].issue = failure;
        else {
            // A consumed input from a lookahead object is a real hard dependency.
            // Promote that complete source token, rather than depending on an
            // uncommitted speculative DOWN in an otherwise accepted result.
            bool promoted = true;
            while (promoted) {
                promoted = false;
                for (int id : QList<int>(core.values())) for (const auto& marker : tokens.at(id).markers)
                    for (const auto& full : best.state.fullReplay.judgements) if (full.parseOrder == marker.parseOrder) {
                        auto causes = full.causes; causes += full.headCauses;
                        for (const auto& cause : causes) for (int other : committedTokens)
                            if (tokens.at(other).start == cause.tokenStart) join(id, other);
                        for (const auto& cause : causes) for (int other : windowTokens)
                            if (tokens.at(other).start == cause.tokenStart && !core.contains(other)) {
                                for (const auto& hard : cores) if (hard.contains(other)) core.unite(hard);
                                promoted = true;
                            }
                    }
            }
            for (int id : core) {
                participated[id] = true; costs[id] = best.state.cost; peaks[id] = best.state.focusPeak;
                playerCosts[id] = best.state.playerCost;
                tapRates[id] = best.state.peakTapRate; loads[id] = best.state.peakLoad;
                for (int other : core) join(id, other);
            }
            for (const auto& action : best.state.physical) {
                int owner = -1;
                for (int id : core) if (tokens.at(id).start == action.action.tokenStart) owner = id;
                if (owner < 0 || committedTokens.contains(owner)) continue;
                for (const auto& previous : committed.actions) if (previous.action.hand == action.action.hand
                    && qMax(previous.action.start, action.action.start)
                    < qMin(previous.action.end, action.action.end) - epsilon)
                    for (int other : committedTokens) if (tokens.at(other).start == previous.action.tokenStart) join(owner, other);
                auto fixed = action; fixed.committed = true; committed.actions.append(fixed);
            }
            for (int id : core) if (!committedTokens.contains(id)) { committedTokens.insert(id); committedMarkers += tokens.at(id).markers; }
            for (const auto& d : block) {
                if (d.kind == Decision::Fixed && core.contains(primitives.at(d.index).token)) assigned[d.index] = best.state.primitiveHands.at(d.index);
                else if (d.kind == Decision::Slide && core.contains(tracks.at(d.index).token)) {
                    planned[d.index] = best.state.tracks.at(d.index);
                    plannedWifiSingle[d.index] = best.state.wifiSingle.at(d.index);
                    transferEvidence[d.index] = best.state.transferEvidence.value(d.index);
                    if (tracks.at(d.index).wifi) {
                        const int peer = tracks.at(d.index).peer;
                        planned[peer] = best.state.tracks.at(peer);
                        plannedWifiSingle[peer] = best.state.wifiSingle.at(peer);
                        transferEvidence[peer] = best.state.transferEvidence.value(peer);
                    }
                }
            }
            committed.primitiveHands = assigned; committed.tracks = planned; committed.wifiSingle = plannedWifiSingle;
            for (const auto& judgement : best.state.fullReplay.judgements)
                if (judgement.parseOrder >= 0 && judgement.parseOrder < fullJudged.size()) fullJudged[judgement.parseOrder] = judgement;
            for (auto it = localIndex.cbegin(); it != localIndex.cend(); ++it) judged[it.key()] = best.state.replay.judgements.at(it.value());
        }
        BasicWindowEvidence scope; scope.start = text.size(); scope.tasks = tasks;
        for (int id : core) { scope.start = qMin(scope.start, tokens.at(id).start); scope.end = qMax(scope.end, tokens.at(id).end); }
        scope.workUsed = beforeMain - qMax(qint64(0), work);
        scope.outcome = failure.isEmpty() ? QStringLiteral("validated") : failure;
        result.searchWindows.append(scope);
    }
    // Every retained label is audited against the same combined input stream.
    // Budget exhaustion in a later window does not invent inputs for that window
    // or discard independent, previously validated candidate scopes.
    work = qMax(qint64(0), work) + auditReserve;
    if (!committedMarkers.isEmpty()) {
        QVector<ChartInputAction> inputs;
        for (const auto& action : committed.actions) inputs.append({action.action, action.button, action.lane});
        // Unresolved notes still participate in consumption/Touch groups. They
        // need no fabricated autoplay inputs and need not themselves succeed,
        // but may never disappear merely because no label was committed.
        const auto audit = replayHandOrderChart(parsed.noteMarkers, inputs, {}, &work);
        result.auditedObjects = parsed.noteMarkers.size();
        QSet<int> failedUnits;
        if (audit.exhausted) {
            result.workLimited = true;
            for (int id : committedTokens) failedUnits.insert(root(id));
        } else {
            for (const auto& full : audit.judgements) {
                if (full.parseOrder >= 0 && full.parseOrder < fullJudged.size()) fullJudged[full.parseOrder] = full;
                bool valid = full.meetsTarget;
                for (int primitiveId = 0; primitiveId < primitives.size(); ++primitiveId) {
                    const auto& primitive = primitives.at(primitiveId);
                    if (primitive.marker.parseOrder != full.parseOrder || assigned.at(primitiveId) < 0) continue;
                    const int mask = 1 << assigned.at(primitiveId);
                    if (primitive.marker.type == "hold" || primitive.marker.type == "touch_hold") valid &= (full.singleHandCompletionMask & mask) != 0;
                    else {
                        int actual = 0;
                        const bool star = primitive.marker.type == "slide" || primitive.marker.type == "wifi";
                        for (const auto& cause : (star ? full.headCauses : full.causes)) actual |= cause.handMask;
                        valid &= actual == mask;
                    }
                }
                for (int touch = 0; touch < touches.size(); ++touch) if (touches.at(touch).parseOrder == full.parseOrder) {
                    auto& target = judged[touch];
                    if (!touches.at(touch).hold && target.handMask != full.handMask) valid = false;
                    target.judged = full.completed; target.highQuality = full.highQuality;
                    target.handMask = full.handMask; target.second = full.second; target.margin = full.margin;
                    target.quality = full.quality; target.causes = full.causes;
                }
                if (!valid) for (int id : committedTokens) for (const auto& marker : tokens.at(id).markers)
                    if (marker.parseOrder == full.parseOrder) failedUnits.insert(root(id));
            }
        }
        for (int id : committedTokens) if (failedUnits.contains(root(id))) {
            tokens[id].issue = audit.exhausted ? "work_limit" : "boundary_replay_failed";
            participated[id] = false; confidenceEvidence[id].clear();
            for (int primitive : tokens.at(id).primitives) assigned[primitive] = -1;
            for (int track : tokens.at(id).tracks) planned[track].clear();
        }
    }
    for (int index = 0; index < tokens.size(); ++index) {
        const auto& token = tokens.at(index);
        if (!token.target && !token.partial && !participated.at(index) && !included.at(index)) continue;
        BasicRow row; row.start = token.start; row.end = token.end; row.line = token.line; row.col = token.col;
        row.context = !token.target; row.originalAnnotation = token.suffix; row.dependencyUnit = root(index);
        row.hasCandidate = participated.at(index);
        row.counterfactualEvidence = confidenceEvidence.at(index);
        bool sufficient = !row.counterfactualEvidence.isEmpty();
        row.confidence = sufficient ? 1.0 : 0;
        for (const auto& proof : row.counterfactualEvidence) {
            sufficient &= proof.searched && !proof.limited;
            row.confidence = qMin(row.confidence, proof.score);
        }
        const double playerPressure = qMax(peaks.at(index) / p.player.comfortableMoveSpeed,
            qMax(tapRates.at(index) / p.player.maxComfortableTapRate, loads.at(index)));
        if (playerPressure > 1.5) { row.confidence = qMin(row.confidence, 0.39); sufficient = false; }
        else if (playerPressure > 1.0) row.confidence = qMin(row.confidence, 0.79);
        row.confidenceLevel = !sufficient ? QStringLiteral("insufficient_evidence")
            : row.confidence >= 0.8 ? QStringLiteral("high") : row.confidence >= 0.4 ? QStringLiteral("medium") : QStringLiteral("low");
        row.cost = costs.at(index); row.peakSpeed = peaks.at(index);
        row.playerCost = playerCosts.at(index); row.peakTapRate = tapRates.at(index); row.peakLoad = loads.at(index);
        if (row.hasCandidate) row.pressureLevel = playerPressure > 1.5 ? QStringLiteral("extreme")
            : playerPressure > 1.0 ? QStringLiteral("high")
            : playerPressure >= 0.66 ? QStringLiteral("medium") : QStringLiteral("low");
        row.hasCounterfactual = counter.at(index);
        row.signedCostGap = gaps.at(index); row.signedPeakTierGap = tierGaps.at(index); row.counterfactualHand = counterHands.at(index);
        for (const auto& proof : row.counterfactualEvidence) if (proof.found) row.counterfactualComponent = proof.component;
        chart_transform::HandOrderTokenEdit edit; edit.baseToken = token.base;
        bool complete = participated.at(index), expressible = true, mixedResponsibility = false; QStringList cluster;
        for (int id : token.primitives) {
            const auto& primitive = primitives.at(id); const auto hand = namedHand(assigned.at(id));
            complete &= hand != HandOrderHand::Unspecified; row.head = hand; edit.hasHead = true; edit.head = hand;
            if (!token.slide) { row.lanes.append(primitive.lane); row.noteHands.append(hand); cluster.append(QString::number(primitive.lane) + '~' + glyph(assigned.at(id))); }
        }
        for (int id : token.tracks) {
            const auto& track = tracks.at(id); if (track.sector == 1) continue;
            const auto& stops = planned.at(id); complete &= !stops.isEmpty();
            row.trackIntervals.append(stops); edit.tracks.append(stops); row.handoffCount += switches(stops);
            HandOrderWifiMode mode = HandOrderWifiMode::Legacy;
            QVector<QVector<HandOrderTrackStop>> sectors;
            if (track.wifi) {
                mode = plannedWifiSingle.at(id) ? (track.legacyAnnotated ? HandOrderWifiMode::Legacy : HandOrderWifiMode::Single)
                    : HandOrderWifiMode::Split;
                if (mode == HandOrderWifiMode::Split) {
                    sectors = {stops, planned.at(track.peer)}; complete &= !sectors.at(1).isEmpty();
                    row.handoffCount += switches(sectors.at(1));
                }
            }
            row.wifiModes.append(mode); row.wifiSectors.append(sectors);
            edit.wifiModes.append(mode); edit.wifiSectors.append(sectors);
            if (mode == HandOrderWifiMode::Split) edit.tracks.last().clear();
            const auto& proof = transferEvidence.at(id); row.handoffEvidence.append(proof);
            if (track.wifi && mode == HandOrderWifiMode::Split) row.handoffEvidence.append(transferEvidence.at(track.peer));
            if (row.handoffReason.isEmpty() && !proof.reason.isEmpty()) {
                row.handoffReason = proof.reason; row.oneHandoffCost = proof.baselineCost; row.oneHandoffPeak = proof.baselinePeak;
            }
            if (!stops.isEmpty()) row.track = stops.first().hand;
        }
        for (int id : token.touches) {
            const auto& judgement = judged.at(id); complete &= judgement.judged && judgement.highQuality;
            row.judgementKind = judgement.kind; row.judgementQuality = judgement.quality;
            row.judgementSecond = judgement.second; row.judgementMargin = judgement.margin; row.causes += judgement.causes;
            if (!touches.at(id).hold) row.head = namedHand(maskHand(judgement.handMask));
            if (!touches.at(id).hold) {
                expressible &= maskHand(judgement.handMask) >= 0 && judgement.stableResponsibility;
                mixedResponsibility |= maskHand(judgement.handMask) < 0;
            }
        }
        if (participated.at(index)) {
            row.fullReplayVerified = true;
            row.conditions = "community_profile_v2; target_critical; finite_physical_contacts; full_candidate_scope_replay; rolling_local; committed_history; player_profile_heuristic; window_peak";
            for (const auto& marker : token.markers) {
                const auto& judgement = fullJudged.value(marker.parseOrder);
                row.fullReplayVerified &= judgement.meetsTarget;
                row.inputChannel = judgement.inputChannel;
                if (token.touches.isEmpty()) {
                    row.judgementKind = judgement.kind; row.judgementQuality = judgement.quality;
                    row.judgementSecond = judgement.second; row.judgementMargin = judgement.margin;
                    row.causes += judgement.causes;
                }
            }
            complete &= row.fullReplayVerified;
        }
        if (row.context) ++result.contextObjects;
        row.reason = token.partial ? "partial_token" : token.issue;
        if (token.partial) row.status = "skipped";
        else if (row.context && token.issue.isEmpty()) { row.status = "context"; row.reason = "context_hypothesis"; }
        else if (token.issue == "button_down_conflict") row.status = "conflict";
        else if (token.issue == "candidate_search_failed" || token.issue == "full_replay_target_failed" || token.issue == "boundary_replay_failed") row.status = "not_found";
        else if (token.issue == "resource_limit" || token.issue == "task_limit" || token.issue == "work_limit" || token.issue == "window_work_limit" || token.issue == "counterfactual_unstable") row.status = "undecided";
        else if (token.issue == "invalid_native" || token.issue == "invalid_annotation") row.status = "invalid";
        else if (!token.issue.isEmpty()) row.status = "unsupported";
        else if (token.protectedText) { row.status = "preserved"; row.reason = "existing_annotation"; }
        else if (!complete) { row.status = "not_found"; row.reason = "candidate_search_failed"; }
        else if (!expressible) { row.status = "undecided"; row.reason = mixedResponsibility
            ? "mixed_touch_responsibility" : "unstable_touch_responsibility"; }
        else {
            row.status = "review"; row.reason = row.judgementKind.isEmpty() ? "joint_action_candidate" : row.judgementKind;
            QString encoded;
            if (token.slide) encoded = token.base + chart_transform::handOrderSuffix(edit);
            else if (token.primitives.size() > 1) encoded = cluster.join('/');
            else encoded = token.base + '~' + (row.head == HandOrderHand::Left ? QLatin1Char('l') : QLatin1Char('r'));
            result.patches.append({token.start, token.end, encoded});
        }
        if (!p.highConfidenceOnly && token.target && !token.protectedText
            && row.status == QStringLiteral("undecided")
            && (token.issue == QStringLiteral("window_work_limit")
                || token.issue == QStringLiteral("work_limit"))
            && !token.layoutFallback.isEmpty()
            && token.layoutFallback != text.mid(token.start, token.end - token.start)) {
            row.status = QStringLiteral("heuristic");
            row.reason = QStringLiteral("layout_fallback_unverified");
            row.hasCandidate = true;
            row.confidence = 0;
            row.confidenceLevel = QStringLiteral("insufficient_evidence");
            row.fullReplayVerified = false;
            row.pressureLevel.clear();
            row.conditions.clear();
            row.lanes.clear(); row.noteHands.clear(); row.trackIntervals.clear();
            row.wifiModes.clear(); row.wifiSectors.clear();
            for (const auto& marker : token.markers) {
                const auto* suggested = layoutByOrder.value(marker.parseOrder, nullptr);
                if (!suggested) continue;
                if (suggested->handOrderHead != HandOrderHand::Unspecified)
                    row.head = suggested->handOrderHead;
                if (marker.type == QStringLiteral("tap") || marker.type == QStringLiteral("hold")) {
                    row.lanes.append(marker.lane);
                    row.noteHands.append(suggested->handOrderHead);
                }
                if (marker.type == QStringLiteral("slide")) {
                    row.trackIntervals.append(suggested->handOrderTrackStops);
                    row.wifiModes.append(HandOrderWifiMode::Legacy);
                    row.wifiSectors.append(QVector<QVector<HandOrderTrackStop>>{});
                } else if (marker.type == QStringLiteral("wifi")) {
                    row.trackIntervals.append(suggested->handOrderTrackStops);
                    row.wifiModes.append(suggested->handOrderWifiMode);
                    row.wifiSectors.append(suggested->handOrderWifiSectorStops);
                }
                if (!suggested->handOrderTrackStops.isEmpty())
                    row.track = suggested->handOrderTrackStops.constFirst().hand;
            }
            result.patches.append({token.start, token.end, token.layoutFallback});
        }
        result.rows.append(row);
    }
    if (p.highConfidenceOnly) {
        QMap<int, bool> eligible;
        for (const auto& row : result.rows) if (!row.context && row.dependencyUnit >= 0) {
            if (!eligible.contains(row.dependencyUnit)) eligible[row.dependencyUnit] = true;
            if (row.status != "preserved") eligible[row.dependencyUnit] = eligible.value(row.dependencyUnit)
                && row.status == "review" && row.confidenceLevel == "high";
        }
        result.patches.removeIf([&](const auto& patch) {
            for (const auto& row : result.rows) if (row.start == patch.start) return !eligible.value(row.dependencyUnit, false);
            return true;
        });
    }
    result.workUsed = p.maxWorkUnits - qMax(qint64(0), work); return result;
}
} // namespace miacode::hand_order

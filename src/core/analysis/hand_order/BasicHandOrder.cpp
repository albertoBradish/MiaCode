#include "core/analysis/hand_order/BasicHandOrder.h"
#include "core/analysis/hand_order/HandOrderJudge.h"
#include "core/analysis/hand_order/HandOrderFullReplay.h"
#include "core/analysis/hand_order/BasicHandOrderActions.h"
#include "core/analysis/hand_order/BasicHandOrderWifi.h"
#include "core/chart/SlideReferenceData.h"
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
    int token = -1, lane = 0, lock = -1;
    bool button = true;
    TimelineNoteMarker marker;
};
struct Track {
    int token = -1, branch = 0;
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
    bool target = false, partial = false, slide = false, protectedText = false;
    double beginTime = 0, endTime = 0;
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
struct State {
    int inputPreference = 0;
    struct ButtonHand { double free = -1e9; QPointF point; int lane = 0; bool known = false; };
    std::array<ButtonHand, 2> buttonHands;
    QVector<Scheduled> actions;
    QVector<Scheduled> physical;
    QVector<bool> wifiSingle;
    FullReplay fullReplay;
    QVector<int> primitiveHands;
    QVector<QVector<HandOrderTrackStop>> tracks;
    QVector<BasicHandoffEvidence> transferEvidence;
    double cost = 0, peak = 0, risk = 0;
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
    if (peakTier(a.peak) != peakTier(b.peak)) return peakTier(a.peak) < peakTier(b.peak);
    if (std::abs(a.risk - b.risk) > epsilon) return a.risk < b.risk;
    if (std::abs(a.cost - b.cost) > epsilon) return a.cost < b.cost;
    return std::lexicographical_compare(a.primitiveHands.begin(), a.primitiveHands.end(), b.primitiveHands.begin(), b.primitiveHands.end());
}
QPointF lanePoint(int lane)
{
    const double angle = qDegreesToRadians(muri::kPadAngleBaseDegrees + (lane - 1) * muri::kPadAngleStepDegrees);
    return {std::cos(angle), std::sin(angle)};
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
// Score and replay one realizable centre path per physical hand.
bool score(State& state, const QVector<Track>& tracks, qint64& work, bool* sideEffect = nullptr)
{
    Q_UNUSED(sideEffect);
    state.cost = state.peak = 0;
    if (!resolveHandOrderActions(state.actions, &state.physical, work)) return false;
    for (const auto& action : state.actions) {
        if (state.inputPreference == 1 && action.button) state.cost += 0.12;
        if (state.inputPreference == 0 && action.action.kind == "screen_button") state.cost += 0.12;
    }
    for (int hand = 0; hand < 2; ++hand) {
        QVector<const Scheduled*> active;
        for (const auto& action : state.physical) if (action.action.hand == hand) active.append(&action);
        std::stable_sort(active.begin(), active.end(), [](const auto* a, const auto* b) { return a->action.start < b->action.start; });
        QVector<double> times;
        for (const auto* action : active) { times.append(action->action.start); times.append(action->action.end); times += action->action.times; }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end(), [](double a, double b) { return std::abs(a - b) < epsilon; }), times.end());
        bool known = false, wasActive = false;
        double free = -1e9; QPointF position(hand == 0 ? -0.7 : 0.7, 0);
        for (int i = 1; i < times.size(); ++i) {
            if (!spend(work, active.size() + 1)) return false;
            const double first = times.at(i - 1), last = times.at(i), middle = (first + last) / 2;
            const Scheduled* selected = nullptr;
            for (const auto* action : active) if (middle >= action->action.start && middle < action->action.end) { selected = action; break; }
            if (!selected) { wasActive = false; continue; }
            const auto from = contactPosition(selected->action, first), to = contactPosition(selected->action, last);
            if (!wasActive) {
                const double distance = QLineF(position, from).length(), gap = known ? first - free : 1.0;
                if (known && gap <= epsilon && distance > 1e-6) return false;
                const double prep = gap > epsilon ? gap : 1.0;
                state.cost += distance + 0.1 * distance * distance / prep;
                state.peak = qMax(state.peak, distance / prep);
                if (!selected->motion) state.cost += 0.16 * qMax(0.0, hand == 0 ? from.x() : -from.x());
            } else if (QLineF(position, from).length() > 1e-6) return false;
            const double dt = last - first, distance = QLineF(from, to).length(), speed = distance / dt;
            state.cost += distance + 0.1 * speed * speed * dt;
            if (selected->motion) state.cost += 0.16 * qMax(0.0, hand == 0 ? (from.x() + to.x()) / 2 : -(from.x() + to.x()) / 2) * dt;
            state.peak = qMax(state.peak, speed);
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
    state.cost += physicalHandTransferCharge(transfers);
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
    visit(0, {{0, HandOrderHand::Left}}); visit(0, {{0, HandOrderHand::Right}}); return options;
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
    const SearchConstraint& constraint = {})
{
    Search result; State initial; initial.inputPreference = p.inputPreference;
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
    initial.buttonHands[0].point = {-0.7, 0}; initial.buttonHands[1].point = {0.7, 0};
    bool buttonOnly = p.inputPreference == 0 && touches.isEmpty() && std::all_of(decisions.begin(), decisions.end(), [&](const Decision& d) {
        return d.kind == Decision::Fixed && primitives.at(d.index).button;
    });
    if (buttonOnly) for (int i = 0; i + 2 < decisions.size(); ++i)
        if (std::abs(decisions.at(i + 2).second - decisions.at(i).second) < epsilon) buttonOnly = false;
    if (buttonOnly) for (int i = 0; i < decisions.size(); ++i) for (int j = i + 1; j < decisions.size(); ++j) {
        const auto& a = primitives.at(decisions.at(i).index); const auto& b = primitives.at(decisions.at(j).index);
        if (a.lane == b.lane && qMax(a.action.start, b.action.start) < qMin(a.action.end, b.action.end) - epsilon)
            buttonOnly = false;
    }
    initial.primitiveHands.fill(-1, primitives.size()); initial.tracks.resize(tracks.size()); initial.wifiSingle.fill(false, tracks.size()); QVector<State> beam{initial};
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
            QVector<State> next;
            for (const auto& forecast : forecasts) for (const auto& physical : templates) {
                State candidate = forecast; candidate.actions.append(physical);
                if (score(candidate, tracks, work)) next.append(std::move(candidate));
            }
            if (next.isEmpty()) return false;
            std::stable_sort(next.begin(), next.end(), better);
            if (next.size() > 4) { next.resize(4); result.truncated = true; }
            forecasts = std::move(next);
        }
        if (!forecasts.isEmpty()) { state.cost = forecasts.first().cost; state.peak = forecasts.first().peak; }
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
                        const auto existing = replayHandOrderChart({primitive.marker}, inputs, prefix, &work);
                        if (existing.exhausted) { result.exhausted = true; return; }
                        if (!existing.judgements.isEmpty()) {
                            const auto& judged = existing.judgements.first();
                            for (const auto& cause : (judged.headRequired ? judged.headCauses : judged.causes)) inheritedMask |= cause.handMask;
                            inheritedQuality = judged.headRequired ? judged.headHighQuality : judged.highQuality;
                        }
                    }
                    for (int hand = 0; hand < 2; ++hand) {
                        if ((primitive.lock >= 0 && primitive.lock != hand) || (primitiveId == constraint.primitive && hand != constraint.hand)) continue;
                        auto action = primitive.action; action.hand = hand;
                        QVector<Scheduled> choices{{action, primitive.button, false, primitive.lane}};
                        if (!buttonOnly && primitive.button) {
                            auto screen = action; screen.kind = QStringLiteral("screen_button");
                            screen.points = {touchPadPoint(QStringLiteral("A%1").arg(primitive.lane))};
                            choices.append({screen, false, false, primitive.lane});
                            if (inheritedQuality && inheritedMask == (1 << hand)) {
                                State inherited = candidate; inherited.primitiveHands[primitiveId] = hand;
                                assign(index + 1, std::move(inherited));
                            }
                        }
                        if (!buttonOnly && primitive.button && primitive.marker.type != "hold") {
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
                        branch.actions.append(choice); branch.primitiveHands[primitiveId] = hand;
                        bool valid = true;
                        if (buttonOnly) {
                            const auto& previous = candidate.buttonHands[hand]; const auto& other = candidate.buttonHands[1 - hand];
                            const double distance = QLineF(previous.point, action.points.first()).length();
                            const double gap = previous.known ? action.start - previous.free : 1.0;
                            valid = previous.free <= action.start + epsilon
                                && !(other.free > action.start + epsilon && other.lane == primitive.lane)
                                && !(previous.known && gap <= epsilon && distance > 1e-6);
                            if (valid) {
                                const double prep = gap > epsilon ? gap : 1.0;
                                branch.cost += distance + 0.1 * distance * distance / prep
                                    + 0.16 * qMax(0.0, hand == 0 ? action.points.first().x() : -action.points.first().x());
                                branch.peak = qMax(branch.peak, distance / prep);
                                branch.buttonHands[hand] = {action.end, action.points.first(), primitive.lane, true};
                            }
                        } else valid = score(branch, tracks, work, &result.sideEffect);
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
                        if (score(candidate, tracks, work, &result.sideEffect) && rankWithFutureHolds(candidate)) next.append(std::move(candidate));
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
                            if (score(candidate, tracks, work, &result.sideEffect) && rankWithFutureHolds(candidate)) next.append(std::move(candidate));
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
                                if (score(candidate, tracks, work, &result.sideEffect) && rankWithFutureHolds(candidate)) next.append(std::move(candidate));
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
                            if (!score(branch, tracks, work, &result.sideEffect)) continue;
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
        if (!buttonOnly && !score(candidate, tracks, work)) continue;
        candidate.replay = replayHandOrderTouches(touches, contacts(candidate), std::numeric_limits<double>::infinity(), &work);
        if (!locksSatisfied(touches, candidate.replay, std::numeric_limits<double>::infinity(), true)) continue;
        QVector<ChartInputAction> inputs;
        const auto& physical = candidate.physical.isEmpty() ? candidate.actions : candidate.physical;
        for (const auto& action : physical) inputs.append({action.action, action.button, action.lane});
        candidate.fullReplay = replayHandOrderChart(markers, inputs, {}, &work);
        if (candidate.fullReplay.exhausted) { result.exhausted = true; return result; }
        if (!candidate.fullReplay.valid || !candidate.fullReplay.meetsTarget) { result.sideEffect = true; continue; }
        bool fixedResponsibilities = true;
        for (const auto& decision : decisions) if (decision.kind == Decision::Fixed) {
            const auto& primitive = primitives.at(decision.index);
            for (const auto& full : candidate.fullReplay.judgements) if (full.parseOrder == primitive.marker.parseOrder) {
                const int assignedMask = 1 << candidate.primitiveHands.at(decision.index);
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
    if (result.found) for (const auto& candidate : finished) {
        if (peakTier(candidate.peak) != peakTier(result.state.peak) || std::abs(candidate.risk - result.state.risk) > epsilon
            || candidate.cost > result.state.cost + 0.25) continue;
        for (int i = 0; i < touches.size(); ++i) {
            auto& judged = result.state.replay.judgements[i];
            if (judged.kind == "group_carry" && judged.handMask != candidate.replay.judgements.at(i).handMask)
                judged.stableResponsibility = false;
        }
    }
    result.exhausted = work < 0; return result;
}
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

BasicResult inferBasicHandOrder(const QString& text, const SimaiParseResult& parsed, int begin, int end, const BasicParameters& p)
{
    BasicResult result;
    begin = qBound(0, begin, int(text.size())); end = qBound(begin, end, int(text.size()));
    if (p.version != basicAlgorithmVersion || p.judgeProfileRevision != basicJudgeProfileRevision
        || p.beamWidth < 1 || p.beamWidth > 128 || p.maxTasks < 1 || p.maxTasks > 2048
        || p.counterfactualBudget < 0 || p.counterfactualBudget > 128
        || !std::isfinite(p.contextSeconds) || p.contextSeconds < 0 || p.contextSeconds > 10
        || !std::isfinite(p.pressSeconds) || p.pressSeconds <= 0 || p.pressSeconds > 1
        || p.maxWorkUnits < 1000 || p.maxWorkUnits > 100000000 || p.inputPreference < 0 || p.inputPreference > 2) {
        result.error = "invalid_parameters"; return result;
    }
    QVector<int> lines{0}; for (int i = 0; i < text.size(); ++i) if (text.at(i) == '\n') lines.append(i + 1);
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
        token.beginTime = std::numeric_limits<double>::infinity(); token.endTime = -std::numeric_limits<double>::infinity();
        const int tokenIndex = tokens.size(); bool headCreated = false;
        for (int i = 0; i < source.markerCount; ++i) {
            const auto* pointer = byOrder.value(source.firstParseOrder + i); if (!pointer) { token.issue = "invalid_native"; continue; }
            auto marker = *pointer;
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
                touches.append({token.start, marker.parseOrder, marker.eachGroupId, marker.touchPad, marker.second, touchHold, fixedHand(marker.handOrderHead)});
            }
            if (!touch && (!slide || (marker.hasHeadStar && !headCreated))) {
                Primitive primitive; primitive.token = tokenIndex; primitive.lane = marker.lane; primitive.marker = marker;
                primitive.action.tokenStart = token.start; primitive.action.kind = touchHold ? "touch_hold" : "button";
                primitive.action.start = marker.second;
                primitive.action.end = (marker.type == "hold" || touchHold) ? qMax(marker.endSecond, marker.second + p.pressSeconds) : marker.second + p.pressSeconds;
                primitive.action.points = {touchHold ? touchPadPoint(marker.touchPad) : lanePoint(marker.lane)};
                primitive.action.times = {primitive.action.start}; primitive.button = !touchHold;
                primitive.lock = fixedHand(marker.handOrderHead);
                token.primitives.append(primitives.size()); primitives.append(primitive); headCreated |= slide;
            }
            if (slide) {
                Track track; track.token = tokenIndex; track.branch = marker.slideBranchIndex; track.locks = marker.handOrderTrackStops;
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
        if (!token.issue.isEmpty() && std::isfinite(token.beginTime)) risks.append({token.beginTime - p.contextSeconds, token.endTime + p.contextSeconds});
        tokens.append(token);
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
    bool expanded = true;
    while (expanded) {
        expanded = false; task::CancellationScope::check();
        for (auto& risk : risks) for (const auto& token : tokens) {
            if (token.endTime - token.beginTime <= p.pressSeconds + epsilon || token.beginTime > risk.last || token.endTime < risk.first) continue;
            const double first = qMin(risk.first, token.beginTime), last = qMax(risk.last, token.endTime);
            if (first != risk.first || last != risk.last) { risk = {first, last}; expanded = true; }
        }
    }
    for (auto& token : tokens) if (token.issue.isEmpty()) for (const auto& risk : risks)
        if (token.beginTime <= risk.last && token.endTime >= risk.first) { token.issue = "unsupported_context"; break; }
    double selectedFirst = std::numeric_limits<double>::infinity(), selectedLast = -selectedFirst;
    for (const auto& token : tokens) if (token.target || token.partial) { selectedFirst = qMin(selectedFirst, token.beginTime); selectedLast = qMax(selectedLast, token.endTime); }
    QVector<bool> included(tokens.size(), false);
    for (int i = 0; i < tokens.size(); ++i) included[i] = tokens.at(i).issue.isEmpty()
        && tokens.at(i).endTime >= selectedFirst - p.contextSeconds && tokens.at(i).beginTime <= selectedLast + p.contextSeconds;
    const auto groups = buildHandOrderTouchGroups(touches);
    bool dependencyExpanded = true;
    while (dependencyExpanded) {
        dependencyExpanded = false; task::CancellationScope::check();
        for (int i = 0; i < tokens.size(); ++i) if (included.at(i)) {
            const auto& token = tokens.at(i); bool sustained = !token.tracks.isEmpty();
            double first = token.beginTime, last = token.endTime;
            for (int id : token.tracks) first = qMin(first, tracks.at(id).judgeAvailable);
            for (int id : token.primitives) sustained |= primitives.at(id).action.end - primitives.at(id).action.start > p.pressSeconds + epsilon;
            if (!sustained) continue;
            for (int j = 0; j < tokens.size(); ++j) {
                task::CancellationScope::checkpoint();
                if (!included.at(j) && tokens.at(j).issue.isEmpty() && tokens.at(j).beginTime <= last + epsilon
                    && tokens.at(j).endTime >= first - epsilon) { included[j] = true; dependencyExpanded = true; }
            }
        }
        for (const auto& group : groups) {
            bool any = false;
            for (int id : group) for (int i = 0; i < tokens.size(); ++i) {
                task::CancellationScope::checkpoint();
                if (tokens.at(i).touches.contains(id)) any |= included.at(i);
            }
            if (any) for (int id : group) for (int i = 0; i < tokens.size(); ++i) {
                task::CancellationScope::checkpoint();
                if (!included.at(i) && tokens.at(i).touches.contains(id) && tokens.at(i).issue.isEmpty()) { included[i] = true; dependencyExpanded = true; }
            }
        }
    }
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
    qint64 work = p.maxWorkUnits;
    QVector<int> assigned(primitives.size(), -1); QVector<QVector<HandOrderTrackStop>> planned(tracks.size());
    QVector<bool> plannedWifiSingle(tracks.size(), false);
    QVector<FullReplayJudgement> fullJudged(parsed.noteMarkers.size());
    QVector<TouchJudgement> judged(touches.size());
    QVector<double> costs(tokens.size(), 0), peaks(tokens.size(), 0), gaps(tokens.size(), 0);
    QVector<int> tierGaps(tokens.size(), 0); QVector<bool> counter(tokens.size(), false), participated(tokens.size(), false);
    QVector<HandOrderHand> counterHands(tokens.size(), HandOrderHand::Unspecified);
    QVector<int> counterLanes(tokens.size(), 0);
    QVector<BasicHandoffEvidence> transferEvidence(tracks.size());
    QVector<QVector<BasicCounterfactualEvidence>> confidenceEvidence(tokens.size());
    const auto dependencyStart = [&](const Decision& d) {
        if (d.kind == Decision::Slide) return tracks.at(d.index).judgeAvailable;
        double first = d.kind == Decision::Fixed ? primitives.at(d.index).marker.second - FullReplayProfile::tapAvailableSeconds : d.second;
        if (d.kind == Decision::Touches) for (int id : d.members) first = qMin(first, touches.at(id).second - TouchJudgeProfile::availableSeconds);
        return first;
    };
    QVector<int> dependencyOrder(decisions.size()); for (int i = 0; i < decisions.size(); ++i) dependencyOrder[i] = i;
    std::stable_sort(dependencyOrder.begin(), dependencyOrder.end(), [&](int a, int b) { return dependencyStart(decisions.at(a)) < dependencyStart(decisions.at(b)); });
    QVector<QVector<Decision>> blocks;
    for (int first = 0; first < dependencyOrder.size();) {
        int last = first + 1; double activeEnd = decisions.at(dependencyOrder.at(first)).end;
        while (last < dependencyOrder.size() && dependencyStart(decisions.at(dependencyOrder.at(last))) <= activeEnd + p.contextSeconds) {
            activeEnd = qMax(activeEnd, decisions.at(dependencyOrder.at(last)).end); ++last;
        }
        QVector<int> ordered;
        for (int i = first; i < last; ++i) ordered.append(dependencyOrder.at(i));
        std::sort(ordered.begin(), ordered.end());
        QVector<Decision> block; for (int id : ordered) block.append(decisions.at(id));
        blocks.append(std::move(block)); first = last;
    }
    int dependencyUnit = 0;
    for (auto block : blocks) {
        ++dependencyUnit;
        task::CancellationScope::check(); QSet<int> blockTokens, touchIds;
        for (const auto& d : block) {
            if (d.kind == Decision::Fixed) blockTokens.insert(primitives.at(d.index).token);
            else if (d.kind == Decision::Slide) blockTokens.insert(tracks.at(d.index).token);
            else for (int id : d.members) { touchIds.insert(id); for (int t = 0; t < tokens.size(); ++t) if (tokens.at(t).touches.contains(id)) blockTokens.insert(t); }
        }
        bool target = false; for (int id : blockTokens) { target |= tokens.at(id).target; tokens[id].dependencyUnit = dependencyUnit; }
        if (!target) continue;
        QVector<TouchTarget> localTouches; QMap<int, int> localIndex;
        QList<int> orderedIds = touchIds.values(); std::sort(orderedIds.begin(), orderedIds.end());
        for (int id : orderedIds) { localIndex[id] = localTouches.size(); localTouches.append(touches.at(id)); }
        for (auto& d : block) if (d.kind == Decision::Touches) for (int& id : d.members) id = localIndex.value(id);
        for (int id : blockTokens) for (int touch : tokens.at(id).touches) if (touches.at(touch).hold && !localIndex.contains(touch)) {
            localIndex[touch] = localTouches.size(); localTouches.append(touches.at(touch));
        }
        QVector<TimelineNoteMarker> blockMarkers;
        for (int id : blockTokens) blockMarkers += tokens.at(id).markers;
        QString failure;
        if (block.size() + localTouches.size() > p.maxTasks) failure = "resource_limit";
        QVector<int> caps(tracks.size(), 3);
        auto best = failure.isEmpty() ? search(block, primitives, tracks, localTouches, blockMarkers, p, caps, work) : Search{};
        result.truncated |= best.truncated;
        const auto admitTransfers = [&](Search candidate, const SearchConstraint& required = SearchConstraint{}) {
            auto localCaps = caps;
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
                    const auto baseline = search(block, primitives, tracks, localTouches, blockMarkers, p, limited, work, required);
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
        if (failure.isEmpty() && (best.exhausted || work < 0)) failure = "resource_limit";
        if (failure.isEmpty() && !best.found) failure = best.sideEffect ? "full_replay_target_failed" : "candidate_search_failed";
        if (failure.isEmpty()) {
            const qint64 reserved = qMax(qint64(0), work - qMin(work, p.maxWorkUnits / 3));
            work -= reserved;
            struct Component { int token = -1; QString name; SearchConstraint opposite; };
            const auto componentsFor = [&](const State& state) {
                QVector<Component> components;
                for (const auto& d : block) {
                    if (d.kind == Decision::Fixed) {
                        const auto& primitive = primitives.at(d.index); const auto& token = tokens.at(primitive.token);
                        if (!token.target || token.protectedText || primitive.lock >= 0) continue;
                        const QString name = token.slide ? QStringLiteral("head") : QStringLiteral("note%1").arg(primitive.lane);
                        components.append({primitive.token, name, {d.index, -1, -1, 1 - state.primitiveHands.at(d.index), 0}});
                    } else if (d.kind == Decision::Slide) {
                        QVector<int> ids{d.index}; const auto& track = tracks.at(d.index);
                        if (track.wifi && !state.wifiSingle.at(d.index)) ids.append(track.peer);
                        for (int id : ids) {
                            const auto& path = tracks.at(id); const auto& token = tokens.at(path.token);
                            if (!token.target || token.protectedText || !path.locks.isEmpty()) continue;
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
                        if (tokenId < 0 || !tokens.at(tokenId).target || tokens.at(tokenId).protectedText) continue;
                        const int hand = maskHand(state.replay.judgements.at(id).handMask);
                        components.append({tokenId, QStringLiteral("touch"), {-1, -1, id, hand < 0 ? -1 : 1 - hand, 0}});
                    }
                }
                return components;
            };
            bool evidenceLimited = false, settled = false;
            for (int pass = 0; pass < 3 && !settled && !evidenceLimited; ++pass) {
                for (int token : blockTokens) { confidenceEvidence[token].clear(); counter[token] = false; }
                const auto components = componentsFor(best.state);
                bool changed = false; int count = 0;
                for (const auto& component : components) {
                    BasicCounterfactualEvidence proof; proof.component = component.name;
                    proof.progress = component.opposite.progress; proof.alternativeHand = namedHand(component.opposite.hand);
                    if (component.opposite.hand < 0 || count >= p.counterfactualBudget || work <= 0) {
                        proof.limited = true; evidenceLimited |= component.opposite.hand >= 0;
                        confidenceEvidence[component.token].append(proof); continue;
                    }
                    ++count; proof.searched = true;
                    auto alternative = search(block, primitives, tracks, localTouches, blockMarkers, p, caps, work, component.opposite);
                    if (alternative.found) alternative = admitTransfers(std::move(alternative), component.opposite);
                    result.truncated |= alternative.truncated;
                    proof.limited = alternative.truncated || alternative.exhausted;
                    if (alternative.exhausted) evidenceLimited = true;
                    proof.found = alternative.found;
                    if (alternative.found) {
                        if (better(alternative.state, best.state)) { best = std::move(alternative); changed = true; break; }
                        proof.signedCostGap = alternative.state.cost - best.state.cost;
                        proof.signedPeakTierGap = peakTier(alternative.state.peak) - peakTier(best.state.peak);
                        const auto riskTier = [](double risk) { return int(std::ceil(risk / 0.025 - epsilon)); };
                        proof.signedRiskTierGap = riskTier(alternative.state.risk) - riskTier(best.state.risk);
                        const double separation = qMax(0.0, proof.signedCostGap)
                            + 2.0 * qMax(0, proof.signedPeakTierGap) + 3.0 * qMax(0, proof.signedRiskTierGap);
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
                for (int token : blockTokens) {
                    counter[token] = false;
                    for (auto& proof : confidenceEvidence[token]) { proof.limited = true; proof.score = qMin(proof.score, 0.39); }
                }
            }
            work = qMax(qint64(0), work) + reserved;
        }
        if (!failure.isEmpty()) for (int id : blockTokens) tokens[id].issue = failure;
        else {
            for (int id : blockTokens) { participated[id] = true; costs[id] = best.state.cost; peaks[id] = best.state.peak; }
            for (const auto& d : block) {
                if (d.kind == Decision::Fixed) assigned[d.index] = best.state.primitiveHands.at(d.index);
                else if (d.kind == Decision::Slide) {
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
            for (const auto& judgement : best.state.fullReplay.judgements)
                if (judgement.parseOrder >= 0 && judgement.parseOrder < fullJudged.size()) fullJudged[judgement.parseOrder] = judgement;
            for (auto it = localIndex.cbegin(); it != localIndex.cend(); ++it) judged[it.key()] = best.state.replay.judgements.at(it.value());
        }
    }
    for (int index = 0; index < tokens.size(); ++index) {
        const auto& token = tokens.at(index);
        if (!token.target && !token.partial && !participated.at(index)) continue;
        BasicRow row; row.start = token.start; row.end = token.end; row.line = token.line; row.col = token.col;
        row.context = !token.target; row.originalAnnotation = token.suffix; row.dependencyUnit = token.dependencyUnit;
        row.counterfactualEvidence = confidenceEvidence.at(index);
        bool sufficient = !row.counterfactualEvidence.isEmpty();
        row.confidence = sufficient ? 1.0 : 0;
        for (const auto& proof : row.counterfactualEvidence) {
            sufficient &= proof.searched && !proof.limited;
            row.confidence = qMin(row.confidence, proof.score);
        }
        if (peaks.at(index) >= 12.0) { row.confidence = qMin(row.confidence, 0.39); sufficient = false; }
        row.confidenceLevel = !sufficient ? QStringLiteral("insufficient_evidence")
            : row.confidence >= 0.8 ? QStringLiteral("high") : row.confidence >= 0.4 ? QStringLiteral("medium") : QStringLiteral("low");
        row.cost = costs.at(index); row.peakSpeed = peaks.at(index);
        row.pressureLevel = row.peakSpeed >= 12.0 ? QStringLiteral("extreme") : row.peakSpeed >= 8.0 ? QStringLiteral("high")
            : row.peakSpeed >= 4.0 ? QStringLiteral("medium") : QStringLiteral("low");
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
            row.conditions = "community_profile_v2; target_critical; finite_physical_contacts; full_chart_replay; conditional_context";
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
        if (row.context && participated.at(index)) ++result.contextObjects;
        row.reason = token.partial ? "partial_token" : token.issue;
        if (token.partial) row.status = "skipped";
        else if (row.context) { row.status = "context"; row.reason = "context_hypothesis"; }
        else if (token.issue == "button_down_conflict") row.status = "conflict";
        else if (token.issue == "candidate_search_failed" || token.issue == "full_replay_target_failed") row.status = "not_found";
        else if (token.issue == "resource_limit" || token.issue == "counterfactual_unstable") row.status = "undecided";
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

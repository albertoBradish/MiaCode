#pragma once

#include "core/analysis/hand_order/HandOrderFullReplay.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace miacode::hand_order {

// Alternative physical releases only. The original template remains first;
// the full chart replay must validate every selected candidate independently.
inline QVector<ContactAction> holdReleaseCandidates(const ContactAction& full,
    const TimelineNoteMarker& marker, double pressSeconds)
{
    QVector<ContactAction> candidates{full};
    constexpr double epsilon = 1e-8;
    const bool touch = marker.type == QStringLiteral("touch_hold");
    if ((!touch && marker.type != QStringLiteral("hold"))
        || !std::isfinite(marker.second) || !std::isfinite(marker.endSecond)
        || marker.endSecond <= marker.second
        || !std::isfinite(full.start) || !std::isfinite(full.end) || full.end <= full.start
        || !std::isfinite(pressSeconds) || pressSeconds <= 0.0
        || full.points.isEmpty() || full.points.size() != full.times.size()) return candidates;

    // This helper accepts a stationary hold, never a resolved moving palm or
    // slide. Moving/shared trajectories must retain their original template.
    const QPointF point = full.points.first();
    if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return candidates;
    for (int i = 0; i < full.points.size(); ++i) {
        const auto& current = full.points.at(i);
        const double time = full.times.at(i);
        if (!std::isfinite(current.x()) || !std::isfinite(current.y()) || !std::isfinite(time)
            || std::hypot(current.x() - point.x(), current.y() - point.y()) > epsilon
            || time < full.start - epsilon || time > full.end + epsilon
            || (i > 0 && time <= full.times.at(i - 1))) return candidates;
    }

    const double headIgnore = touch ? FullReplayProfile::touchHoldHeadIgnoreSeconds
                                    : FullReplayProfile::holdHeadIgnoreSeconds;
    const double bodyFirst = marker.second + headIgnore;
    const double bodyLast = std::max(bodyFirst, marker.endSecond - FullReplayProfile::holdTailIgnoreSeconds);
    const double guard = 2.0 / FullReplayProfile::ticksPerSecond;
    const bool hasBody = marker.endSecond - marker.second > headIgnore
        + FullReplayProfile::holdTailIgnoreSeconds + epsilon;
    const double earliest = hasBody ? std::max(full.start + pressSeconds, bodyLast + guard)
                                    : full.start + pressSeconds;
    if (!std::isfinite(earliest)) return candidates;

    const auto append = [&](double requestedEnd) {
        const double release = std::max(earliest, requestedEnd);
        if (!std::isfinite(release) || release > marker.endSecond + epsilon
            || release >= full.end - epsilon || release <= full.start + epsilon) return;
        for (const auto& candidate : candidates)
            if (std::abs(candidate.end - release) <= epsilon) return;
        ContactAction candidate = full;
        candidate.end = release;
        candidate.motionStart = full.start;
        candidate.motionEnd = release;
        candidate.points = {point};
        candidate.times = {full.start};
        candidates.append(std::move(candidate));
    };
    append(marker.endSecond - 0.05);
    append(marker.endSecond - FullReplayProfile::holdTailIgnoreSeconds + guard);
    if (!hasBody) append(full.start + pressSeconds);
    return candidates;
}

} // namespace miacode::hand_order

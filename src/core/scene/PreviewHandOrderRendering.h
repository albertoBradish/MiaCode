#pragma once

#include "core/chart/model/TimelineData.h"

#include <QtMath>

namespace miacode::preview::scene {

inline HandOrderHand previewHandOrderAtProgress(const QVector<HandOrderTrackStop>& stops, qreal progress)
{
    HandOrderHand hand = HandOrderHand::Unspecified;
    for (const HandOrderTrackStop& stop : stops) {
        if (progress < stop.progress) {
            break;
        }
        hand = stop.hand;
    }
    return hand;
}

inline qreal previewSlideMovementProgress(
    const TimelineNoteMarker& marker,
    int segmentIndex,
    qreal segmentProgress)
{
    qreal totalDuration = 0.0;
    qreal precedingDuration = 0.0;
    for (int index = 0; index < marker.slideSegmentDurations.size(); ++index) {
        const qreal duration = qMax<qreal>(0.0, marker.slideSegmentDurations.at(index));
        totalDuration += duration;
        if (index < segmentIndex) {
            precedingDuration += duration;
        }
    }
    if (totalDuration <= 0.0 || segmentIndex < 0 || segmentIndex >= marker.slideSegmentDurations.size()) {
        return qBound<qreal>(0.0, segmentProgress, 1.0);
    }
    const qreal duration = qMax<qreal>(0.0, marker.slideSegmentDurations.at(segmentIndex));
    return qBound<qreal>(
        0.0,
        (precedingDuration + qBound<qreal>(0.0, segmentProgress, 1.0) * duration) / totalDuration,
        1.0);
}

// Track checkpoints map movement progress to arrow cuts. Invert that mapping at
// each arrow's centre, so a hand boundary follows time rather than arrow count.
inline qreal previewSlideArrowSegmentProgress(
    const TimelineNoteMarker& marker,
    int segmentIndex,
    int areaIndex,
    int arrowIndex)
{
    if (segmentIndex < 0 || segmentIndex >= marker.slideTrackAreaPoints.size()) {
        return 0.0;
    }
    const auto& areas = marker.slideTrackAreaPoints.at(segmentIndex);
    if (areaIndex < 0 || areaIndex >= areas.size()) {
        return 0.0;
    }
    const QVector<QPointF>& arrows = areas.at(areaIndex);
    if (arrows.isEmpty()) {
        return 0.0;
    }
    const QVector<double>* thresholds = segmentIndex < marker.slideTrackAreaThresholds.size()
        ? &marker.slideTrackAreaThresholds.at(segmentIndex) : nullptr;
    const qreal areaStart = qBound<qreal>(0.0, thresholds != nullptr ? thresholds->value(areaIndex, 0.0) : 0.0, 1.0);
    const qreal areaEnd = thresholds != nullptr && areaIndex + 1 < thresholds->size()
        ? qBound<qreal>(areaStart, thresholds->at(areaIndex + 1), 1.0)
        : 1.0;
    const auto* checkpointAreas = segmentIndex < marker.slideTrackAreaCheckpoints.size()
        ? &marker.slideTrackAreaCheckpoints.at(segmentIndex) : nullptr;
    const auto* cutAreas = segmentIndex < marker.slideTrackAreaCutIndices.size()
        ? &marker.slideTrackAreaCutIndices.at(segmentIndex) : nullptr;
    const QVector<double>* checkpoints = checkpointAreas != nullptr && areaIndex < checkpointAreas->size()
        ? &checkpointAreas->at(areaIndex) : nullptr;
    const QVector<int>* cuts = cutAreas != nullptr && areaIndex < cutAreas->size()
        ? &cutAreas->at(areaIndex) : nullptr;
    // Source arrow rows run from the area end back toward its start; track
    // erasure removes the array tail first for the same reason.
    const qreal arrowCentre = qBound<qreal>(0.5, arrows.size() - arrowIndex - 0.5, arrows.size() - 0.5);

    int previousCut = 0;
    qreal previousProgress = areaStart;
    for (int index = 0; checkpoints != nullptr && index < checkpoints->size(); ++index) {
        const int cut = qBound(
            previousCut,
            cuts != nullptr && cuts->size() == checkpoints->size()
                ? cuts->at(index)
                : qRound(static_cast<qreal>(index + 1) * arrows.size() / checkpoints->size()),
            arrows.size());
        const qreal checkpoint = qBound<qreal>(previousProgress, checkpoints->at(index), areaEnd);
        if (arrowCentre < cut) {
            return previousProgress + (checkpoint - previousProgress)
                * (arrowCentre - previousCut) / (cut - previousCut);
        }
        previousCut = cut;
        previousProgress = checkpoint;
    }
    if (previousCut >= arrows.size()) {
        return previousProgress;
    }
    return previousProgress + (areaEnd - previousProgress)
        * (arrowCentre - previousCut) / (arrows.size() - previousCut);
}

// Wifi arrow rows lie on the centre lane. Its samples are linear in movement
// time, so projection places each row at the same progress as the moving stars.
inline qreal previewWifiArrowMovementProgress(const TimelineNoteMarker& marker, const QPointF& point)
{
    if (marker.wifiLanePoints.isEmpty()) {
        return 0.0;
    }
    const QVector<QPointF>& centreLane = marker.wifiLanePoints.at(marker.wifiLanePoints.size() / 2);
    if (centreLane.size() < 2) {
        return 0.0;
    }
    const QPointF start = centreLane.constFirst();
    const QPointF displacement = centreLane.constLast() - start;
    const qreal lengthSquared = displacement.x() * displacement.x() + displacement.y() * displacement.y();
    if (lengthSquared <= 0.0) {
        return 0.0;
    }
    const QPointF relative = point - start;
    return qBound<qreal>(
        0.0,
        (relative.x() * displacement.x() + relative.y() * displacement.y()) / lengthSquared,
        1.0);
}

}  // namespace miacode::preview::scene

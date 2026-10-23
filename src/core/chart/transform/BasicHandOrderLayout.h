#pragma once

#include "core/chart/transform/HandOrderEdit.h"
#include <QPair>

namespace miacode::chart_transform {

// Points use native slide coordinates centred at (0, 0).
HandOrderHand basicLayoutHandForPoint(const QPointF& point);
HandOrderHand basicLayoutHeadHand(const TimelineNoteMarker& marker);
QPointF basicLayoutTrackStartPoint(const TimelineNoteMarker& marker);
HandOrderHand basicLayoutTrackHand(const TimelineNoteMarker& marker);
HandOrderHand basicLayoutWifiSectorHand(const TimelineNoteMarker& marker, int sector);

struct BasicHandOrderLayoutPlan {
    HandOrderEditPlan edit;
    int preserved = 0;
    // Parse-order pairs for statically recognized cross-token one-stroke traces.
    QVector<QPair<int, int>> oneStrokeLinks;
};

// An empty selection means the complete difficulty text. Existing suffixes,
// including malformed ones, remain untouched. No execution feasibility is inferred.
BasicHandOrderLayoutPlan basicHandOrderLayoutInSelection(const QString& text, int begin, int end,
    const miacode::simai::SimaiTimingMetadata& metadata = {}, double returnSpeed = 3.0);

} // namespace miacode::chart_transform

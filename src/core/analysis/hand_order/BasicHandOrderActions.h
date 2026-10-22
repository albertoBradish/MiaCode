#pragma once
#include "core/analysis/hand_order/HandOrderJudge.h"

namespace miacode::hand_order {
struct ScheduledAction {
    ContactAction action;
    bool button = false, motion = false;
    int lane = 0;
};
// Drops only points equal to their time-linear interpolation (round-off
// tolerance); collinearity alone is insufficient when velocity changes.
void compactContactAction(ContactAction& action);

// A single finite screen contact is constructed for each physical hand.
// Logical paths remain separate for attribution, but have identical physical
// centres during overlap. Replay must still prove every logical judgement.
bool resolveHandOrderActions(const QVector<ScheduledAction>& logical,
    QVector<ScheduledAction>* physical, qint64& work);

struct PhysicalHandTransfer {
    double second = 0;
    QPointF position;
    int beforeHand = 0, afterHand = 1, ordinal = 1;
};
double physicalHandTransferCharge(const QVector<PhysicalHandTransfer>& transfers);
} // namespace miacode::hand_order

#pragma once

#include <QPointF>
#include <QRectF>
#include <QVector>

#include "core/scene/PreviewAnimatedSpriteHelpers.h"
#include "core/chart/model/TimelineData.h"

class QImage;

namespace miacode::preview::scene {

struct PreviewSpriteDescriptor {
    const QImage* image = nullptr;
    QPointF center;
    qreal width = 0.0;
    qreal height = 0.0;
    qreal rotationDegrees = 0.0;
    qreal opacity = 1.0;
    QRectF sourceRect;
    PreviewAnimatedSpriteEffect effect = PreviewAnimatedSpriteEffect::None;
    HandOrderHand handOrderHand = HandOrderHand::Unspecified;
    // Optional screen-space half-plane, shared by preview and export QSG.
    // A zero normal leaves the complete original sprite visible.
    QPointF handOrderClipOrigin;
    QPointF handOrderClipNormal;
    bool cacheable = true;
};

using PreviewSpriteDescriptors = QVector<PreviewSpriteDescriptor>;

}  // namespace miacode::preview::scene

#pragma once

#include <QString>
#include <QVector>

namespace miacode::ui {
struct HandOrderCommandSpec { QString id; QString labelKey; bool requiresSelection = true; };
inline QVector<HandOrderCommandSpec> handOrderCommandSpecs()
{
    return {{QStringLiteral("hand_order.set_left"), QStringLiteral("hand_order.set_left")},
        {QStringLiteral("hand_order.set_right"), QStringLiteral("hand_order.set_right")},
        {QStringLiteral("hand_order.set_either"), QStringLiteral("hand_order.set_either")},
        {QStringLiteral("hand_order.split_annotation"), QStringLiteral("hand_order.split_annotation")},
        {QStringLiteral("hand_order.bisect_annotation"), QStringLiteral("hand_order.bisect_annotation")},
        {QStringLiteral("hand_order.swap_hands"), QStringLiteral("hand_order.swap_hands")},
        {QStringLiteral("hand_order.split_native_slide"), QStringLiteral("hand_order.split_native_slide")}};
}
} // namespace miacode::ui

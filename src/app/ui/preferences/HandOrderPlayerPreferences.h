#pragma once

#include <QJsonObject>
#include <QVariantMap>
#include <cmath>
#include "core/analysis/hand_order/HandOrderPlayerModel.h"

namespace miacode::ui::hand_order_player_preferences {
struct Parameter {
    const char* name;
    double minimum, maximum, defaultValue;
};
inline constexpr Parameter parameters[] = {
    {"maxComfortableTapRate", hand_order::PlayerProfile::minTapRate, hand_order::PlayerProfile::maxTapRate, hand_order::PlayerProfile{}.maxComfortableTapRate},
    {"comfortableMoveSpeed", hand_order::PlayerProfile::minMoveSpeed, hand_order::PlayerProfile::maxMoveSpeed, hand_order::PlayerProfile{}.comfortableMoveSpeed},
    {"staminaCapacity", hand_order::PlayerProfile::minStamina, hand_order::PlayerProfile::maxStamina, hand_order::PlayerProfile{}.staminaCapacity},
    {"recoveryHalfLife", hand_order::PlayerProfile::minRecoveryHalfLife, hand_order::PlayerProfile::maxRecoveryHalfLife, hand_order::PlayerProfile{}.recoveryHalfLife},
    {"techniquePreference", hand_order::PlayerProfile::minTechniquePreference, hand_order::PlayerProfile::maxTechniquePreference, hand_order::PlayerProfile{}.techniquePreference},
};

inline QVariantMap read(const QJsonObject& root)
{
    const auto profile = root.value(QStringLiteral("app")).toObject()
        .value(QStringLiteral("hand_order_player")).toObject();
    QVariantMap values;
    for (const auto& parameter : parameters) {
        const QString name = QString::fromLatin1(parameter.name);
        const auto stored = profile.value(name);
        const double value = stored.toDouble(parameter.defaultValue);
        const bool valid = stored.isDouble() && std::isfinite(value)
            && value >= parameter.minimum && value <= parameter.maximum;
        values.insert(name, valid ? value : parameter.defaultValue);
    }
    return values;
}

inline bool set(QJsonObject* root, const QString& name, double value)
{
    if (root == nullptr || !std::isfinite(value)) return false;
    const Parameter* selected = nullptr;
    for (const auto& parameter : parameters)
        if (name == QLatin1String(parameter.name)) { selected = &parameter; break; }
    if (selected == nullptr) return false;
    value = qBound(selected->minimum, value, selected->maximum);
    QJsonObject app = root->value(QStringLiteral("app")).toObject();
    QJsonObject profile = app.value(QStringLiteral("hand_order_player")).toObject();
    if (profile.value(name).isDouble() && profile.value(name).toDouble() == value) return false;
    profile.insert(name, value);
    app.insert(QStringLiteral("hand_order_player"), profile);
    root->insert(QStringLiteral("app"), app);
    return true;
}

inline bool reset(QJsonObject* root)
{
    if (root == nullptr) return false;
    bool changed = false;
    for (const auto& parameter : parameters)
        changed = set(root, QString::fromLatin1(parameter.name), parameter.defaultValue) || changed;
    return changed;
}
} // namespace miacode::ui::hand_order_player_preferences

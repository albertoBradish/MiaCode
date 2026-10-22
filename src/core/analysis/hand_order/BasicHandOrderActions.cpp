#include "core/analysis/hand_order/BasicHandOrderActions.h"
#include "common/TaskCancellation.h"
#include <QLineF>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <limits>

namespace miacode::hand_order {
namespace {
constexpr double epsilon = 1e-8;
// A bounded path proposal tolerance, not an enlarged contact radius. Every
// resulting centre is replayed with the ordinary 15/240 finite contact circle.
constexpr double proposalReach = 0.20;
constexpr double buttonScreenReach = 0.32;
bool spend(qint64& work, qint64 count = 1)
{
    task::CancellationScope::checkpoint(); work -= count; return work >= 0;
}
bool enclosingCentre(const QVector<QPointF>& points, QPointF* centre, qint64& work)
{
    if (points.isEmpty()) return false;
    double best = std::numeric_limits<double>::infinity();
    const auto test = [&](QPointF candidate) {
        if (!spend(work, points.size())) return;
        double radius = 0;
        for (const auto& point : points) radius = qMax(radius, QLineF(candidate, point).length());
        if (radius < best) { best = radius; *centre = candidate; }
    };
    for (int i = 0; i < points.size(); ++i) {
        test(points.at(i));
        for (int j = i + 1; j < points.size(); ++j) {
            test((points.at(i) + points.at(j)) / 2);
            for (int k = j + 1; k < points.size(); ++k) {
                if (!spend(work)) return false;
                const auto a = points.at(i), b = points.at(j), c = points.at(k);
                const double d = 2 * (a.x() * (b.y() - c.y()) + b.x() * (c.y() - a.y()) + c.x() * (a.y() - b.y()));
                if (std::abs(d) < epsilon) continue;
                const double aa = QPointF::dotProduct(a, a), bb = QPointF::dotProduct(b, b), cc = QPointF::dotProduct(c, c);
                test({(aa * (b.y() - c.y()) + bb * (c.y() - a.y()) + cc * (a.y() - b.y())) / d,
                    (aa * (c.x() - b.x()) + bb * (a.x() - c.x()) + cc * (b.x() - a.x())) / d});
            }
        }
    }
    return work >= 0 && best <= proposalReach + epsilon;
}
}

void compactContactAction(ContactAction& action)
{
    if (action.points.size() != action.times.size() || action.points.size() < 3) return;
    QVector<QPointF> points; QVector<double> times;
    points.reserve(action.points.size()); times.reserve(action.times.size());
    for (int i = 0; i < action.points.size(); ++i) {
        task::CancellationScope::checkpoint();
        while (points.size() >= 2) {
            const int before = points.size() - 2;
            const double duration = action.times.at(i) - times.at(before);
            if (duration <= epsilon) break;
            const double progress = (times.last() - times.at(before)) / duration;
            const auto expected = points.at(before) + (action.points.at(i) - points.at(before)) * progress;
            if (QLineF(points.last(), expected).length() > 1e-12) break;
            points.removeLast(); times.removeLast();
        }
        points.append(action.points.at(i)); times.append(action.times.at(i));
    }
    action.points = std::move(points); action.times = std::move(times);
}

bool resolveHandOrderActions(const QVector<ScheduledAction>& logical,
    QVector<ScheduledAction>* physical, qint64& work)
{
    *physical = logical;
    for (int hand = 0; hand < 2; ++hand) {
        QVector<int> ids; QVector<double> times;
        for (int i = 0; i < logical.size(); ++i) if (logical.at(i).action.hand == hand) {
            ids.append(i); times.append(logical.at(i).action.start); times.append(logical.at(i).action.end);
            times += logical.at(i).action.times;
        }
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end(), [](double a, double b) { return std::abs(a - b) < epsilon; }), times.end());
        QVector<QPointF> centres; centres.reserve(times.size());
        for (double second : times) {
            QVector<QPointF> screens, buttons;
            int buttonLane = 0;
            for (int id : ids) {
                if (!spend(work)) return false;
                const auto& scheduled = logical.at(id); const auto& action = scheduled.action;
                if (second < action.start - epsilon || second > action.end + epsilon) continue;
                if (scheduled.button) {
                    if (buttonLane && buttonLane != scheduled.lane) return false;
                    buttonLane = scheduled.lane; buttons.append(contactPosition(action, second));
                } else screens.append(contactPosition(action, second));
            }
            QPointF centre;
            if (!screens.isEmpty()) {
                if (!enclosingCentre(screens, &centre, work)) return false;
                for (const auto& point : buttons)
                    if (QLineF(centre, point).length() > buttonScreenReach + epsilon) return false;
            } else if (!buttons.isEmpty()) centre = buttons.first();
            centres.append(centre);
        }
        for (int id : ids) {
            auto& action = (*physical)[id].action;
            action.points.clear(); action.times.clear();
            for (int i = 0; i < times.size(); ++i) {
                if (!spend(work)) return false;
                if (times.at(i) < action.start - epsilon || times.at(i) > action.end + epsilon) continue;
                action.times.append(times.at(i)); action.points.append(centres.at(i));
            }
            compactContactAction(action);
        }
    }
    return work >= 0;
}

double physicalHandTransferCharge(const QVector<PhysicalHandTransfer>& transfers)
{
    QMap<QString, double> charges;
    for (const auto& transfer : transfers) {
        task::CancellationScope::checkpoint();
        const QString key = QStringLiteral("%1/%2/%3/%4/%5")
            .arg(qRound64(transfer.second * 1e7)).arg(qRound64(transfer.position.x() * 1e7))
            .arg(qRound64(transfer.position.y() * 1e7)).arg(transfer.beforeHand).arg(transfer.afterHand);
        const int k = transfer.ordinal;
        const double marginal = 0.6 + 1.8 * (qMax(0, k - 1) * qMax(0, k - 1) - qMax(0, k - 2) * qMax(0, k - 2));
        charges[key] = qMax(charges.value(key), marginal);
    }
    double charge = 0; for (double value : charges) charge += value;
    return charge;
}
} // namespace miacode::hand_order

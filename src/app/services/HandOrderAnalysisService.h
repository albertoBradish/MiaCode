#pragma once
#include "app/services/AnalysisService.h"
#include "core/analysis/hand_order/BasicHandOrder.h"
#include "editor/SimaiTextEditPolicy.h"
#include "app/services/JobProgressService.h"
#include <QPointer>
#include <QVariantList>
#include "common/LocalizedText.h"

namespace miacode {
class HandOrderAnalysisService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool pending READ pending NOTIFY changed)
    Q_PROPERTY(bool canApply READ canApply NOTIFY changed)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(bool overwriteExisting READ overwriteExisting WRITE setOverwriteExisting NOTIFY changed)
    Q_PROPERTY(bool highConfidenceOnly READ highConfidenceOnly WRITE setHighConfidenceOnly NOTIFY changed)
    Q_PROPERTY(int inputPreference READ inputPreference WRITE setInputPreference NOTIFY changed)
public:
    HandOrderAnalysisService(ChartWorkspace& workspace, AnalysisService* analysis = nullptr, QObject* parent = nullptr);
    ~HandOrderAnalysisService() override;
    void setJobProgress(JobProgressService* progress);
    bool pending() const { return pending_; }
    bool canApply() const;
    QVariantList rows() const;
    QString message() const { return message_.text(); }
    void retranslate();
    bool overwriteExisting() const { return parameters_.overwriteExisting; }
    bool highConfidenceOnly() const { return parameters_.highConfidenceOnly; }
    int inputPreference() const { return parameters_.inputPreference; }
    void setOverwriteExisting(bool value);
    void setHighConfidenceOnly(bool value);
    void setInputPreference(int value);
    void setReturnSpeed(double speed);
    void setPlayerProfile(const hand_order::PlayerProfile& profile);
    bool request(const QString& text, int begin, int end);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void setBeamWidth(int width);
    void setParameters(const hand_order::BasicParameters& parameters);
    editor::SimaiTextEditResult takeEdit(const QString& currentText);
    Q_INVOKABLE QVariantMap navigationRange(int row, const QString& currentText) const;
signals:
    void changed();
private:
    struct Request {
        ParsedChartSnapshot snapshot;
        ChartWorkspaceSnapshot identity;
        quint64 sequence = 0, parameterRevision = 0;
        int begin = 0, end = 0;
        hand_order::BasicParameters parameters;
    };
    bool isCurrent(const Request& request) const;
    void invalidate(const LocalizedText& message);
    void dispatch();
    QPointer<JobProgressService> progress_;
    quint64 progressToken_ = 0;
    void endProgress();
    ChartWorkspace* workspace_;
    AnalysisService* analysis_;
    hand_order::BasicParameters parameters_;
    quint64 sequence_ = 0, parameterRevision_ = 1;
    bool pending_ = false, running_ = false, taken_ = false;
    LocalizedText message_;
    task::CancellationFlag cancellation_;
    std::optional<Request> queued_, completed_;
    hand_order::BasicResult result_;
};
} // namespace miacode

#pragma once
#include "core/chart/parser/SimaiParser.h"
#include "core/chart/transform/HandOrderEdit.h"
#include "core/analysis/hand_order/HandOrderPlayerModel.h"

namespace miacode::hand_order {
inline constexpr int basicAlgorithmVersion = 10;
inline constexpr int basicJudgeProfileRevision = 2;
struct BasicParameters {
    static constexpr double minReturnSpeed = 1.0;
    static constexpr double maxReturnSpeed = 8.0;
    static constexpr qint64 maxAllowedWorkUnits = 300000000;
    int version = basicAlgorithmVersion;
    int judgeProfileRevision = basicJudgeProfileRevision;
    int beamWidth = 64;
    int maxTasks = 512;
    int counterfactualBudget = 64;
    double contextSeconds = 1.0;
    double pressSeconds = 0.025;
    double returnSpeed = 3.0;
    PlayerProfile player;
    qint64 maxWorkUnits = 200000000;
    bool overwriteExisting = false;
    bool highConfidenceOnly = false;
    // 0: exterior preferred, 1: screen preferred, 2: mixed.
    int inputPreference = 0;
};
struct BasicCause {
    int tokenStart = -1;
    int branch = 0;
    QString kind;
    QString pad;
    double second = 0;
    double progress = 0;
    int handMask = 0;
};
struct BasicHandoffEvidence {
    int branch = 0, handoffCount = 0;
    int sector = -1;
    bool baselineFound = false;
    double baselineCost = 0, baselinePeak = 0;
    QString reason;
};
struct BasicCounterfactualEvidence {
    QString component;
    double progress = 0;
    HandOrderHand alternativeHand = HandOrderHand::Unspecified;
    bool searched = false, found = false, limited = false;
    double signedCostGap = 0, score = 0;
    int signedRiskTierGap = 0, signedPeakTierGap = 0;
};
struct BasicRow {
    int start = 0, end = 0, line = 1, col = 1;
    bool context = false;
    QString originalAnnotation;
    QString status;
    QString reason;
    HandOrderHand head = HandOrderHand::Unspecified;
    HandOrderHand track = HandOrderHand::Unspecified;
    double cost = 0, peakSpeed = 0;
    double playerCost = 0, peakTapRate = 0, peakLoad = 0;
    QVector<int> lanes;
    QVector<HandOrderHand> noteHands;
    bool hasCounterfactual = false;
    QString counterfactualComponent;
    HandOrderHand counterfactualHand = HandOrderHand::Unspecified;
    double signedCostGap = 0;
    int signedPeakTierGap = 0;
    QVector<QVector<HandOrderTrackStop>> trackIntervals;
    QVector<HandOrderWifiMode> wifiModes;
    QVector<QVector<QVector<HandOrderTrackStop>>> wifiSectors;
    bool fullReplayVerified = false;
    bool hasCandidate = false;
    int dependencyUnit = -1;
    double confidence = 0;
    QString confidenceLevel = QStringLiteral("insufficient_evidence");
    QString pressureLevel;
    QString inputChannel;
    QVector<BasicCounterfactualEvidence> counterfactualEvidence;
    QVector<BasicCause> causes;
    QString judgementKind, judgementQuality, conditions, handoffReason;
    double judgementSecond = 0, judgementMargin = 0;
    int handoffCount = 0;
    double oneHandoffCost = 0, oneHandoffPeak = 0;
    QVector<BasicHandoffEvidence> handoffEvidence;
};
struct BasicWindowEvidence {
    int start = 0, end = 0, tasks = 0;
    qint64 workUsed = 0;
    QString outcome;
};
struct BasicResult {
    QVector<BasicRow> rows;
    QVector<chart_transform::HandOrderEditPlan::Patch> patches;
    bool truncated = false;
    bool evidenceTruncated = false;
    QString error;
    int contextObjects = 0;
    int algorithmVersion = basicAlgorithmVersion;
    int judgeProfileRevision = basicJudgeProfileRevision;
    qint64 workUsed = 0;
    int includedObjects = 0, auditedObjects = 0, windowCount = 0, maxWindowTasks = 0;
    bool taskLimited = false, workLimited = false;
    QVector<BasicWindowEvidence> searchWindows;
};
double basicOrdinaryAssociationWeight(double seconds, int intermediateNotes);
// Additive cost over fixed-time motion segments. Coordinates use field-radius units.
double basicMotionCost(const QVector<QPointF>& points, const QVector<double>& segmentSeconds,
                       double* peakSpeed = nullptr);
BasicResult inferBasicHandOrder(const QString& text, const SimaiParseResult& parsed,
    int begin, int end, const BasicParameters& parameters = {},
    const miacode::simai::SimaiTimingMetadata& metadata = {});
} // namespace miacode::hand_order

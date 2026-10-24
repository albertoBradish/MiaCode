#include "app/services/HandOrderAnalysisService.h"
#include <cmath>
#include <QPointer>
#include <QThreadPool>
#include <algorithm>

namespace miacode {
HandOrderAnalysisService::HandOrderAnalysisService(ChartWorkspace& workspace, AnalysisService* analysis, QObject* parent)
    : QObject(parent), workspace_(&workspace), analysis_(analysis)
{
    connect(&workspace, &ChartWorkspace::changed, this, [this] {
        invalidate(localizedText("hand_order.auto.stale"));
    });
}
void HandOrderAnalysisService::retranslate()
{
    // Presentation refresh never invalidates the current solution or request.
    emit changed();
}
void HandOrderAnalysisService::setJobProgress(JobProgressService* progress)
{
    if (progress_) disconnect(progress_, nullptr, this, nullptr);
    endProgress(); progress_ = progress;
    if (progress_) connect(progress_, &JobProgressService::cancellationRequested, this, [this](quint64 token) {
        if (token == progressToken_) cancel();
    });
}
void HandOrderAnalysisService::endProgress()
{
    if (progress_ && progressToken_ != 0 && progress_->token() == progressToken_) progress_->end();
    progressToken_ = 0;
}
HandOrderAnalysisService::~HandOrderAnalysisService()
{
    endProgress();
    if (cancellation_) cancellation_->store(true, std::memory_order_relaxed);
}
void HandOrderAnalysisService::invalidate(const LocalizedText& message)
{
    ++sequence_;
    endProgress();
    if (cancellation_) cancellation_->store(true, std::memory_order_relaxed);
    pending_ = false; queued_.reset(); completed_.reset(); result_ = {};
    taken_ = false; message_ = message;
    emit changed();
}
void HandOrderAnalysisService::cancel() { invalidate(localizedText("hand_order.auto.cancelled")); }
void HandOrderAnalysisService::setBeamWidth(int width)
{
    width = qBound(8, width, 128);
    if (parameters_.beamWidth == width) return;
    parameters_.beamWidth = width; ++parameterRevision_;
    invalidate(localizedText("hand_order.auto.stale"));
}
void HandOrderAnalysisService::setParameters(const hand_order::BasicParameters& parameters)
{
    parameters_ = parameters; ++parameterRevision_;
    invalidate(localizedText("hand_order.auto.stale"));
}
void HandOrderAnalysisService::setOverwriteExisting(bool value)
{
    if (parameters_.overwriteExisting == value) return;
    auto parameters = parameters_; parameters.overwriteExisting = value; setParameters(parameters);
}
void HandOrderAnalysisService::setHighConfidenceOnly(bool value)
{
    if (parameters_.highConfidenceOnly == value) return;
    auto parameters = parameters_; parameters.highConfidenceOnly = value; setParameters(parameters);
}
void HandOrderAnalysisService::setInputPreference(int value)
{
    value = qBound(0, value, 2);
    if (parameters_.inputPreference == value) return;
    auto parameters = parameters_; parameters.inputPreference = value; setParameters(parameters);
}
void HandOrderAnalysisService::setReturnSpeed(double speed)
{
    if (!std::isfinite(speed)) return;
    speed = qBound(hand_order::BasicParameters::minReturnSpeed, speed,
        hand_order::BasicParameters::maxReturnSpeed);
    if (qFuzzyCompare(parameters_.returnSpeed, speed)) return;
    auto parameters = parameters_; parameters.returnSpeed = speed; setParameters(parameters);
}

void HandOrderAnalysisService::setSweepThreshold(int threshold)
{
    threshold = qBound(chart::handOrderMinSweepThreshold, threshold,
        chart::handOrderMaxSweepThreshold);
    if (parameters_.sweepThreshold == threshold) return;
    auto parameters = parameters_; parameters.sweepThreshold = threshold; setParameters(parameters);
}

void HandOrderAnalysisService::setColorUnresolved(bool enabled)
{
    if (parameters_.colorUnresolved == enabled) return;
    auto parameters = parameters_; parameters.colorUnresolved = enabled; setParameters(parameters);
}
void HandOrderAnalysisService::setPlayerProfile(const hand_order::PlayerProfile& profile)
{
    if (!hand_order::validPlayerProfile(profile)) return;
    const auto& old = parameters_.player;
    if (old.maxComfortableTapRate == profile.maxComfortableTapRate
        && old.comfortableMoveSpeed == profile.comfortableMoveSpeed
        && old.staminaCapacity == profile.staminaCapacity
        && old.recoveryHalfLife == profile.recoveryHalfLife
        && old.techniquePreference == profile.techniquePreference
        && old.postureWeight == profile.postureWeight) return;
    auto parameters = parameters_; parameters.player = profile; setParameters(parameters);
}
void HandOrderAnalysisService::setMaxWorkUnits(qint64 units)
{
    units = qBound(qint64(1000), units, hand_order::BasicParameters::maxAllowedWorkUnits);
    if (parameters_.maxWorkUnits == units) return;
    auto parameters = parameters_; parameters.maxWorkUnits = units; setParameters(parameters);
}
bool HandOrderAnalysisService::isCurrent(const Request& request) const
{
    const auto current = workspace_->snapshot();
    const auto* difficulty = workspace_->document().difficulty(current.activeDifficultyId);
    return current.hasDocument && difficulty && request.sequence == sequence_
        && request.parameterRevision == parameterRevision_ && request.parameters.version == parameters_.version
        && request.parameters.judgeProfileRevision == parameters_.judgeProfileRevision
        && current.revision == request.identity.revision
        && current.documentOpenGeneration == request.identity.documentOpenGeneration
        && current.activeDifficultyId == request.identity.activeDifficultyId
        && current.filePath == request.identity.filePath && current.sourceText == request.identity.sourceText
        && difficulty->chart == request.snapshot.chartText
        && simai::buildTimingMetadata(workspace_->document()) == request.snapshot.timingMetadata;
}
bool HandOrderAnalysisService::request(const QString& text, int begin, int end)
{
    invalidate({});
    if (parameters_.version != hand_order::basicAlgorithmVersion
        || parameters_.judgeProfileRevision != hand_order::basicJudgeProfileRevision) {
        message_ = localizedText("hand_order.auto.invalid_parameters"); emit changed(); return false;
    }
    Request request;
    request.identity = workspace_->snapshot();
    const auto* difficulty = workspace_->document().difficulty(request.identity.activeDifficultyId);
    if (!request.identity.hasDocument || !difficulty || difficulty->chart != text) {
        message_ = localizedText("hand_order.auto.stale"); emit changed(); return false;
    }
    request.sequence = sequence_; request.parameterRevision = parameterRevision_;
    request.parameters = parameters_;
    request.snapshot.chartText = text;
    request.snapshot.revision = request.identity.revision;
    request.snapshot.documentOpenGeneration = request.identity.documentOpenGeneration;
    request.snapshot.difficultyId = request.identity.activeDifficultyId;
    request.snapshot.timingMetadata = simai::buildTimingMetadata(workspace_->document());
    if (analysis_) {
        const auto& shared = analysis_->parsedSnapshot();
        if (shared.available && shared.revision == request.identity.revision
            && shared.documentOpenGeneration == request.identity.documentOpenGeneration
            && shared.difficultyId == request.identity.activeDifficultyId && shared.chartText == text
            && shared.timingMetadata == request.snapshot.timingMetadata) request.snapshot = shared;
    }
    request.begin = qBound(0, qMin(begin, end), int(text.size()));
    request.end = qBound(request.begin, qMax(begin, end), int(text.size()));
    if (request.begin == request.end) { request.begin = 0; request.end = text.size(); }
    pending_ = true; taken_ = false;
    if (progress_ && !progress_->active()) {
        progressToken_ = progress_->begin(LocalizedText::translated("hand_order.analyze"),
            LocalizedText::translated("hand_order.auto.running"), true);
        progress_->reportIndeterminate(LocalizedText::translated("hand_order.auto.running"));
    }
    message_ = localizedText("hand_order.auto.running");
    queued_ = std::move(request);
    emit changed(); dispatch(); return true;
}
void HandOrderAnalysisService::dispatch()
{
    if (running_ || !queued_) return;
    Request request = std::move(*queued_); queued_.reset(); running_ = true;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = cancellation_;
    QPointer<HandOrderAnalysisService> guard(this);
    QThreadPool::globalInstance()->start([guard, request = std::move(request), cancellation]() mutable {
        std::optional<hand_order::BasicResult> result;
        task::CancellationScope scope(cancellation);
        try {
            task::CancellationScope::check();
            // Strict validation still runs even when the shared timeline parse is reusable.
            const auto strict = SimaiParser::validateSyntax(request.snapshot.chartText, request.snapshot.timingMetadata);
            result = hand_order::inferBasicHandOrder(request.snapshot.chartText, strict,
                request.begin, request.end, request.parameters, request.snapshot.timingMetadata);
            task::CancellationScope::check();
        } catch (const task::Cancelled&) { result.reset(); }
        if (!guard) return;
        QMetaObject::invokeMethod(guard.data(), [guard, cancellation, request = std::move(request), result = std::move(result)]() mutable {
            if (!guard) return;
            guard->running_ = false;
            if (result && result->algorithmVersion == request.parameters.version
                && result->judgeProfileRevision == request.parameters.judgeProfileRevision
                && !cancellation->load(std::memory_order_relaxed) && guard->isCurrent(request)) {
                guard->completed_ = std::move(request); guard->result_ = std::move(*result); guard->pending_ = false; guard->endProgress();
                int preserved = 0, conflicts = 0, undecided = 0, unsupported = 0;
                for (const auto& row : guard->result_.rows) {
                    preserved += row.status == "preserved";
                    conflicts += row.status == "conflict";
                    undecided += row.status == "undecided" || row.status == "not_found"
                        || row.status == "heuristic";
                    unsupported += row.status == "unsupported" || row.status == "invalid" || row.status == "skipped";
                }
                guard->message_ = localizedText("hand_order.auto.summary").arg(guard->result_.patches.size())
                    .arg(preserved).arg(conflicts).arg(undecided).arg(unsupported).arg(guard->result_.contextObjects)
                    + (guard->result_.truncated ? localizedText("hand_order.auto.truncated") : LocalizedText{})
                    + (guard->result_.evidenceTruncated ? localizedText("hand_order.auto.evidence_truncated") : LocalizedText{});
                guard->message_ = guard->message_ + localizedText("hand_order.auto.search_scope")
                    .arg(guard->result_.includedObjects).arg(guard->result_.windowCount).arg(guard->result_.maxWindowTasks)
                    .arg(guard->result_.workUsed).arg(guard->parameters_.maxWorkUnits).arg(guard->result_.auditedObjects);
                if (guard->parameters_.highConfidenceOnly) guard->message_ = guard->message_ + localizedText("hand_order.auto.high_confidence_closed");
                if (!guard->result_.error.isEmpty()) guard->message_ = localizedText("hand_order.auto.invalid_parameters");
                emit guard->changed();
            } else if (result && !cancellation->load(std::memory_order_relaxed) && guard->isCurrent(request)) {
                guard->invalidate(localizedText("hand_order.auto.invalid_output"));
            }
            guard->dispatch();
        }, Qt::QueuedConnection);
    });
}
bool HandOrderAnalysisService::canApply() const
{
    return !pending_ && !taken_ && completed_ && !result_.patches.isEmpty()
        && result_.algorithmVersion == parameters_.version && result_.judgeProfileRevision == parameters_.judgeProfileRevision
        && isCurrent(*completed_);
}
QVariantMap HandOrderAnalysisService::navigationRange(int row, const QString& currentText) const
{
    if (pending_ || !completed_ || row < 0 || row >= result_.rows.size()
        || currentText != completed_->snapshot.chartText || !isCurrent(*completed_)) return {};
    const auto& target = result_.rows.at(row);
    return {{"start", target.start}, {"end", target.end}, {"line", target.line}, {"col", target.col}};
}
QVariantList HandOrderAnalysisService::rows() const
{
    QVariantList rows;
    const QString source = completed_ ? completed_->snapshot.chartText : QString();
    QVector<int> sourceLines{0};
    for (int i = 0; i < source.size(); ++i) if (source.at(i) == '\n') sourceLines.append(i + 1);
    for (const auto& row : result_.rows) {
        const auto handName = [](HandOrderHand hand) {
            return hand == HandOrderHand::Left ? qtTrId("hand_order.left")
                : hand == HandOrderHand::Right ? qtTrId("hand_order.right") : qtTrId("hand_order.unspecified");
        };
        QStringList noteAssignments;
        for (int i = 0; i < row.lanes.size(); ++i)
            noteAssignments.append(QString::number(row.lanes.at(i)) + QLatin1Char('=') + handName(row.noteHands.at(i)));
        QStringList intervals, evidence, handoffs, alternatives;
        QString counterfactualComponentText;
        QVariantList intervalData, causeData, handoffData, counterfactualData;
        for (int branch = 0; branch < row.trackIntervals.size(); ++branch) {
            const auto mode = row.wifiModes.value(branch, HandOrderWifiMode::Legacy);
            const auto appendStops = [&](const QVector<HandOrderTrackStop>& stops, int sector) {
                for (int i = 0; i < stops.size(); ++i) {
                    const double last = i + 1 < stops.size() ? stops.at(i + 1).progress : 1.0;
                    QString label = qtTrId("hand_order.auto.interval").arg(branch + 1)
                        .arg(stops.at(i).progress * 100, 0, 'f', 1).arg(last * 100, 0, 'f', 1).arg(handName(stops.at(i).hand));
                    if (sector >= 0) label = qtTrId("hand_order.auto.wifi_sector").arg(sector + 1).arg(label);
                    else if (mode == HandOrderWifiMode::Single) label = qtTrId("hand_order.auto.wifi_single").arg(label);
                    intervals.append(label);
                    intervalData.append(QVariantMap{{"branch", branch}, {"sector", sector}, {"wifiMode", int(mode)},
                        {"start", stops.at(i).progress}, {"end", last}, {"hand", handName(stops.at(i).hand)}});
                }
            };
            if (mode == HandOrderWifiMode::Split) {
                const auto sectors = row.wifiSectors.value(branch);
                for (int sector = 0; sector < sectors.size(); ++sector) appendStops(sectors.at(sector), sector);
            } else appendStops(row.trackIntervals.at(branch), -1);
        }
        for (const auto& cause : row.causes) {
            const QString hands = cause.handMask == 1 ? handName(HandOrderHand::Left)
                : cause.handMask == 2 ? handName(HandOrderHand::Right) : qtTrId("hand_order.auto.both_contributors");
            const int at = qBound(0, cause.tokenStart, int(source.size()));
            const int sourceLine = int(std::upper_bound(sourceLines.begin(), sourceLines.end(), at) - sourceLines.begin());
            const int sourceCol = at - sourceLines.at(sourceLine - 1) + 1;
            const QString kind = cause.kind == "slide" ? qtTrId("hand_order.auto.action.slide")
                : cause.kind == "touch_hold" ? qtTrId("hand_order.auto.action.touch_hold")
                : cause.kind == "button" ? qtTrId("hand_order.auto.action.button")
                : cause.kind == "screen_button" ? qtTrId("hand_order.auto.action.screen_button") : qtTrId("hand_order.auto.action.touch");
            evidence.append(qtTrId("hand_order.auto.cause").arg(QStringLiteral("%1:%2").arg(sourceLine).arg(sourceCol)).arg(cause.branch + 1).arg(kind)
                .arg(cause.pad).arg(cause.second, 0, 'f', 4).arg(cause.progress * 100, 0, 'f', 1).arg(hands));
            causeData.append(QVariantMap{{"sourceStart", cause.tokenStart}, {"branch", cause.branch}, {"kind", cause.kind},
                {"pad", cause.pad}, {"second", cause.second}, {"progress", cause.progress}, {"handMask", cause.handMask},
                {"sourceLine", sourceLine}, {"sourceCol", sourceCol}});
        }
        for (const auto& proof : row.counterfactualEvidence) {
            QString component;
            if (proof.component == "head") component = qtTrId("hand_order.auto.component.head");
            else if (proof.component == "touch") component = qtTrId("hand_order.auto.component.touch");
            else if (proof.component.startsWith("note")) component = qtTrId("hand_order.auto.component.note").arg(proof.component.mid(4));
            else {
                const auto parts = proof.component.split(QLatin1Char(':'));
                component = qtTrId("hand_order.auto.component.track").arg(parts.value(1).toInt() + 1).arg(proof.progress * 100, 0, 'f', 1);
                bool sectorOk = false;
                const int sector = parts.value(3).toInt(&sectorOk);
                if (parts.size() == 4 && parts.value(2) == "sector" && sectorOk && sector >= 0)
                    component = qtTrId("hand_order.auto.wifi_sector").arg(sector + 1).arg(component);
            }
            if (proof.found && proof.component == row.counterfactualComponent) counterfactualComponentText = component;
            const QString state = !proof.searched || proof.limited ? qtTrId("hand_order.auto.evidence_limited")
                : proof.found ? qtTrId("hand_order.auto.evidence_alternative") : qtTrId("hand_order.auto.evidence_no_alternative");
            QString summary = component + QStringLiteral(": ") + state;
            if (proof.found) summary += qtTrId("hand_order.auto.evidence_gap").arg(proof.signedRiskTierGap)
                .arg(proof.signedPeakTierGap).arg(proof.signedCostGap, 0, 'f', 3);
            alternatives.append(summary);
            counterfactualData.append(QVariantMap{{"component", proof.component}, {"progress", proof.progress},
                {"alternativeHand", handName(proof.alternativeHand)}, {"searched", proof.searched}, {"found", proof.found},
                {"limited", proof.limited}, {"signedCostGap", proof.signedCostGap}, {"signedRiskTierGap", proof.signedRiskTierGap},
                {"signedPeakTierGap", proof.signedPeakTierGap}, {"score", proof.score}});
        }
        for (const auto& proof : row.handoffEvidence) {
            const QString reason = proof.reason.isEmpty() ? QString() : qtTrId(("hand_order.auto.reason." + proof.reason).toUtf8().constData());
            handoffData.append(QVariantMap{{"branch", proof.branch}, {"sector", proof.sector}, {"handoffCount", proof.handoffCount},
                {"baselineFound", proof.baselineFound}, {"baselineCost", proof.baselineCost}, {"baselinePeak", proof.baselinePeak},
                {"reason", proof.reason}, {"reasonText", reason}});
            if (proof.handoffCount <= 0) continue;
            QString summary = qtTrId("hand_order.auto.handoff_branch").arg(proof.branch + 1).arg(proof.handoffCount).arg(reason);
            if (proof.sector >= 0) summary = qtTrId("hand_order.auto.wifi_sector").arg(proof.sector + 1).arg(summary);
            if (proof.baselineFound) summary += qtTrId("hand_order.auto.handoff_baseline")
                .arg(proof.baselinePeak, 0, 'f', 2).arg(proof.baselineCost, 0, 'f', 3);
            handoffs.append(summary);
        }
        rows.append(QVariantMap{{"line", row.line}, {"col", row.col}, {"status", row.status},
            {"context", row.context}, {"originalAnnotation", row.originalAnnotation},
            {"hasCandidate", row.hasCandidate},
            {"hasAssignments", row.hasCandidate && (!row.noteHands.isEmpty() || row.head != HandOrderHand::Unspecified || row.track != HandOrderHand::Unspecified || !row.wifiSectors.isEmpty())},
            {"statusText", qtTrId(("hand_order.auto.status." + row.status).toUtf8().constData())},
            {"reason", qtTrId(("hand_order.auto.reason." + row.reason).toUtf8().constData())},
            {"noteAssignments", noteAssignments.join(QStringLiteral(" / "))}, {"head", handName(row.head)}, {"track", handName(row.track)},
            {"cost", row.cost}, {"peakSpeed", row.peakSpeed}, {"inputChannel", row.inputChannel}, {"pressureLevel", row.pressureLevel},
            {"playerCost", row.playerCost}, {"peakTapRate", row.peakTapRate}, {"peakLoad", row.peakLoad},
            {"pressureText", row.pressureLevel.isEmpty() ? QString() : qtTrId(("hand_order.auto.pressure." + row.pressureLevel).toUtf8().constData())}, {"hasCounterfactual", row.hasCounterfactual},
            {"dependencyUnit", row.dependencyUnit}, {"confidence", row.confidence}, {"confidenceLevel", row.confidenceLevel},
            {"confidenceText", qtTrId("hand_order.auto.confidence_text").arg(qRound(row.confidence * 100))
                .arg(qtTrId(("hand_order.auto.confidence." + row.confidenceLevel).toUtf8().constData()))},
            {"counterfactualData", counterfactualData}, {"counterfactualSummary", alternatives.join(QStringLiteral("; "))},
            {"intervals", intervals.join(QStringLiteral(" / "))}, {"intervalData", intervalData},
            {"evidence", evidence.join(QStringLiteral("; "))}, {"causeData", causeData},
            {"judgementKind", row.judgementKind}, {"judgementQuality", row.judgementQuality},
            {"judgementKindText", row.judgementKind.isEmpty() ? QString() : qtTrId(("hand_order.auto.reason." + row.judgementKind).toUtf8().constData())},
            {"judgementQualityText", row.judgementQuality.isEmpty() ? QString() : qtTrId(("hand_order.auto.quality." + row.judgementQuality).toUtf8().constData())},
            {"fullReplayVerified", row.fullReplayVerified}, {"judgementSecond", row.judgementSecond}, {"judgementMargin", row.judgementMargin},
            {"conditions", row.conditions.isEmpty() ? QString() : qtTrId("hand_order.auto.conditions_text")
                + (row.conditions.contains(QStringLiteral("adjusted_slide_input_timing"))
                    ? QStringLiteral(" · ") + qtTrId("hand_order.auto.adjusted_slide_input_timing") : QString())},
            {"handoffCount", row.handoffCount},
            {"handoffReason", row.handoffReason.isEmpty() ? QString() : qtTrId(("hand_order.auto.reason." + row.handoffReason).toUtf8().constData())},
            {"oneHandoffCost", row.oneHandoffCost}, {"oneHandoffPeak", row.oneHandoffPeak},
            {"handoffEvidence", handoffData}, {"handoffSummary", handoffs.join(QStringLiteral("; "))},
            {"counterfactualComponent", counterfactualComponentText},
            {"counterfactualHand", handName(row.counterfactualHand)}, {"signedCostGap", row.signedCostGap}, {"signedPeakTierGap", row.signedPeakTierGap}});
    }
    return rows;
}
editor::SimaiTextEditResult HandOrderAnalysisService::takeEdit(const QString& text)
{
    editor::SimaiTextEditResult edit;
    if (!canApply() || text != completed_->snapshot.chartText) {
        invalidate(localizedText("hand_order.auto.stale")); return edit;
    }
    const auto& patches = result_.patches;
    const int begin = patches.first().start, end = patches.last().end;
    QString replacement = text.mid(begin, end - begin);
    int delta = 0;
    for (const auto& patch : patches) {
        if (patch.start < completed_->begin || patch.end > completed_->end) return edit;
        replacement.replace(patch.start - begin + delta, patch.end - patch.start, patch.text);
        delta += patch.text.size() - (patch.end - patch.start);
    }
    const auto original = SimaiParser::validateSyntax(text, completed_->snapshot.timingMetadata);
    const auto verified = SimaiParser::validateSyntax(text.left(begin) + replacement + text.mid(end),
        completed_->snapshot.timingMetadata);
    if (!verified.ok || verified.annotationErrors.size() > original.annotationErrors.size()) {
        message_ = localizedText("hand_order.auto.invalid_output"); emit changed(); return edit;
    }
    edit.consumed = true;
    edit.transaction = {begin, begin + int(replacement.size()), true, begin, end, replacement, -1};
    taken_ = true; emit changed();
    return edit;
}
} // namespace miacode

#include "app/services/HandOrderAnalysisService.h"
#include "core/chart/parser/HandOrderAnnotationSyntax.h"
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QTextStream>
#include <QTranslator>
using namespace miacode;
void settle(HandOrderAnalysisService& service) {
    for (int n = 0; service.pending() && n < 500; ++n) QTest::qWait(10);
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    bool ok = true;
    const auto expect = [&](bool pass, const char* message) { QTextStream(pass ? stdout : stderr)
        << (pass ? "PASS: " : "FAIL: ") << message << '\n'; ok &= pass; };
    ChartWorkspace workspace;
    workspace.openSource("&inote_5=(120){4}6,1,\n&inote_6=(120){4}2,\n", {}, 5);
    HandOrderAnalysisService service(workspace);
    const auto chart = [&] { return workspace.document().difficulty(workspace.snapshot().activeDifficultyId)->chart; };
    QString source = chart();
    expect(service.request(source, 0, 0), "empty selection requests current difficulty");
    settle(service);
    expect(!service.pending() && service.canApply() && service.rows().size() == 2,
        "real asynchronous inference publishes a current preview");
    const auto location = service.navigationRange(0, source);
    expect(location.value("start").toInt() == source.indexOf("6,")
        && location.value("end").toInt() == source.indexOf("6,") + 1,
        "navigation resolves the exact UTF-16 source token from the current result identity");
    workspace.replaceActiveDifficultyChart(source + " ");
    workspace.replaceActiveDifficultyChart(source);
    expect(!service.canApply() && !service.takeEdit(source).consumed && service.navigationRange(0, source).isEmpty(),
        "editing then returning to identical text still rejects the old revision");
    service.request(source, 0, 0); service.cancel(); QTest::qWait(50);
    expect(!service.pending() && !service.canApply(), "cancel prevents an in-flight result from becoming applicable");
    service.request(source, 0, 0); workspace.selectDifficulty(6); settle(service); QTest::qWait(30);
    expect(!service.canApply(), "difficulty switches reject pending work");
    workspace.selectDifficulty(5); source = chart();
    service.request(source, 0, 0);
    workspace.openSource("&inote_5=" + source + "\n", {}, 5);
    QTest::qWait(30);
    expect(!service.canApply(), "reopening the same text with a new document generation rejects old work");
    source = chart(); service.request(source, 0, 0); settle(service);
    service.setBeamWidth(32);
    expect(!service.canApply(), "parameter revision invalidates previous evidence");
    const int second = source.indexOf("1,");
    service.request(source, 0, source.indexOf(','));
    service.request(source, second, second + 1); settle(service);
    int writableRows = 0, contextRows = 0; bool latestTarget = false;
    for (const auto& item : service.rows()) {
        const auto row = item.toMap();
        contextRows += row.value("context").toBool();
        writableRows += !row.value("context").toBool();
        latestTarget |= !row.value("context").toBool() && row.value("col").toInt() == second + 1;
    }
    expect(service.canApply() && writableRows == 1 && contextRows == 1 && latestTarget,
        "latest sequence owns the preview after rapid requests");
    const auto edit = service.takeEdit(source);
    expect(edit.consumed && edit.transaction.hasEdit && edit.transaction.replacementStart == second
        && edit.transaction.replacementEnd == second + 1 && !service.canApply(),
        "applying produces one scoped transaction and consumes the result once");
    QString modified = source; modified.replace(edit.transaction.replacementStart,
        edit.transaction.replacementEnd - edit.transaction.replacementStart, edit.transaction.replacementText);
    expect(modified.left(second) == source.left(second) && modified.mid(second).startsWith("1~")
        && SimaiParser::validateSyntax(modified).annotationErrors.isEmpty(), "application preserves the read-only context and passes strict syntax");
    workspace.openSource("&inote_5=(120){4}5h[1:1]~m,6,\n", {}, 5);
    source = chart(); const int targetSix = source.indexOf("6,");
    service.request(source, targetSix, targetSix + 1); settle(service);
    bool shownConditional = false;
    for (const auto& item : service.rows()) {
        const auto row = item.toMap();
        shownConditional |= row.value("context").toBool() && row.value("originalAnnotation") == "~m"
            && row.value("hasAssignments").toBool();
    }
    const auto conditionalEdit = service.takeEdit(source);
    QString conditionalText = source;
    if (conditionalEdit.transaction.hasEdit) conditionalText.replace(conditionalEdit.transaction.replacementStart,
        conditionalEdit.transaction.replacementEnd - conditionalEdit.transaction.replacementStart,
        conditionalEdit.transaction.replacementText);
    expect(shownConditional && conditionalEdit.consumed && conditionalEdit.transaction.replacementStart == targetSix
        && conditionalText.left(targetSix) == source.left(targetSix) && conditionalText.contains("5h[1:1]~m"),
        "preview shows read-only Either assumptions while application preserves its exact source and patch boundary");
    workspace.openSource("&inote_5=(120){4}6,,,,,,,,,,1~z,\n", {}, 5);
    source = chart(); const int six = source.indexOf("6,");
    service.request(source, six, six + 1); settle(service);
    expect(service.canApply() && service.takeEdit(source).consumed,
        "an unrelated existing invalid annotation is preserved instead of misreported as a new output error");
    hand_order::BasicParameters old; old.version = 1;
    service.setParameters(old);
    expect(!service.request(source, 0, 0) && !service.pending() && !service.canApply(),
        "service rejects an old algorithm request without leaving a pending or applicable preview");
    old = {}; old.judgeProfileRevision = 0; service.setParameters(old);
    expect(!service.request(source, 0, 0) && !service.canApply(), "service rejects an old judging profile");
    service.setParameters({});
    workspace.openSource("&inote_5=|| provenance\n(120){4}1-5[2:1]~r,,B1/C/B3,\n", {}, 5); source = chart();
    service.request(source, 0, 0); settle(service);
    bool completeCauses = false;
    bool locatedCauses = true;
    for (const auto& item : service.rows()) {
        const auto row = item.toMap();
        completeCauses |= row.value("judgementKind") == "group_carry" && row.value("status") == "undecided"
            && row.value("causeData").toList().size() >= 2 && !row.value("conditions").toString().isEmpty();
        if (!row.value("judgementKind").toString().isEmpty()) locatedCauses &= !row.value("judgementKindText").toString().isEmpty()
            && !row.value("judgementQualityText").toString().isEmpty();
        for (const auto& cause : row.value("causeData").toList()) {
            const auto data = cause.toMap();
            locatedCauses &= data.value("sourceLine").toInt() == 2
                && data.value("sourceCol").toInt() == data.value("sourceStart").toInt() - source.indexOf('\n');
        }
    }
    const auto touchEdit = service.takeEdit(source);
    QString touchText = source;
    if (touchEdit.transaction.hasEdit) touchText.replace(touchEdit.transaction.replacementStart,
        touchEdit.transaction.replacementEnd - touchEdit.transaction.replacementStart, touchEdit.transaction.replacementText);
    expect(completeCauses && locatedCauses && touchEdit.consumed && hand_order::stripAnnotations(touchText) == hand_order::stripAnnotations(source)
        && touchText.count('~') == 3 && !touchText.contains("~m") && SimaiParser::validateSyntax(touchText).annotationErrors.isEmpty(),
        "real worker exports complete mixed-group evidence and one transaction writes only the two expressible direct members");
    service.request(source, 0, 0); settle(service);
    service.setParameters({});
    expect(!service.canApply() && !service.takeEdit(source).consumed,
        "changing complete solver parameters invalidates a finished Touchgroup result");
    workspace.openSource("&inote_5=(120){4}1-5[4:1]~[head=r;track=0:l|0.5:r],\n", {}, 5);
    source = chart(); service.request(source, 0, 0); settle(service);
    expect(!service.rows().isEmpty() && service.rows().first().toMap().value("intervalData").toList().size() == 2
        && !service.canApply(), "complex original hand intervals reach the actual service preview and remain byte-for-byte protected");
    workspace.openSource("&inote_5=(120){4}5h[1:1]~l,6~l,\n", {}, 5); source = chart();
    const int rewriteSix = source.indexOf("6~l");
    service.setOverwriteExisting(true);
    service.request(source, rewriteSix, rewriteSix + 3); settle(service);
    const auto rewritten = service.takeEdit(source); QString rewrittenText = source;
    if (rewritten.transaction.hasEdit) rewrittenText.replace(rewritten.transaction.replacementStart,
        rewritten.transaction.replacementEnd - rewritten.transaction.replacementStart, rewritten.transaction.replacementText);
    expect(rewritten.consumed && rewritten.transaction.replacementStart == rewriteSix
        && rewrittenText.left(rewriteSix) == source.left(rewriteSix) && rewrittenText.contains("6~r")
        && SimaiParser::validateSyntax(rewrittenText).annotationErrors.isEmpty(),
        "recompute unlocks selected existing labels while the external Hold remains a read-only locked context");
    service.setOverwriteExisting(false);
    workspace.openSource("&inote_5=(120){4}6,1,\n", {}, 5); source = chart();
    service.request(source, 0, 0); settle(service);
    bool hasConfidence = !service.rows().isEmpty();
    for (const auto& item : service.rows()) {
        const auto row = item.toMap();
        hasConfidence &= row.contains("confidence") && row.contains("confidenceLevel")
            && !row.value("counterfactualData").toList().isEmpty() && row.value("fullReplayVerified").toBool();
    }
    expect(hasConfidence && service.canApply(), "real worker publishes component confidence and complete-replay evidence");
    service.setHighConfidenceOnly(true);
    expect(!service.canApply() && !service.takeEdit(source).consumed,
        "changing the high-confidence policy immediately invalidates the previous applicable result");
    service.request(source, 0, 0); settle(service);
    bool weakMember = false;
    for (const auto& item : service.rows()) weakMember |= item.toMap().value("confidenceLevel") != "high";
    expect(weakMember && !service.canApply(), "high-confidence mode withholds the complete dependency unit when any generated member is weak");
    service.setHighConfidenceOnly(false); service.setInputPreference(1);
    service.request(source, 0, 0); settle(service);
    bool screenInput = false;
    for (const auto& item : service.rows()) screenInput |= item.toMap().value("inputChannel").toString().startsWith('A');
    expect(service.canApply() && screenInput, "screen preference reaches actual physical A-channel candidates through the asynchronous service");
    service.setInputPreference(2);
    expect(!service.canApply(), "input-channel preference changes invalidate stale candidates");

    QTranslator chineseCatalog, englishCatalog;
    expect(chineseCatalog.load(":/i18n/zh_CN.qm") && englishCatalog.load(":/i18n/en_US.qm")
        && QCoreApplication::installTranslator(&chineseCatalog), "real compiled catalogs are available for live result translation");
    service.setParameters({});
    workspace.openSource("&inote_5=(120){4}6,1,\n", {}, 5); source = chart();
    service.request(source, 0, 0); settle(service);
    const QString chineseSummary = service.message();
    const auto chineseRows = service.rows();
    const auto translatedLocation = service.navigationRange(0, source);
    expect(service.canApply() && chineseRows.size() == 2, "live translation starts from a real applicable completed solution");
    QSignalSpy translated(&service, &HandOrderAnalysisService::changed);
    expect(QCoreApplication::installTranslator(&englishCatalog), "live language installs the actual English QM");
    service.retranslate();
    const auto englishRows = service.rows();
    bool unchangedCandidate = englishRows.size() == chineseRows.size();
    bool translatedStatuses = !englishRows.isEmpty();
    for (int i = 0; i < englishRows.size() && i < chineseRows.size(); ++i) {
        const auto before = chineseRows.at(i).toMap(), after = englishRows.at(i).toMap();
        for (const char* key : {"line", "col", "status", "context", "originalAnnotation", "inputChannel", "cost",
                 "peakSpeed", "dependencyUnit", "confidence", "confidenceLevel", "fullReplayVerified"})
            unchangedCandidate &= before.value(key) == after.value(key);
        const QByteArray statusId = "hand_order.auto.status." + after.value("status").toString().toUtf8();
        translatedStatuses &= after.value("statusText").toString() == qtTrId(statusId.constData())
            && after.value("statusText") != before.value("statusText");
    }
    expect(translated.count() == 1 && service.message() != chineseSummary && service.message().startsWith("Writable ")
        && translatedStatuses && unchangedCandidate && service.canApply()
        && service.navigationRange(0, source) == translatedLocation,
        "retranslation updates the completed summary and row labels without invalidating or changing its candidate identity");
    const auto translatedEdit = service.takeEdit(source);
    QString translatedText = source;
    if (translatedEdit.transaction.hasEdit) translatedText.replace(translatedEdit.transaction.replacementStart,
        translatedEdit.transaction.replacementEnd - translatedEdit.transaction.replacementStart, translatedEdit.transaction.replacementText);
    expect(translatedEdit.consumed && translatedEdit.transaction.hasEdit && translatedText.count('~') == 2
        && SimaiParser::validateSyntax(translatedText).annotationErrors.isEmpty(),
        "a solution remains strictly writable exactly once after its language changes");
    service.request(source, 0, 0);
    QCoreApplication::removeTranslator(&englishCatalog);
    service.retranslate();
    expect(service.pending() && service.message() == qtTrId("hand_order.auto.running"),
        "language refresh retains the in-flight request and immediately translates its running state");
    settle(service);
    expect(service.canApply() && service.message() == chineseSummary && service.rows() == chineseRows,
        "an in-flight solution completes in the current language with unchanged candidate evidence");
    workspace.replaceActiveDifficultyChart(source + " ");
    expect(QCoreApplication::installTranslator(&englishCatalog), "English catalog reinstalls for stale-state translation");
    service.retranslate();
    expect(!service.canApply() && service.message() == qtTrId("hand_order.auto.stale"),
        "an already stale state translates without reviving an invalid result");
    service.cancel();
    QCoreApplication::removeTranslator(&englishCatalog); service.retranslate();
    expect(!service.pending() && !service.canApply() && service.message() == qtTrId("hand_order.auto.cancelled"),
        "cancelled state retains its semantics across catalog changes");
    QCoreApplication::removeTranslator(&chineseCatalog);
    return ok ? 0 : 1;
}

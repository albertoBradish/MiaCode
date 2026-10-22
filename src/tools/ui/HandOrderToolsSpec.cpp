#include "app/ui/editor/EditorController.h"
#include "app/ui/document/HandOrderCommands.h"
#include "app/services/HandOrderAnalysisService.h"
#include <scintillaquick/scintillaquick_item.h>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTextStream>
#include <QTranslator>

class EditHost final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* automaticService READ automaticService CONSTANT)
public:
    miacode::ChartWorkspace workspace;
    miacode::HandOrderAnalysisService automatic{workspace};
    bool automaticMode = false;
    QObject* automaticService() { return &automatic; }
    miacode::ui::EditorController controller;
    ScintillaQuick_item editor;
    int applications = 0;
    QString text() const { return editor.property("text").toString(); }
    void load(const QString& text) {
        applications = 0;
        editor.setProperty("text", text);
        editor.send(SCI_EMPTYUNDOBUFFER);
        controller.activateHandOrderDocument(QStringLiteral("fixture:5"), text);
    }
    bool apply(const miacode::editor::SimaiTextEditResult& result) {
        if (!result.consumed) return false;
        const auto& tx = result.transaction;
        if (!tx.hasEdit) return true;
        const QString before = text();
        editor.send(SCI_BEGINUNDOACTION);
        editor.send(SCI_SETTARGETSTART, before.left(tx.replacementStart).toUtf8().size());
        editor.send(SCI_SETTARGETEND, before.left(tx.replacementEnd).toUtf8().size());
        const QByteArray replacement = tx.replacementText.toUtf8();
        editor.sends(SCI_REPLACETARGET, replacement.size(), replacement.constData());
        editor.send(SCI_ENDUNDOACTION);
        controller.activateHandOrderDocument(QStringLiteral("fixture:5"), text());
        if (automaticMode) workspace.replaceActiveDifficultyChart(text());
        ++applications;
        return true;
    }
    void loadAutomatic(const QString& chart = "(120){4}6,") {
        workspace.openSource("&inote_5=" + chart + "\n", {}, 5);
        load(workspace.document().difficulty(5)->chart);
        automaticMode = true;
        automatic.request(text(), 0, 0);
    }
    Q_INVOKABLE bool applyAutomatic() { return apply(automatic.takeEdit(text())); }
    Q_INVOKABLE bool applyBasicLayout() {
        return apply(controller.handOrderCommand(text(), 0, 0, QStringLiteral("hand_order.basic_layout")));
    }
    Q_INVOKABLE bool navigateAutomatic(int row) {
        const auto range = automatic.navigationRange(row, text());
        if (range.isEmpty()) return false;
        const int first = text().left(range.value("start").toInt()).toUtf8().size();
        const int last = text().left(range.value("end").toInt()).toUtf8().size();
        editor.send(SCI_SETSEL, first, last); return true;
    }
    Q_INVOKABLE void requestAutomatic() { automatic.request(text(), 0, 0); }
    Q_INVOKABLE bool applyIntervals() { return apply(controller.applyHandOrderIntervals(text())); }
    miacode::ui::NativeSlideSplitContext nativeContext() const {
        miacode::ui::NativeSlideSplitContext context;
        context.workspace = workspace.snapshot(); context.scope = QStringLiteral("difficulty:5");
        context.chartText = text(); context.timing = miacode::simai::buildTimingMetadata(workspace.document());
        return context;
    }
    void loadNative(const QString& chart) {
        workspace.openSource("&inote_5=" + chart + "\n", {}, 5);
        load(workspace.document().difficulty(5)->chart); automaticMode = true;
        controller.updateNativeSlideSplitContext(nativeContext());
    }
    Q_INVOKABLE QVariantList handOrderRows() const {
        QVariantList rows;
        for (const auto& spec : miacode::ui::handOrderCommandSpecs())
            rows.append(QVariantMap{{"id", spec.id}, {"labelKey", spec.labelKey}, {"requiresSelection", spec.requiresSelection}});
        return rows;
    }
    Q_INVOKABLE bool prepareNative() {
        const QByteArray source = text().toUtf8();
        const int first = QString::fromUtf8(source.left(editor.send(SCI_GETSELECTIONSTART))).size();
        const int last = QString::fromUtf8(source.left(editor.send(SCI_GETSELECTIONEND))).size();
        return first != last && controller.prepareNativeSlideSplit(nativeContext(), first, last);
    }
    Q_INVOKABLE bool applyNative() { return apply(controller.applyNativeSlideSplit(nativeContext())); }
};

QQuickItem* findVisualItem(QQuickItem* root, const QString& name)
{
    if (root->objectName() == name && root->isVisible()) return root;
    for (auto* child : root->childItems())
        if (auto* found = findVisualItem(child, name)) return found;
    return nullptr;
}
bool popupAtRest(QObject* popup, bool open)
{
    if (!popup || popup->property("visible").toBool() != open || popup->property("closing").toBool()) return false;
    const auto opened = popup->property("opened");
    return !opened.isValid() || opened.toBool() == open;
}

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    bool ok = true;
    const auto expect = [&](bool pass, const QString& name) {
        QTextStream(pass ? stdout : stderr) << (pass ? "PASS: " : "FAIL: ") << name << '\n';
        ok &= pass;
    };
    QTranslator chineseCatalog;
    expect(chineseCatalog.load(QStringLiteral(":/i18n/zh_CN.qm"))
        && QCoreApplication::installTranslator(&chineseCatalog),
        "the real compiled Chinese resource catalog loads before hand-order QML");
    EditHost host;
    const QString layoutSource = "(120){4}1,8,1-5[4:1],8~r,";
    host.load(layoutSource);
    const int layoutCursor = layoutSource.indexOf("8,");
    const auto layoutEdit = host.controller.handOrderCommand(layoutSource, layoutCursor, layoutCursor, "hand_order.basic_layout");
    expect(layoutEdit.transaction.hasEdit && layoutEdit.transaction.anchor == layoutEdit.transaction.position
        && host.apply(layoutEdit) && host.text().contains("1~r,8~l,1-5[4:1]~[head=r;track=0:r],8~r,"),
        "basic layout uses the whole difficulty without a selection and preserves existing explicit annotations");
    expect(!host.controller.handOrderCommand(host.text(), 0, 0, "hand_order.basic_layout").transaction.hasEdit,
        "repeated basic layout creates no empty undo entry");
    host.editor.send(SCI_UNDO);
    expect(host.text() == layoutSource && !host.editor.send(SCI_CANUNDO), "one native Scintilla undo restores the complete basic layout");
    const auto selectedLayout = host.controller.handOrderCommand(layoutSource, layoutCursor + 1, layoutCursor, "hand_order.basic_layout");
    expect(selectedLayout.transaction.replacementStart == layoutCursor && selectedLayout.transaction.replacementEnd == layoutCursor + 1
        && host.apply(selectedLayout) && host.text() == "(120){4}1,8~l,1-5[4:1],8~r,",
        "backward selection lays out only its complete token and leaves the rest of the difficulty untouched");
    host.editor.send(SCI_UNDO);
    expect(host.text() == layoutSource && !host.editor.send(SCI_CANUNDO), "selected basic layout is one native undo transaction");
    const QString nativeChain = "(120){4}1-3-7[2:1]~[head=l;track=0:l|0.25:r|0.75:l],";
    miacode::ChartWorkspace nativeWorkspace;
    nativeWorkspace.openSource("&inote_5=" + nativeChain + "\n&inote_6=2,\n", {}, 5);
    miacode::ui::EditorController nativeController;
    const auto nativeContext = [&] {
        miacode::ui::NativeSlideSplitContext context;
        context.workspace = nativeWorkspace.snapshot();
        context.scope = QStringLiteral("difficulty:%1").arg(context.workspace.activeDifficultyId);
        context.chartText = nativeWorkspace.document().difficulty(context.workspace.activeDifficultyId)->chart;
        context.timing = miacode::simai::buildTimingMetadata(nativeWorkspace.document()); return context;
    };
    auto nativeIdentity = nativeContext();
    expect(!nativeController.prepareNativeSlideSplit(nativeIdentity, 0, 0), "native split is inert without a selection");
    expect(nativeController.prepareNativeSlideSplit(nativeIdentity, 0, nativeIdentity.chartText.size())
        && nativeController.nativeSlideSplitCanApply() && nativeWorkspace.snapshot().revision == nativeIdentity.workspace.revision,
        "native split preparation produces a reviewable plan without editing the workspace");
    const auto nativeRows = nativeController.nativeSlideSplitRows();
    expect(nativeRows.size() == 1 && nativeRows.first().toMap().value("beforeTracks").toInt() == 1
        && nativeRows.first().toMap().value("afterTracks").toInt() == 2
        && nativeRows.first().toMap().value("beforeHeads") == nativeRows.first().toMap().value("afterHeads")
        && nativeRows.first().toMap().value("before").toString() != nativeRows.first().toMap().value("after").toString(),
        "native preview exposes exact before/after, track counts and unchanged real head count");
    nativeController.cancelNativeSlideSplit();
    expect(!nativeController.nativeSlideSplitCanApply() && !nativeController.applyNativeSlideSplit(nativeIdentity).consumed,
        "cancelling native preview creates no text transaction");
    for (int field = 0; field < 8; ++field) {
        nativeController.prepareNativeSlideSplit(nativeIdentity, 0, nativeIdentity.chartText.size());
        auto changed = nativeIdentity;
        if (field == 0) ++changed.workspace.revision;
        if (field == 1) ++changed.workspace.documentOpenGeneration;
        if (field == 2) changed.workspace.activeDifficultyId = 6;
        if (field == 3) changed.workspace.filePath = "other-fixture.simai";
        if (field == 4) changed.workspace.sourceText += " ";
        if (field == 5) changed.scope += "/other";
        if (field == 6) changed.chartText += " ";
        if (field == 7) changed.timing.wholeTimeSignatureNumerator += 1;
        const auto stale = nativeController.applyNativeSlideSplit(changed);
        expect(!stale.consumed && !stale.transaction.hasEdit && !nativeController.nativeSlideSplitCanApply(),
            QStringLiteral("native preview rejects stale identity field %1").arg(field));
    }
    nativeController.prepareNativeSlideSplit(nativeIdentity, 0, nativeIdentity.chartText.size());
    nativeWorkspace.replaceActiveDifficultyChart(nativeIdentity.chartText + " ");
    nativeWorkspace.replaceActiveDifficultyChart(nativeIdentity.chartText);
    expect(!nativeController.applyNativeSlideSplit(nativeContext()).consumed,
        "editing back to identical text cannot revive a native preview from the old revision");
    nativeIdentity = nativeContext();
    nativeController.prepareNativeSlideSplit(nativeIdentity, 0, nativeIdentity.chartText.size());
    const bool parserPolicy = SimaiParser::allowNegativeHsEnabled();
    SimaiParser::setAllowNegativeHsEnabled(!parserPolicy);
    expect(!nativeController.nativeSlideSplitCanApply() && !nativeController.applyNativeSlideSplit(nativeIdentity).consumed,
        "parser policy changes invalidate native previews before application");
    SimaiParser::setAllowNegativeHsEnabled(parserPolicy);
    nativeWorkspace.openSource("&inote_5=" + nativeChain + "1~z,\n", {}, 5);
    nativeIdentity = nativeContext();
    const int nativeSelectedEnd = nativeIdentity.chartText.indexOf(',');
    nativeController.prepareNativeSlideSplit(nativeIdentity, 0, nativeSelectedEnd);
    const auto remoteErrorEdit = nativeController.applyNativeSlideSplit(nativeIdentity);
    QString remoteErrorResult = nativeIdentity.chartText;
    if (remoteErrorEdit.transaction.hasEdit) remoteErrorResult.replace(remoteErrorEdit.transaction.replacementStart,
        remoteErrorEdit.transaction.replacementEnd - remoteErrorEdit.transaction.replacementStart, remoteErrorEdit.transaction.replacementText);
    expect(remoteErrorEdit.consumed && remoteErrorResult.contains("1~z")
        && SimaiParser::validateSyntax(remoteErrorResult).annotationErrors.size()
            == SimaiParser::validateSyntax(nativeIdentity.chartText).annotationErrors.size(),
        "native apply revalidates the same plan without rejecting or altering unrelated existing annotation errors");
    nativeController.prepareNativeSlideSplit(nativeIdentity, 0, nativeSelectedEnd);
    const QString reopenedSource = nativeWorkspace.snapshot().sourceText;
    nativeWorkspace.openSource(reopenedSource, {}, 5);
    expect(!nativeController.applyNativeSlideSplit(nativeContext()).consumed,
        "reopening the identical source rejects the previous native preview generation");
    const QString initial = "(120){4}12/1-5[4:1],";
    host.load(initial);
    const auto batch = host.controller.handOrderCommand(initial, 0, initial.size(), "hand_order.set_left");
    expect(batch.transaction.hasEdit && host.apply(batch), "controller produces one selection transaction");
    const auto repeat = host.controller.handOrderCommand(host.text(), 0, host.text().size(), "hand_order.set_left");
    expect(!repeat.transaction.hasEdit, "identical commands add no empty undo entry");
    host.editor.send(SCI_UNDO);
    expect(host.text() == initial && !host.editor.send(SCI_CANUNDO), "one native Scintilla undo restores the full batch");

    const QString separate = "(120){4}1-5[4:1],2-6[4:1],";
    host.load(separate);
    const int aEnd = separate.indexOf(',');
    const int bStart = aEnd + 1;
    host.controller.handOrderCommand(separate, 0, aEnd, "hand_order.split_annotation");
    host.controller.setHandOrderInterval(1, 1);
    host.controller.handOrderCommand(separate, bStart, separate.size() - 1, "hand_order.split_annotation");
    host.controller.setHandOrderInterval(2, 2);
    host.controller.handOrderCommand(separate, 0, aEnd, "hand_order.split_annotation");
    expect(host.controller.handOrderRows().size() == 3
        && host.controller.handOrderRows().at(1).toMap().value("hand").toInt() == 1,
        "splitting B then returning to A preserves A's draft and shows only A");
    host.controller.setHandOrderInterval(2, 1);
    host.applyIntervals();
    const QString rebased = host.text();
    const int rebasedB = rebased.indexOf(',') + 1;
    host.controller.handOrderCommand(rebased, rebasedB, rebased.size() - 1, "hand_order.split_annotation");
    expect(host.controller.handOrderRows().size() == 3
        && host.controller.handOrderRows().at(2).toMap().value("hand").toInt() == 2,
        "writing A rebases B's temporary source range and preserves B's choices");
    const QString unknown = "(120){4}1-5[4:1],2,";
    host.load(unknown);
    const int finish = unknown.indexOf(',');
    const auto split = host.controller.handOrderCommand(unknown, 0, finish, "hand_order.split_annotation");
    expect(!split.transaction.hasEdit && host.controller.handOrderSplitAvailable()
        && host.controller.handOrderRows().size() == 3 && !host.controller.handOrderCanApply(),
        "unknown slide opens three temporary rows and cannot write unspecified tracks");
    host.controller.setHandOrderInterval(1, 1);
    host.controller.activateHandOrderDocument("other", unknown);
    expect(host.controller.handOrderRows().isEmpty(), "drafts are isolated between document scopes");
    host.controller.activateHandOrderDocument("fixture:5", unknown + " ");
    expect(host.controller.handOrderRows().isEmpty(), "drafts cannot target a different text version");
    host.controller.activateHandOrderDocument("fixture:5", unknown);
    expect(host.controller.handOrderRows().size() == 3
        && host.controller.handOrderRows().at(1).toMap().value("hand").toInt() == 1,
        "returning to the exact version restores interval choices");
    host.controller.handOrderCommand(unknown, finish + 1, finish + 2, "hand_order.split_annotation");
    expect(!host.controller.handOrderSplitAvailable() && host.controller.handOrderRows().size() == 3,
        "a no-target split does not present an older slide as its result");

    miacode::ui::EditorController wifiController;
    const QString wifiText = "(120){4}1w5[4:1]~[track=split:0:l|0.5:l&0:r|0.5:r],";
    wifiController.activateHandOrderDocument("wifi-fixture:5", wifiText);
    wifiController.handOrderCommand(wifiText, 0, wifiText.size(), "hand_order.split_annotation");
    auto wifiRows = wifiController.handOrderRows();
    expect(wifiRows.size() == 5 && wifiRows.at(1).toMap().value("sector").toInt() == 0
        && wifiRows.at(3).toMap().value("sector").toInt() == 1,
        "manual Wifi drafts expose stable source branch plus native sector identities");
    wifiController.setHandOrderWifiMode(1, int(HandOrderWifiMode::Single));
    wifiRows = wifiController.handOrderRows();
    expect(wifiRows.size() == 2 && wifiRows.at(1).toMap().value("hand").toInt() == 0
        && !wifiController.handOrderCanApply(), "switching mixed sectors to single mode requires a whole-track hand choice");
    wifiController.setHandOrderInterval(1, int(HandOrderHand::Either));
    const auto singleEdit = wifiController.applyHandOrderIntervals(wifiText);
    expect(singleEdit.transaction.hasEdit && singleEdit.transaction.replacementText.contains("single:0:m|0.5:m"),
        "single-mode editor applies one hand across preserved interval boundaries");
    wifiController.setHandOrderWifiMode(1, int(HandOrderWifiMode::Split));
    expect(wifiController.handOrderRows().size() == 5, "explicitly returning to split mode restores two independent sequences");

    QQmlEngine engine;
    engine.addImportPath(QStringLiteral(MIACODE_QML_SPEC_IMPORT_ROOT));
    engine.rootContext()->setContextProperty("editHost", &host);
    engine.rootContext()->setContextProperty("editController", &host.controller);
    QQmlComponent component(&engine);
    component.setData(R"QML(
import QtQuick
import QtQuick.Controls
import MiaCode.UI
Window {
    id: window
    width: 800; height: 600; visible: true
    property int toolCommands: 0
    property string lastToolCommand: ""
    QtObject {
        id: fakeShortcuts
        property int revision: 1
        function displayText(id) { return "Ctrl+Alt+1" }
    }
    MainToolBar {
        anchors.top: parent.top
        width: parent.width
        hostWindow: window
        windowChrome: null
        previewSettingsVisible: false
        handOrderEnabled: true
        handOrderAnalyzeEnabled: true
        handOrderRows: editHost.handOrderRows()
        shortcuts: fakeShortcuts
        onHandOrderRequested: command => {
            window.toolCommands++; window.lastToolCommand = command
            if (command === "hand_order.basic_layout") editHost.applyBasicLayout()
            if (command === "hand_order.split_native_slide" && editHost.prepareNative()) nativeDialog.open()
        }
    }
    HandOrderAnalysisDialog {
        objectName: "automaticDialog"
        service: editHost.automaticService
        applyEdit: () => editHost.applyAutomatic()
        analyzeSelection: () => editHost.requestAutomatic()
        analyzeDifficulty: () => editHost.requestAutomatic()
        navigateResult: row => editHost.navigateAutomatic(row)
    }
    HandOrderIntervalsDialog {
        objectName: "intervalDialog"
        controller: editController
        applyEdit: () => editHost.applyIntervals()
    }
    NativeSlideSplitDialog {
        id: nativeDialog
        objectName: "nativeDialog"
        controller: editController
        applyEdit: () => editHost.applyNative()
    }
}
)QML", QUrl());
    QScopedPointer<QObject> object(component.create());
    auto* window = qobject_cast<QQuickWindow*>(object.data());
    expect(window != nullptr, window ? QStringLiteral("real hand-order dialog and toolbar QML instantiate") : component.errorString());
    if (window) {
        window->requestActivate();
        QTest::qWait(100);
        auto* dialog = window->findChild<QObject*>("intervalDialog");
        QMetaObject::invokeMethod(dialog, "open");
        expect(QTest::qWaitFor([&] { return popupAtRest(dialog, true); }, 2000), "interval popup finishes opening");
        expect(dialog->property("visible").toBool(), "interval dialog opens offscreen");
        // Exercise the ComboBox key path rather than assigning the controller directly.
        for (int row : {0, 2}) {
            auto* combo = findVisualItem(window->contentItem(), QStringLiteral("handOrderIntervalHand_%1").arg(row));
            expect(combo != nullptr, QStringLiteral("interval selector %1 exists").arg(row));
            if (!combo) continue;
            combo->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_Down);
            expect(QTest::qWaitFor([&] {
                const auto rows = host.controller.handOrderRows();
                const auto* current = findVisualItem(window->contentItem(), QStringLiteral("handOrderIntervalHand_%1").arg(row));
                return row < rows.size() && rows.at(row).toMap().value("hand").toInt() == int(HandOrderHand::Left)
                    && current && current->property("currentIndex").toInt() == int(HandOrderHand::Left);
            }, 1000), QStringLiteral("keyboard choice %1 reaches the rebuilt delegate and draft").arg(row));
            // The translated notice wraps to a different height. Let the
            // replacement delegates and ScrollView finish a layout frame.
            QTest::qWait(60);
        }
        expect(host.controller.handOrderCanApply(), "choosing remaining intervals enables writing");
        QTest::qWait(80);
        auto* accept = findVisualItem(window->contentItem(), "dialogFooterAccept");
        expect(accept != nullptr && accept->isEnabled(), "write button reflects completed tracks");
        if (accept && accept->isEnabled()) {
            const QPoint point = accept->mapToScene(QPointF(accept->width() / 2, accept->height() / 2)).toPoint();
            QTest::mouseClick(window, Qt::LeftButton, {}, point);
            expect(QTest::qWaitFor([&] { return popupAtRest(dialog, false) && host.applications == 1; }, 2000),
                "interval Apply completes its one edit and exit transition");
        }
        expect(host.applications == 1 && host.text() == "(120){4}1-5[4:1]~[head=l;track=0:l|0.5:l],2,"
            && SimaiParser::validateSyntax(host.text()).annotationErrors.isEmpty(), "clicking write applies a complete legal annotation");
        host.editor.send(SCI_UNDO);
        expect(host.text() == unknown && !host.editor.send(SCI_CANUNDO), "one native undo restores the interval edit");
        auto* toolButton = findVisualItem(window->contentItem(), "handOrderButton");
        expect(toolButton && toolButton->property("label").toString() == QStringLiteral("手序")
            && toolButton->property("tooltip").toString() == qtTrId("hand_order.selection_scope")
            && toolButton->property("tooltip").toString() != QStringLiteral("hand_order.selection_scope"),
            "actual toolbar label and tooltip resolve compiled Chinese IDs instead of displaying raw keys");
        if (toolButton) {
            const QPoint point = toolButton->mapToScene(QPointF(toolButton->width() / 2, toolButton->height() / 2)).toPoint();
            QTest::mouseClick(window, Qt::LeftButton, {}, point);
            auto* menu = window->findChild<QObject*>("handOrderToolMenu");
            expect(QTest::qWaitFor([&] { return popupAtRest(menu, true); }, 2000), "hand-order toolbar menu finishes opening");
            auto* item = findVisualItem(window->contentItem(), "handOrderTool_hand_order.set_left");
            expect(item != nullptr && item->isVisible(), "toolbar opens hand-order submenu");
            if (item) {
                const QPoint itemPoint = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
                QTest::mouseClick(window, Qt::LeftButton, {}, itemPoint);
                expect(QTest::qWaitFor([&] { return popupAtRest(menu, false) && window->property("toolCommands").toInt() == 1; }, 2000),
                    "toolbar selection dispatches only after its menu closes");
            }
        }
        expect(window->property("toolCommands").toInt() == 1
            && window->property("lastToolCommand").toString() == "hand_order.set_left",
            "toolbar menu dispatches one command after it closes");
        const QString buttonLayout = "(120){4}1,8,";
        host.load(buttonLayout);
        auto* basicButton = findVisualItem(window->contentItem(), "handOrderBasicLayoutButton");
        expect(basicButton && basicButton->isEnabled()
            && basicButton->property("label").toString() == QStringLiteral("基础排布"),
            "toolbar exposes a translated basic-layout button without requiring a selection");
        if (basicButton) {
            const QPoint point = basicButton->mapToScene(QPointF(basicButton->width()/2, basicButton->height()/2)).toPoint();
            QTest::mouseClick(window, Qt::LeftButton, {}, point);
            expect(QTest::qWaitFor([&] { return host.applications == 1; }, 1000), "basic-layout button dispatches one edit");
        }
        expect(host.text() == "(120){4}1~r,8~l,", "basic-layout toolbar action executes the deterministic half-screen assignment");
        host.editor.send(SCI_UNDO);
        expect(host.text() == buttonLayout && !host.editor.send(SCI_CANUNDO), "toolbar basic layout is one native undo action");
        host.load(wifiText);
        host.controller.handOrderCommand(wifiText, 0, wifiText.size(), "hand_order.split_annotation");
        QMetaObject::invokeMethod(dialog, "open");
        expect(QTest::qWaitFor([&] { return popupAtRest(dialog, true); }, 2000), "interval popup finishes opening");
        auto* modeCombo = findVisualItem(window->contentItem(), "handOrderWifiMode_1");
        expect(modeCombo != nullptr, "real interval QML exposes the Wifi execution-mode selector");
        if (modeCombo) {
            modeCombo->forceActiveFocus();
            QTest::keyClick(window, Qt::Key_Down);
            expect(QTest::qWaitFor([&] { return host.controller.handOrderRows().size() == 2; }, 1000),
                "Wifi mode rebuild finishes before closing its popup");
        }
        expect(host.controller.handOrderRows().size() == 2
            && host.controller.handOrderRows().at(1).toMap().value("wifiMode").toInt() == int(HandOrderWifiMode::Single)
            && !host.controller.handOrderCanApply(),
            "keyboard mode selection requires a fresh whole-track hand before single-mode writing");
        QMetaObject::invokeMethod(dialog, "close");
        expect(QTest::qWaitFor([&] { return popupAtRest(dialog, false); }, 2000), "interval popup finishes closing before the next preview");
        host.loadAutomatic();
        for (int n = 0; host.automatic.pending() && n < 500; ++n) QTest::qWait(10);
        expect(host.automatic.canApply(), "automatic panel receives a current asynchronous candidate");
        auto* automaticDialog = window->findChild<QObject*>("automaticDialog");
        QMetaObject::invokeMethod(automaticDialog, "open");
        expect(QTest::qWaitFor([&] { return popupAtRest(automaticDialog, true); }, 2000), "automatic popup finishes opening");
        auto* location = findVisualItem(window->contentItem(), "handOrderResultLocation_0");
        expect(location != nullptr && location->isEnabled(), "real automatic QML offers current-result source navigation");
        if (location) {
            const QPoint point = location->mapToScene(QPointF(location->width()/2, location->height()/2)).toPoint();
            QTest::mouseClick(window, Qt::LeftButton, {}, point);
            expect(QTest::qWaitFor([&] { return popupAtRest(automaticDialog, false); }, 2000), "Locate closes the automatic popup before reopening");
        }
        expect(host.editor.send(SCI_GETSELECTIONSTART) == host.text().indexOf("6,")
            && host.editor.send(SCI_GETSELECTIONEND) == host.text().indexOf("6,") + 1 && host.automatic.canApply(),
            "clicking Locate selects the exact current token without changing text or consuming its candidate");
        QMetaObject::invokeMethod(automaticDialog, "open");
        expect(QTest::qWaitFor([&] { return popupAtRest(automaticDialog, true); }, 2000), "automatic popup finishes reopening");
        auto* automaticAccept = findVisualItem(window->contentItem(), "dialogFooterAccept");
        expect(automaticAccept && automaticAccept->isEnabled(), "experimental preview enables its apply button");
        if (automaticAccept && automaticAccept->isEnabled()) {
            const QPoint point = automaticAccept->mapToScene(QPointF(automaticAccept->width()/2, automaticAccept->height()/2)).toPoint();
            QTest::mouseClick(window, Qt::LeftButton, {}, point);
            expect(QTest::qWaitFor([&] { return popupAtRest(automaticDialog, false) && host.applications == 1; }, 2000),
                "automatic Apply finishes its transaction and closes");
        }
        expect(host.applications == 1 && host.text().contains("6~l") && !host.automatic.canApply(),
            "clicking automatic apply performs one edit and invalidates the consumed preview");
        host.editor.send(SCI_UNDO);
        expect(!host.text().contains('~') && !host.editor.send(SCI_CANUNDO), "automatic application is one native undo action");
        host.loadAutomatic("(120){4}1-5[2:1]~r,,B1/C/B3,");
        for (int n = 0; host.automatic.pending() && n < 500; ++n) QTest::qWait(10);
        const QString beforeTouches = host.text();
        QMetaObject::invokeMethod(automaticDialog, "open");
        expect(QTest::qWaitFor([&] { return popupAtRest(automaticDialog, true); }, 2000), "Touch evidence popup finishes opening");
        bool hasTouchEvidence = false;
        for (const auto& item : host.automatic.rows()) {
            const auto row = item.toMap();
            if (row.value("judgementKind") == "group_carry") {
                const QString evidenceName = QStringLiteral("handOrderAutomaticEvidence_%1_%2").arg(row.value("line").toInt()).arg(row.value("col").toInt());
                hasTouchEvidence |= findVisualItem(window->contentItem(), evidenceName) != nullptr
                    && row.value("causeData").toList().size() >= 2;
            }
        }
        auto* touchAccept = findVisualItem(window->contentItem(), "dialogFooterAccept");
        expect(hasTouchEvidence && touchAccept && touchAccept->isEnabled(), "real QML displays mixed carry evidence and permits expressible direct members");
        if (touchAccept && touchAccept->isEnabled()) {
            const QPoint point = touchAccept->mapToScene(QPointF(touchAccept->width()/2, touchAccept->height()/2)).toPoint();
            QTest::mouseClick(window, Qt::LeftButton, {}, point);
            expect(QTest::qWaitFor([&] { return popupAtRest(automaticDialog, false) && host.applications == 1; }, 2000),
                "Touch Apply closes after exactly one transaction");
        }
        expect(host.applications == 1 && host.text().count('~') == 3 && !host.text().contains("~m"),
            "actual automatic apply writes direct Touch labels while leaving mixed carry unannotated");
        host.editor.send(SCI_UNDO);
        expect(host.text() == beforeTouches && !host.editor.send(SCI_CANUNDO), "Touchgroup automatic application remains one native undo action");
        host.loadNative(nativeChain);
        host.editor.send(SCI_SETSEL, 0, host.text().toUtf8().size());
        const QString beforeNative = host.text();
        if (toolButton) {
            const QPoint point = toolButton->mapToScene(QPointF(toolButton->width()/2, toolButton->height()/2)).toPoint();
            QTest::mouseClick(window, Qt::LeftButton, {}, point);
            auto* menu = window->findChild<QObject*>("handOrderToolMenu");
            expect(QTest::qWaitFor([&] { return popupAtRest(menu, true); }, 2000), "native command menu finishes opening");
            auto* entry = findVisualItem(window->contentItem(), "handOrderTool_hand_order.split_native_slide");
            expect(entry && entry->isEnabled(), "toolbar uses the registered independent native-slide command");
            if (entry) {
                const QPoint itemPoint = entry->mapToScene(QPointF(entry->width()/2, entry->height()/2)).toPoint();
                QTest::mouseClick(window, Qt::LeftButton, {}, itemPoint);
                expect(QTest::qWaitFor([&] { return popupAtRest(menu, false); }, 2000), "native menu finishes closing before its diff is inspected");
            }
        }
        auto* nativeDialog = window->findChild<QObject*>("nativeDialog");
        expect(QTest::qWaitFor([&] { return popupAtRest(nativeDialog, true); }, 2000), "native diff finishes opening");
        auto* beforeCode = findVisualItem(window->contentItem(), "nativeSlideSplitBefore_0");
        auto* afterCode = findVisualItem(window->contentItem(), "nativeSlideSplitAfter_0");
        expect(window->property("lastToolCommand") == "hand_order.split_native_slide"
            && beforeCode && afterCode && beforeCode->property("readOnly").toBool()
            && afterCode->property("text").toString().contains('/') && host.text() == beforeNative && host.applications == 0,
            "clicking the toolbar native command opens a read-only exact diff and performs no edit");
        auto* cancelNative = findVisualItem(window->contentItem(), "dialogFooterCancel");
        if (cancelNative) {
            const QPoint point = cancelNative->mapToScene(QPointF(cancelNative->width()/2, cancelNative->height()/2)).toPoint();
            QTest::mouseClick(window, Qt::LeftButton, {}, point);
        }
        expect(QTest::qWaitFor([&] { return popupAtRest(nativeDialog, false)
            && !host.controller.nativeSlideSplitAvailable() && !host.controller.nativeSlideSplitCanApply(); }, 2000),
            "native Cancel waits for onClosed to discard the plan");
        expect(cancelNative && host.text() == beforeNative && host.applications == 0
            && !host.editor.send(SCI_CANUNDO) && !host.controller.nativeSlideSplitCanApply(),
            "native preview Cancel leaves text and native undo history untouched");
        expect(host.prepareNative(), "native chain can be selected again after cancellation");
        QMetaObject::invokeMethod(nativeDialog, "open");
        expect(QTest::qWaitFor([&] { return popupAtRest(nativeDialog, true); }, 2000), "native diff finishes reopening");
        auto* nativeAccept = findVisualItem(window->contentItem(), "dialogFooterAccept");
        expect(nativeAccept && nativeAccept->isEnabled(), "reliable whole native split batch enables Apply");
        if (nativeAccept && nativeAccept->isEnabled()) {
            const QPoint point = nativeAccept->mapToScene(QPointF(nativeAccept->width()/2, nativeAccept->height()/2)).toPoint();
            QTest::mouseClick(window, Qt::LeftButton, {}, point);
        }
        expect(QTest::qWaitFor([&] { return popupAtRest(nativeDialog, false) && host.applications == 1
            && !host.controller.nativeSlideSplitAvailable(); }, 2000), "native Apply completes one edit and its closing lifecycle");
        expect(host.applications == 1 && host.text() != beforeNative && SimaiParser::validateSyntax(host.text()).ok
            && !host.controller.nativeSlideSplitCanApply(), "native preview Apply submits one strict transaction and consumes its result");
        host.editor.send(SCI_UNDO);
        expect(host.text() == beforeNative && !host.editor.send(SCI_CANUNDO), "one native undo restores the original chain and annotation");
        host.loadNative("(120){4}1-3-7[2:1]/2w6[4:1],");
        host.editor.send(SCI_SETSEL, 0, host.text().toUtf8().size());
        const QString blockedNative = host.text(); host.prepareNative();
        QMetaObject::invokeMethod(nativeDialog, "open");
        expect(QTest::qWaitFor([&] { return popupAtRest(nativeDialog, true); }, 2000), "native diff finishes reopening");
        nativeAccept = findVisualItem(window->contentItem(), "dialogFooterAccept");
        expect(nativeAccept && !nativeAccept->isEnabled() && !host.controller.nativeSlideSplitCanApply(),
            "one unsupported complete Wifi target blocks the entire native split batch");
        expect(!host.applyNative() && host.text() == blockedNative && host.applications == 0 && !host.editor.send(SCI_CANUNDO),
            "a blocked native batch cannot partially edit even through the apply endpoint");
        QMetaObject::invokeMethod(nativeDialog, "close");
        expect(QTest::qWaitFor([&] { return popupAtRest(nativeDialog, false) && !host.controller.nativeSlideSplitAvailable(); }, 2000),
            "blocked native popup finishes closing and clears its draft");

        host.loadNative(nativeChain);
        host.editor.send(SCI_SETSEL, 0, host.text().toUtf8().size());
        expect(host.prepareNative(), "a fresh native plan is ready before live language switching");
        host.controller.handOrderCommand(host.text(), 0, host.text().size(), "hand_order.split_annotation");
        const auto chineseNativeRows = host.controller.nativeSlideSplitRows();
        const QString chineseNativeSummary = host.controller.nativeSlideSplitMessage();
        const QString chineseFeedback = host.controller.handOrderFeedback();
        QSignalSpy nativeLanguageNotifications(&host.controller, &miacode::ui::EditorController::nativeSlideSplitChanged);
        QSignalSpy intervalLanguageNotifications(&host.controller, &miacode::ui::EditorController::handOrderChanged);
        QTranslator englishCatalog;
        expect(englishCatalog.load(":/i18n/en_US.qm") && QCoreApplication::installTranslator(&englishCatalog),
            "actual English resource catalog is installed while the existing QML engine stays alive");
        host.controller.retranslate(); engine.retranslate();
        const auto englishNativeRows = host.controller.nativeSlideSplitRows();
        bool unchangedNative = englishNativeRows.size() == chineseNativeRows.size() && !englishNativeRows.isEmpty();
        bool translatedNative = unchangedNative;
        for (int i = 0; i < englishNativeRows.size() && i < chineseNativeRows.size(); ++i) {
            const auto before = chineseNativeRows.at(i).toMap(), after = englishNativeRows.at(i).toMap();
            for (const char* key : {"line", "col", "before", "after", "status", "beforeTracks", "afterTracks", "beforeHeads", "afterHeads"})
                unchangedNative &= before.value(key) == after.value(key);
            translatedNative &= before.value("statusText") != after.value("statusText")
                && before.value("reason") != after.value("reason") && before.value("changes") != after.value("changes")
                && before.value("counts") != after.value("counts")
                && after.value("counts").toString() == qtTrId("hand_order.native.counts")
                    .arg(after.value("beforeTracks").toInt()).arg(after.value("afterTracks").toInt())
                    .arg(after.value("beforeHeads").toInt()).arg(after.value("afterHeads").toInt())
                    .arg(qMax(0, after.value("afterTracks").toInt() - after.value("beforeTracks").toInt()));
        }
        expect(nativeLanguageNotifications.count() == 1 && intervalLanguageNotifications.count() == 1
            && host.controller.nativeSlideSplitCanApply() && unchangedNative && translatedNative
            && host.controller.nativeSlideSplitMessage() != chineseNativeSummary
            && host.controller.handOrderFeedback() != chineseFeedback && host.text() == beforeNative && host.applications == 0,
            "live language refresh changes native counts, reasons and manual feedback while retaining the exact applicable draft");
        expect(QTest::qWaitFor([&] { return toolButton && toolButton->property("label").toString() == "Hand Order"
            && toolButton->property("tooltip").toString() == qtTrId("hand_order.selection_scope"); }, 1000),
            "the existing toolbar QML updates its label and tooltip after a real catalog switch");
        expect(host.applyNative() && host.applications == 1 && host.text() != beforeNative
            && SimaiParser::validateSyntax(host.text()).ok,
            "the unchanged native preview identity still applies one strict edit after retranslation");
        host.editor.send(SCI_UNDO);
        expect(host.text() == beforeNative && !host.editor.send(SCI_CANUNDO),
            "native application after live translation remains one undo action");
        QCoreApplication::removeTranslator(&englishCatalog); host.controller.retranslate(); engine.retranslate();
    }
    return ok ? 0 : 1;
}
#include "HandOrderToolsSpec.moc"

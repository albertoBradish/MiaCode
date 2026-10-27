#include "app/ui/editor/EditorController.h"
#include "app/ui/document/HandOrderCommands.h"
#include <scintillaquick/scintillaquick_item.h>
#include <QGuiApplication>
#include <QTextStream>

namespace {
class EditHost {
public:
    miacode::ui::EditorController controller;
    ScintillaQuick_item editor;
    QString text() const { return editor.property("text").toString(); }
    void load(const QString& text)
    {
        editor.setProperty("text", text);
        editor.send(SCI_EMPTYUNDOBUFFER);
        controller.activateHandOrderDocument(QStringLiteral("fixture:5"), text);
    }
    bool apply(const miacode::editor::SimaiTextEditResult& result)
    {
        if (!result.consumed) return false;
        const auto& transaction = result.transaction;
        if (!transaction.hasEdit) return true;
        const QString before = text();
        editor.send(SCI_BEGINUNDOACTION);
        editor.send(SCI_SETTARGETSTART, before.left(transaction.replacementStart).toUtf8().size());
        editor.send(SCI_SETTARGETEND, before.left(transaction.replacementEnd).toUtf8().size());
        const QByteArray replacement = transaction.replacementText.toUtf8();
        editor.sends(SCI_REPLACETARGET, replacement.size(), replacement.constData());
        editor.send(SCI_ENDUNDOACTION);
        controller.activateHandOrderDocument(QStringLiteral("fixture:5"), text());
        return true;
    }
    bool applyBisectionAtEditorSelection() {
        const QByteArray before = text().toUtf8();
        const int anchor = QString::fromUtf8(before.left(editor.send(SCI_GETANCHOR))).size();
        const int position = QString::fromUtf8(before.left(editor.send(SCI_GETCURRENTPOS))).size();
        const auto edit = controller.handOrderCommand(text(), anchor, position, QStringLiteral("hand_order.bisect_annotation"));
        if (!apply(edit)) return false;
        // Apply the returned selection in the same UTF-16/UTF-8 coordinates
        // used by ScintillaEditorBridge, so a second invocation uses it.
        editor.send(SCI_SETSEL, text().left(edit.transaction.anchor).toUtf8().size(),
            text().left(edit.transaction.position).toUtf8().size());
        return true;
    }
};
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    bool ok = true;
    const auto expect = [&](bool passed, const char* message) {
        QTextStream(passed ? stdout : stderr) << (passed ? "PASS: " : "FAIL: ") << message << '\n';
        ok &= passed;
    };
    EditHost host;
    const QString bisectSource = "(120){4}1-5[4:1]~r,";
    host.load(bisectSource);
    expect(!host.controller.handOrderCommand(host.text(), 8, 8, "hand_order.swap_hands").transaction.hasEdit,
        "hand swap still requires a selection");
    expect(host.apply(host.controller.handOrderCommand(host.text(), 0, host.text().size(), "hand_order.bisect_annotation"))
        && host.text().contains("head=r;track=0:l|0.5:r") && !host.controller.handOrderSplitAvailable(),
        "direct bisection writes L/R without opening the interval draft editor");
    const QString onceBisected = host.text();
    expect(host.apply(host.controller.handOrderCommand(host.text(), host.text().size(), 0, "hand_order.bisect_annotation"))
        && host.text().contains("track=0:l|0.25:l|0.5:r|0.75:r"),
        "repeated bisection preserves hands and accepts backward selection");
    host.editor.send(SCI_UNDO);
    expect(host.text() == onceBisected, "one native undo removes only the second bisection");
    host.editor.send(SCI_UNDO);
    expect(host.text() == bisectSource && !host.editor.send(SCI_CANUNDO), "one native undo removes the first bisection");
    const QString caretSource = QStringLiteral("|| 中文 😀\r\n(120){4}1-5[4:1]~r/8-4[4:1]~l, 2~r, ");
    const int caretTokenStart = caretSource.indexOf("1-5");
    const int suffixCaret = caretSource.indexOf("~r");
    host.load(caretSource);
    host.editor.send(SCI_SETSEL, caretSource.left(suffixCaret).toUtf8().size(), caretSource.left(suffixCaret).toUtf8().size());
    expect(host.applyBisectionAtEditorSelection()
        && host.text().contains("1-5[4:1]~[head=r;track=0:l|0.5:r]/8-4[4:1]~l")
        && host.text().left(caretTokenStart) == caretSource.left(caretTokenStart)
        && host.editor.send(SCI_GETSELECTIONSTART) == caretSource.left(caretTokenStart).toUtf8().size()
        && host.editor.send(SCI_GETSELECTIONEND) == host.text().left(host.text().indexOf('/')).toUtf8().size(),
        "caret within a Slide annotation targets only its complete native token and selects the rewritten token");
    const QString caretOnce = host.text();
    expect(host.applyBisectionAtEditorSelection()
        && host.text().contains("track=0:l|0.25:l|0.5:r|0.75:r]/8-4[4:1]~l"),
        "repeating Alt+C after a caret edit bisects the same Slide without editing its neighbor");
    host.editor.send(SCI_UNDO);
    expect(host.text() == caretOnce, "one native undo restores only the last caret-triggered bisection");
    host.editor.send(SCI_UNDO);
    expect(host.text() == caretSource && !host.editor.send(SCI_CANUNDO), "one native undo restores the first caret-targeted Slide edit");
    for (int caret : QVector<int>{int(caretSource.size()) - 1, int(caretSource.indexOf("2~r")) + 1, int(caretSource.indexOf('/')), 3}) {
        expect(!host.controller.handOrderCommand(caretSource, caret, caret, "hand_order.bisect_annotation").transaction.hasEdit,
            "blank, non-Slide, delimiter and comment carets do not edit a neighboring Slide");
    }
    const QString eofSlide = "1-5[4:1]~r";
    expect(host.controller.handOrderCommand(eofSlide, eofSlide.size(), eofSlide.size(), "hand_order.bisect_annotation").transaction.hasEdit,
        "caret at the end of a final Slide token can bisect that token");
    const QString swapSource = "(120){4}1~l/2~r/3~m/1-5[4:1]~[head=r;track=0:l|0.5:r],";
    host.load(swapSource);
    expect(host.apply(host.controller.handOrderCommand(host.text(), 0, host.text().size(), "hand_order.swap_hands"))
        && host.text().contains("1~r/2~l/3~m") && host.text().contains("head=l;track=0:r|0.5:l"),
        "selected hand swap uses the real editor transaction");
    host.editor.send(SCI_UNDO);
    expect(host.text() == swapSource && !host.editor.send(SCI_CANUNDO),
        "one native undo restores every swapped hand");
    for (const auto& command : miacode::ui::handOrderCommandSpecs()) {
        expect(!command.id.contains("analyze") && !command.id.contains("optimize")
            && !command.id.contains("basic_layout"), "public command table contains manual operations only");
    }
    return ok ? 0 : 1;
}

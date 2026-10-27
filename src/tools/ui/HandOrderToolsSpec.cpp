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
    expect(!host.controller.handOrderCommand(host.text(), 8, 8, "hand_order.bisect_annotation").transaction.hasEdit
        && !host.controller.handOrderCommand(host.text(), 8, 8, "hand_order.swap_hands").transaction.hasEdit,
        "direct manual commands require an eligible source target");
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
    expect(host.text() == bisectSource && !host.editor.send(SCI_CANUNDO),
        "one native undo removes the first bisection");
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

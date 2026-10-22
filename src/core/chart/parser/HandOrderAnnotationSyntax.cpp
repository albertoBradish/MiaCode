#include "core/chart/parser/HandOrderAnnotationSyntax.h"

namespace miacode::hand_order {

TokenParts splitToken(const QString& token)
{
    const int tilde = token.indexOf(QChar('~'));
    if (tilde < 0) {
        return {token, QString(), -1};
    }
    return {token.left(tilde), token.mid(tilde), tilde};
}

QString stripAnnotations(const QString& chartText)
{
    QString plain;
    plain.reserve(chartText.size());
    bool inComment = false;
    for (int i = 0; i < chartText.size(); ++i) {
        const QChar ch = chartText.at(i);
        if (ch == QChar('\n')) {
            inComment = false;
        }
        if (!inComment && ch == QChar('|') && i + 1 < chartText.size()
            && chartText.at(i + 1) == QChar('|')) {
            inComment = true;
        }
        if (inComment || ch != QChar('~')) {
            plain.append(ch);
            continue;
        }

        int end = i + 1;
        bool openContainer = end < chartText.size() && chartText.at(end) == QChar('[');
        while (end < chartText.size()) {
            const QChar next = chartText.at(end);
            if (next == QChar('\n') || next == QChar('\r')
                || (next.isSpace() && !openContainer)
                || next == QChar(',') || next == QChar('/')
                || next == QChar('`')
                || (next == QChar('|') && end + 1 < chartText.size()
                        && chartText.at(end + 1) == QChar('|'))) {
                break;
            }
            if (next == QChar(']')) {
                openContainer = false;
            }
            ++end;
        }
        i = end - 1;
    }
    return plain;
}

} // namespace miacode::hand_order

#include "editor/SimaiCompletionCatalog.h"

#include "core/chart/parser/SimaiParser.h"

#include <QRegularExpression>
#include <QSet>

namespace miacode::editor {

namespace {

bool isForbiddenCompletionCharacter(QChar ch)
{
    return ch.isSpace() || ch.category() == QChar::Other_Control
        || ch == QLatin1Char('[') || ch == QLatin1Char(']')
        || ch == QLatin1Char('{') || ch == QLatin1Char('}');
}

bool mayAnnotate(const QString& base)
{
    if (base.isEmpty()) return false;
    const QChar first = base.front().toUpper();
    if (first >= QLatin1Char('1') && first <= QLatin1Char('8')) {
        bool digitCluster = base.size() > 1;
        for (QChar ch : base) digitCluster = digitCluster && ch >= QLatin1Char('1') && ch <= QLatin1Char('8');
        return !digitCluster;
    }
    return first == QLatin1Char('A') || first == QLatin1Char('B')
        || first == QLatin1Char('C') || first == QLatin1Char('D') || first == QLatin1Char('E');
}

int slideBranchCount(const QString& base)
{
    static const QString slideOperators = QStringLiteral("-^v<>Vpqszw");
    bool hasSlide = false;
    int branches = 1;
    int bracketDepth = 0;
    for (QChar ch : base) {
        if (ch == QLatin1Char('[')) { ++bracketDepth; continue; }
        if (ch == QLatin1Char(']')) { bracketDepth = qMax(0, bracketDepth - 1); continue; }
        if (bracketDepth > 0) continue;
        hasSlide = hasSlide || slideOperators.contains(ch);
        if (ch == QLatin1Char('*')) ++branches;
    }
    return hasSlide ? branches : 0;
}

QStringList matchingCandidates(const QStringList& available, const QString& prefix)
{
    QStringList matches;
    for (const QString& candidate : available) {
        if (candidate.startsWith(prefix)) matches.append(candidate);
    }
    if (matches.size() == 1 && matches.front() == prefix) return {};
    return matches;
}

QStringList handCandidates(const QString& prefix)
{
    return matchingCandidates({QStringLiteral("l"), QStringLiteral("r"), QStringLiteral("m")}, prefix);
}

}  // namespace

HandOrderCompletion handOrderCompletionAt(const QString& chartText, int position)
{
    if (position <= 0 || position > chartText.size()) return {};
    const int lineStart = chartText.lastIndexOf(QLatin1Char('\n'), position - 1) + 1;
    const int commentStart = chartText.indexOf(QStringLiteral("||"), lineStart);
    if (commentStart >= 0 && commentStart < position) return {};

    int tokenStart = position;
    while (tokenStart > lineStart) {
        const QChar previous = chartText.at(tokenStart - 1);
        if (previous.isSpace() || previous == QLatin1Char(',')
            || previous == QLatin1Char('/') || previous == QLatin1Char('`')) break;
        --tokenStart;
    }
    const QString beforeCaret = chartText.mid(tokenStart, position - tokenStart);
    const int tilde = beforeCaret.indexOf(QLatin1Char('~'));
    if (tilde <= 0) return {};
    const QString base = beforeCaret.left(tilde);
    if (!mayAnnotate(base)) return {};
    const int branchCount = slideBranchCount(base);
    const int suffixStart = tokenStart + tilde + 1;
    const QString suffix = beforeCaret.mid(tilde + 1);

    if (!suffix.startsWith(QLatin1Char('['))) {
        if (!suffix.isEmpty() && suffix != QLatin1String("l")
            && suffix != QLatin1String("r") && suffix != QLatin1String("m")) return {};
        QStringList candidates = handCandidates(suffix);
        if (branchCount > 0 && suffix.isEmpty() && branchCount <= 16) {
            for (QChar hand : {QLatin1Char('l'), QLatin1Char('r'), QLatin1Char('m')}) {
                QStringList tracks;
                for (int branch = 0; branch < branchCount; ++branch) {
                    const QString key = branchCount == 1 ? QStringLiteral("track")
                        : QStringLiteral("branch%1").arg(branch);
                    tracks.append(QStringLiteral("%1=0:%2").arg(key, QString(hand)));
                }
                candidates.append(QStringLiteral("[%1]").arg(tracks.join(QLatin1Char(';'))));
            }
        }
        return candidates.isEmpty() ? HandOrderCompletion{} : HandOrderCompletion{suffixStart, candidates};
    }
    if (branchCount == 0 || suffix.indexOf(QLatin1Char(']')) >= 0) return {};

    const QString inside = suffix.mid(1);
    const int segmentOffset = inside.lastIndexOf(QLatin1Char(';')) + 1;
    const QString segment = inside.mid(segmentOffset);
    const int segmentStart = suffixStart + 1 + segmentOffset;
    QSet<QString> usedKeys;
    const QStringList prior = inside.left(segmentOffset).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& entry : prior) usedKeys.insert(entry.section(QLatin1Char('='), 0, 0));

    const int equals = segment.indexOf(QLatin1Char('='));
    if (equals < 0) {
        QStringList keys;
        if (!usedKeys.contains(QStringLiteral("head"))) keys.append(QStringLiteral("head="));
        if (branchCount == 1) {
            if (!usedKeys.contains(QStringLiteral("track"))) keys.append(QStringLiteral("track="));
        } else {
            for (int branch = 0; branch < branchCount; ++branch) {
                const QString key = QStringLiteral("branch%1").arg(branch);
                if (!usedKeys.contains(key)) keys.append(key + QLatin1Char('='));
            }
        }
        const QStringList matches = matchingCandidates(keys, segment);
        return matches.isEmpty() ? HandOrderCompletion{} : HandOrderCompletion{segmentStart, matches};
    }

    const QString key = segment.left(equals);
    const QString value = segment.mid(equals + 1);
    const int valueStart = segmentStart + equals + 1;
    if (key == QLatin1String("head")) {
        const QStringList candidates = handCandidates(value);
        return candidates.isEmpty() ? HandOrderCompletion{} : HandOrderCompletion{valueStart, candidates};
    }
    bool validTrackKey = branchCount == 1 && key == QLatin1String("track");
    if (branchCount > 1 && key.startsWith(QLatin1String("branch"))) {
        bool numberOk = false;
        const int branch = key.mid(6).toInt(&numberOk);
        validTrackKey = numberOk && branch >= 0 && branch < branchCount;
    }
    if (!validTrackKey) return {};
    const int colon = value.lastIndexOf(QLatin1Char(':'));
    const int pipe = value.lastIndexOf(QLatin1Char('|'));
    if (colon > pipe) {
        const int handStart = valueStart + colon + 1;
        const QStringList candidates = handCandidates(value.mid(colon + 1));
        return candidates.isEmpty() ? HandOrderCompletion{} : HandOrderCompletion{handStart, candidates};
    }
    if (value.isEmpty()) {
        return {valueStart, {QStringLiteral("0:l"), QStringLiteral("0:r"), QStringLiteral("0:m")}};
    }
    if (pipe > 0 && value.mid(pipe + 1).isEmpty()) {
        const int previousPipe = value.lastIndexOf(QLatin1Char('|'), pipe - 1);
        const QString priorNode = value.mid(previousPipe + 1, pipe - previousPipe - 1);
        if (priorNode.startsWith(QLatin1String("0:"))) {
            return {valueStart + pipe + 1,
                    {QStringLiteral("0.5:l"), QStringLiteral("0.5:r"), QStringLiteral("0.5:m")}};
        }
    }
    return {};
}

bool isBracketOpening(QChar ch)
{
    return ch == QLatin1Char('(') || ch == QLatin1Char('[') || ch == QLatin1Char('{');
}

bool isBracketClosing(QChar ch)
{
    return ch == QLatin1Char(')') || ch == QLatin1Char(']') || ch == QLatin1Char('}');
}

QChar closingBracketFor(QChar opening)
{
    switch (opening.unicode()) {
    case '(': return QLatin1Char(')');
    case '[': return QLatin1Char(']');
    case '{': return QLatin1Char('}');
    default: return QChar();
    }
}

SimaiCompletionConfig defaultCompletionConfig()
{
    static const SimaiCompletionConfig defaults{
        {QStringLiteral("8:1"), QStringLiteral("4:1"), QStringLiteral("16:3"), QStringLiteral("384:1")},
        {QStringLiteral("16"), QStringLiteral("24"), QStringLiteral("32")}};
    return defaults;
}

bool isValidCompletionValue(CompletionCandidateGroup group, const QString& value)
{
    if (value.isEmpty()) return false;
    for (const QChar ch : value) {
        if (isForbiddenCompletionCharacter(ch)) return false;
    }
    if (group == CompletionCandidateGroup::Duration)
        return SimaiParser::isValidHoldDurationSignature(value);
    bool ok = false;
    const int division = value.toInt(&ok);
    return ok && division > 0;
}

QString sanitizeCompletionInput(const QString& value)
{
    QString sanitized;
    sanitized.reserve(value.size());
    for (const QChar ch : value) {
        if (!isForbiddenCompletionCharacter(ch)) sanitized.append(ch);
    }
    return sanitized;
}

QStringList normalizeCompletionValues(CompletionCandidateGroup group, const QStringList& values)
{
    QStringList normalized;
    QSet<QString> seen;
    for (const QString& raw : values) {
        const QString value = raw.trimmed();
        if (!isValidCompletionValue(group, value) || seen.contains(value)) continue;
        seen.insert(value);
        normalized.append(value);
    }
    if (!normalized.isEmpty()) return normalized;
    const auto defaults = defaultCompletionConfig();
    return group == CompletionCandidateGroup::Duration ? defaults.durations : defaults.divisions;
}

QStringList squareDurationCandidates(const SimaiCompletionConfig& config)
{
    QStringList out;
    out.reserve(config.durations.size());
    for (const QString& value : config.durations) out.append(value + QLatin1Char(']'));
    return out;
}

QStringList holdDurationCandidates(const SimaiCompletionConfig& config)
{
    QStringList out;
    out.reserve(config.durations.size());
    for (const QString& value : config.durations)
        out.append(QLatin1Char('[') + value + QLatin1Char(']'));
    return out;
}

QStringList braceDivisionCandidates(const SimaiCompletionConfig& config)
{
    QStringList out;
    out.reserve(config.divisions.size());
    for (const QString& value : config.divisions) out.append(value + QLatin1Char('}'));
    return out;
}

QStringList squareDurationCandidates() { return squareDurationCandidates(defaultCompletionConfig()); }
QStringList holdDurationCandidates() { return holdDurationCandidates(defaultCompletionConfig()); }
QStringList braceDivisionCandidates() { return braceDivisionCandidates(defaultCompletionConfig()); }

QString normalizeBpmToken(const QString& raw)
{
    const QString trimmed = raw.trimmed();
    bool ok = false;
    const double value = trimmed.toDouble(&ok);
    if (!ok || value <= 0.0) {
        return trimmed;
    }
    QString text = QString::number(value, 'f', 3);
    while (text.contains(QLatin1Char('.')) && text.endsWith(QLatin1Char('0'))) {
        text.chop(1);
    }
    if (text.endsWith(QLatin1Char('.'))) {
        text.chop(1);
    }
    return text;
}

QStringList appearedBpmValues(const QString& chartText)
{
    static const QRegularExpression bpmMarker(QStringLiteral("\\(\\s*([0-9]+(?:\\.[0-9]+)?)\\s*\\)"));
    QStringList seen;
    auto it = bpmMarker.globalMatch(chartText);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString token = normalizeBpmToken(match.captured(1));
        if (!token.isEmpty() && !seen.contains(token)) {
            seen.append(token);
        }
    }
    return seen;
}

QStringList parenBpmCandidates(const QString& wholeBpm, const QString& chartText)
{
    QStringList out;
    QStringList seen;
    const auto push = [&](const QString& rawToken) {
        const QString token = normalizeBpmToken(rawToken);
        if (token.isEmpty() || seen.contains(token)) {
            return;
        }
        seen.append(token);
        out.append(token + QLatin1Char(')'));
    };
    if (!wholeBpm.trimmed().isEmpty()) {
        push(wholeBpm);
    }
    const QStringList appeared = appearedBpmValues(chartText);
    for (const QString& token : appeared) {
        push(token);
    }
    return out;
}

QStringList candidatesForOpening(QChar opening, const QString& wholeBpm, const QString& chartText)
{
    return candidatesForOpening(opening, wholeBpm, chartText, defaultCompletionConfig());
}

QStringList candidatesForOpening(QChar opening, const QString& wholeBpm, const QString& chartText,
                                 const SimaiCompletionConfig& config)
{
    switch (opening.unicode()) {
    case '[': return squareDurationCandidates(config);
    case '{': return braceDivisionCandidates(config);
    case '(': return parenBpmCandidates(wholeBpm, chartText);
    default: return {};
    }
}

}  // namespace miacode::editor

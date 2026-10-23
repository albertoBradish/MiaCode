// Included by SimaiParser.cpp after the base parser helpers.

struct HandOrderAnnotation {
    bool valid = false;
    int startOffset = -1;
    HandOrderHand head = HandOrderHand::Unspecified;
    QVector<QVector<HandOrderTrackStop>> branchTracks;
    QVector<HandOrderWifiMode> wifiModes;
    QVector<QVector<QVector<HandOrderTrackStop>>> wifiSectors;
};

bool parseHandOrderHand(const QString& text, HandOrderHand* hand)
{
    if (hand == nullptr || text.size() != 1) {
        return false;
    }
    switch (text.at(0).unicode()) {
    case 'l': *hand = HandOrderHand::Left; return true;
    case 'r': *hand = HandOrderHand::Right; return true;
    case 'm': *hand = HandOrderHand::Either; return true;
    default: return false;
    }
}

bool parseHandOrderTrack(const QString& text, QVector<HandOrderTrackStop>* stops)
{
    if (stops == nullptr || text.isEmpty()) {
        return false;
    }
    stops->clear();
    const QStringList entries = text.split(QChar('|'), Qt::KeepEmptyParts);
    double previous = -1.0;
    for (const QString& entry : entries) {
        const int colon = entry.indexOf(QChar(':'));
        if (colon <= 0 || colon != entry.lastIndexOf(QChar(':'))) {
            return false;
        }
        const QString progressText = entry.left(colon);
        bool sawPoint = false;
        for (int i = 0; i < progressText.size(); ++i) {
            const QChar ch = progressText.at(i);
            if (ch == QChar('.') && !sawPoint && i > 0 && i + 1 < progressText.size()) {
                sawPoint = true;
            } else if (ch < QChar('0') || ch > QChar('9')) {
                return false;
            }
        }
        bool progressOk = false;
        const double progress = progressText.toDouble(&progressOk);
        HandOrderHand hand = HandOrderHand::Unspecified;
        if (!progressOk || !std::isfinite(progress)
            || progress < 0.0 || progress >= 1.0 || progress <= previous
            || !parseHandOrderHand(entry.mid(colon + 1), &hand)) {
            return false;
        }
        stops->append({progress, hand});
        previous = progress;
    }
    return !stops->isEmpty() && stops->constFirst().progress == 0.0;
}

bool parseHandOrderTrackValue(const QString& text, QVector<HandOrderTrackStop>* stops,
    HandOrderWifiMode* mode, QVector<QVector<HandOrderTrackStop>>* sectors)
{
    *mode = HandOrderWifiMode::Legacy;
    stops->clear();
    sectors->clear();
    if (text.startsWith(QStringLiteral("single:"))) {
        *mode = HandOrderWifiMode::Single;
        if (!parseHandOrderTrack(text.mid(7), stops)) return false;
        const HandOrderHand hand = stops->constFirst().hand;
        // Single names one physical hand for the whole Wifi, including Either.
        return std::all_of(stops->cbegin(), stops->cend(), [hand](const auto& stop) {
            return stop.hand == hand;
        });
    }
    if (text.startsWith(QStringLiteral("split:"))) {
        *mode = HandOrderWifiMode::Split;
        const auto halves = text.mid(6).split(QLatin1Char('&'), Qt::KeepEmptyParts);
        sectors->resize(2);
        return halves.size() == 2 && parseHandOrderTrack(halves.at(0), &(*sectors)[0])
            && parseHandOrderTrack(halves.at(1), &(*sectors)[1]);
    }
    return parseHandOrderTrack(text, stops);
}

void appendHandOrderError(ParseState* state, int line, int col, int endCol, const QString& detail)
{
    if (state == nullptr) {
        return;
    }
    SimaiMessage error;
    error.line = line;
    error.col = qMax(1, col);
    error.endCol = qMax(error.col, endCol);
    error.message = QStringLiteral("Invalid hand-order annotation: ") + detail;
    state->result.annotationErrors.append(error);
}

HandOrderAnnotation parseHandOrderAnnotation(
    ParseState* state,
    const QString& token,
    int line,
    int column,
    QString* baseToken)
{
    HandOrderAnnotation annotation;
    if (baseToken == nullptr) {
        return annotation;
    }
    const miacode::hand_order::TokenParts parts = miacode::hand_order::splitToken(token);
    *baseToken = parts.baseToken;
    if (parts.suffixOffset < 0) {
        return annotation;
    }
    annotation.startOffset = parts.suffixOffset;
    const int annotationCol = column + parts.suffixOffset;
    const int annotationEndCol = column + token.size() - 1;
    const auto fail = [&](const QString& detail) {
        appendHandOrderError(state, line, annotationCol, annotationEndCol, detail);
        return annotation;
    };

    if (baseToken->isEmpty()) {
        return fail(QStringLiteral("missing note"));
    }
    const QString suffix = parts.suffix.mid(1);
    const bool isSlide = tokenContainsSlideShape(*baseToken);
    const int branchCount = isSlide ? baseToken->count(QChar('*')) + 1 : 1;
    if (suffix.size() == 1) {
        if (!parseHandOrderHand(suffix, &annotation.head)) {
            return fail(QStringLiteral("expected ~l, ~r or ~m"));
        }
        if (isSlide) {
            annotation.branchTracks.resize(branchCount);
            for (QVector<HandOrderTrackStop>& track : annotation.branchTracks) {
                track.append({0.0, annotation.head});
            }
        } else if (baseToken->size() > 1) {
            bool digitCluster = true;
            for (QChar ch : *baseToken) {
                if (!isDigitLane(ch)) {
                    digitCluster = false;
                    break;
                }
            }
            if (digitCluster) {
                return fail(QStringLiteral("digit clusters need one annotation per note"));
            }
        }
        annotation.valid = true;
        return annotation;
    }

    if (!suffix.startsWith(QChar('[')) || !suffix.endsWith(QChar(']'))
        || suffix.count(QChar('[')) != 1 || suffix.count(QChar(']')) != 1) {
        return fail(QStringLiteral("expected a closed ~[...] suffix"));
    }
    if (!isSlide) {
        return fail(QStringLiteral("complex form requires a slide"));
    }
    const QString body = suffix.mid(1, suffix.size() - 2);
    if (body.isEmpty()) {
        return fail(QStringLiteral("empty container"));
    }
    QVector<HandOrderTrackStop> sharedTrack;
    annotation.branchTracks.resize(branchCount);
    annotation.wifiModes.fill(HandOrderWifiMode::Legacy, branchCount);
    annotation.wifiSectors.resize(branchCount);
    QVector<bool> branchSeen(branchCount, false);
    bool headSeen = false;
    bool trackSeen = false;
    bool anyBranchSeen = false;
    const QStringList clauses = body.split(QChar(';'), Qt::KeepEmptyParts);
    for (const QString& clause : clauses) {
        const int equals = clause.indexOf(QChar('='));
        if (equals <= 0 || equals != clause.lastIndexOf(QChar('='))) {
            return fail(QStringLiteral("expected key=value clauses"));
        }
        const QString key = clause.left(equals);
        const QString value = clause.mid(equals + 1);
        if (key == QStringLiteral("head")) {
            if (headSeen || !parseHandOrderHand(value, &annotation.head)) {
                return fail(QStringLiteral("invalid or duplicate head"));
            }
            headSeen = true;
        } else if (key == QStringLiteral("track")) {
            if (branchCount != 1 || trackSeen || anyBranchSeen
                || !parseHandOrderTrackValue(value, &sharedTrack, &annotation.wifiModes[0], &annotation.wifiSectors[0])) {
                return fail(value.startsWith(QStringLiteral("single:"))
                    ? QStringLiteral("single mode requires a valid timeline with one unchanged hand for the entire Wifi")
                    : value.startsWith(QStringLiteral("split:"))
                    ? QStringLiteral("split mode requires exactly two valid sector timelines")
                    : QStringLiteral("invalid or duplicate track"));
            }
            trackSeen = true;
        } else if (key.startsWith(QStringLiteral("branch"))) {
            const QString indexText = key.mid(6);
            bool indexOk = false;
            const int branchIndex = indexText.toInt(&indexOk);
            if (branchCount < 2 || trackSeen || !indexOk
                || indexText != QString::number(branchIndex)
                || branchIndex < 0 || branchIndex >= branchCount
                || branchSeen.at(branchIndex)
                || !parseHandOrderTrackValue(value, &annotation.branchTracks[branchIndex],
                    &annotation.wifiModes[branchIndex], &annotation.wifiSectors[branchIndex])) {
                return fail(value.startsWith(QStringLiteral("single:"))
                    ? QStringLiteral("single mode requires a valid branch timeline with one unchanged hand for the entire Wifi")
                    : value.startsWith(QStringLiteral("split:"))
                    ? QStringLiteral("split mode requires exactly two valid branch sector timelines")
                    : QStringLiteral("invalid or duplicate branch"));
            }
            branchSeen[branchIndex] = true;
            anyBranchSeen = true;
        } else {
            return fail(QStringLiteral("unknown key: ") + key);
        }
    }
    if (branchCount == 1 && !trackSeen) {
        return fail(QStringLiteral("track is required"));
    }
    if (branchCount > 1) {
        for (bool seen : branchSeen) {
            if (!seen) {
                return fail(QStringLiteral("all branches require a track"));
            }
        }
    } else {
        annotation.branchTracks[0] = sharedTrack;
    }
    annotation.valid = true;
    return annotation;
}

void applyHandOrderAnnotation(
    ParseState* state,
    const HandOrderAnnotation& annotation,
    int firstMarker,
    int line,
    int column,
    int tokenLength)
{
    if (state == nullptr || !annotation.valid) {
        return;
    }
    const int annotationCol = column + annotation.startOffset;
    const int annotationEndCol = column + tokenLength - 1;
    for (int i = firstMarker; i < state->result.noteMarkers.size(); ++i) {
        const auto& marker = state->result.noteMarkers.at(i);
        const bool slide = marker.type == QStringLiteral("slide") || marker.type == QStringLiteral("wifi");
        if ((slide && (marker.slideBranchIndex < 0 || marker.slideBranchIndex >= annotation.branchTracks.size()))
            || (!slide && !annotation.branchTracks.isEmpty())
            || (marker.type != QStringLiteral("wifi")
                && annotation.wifiModes.value(marker.slideBranchIndex, HandOrderWifiMode::Legacy) != HandOrderWifiMode::Legacy)) {
            appendHandOrderError(state, line, annotationCol, annotationEndCol,
                QStringLiteral("track mode does not match the native note or branch"));
            return;
        }
    }
    for (int i = firstMarker; i < state->result.noteMarkers.size(); ++i) {
        TimelineNoteMarker& marker = state->result.noteMarkers[i];
        marker.handOrderAnnotationCol = annotationCol;
        marker.handOrderAnnotationEndCol = annotationEndCol;
        const bool isSlide = marker.type == QStringLiteral("slide")
            || marker.type == QStringLiteral("wifi");
        if (isSlide) {
            marker.handOrderHead = annotation.head;
            marker.handOrderTrackStops = annotation.branchTracks.at(marker.slideBranchIndex);
            marker.handOrderWifiMode = annotation.wifiModes.value(marker.slideBranchIndex, HandOrderWifiMode::Legacy);
            marker.handOrderWifiSectorStops = annotation.wifiSectors.value(marker.slideBranchIndex);
        } else {
            marker.handOrderHead = annotation.head;
        }
    }
}

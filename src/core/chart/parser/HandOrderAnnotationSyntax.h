#pragma once

#include <QString>

namespace miacode::hand_order {

struct TokenParts {
    QString baseToken;
    QString suffix;
    int suffixOffset = -1;
};

// Splits at the first annotation delimiter, including malformed suffixes.
TokenParts splitToken(const QString& token);

// Removes token annotations while retaining all other source characters.
QString stripAnnotations(const QString& chartText);

} // namespace miacode::hand_order

#pragma once
#include "Types.h"
namespace tb::model::textfile {
inline constexpr int MaxLines=50000;
struct LineRange { qsizetype offset,length,terminatorLength; };
QString validate(const TextFileData& file);
// Returns MaxLines + 1 if the document exceeds the supported limit.
int lineCount(const QByteArray& bytes);
QList<LineRange> lineRanges(const QByteArray& bytes);
QString lineText(const TextFileData& file,int line);
bool isVisible(const TextFileData& file,int line);
}

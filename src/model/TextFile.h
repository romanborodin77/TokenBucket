#pragma once
#include "Types.h"
namespace tb::model::textfile {
inline constexpr int MaxLines=50000;
struct LineRange { qsizetype offset,length,terminatorLength; };
QString validate(const TextFileData& file);
QList<LineRange> lineRanges(const QByteArray& bytes);
QString lineText(const TextFileData& file,int line);
bool isVisible(const TextFileData& file,int line);
}

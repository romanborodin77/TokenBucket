#include "TextFile.h"
#include "VaultModel.h"
#include <algorithm>
#include <QStringDecoder>
namespace tb::model::textfile {
QList<LineRange> lineRanges(const QByteArray& bytes) {
    QList<LineRange> result;
    qsizetype start=bytes.startsWith("\xef\xbb\xbf")?3:0;
    if(start==bytes.size()) return result;
    for(qsizetype i=start;i<bytes.size();++i) {
        if(bytes[i]!='\r' && bytes[i]!='\n') continue;
        const qsizetype terminator=bytes[i]=='\r' && i+1<bytes.size() && bytes[i+1]=='\n'?2:1;
        result.append({start,i-start,terminator});
        if(result.size()>MaxLines) return result;
        i+=terminator-1; start=i+1;
    }
    result.append({start,bytes.size()-start,0}); return result;
}
QString validate(const TextFileData& file) {
    if(file.fileName.isEmpty() || file.fileName.size()>255 || file.fileName=="." || file.fileName==".."
        || file.fileName.contains('/') || file.fileName.contains('\\') || file.fileName.contains(':') || !file.fileName.isValidUtf16())
        return QStringLiteral("Укажите имя файла без каталога, от 1 до 255 символов.");
    for(auto ch:file.fileName.toUcs4()) if(!QChar::isPrint(ch)) return QStringLiteral("Имя файла содержит управляющий символ.");
    if(file.bytes.size()>VaultModel::MaxSecretBytes) return QStringLiteral("Файл превышает 1 МиБ.");
    if(file.bytes.contains('\0') || !file.bytes.isValidUtf8()) return QStringLiteral("Нужен корректный UTF-8 без NUL.");
    const auto lines=lineRanges(file.bytes);
    if(lines.size()>MaxLines) return QStringLiteral("В файле больше 50 000 строк.");
    int previous=-1;
    for(int line:file.visibleLines) {
        if(line<=previous || line>=lines.size()) return QStringLiteral("Недопустимый список видимых строк.");
        previous=line;
    }
    return {};
}
QString lineText(const TextFileData& file,int line) {
    const auto ranges=lineRanges(file.bytes);
    if(line<0 || line>=ranges.size()) return {};
    const auto r=ranges[line];
    return QStringDecoder(QStringConverter::Utf8,QStringConverter::Flag::ConvertInitialBom)(QByteArrayView(file.bytes).sliced(r.offset,r.length));
}
bool isVisible(const TextFileData& file,int line) {
    return line>=0 && std::binary_search(file.visibleLines.cbegin(),file.visibleLines.cend(),line);
}
}

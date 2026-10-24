#pragma once
#include "../src/platform/FileOps.h"
#include <functional>
struct FaultOps : tb::store::FileOps {
    bool failWrite=false;
    int failRenameCall=0,renameCalls=0,writeCalls=0,corruptWriteCall=0,failWriteCall=0;
    std::function<void(const QString&)> beforeWrite;
    std::function<void(const QString&,const QString&)> beforeRename,afterRename;
    bool writeAll(const QString& path,const QByteArray& bytes) override {
        ++writeCalls; if(beforeWrite) beforeWrite(path);
        if(failWrite || writeCalls==failWriteCall) return false;
        return tb::store::realFileOps().writeAll(path,writeCalls==corruptWriteCall?QByteArray("partial"):bytes);
    }
    bool renameReplace(const QString& from,const QString& to) override {
        ++renameCalls; if(beforeRename) beforeRename(from,to);
        if(renameCalls==failRenameCall) return false;
        const bool renamed=tb::store::realFileOps().renameReplace(from,to);
        if(renamed && afterRename) afterRename(from,to);
        return renamed;
    }
    bool removeFile(const QString& path) override {return tb::store::realFileOps().removeFile(path);}
};

#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>
#include "../src/app/Preferences.h"
using namespace tb;
class TestPreferences : public QObject {
    Q_OBJECT
private slots:
    void defaultsAndReadOnly() {
        QTemporaryDir dir; QSettings s(dir.filePath("prefs.ini"),QSettings::IniFormat);
        const auto v=prefs::read(s);
        QCOMPARE(v,prefs::Values{}); QVERIFY(s.allKeys().isEmpty()); QVERIFY(!QFile::exists(s.fileName()));
    }
    void legacyAndInvalidValues() {
        QTemporaryDir dir; QSettings s(dir.filePath("prefs.ini"),QSettings::IniFormat);
        s.setValue("theme","dark"); s.setValue("trayNotifications",false);
        s.setValue("idleTimeoutMinutes",999); s.setValue("secretRevealSeconds",7);
        s.setValue("clipboardClearSeconds",0); s.setValue("quickPasteDefaultField","unknown");
        s.setValue("closeBehavior","unknown"); s.setValue("hotkey","Ctrl+Tab");
        const auto v=prefs::read(s);
        QCOMPARE(v.theme,QStringLiteral("dark")); QVERIFY(!v.notifications);
        QCOMPARE(v.idleMinutes,60); QCOMPARE(v.revealSeconds,15); QCOMPARE(v.clipboardSeconds,20);
        QCOMPARE(v.defaultField,paste::Field::Secret); QCOMPARE(v.closeBehavior,prefs::CloseBehavior::Hide);
        QCOMPARE(v.hotkey,QKeySequence("Ctrl+Tab")); QVERIFY(!v.autostart.has_value());
        QVERIFY(!prefs::validate(v).isEmpty());
        s.setValue("idleTimeoutMinutes","broken"); s.setValue("theme","unknown");
        QCOMPARE(prefs::read(s).idleMinutes,5); QCOMPARE(prefs::read(s).theme,QStringLiteral("system"));
        s.setValue("idleTimeoutMinutes",-5); QCOMPARE(prefs::read(s).idleMinutes,1);
    }
    void validateRanges() {
        prefs::Values v; QVERIFY(prefs::validate(v).isEmpty());
        for(int seconds : {5,10,15,30}) {v.revealSeconds=seconds; QVERIFY(prefs::validate(v).isEmpty());}
        v.revealSeconds=7; QVERIFY(!prefs::validate(v).isEmpty()); v.revealSeconds=15;
        for(int seconds : {10,20,30,60}) {v.clipboardSeconds=seconds; QVERIFY(prefs::validate(v).isEmpty());}
        v.clipboardSeconds=1; QVERIFY(!prefs::validate(v).isEmpty()); v.clipboardSeconds=20;
        v.idleMinutes=0; QVERIFY(!prefs::validate(v).isEmpty()); v.idleMinutes=61; QVERIFY(!prefs::validate(v).isEmpty());
        v.idleMinutes=5; v.hotkey=QKeySequence("F12"); QVERIFY(!prefs::validate(v).isEmpty());
    }
    void checkedRoundtrip() {
        QTemporaryDir dir; QSettings s(dir.filePath("prefs.ini"),QSettings::IniFormat); QString error;
        QVERIFY2(prefs::writeValue(s,"secretRevealSeconds",5,&error),qPrintable(error));
        QVERIFY(prefs::writeValue(s,"quickPasteDefaultField","login",&error));
        QVERIFY(prefs::writeValue(s,"closeBehavior","lock-hide",&error));
        QSettings reader(s.fileName(),s.format());
        const auto v=prefs::read(reader);
        QCOMPARE(v.revealSeconds,5); QCOMPARE(v.defaultField,paste::Field::Login);
        QCOMPARE(v.closeBehavior,prefs::CloseBehavior::LockHide);
        QVERIFY(prefs::writeValue(s,"secretRevealSeconds",30,&error)); QVERIFY(error.isEmpty());
        QCOMPARE(prefs::read(s).revealSeconds,30);
    }
    void failedWriteDoesNotFlushLaterAndRetryWorks() {
        QTemporaryDir dir; const auto blocker=dir.filePath("parent");
        QFile file(blocker); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        QSettings s(blocker+"/prefs.ini",QSettings::IniFormat); QString error;
        QVERIFY(!prefs::writeValue(s,"secretRevealSeconds",5,&error)); QVERIFY(!error.isEmpty());
        QVERIFY(!s.contains("secretRevealSeconds")); QVERIFY(QFile::remove(blocker)); QVERIFY(QDir().mkpath(blocker));
        s.sync(); QVERIFY(!s.contains("secretRevealSeconds"));
        QVERIFY2(prefs::writeValue(s,"secretRevealSeconds",10,&error),qPrintable(error));
        QSettings reader(s.fileName(),s.format()); QCOMPARE(reader.value("secretRevealSeconds").toInt(),10);
    }
    void failedOverwriteRestoresCachedValue() {
        QTemporaryDir dir; const auto parent=dir.filePath("parent"); QVERIFY(QDir().mkpath(parent));
        QSettings s(parent+"/prefs.ini",QSettings::IniFormat); s.setValue("theme","light"); s.sync();
        QVERIFY(QFile::remove(s.fileName())); QVERIFY(QDir().rmdir(parent));
        QFile blocker(parent); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
        QString error; QVERIFY(!prefs::writeValue(s,"theme","dark",&error)); QCOMPARE(s.value("theme").toString(),QStringLiteral("light"));
        QVERIFY(QFile::remove(parent)); QVERIFY(QDir().mkpath(parent)); s.sync();
        QVERIFY(prefs::writeValue(s,"theme","system",&error)); QSettings reader(s.fileName(),s.format());
        QCOMPARE(reader.value("theme").toString(),QStringLiteral("system"));
    }
};
QTEST_MAIN(TestPreferences)
#include "test_preferences.moc"

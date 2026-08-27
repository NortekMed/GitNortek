//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "Test.h"
#include "conf/Settings.h"

#include <QTranslator>

using namespace QTest;

class TestSanity : public QObject {
  Q_OBJECT

private slots:
  void sanity();
  void translationCatalog();
};

void TestSanity::sanity() {
  QCOMPARE(QCoreApplication::applicationName(), QString(GITNORTEK_NAME));
  QCOMPARE(QCoreApplication::applicationVersion(), QString(GITNORTEK_VERSION));
}

void TestSanity::translationCatalog() {
  QTranslator translator;
  const QString path = Settings::l10nDir().absolutePath();
  QVERIFY2(translator.load(QLocale("de_DE"), "gitnortek", "_", path),
           qPrintable(QString("Unable to load GitNortek translations from %1")
                          .arg(path)));
}

TEST_MAIN(TestSanity)

#include "sanity.moc"

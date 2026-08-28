#include "Test.h"

#include "conf/Setting.h"
#include "conf/Settings.h"
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

using namespace QTest;

class TestSetting : public QObject {
  Q_OBJECT

private slots:
  void defines_a_non_empty_settings_key_for_each_id();
  void defines_each_settings_key_only_once();
  void defaults_diff_presentation_settings();
  void persists_diff_mode();
  void persists_edge_whitespace();
  void resolves_about_documents();

private:
  template <typename TId> QList<TId> ids() const {
    QList<TId> ids;
    QMetaEnum metaEnum = QMetaEnum::fromType<TId>();
    for (int i = 0; i < metaEnum.keyCount(); i++) {
      ids << static_cast<TId>(metaEnum.value(i));
    }
    return ids;
  }

  template <class T, typename TId> QStringList settingsKeys() {
    QStringList settingsKeys;
    foreach (const TId id, ids<TId>()) { settingsKeys.append(T::key(id)); }
    return settingsKeys;
  }

  template <class T, typename TId> void verifyNonEmptySettingsKeyForEachId() {
    QMetaEnum metaEnum = QMetaEnum::fromType<TId>();

    foreach (const TId id, ids<TId>()) {
      const QString settingsKey = T::key(id);

      QVERIFY2(!settingsKey.isEmpty(),
               qPrintable(QString("no settings key defined for %1::%2::%3")
                              .arg(metaEnum.scope(), metaEnum.name(),
                                   metaEnum.valueToKey(static_cast<int>(id)))));
    }
  }
};

void TestSetting::defines_a_non_empty_settings_key_for_each_id() {
  verifyNonEmptySettingsKeyForEachId<Setting, Setting::Id>();
  verifyNonEmptySettingsKeyForEachId<Prompt, Prompt::Kind>();
}

void TestSetting::defines_each_settings_key_only_once() {
  QStringList allSettingsKeys;
  allSettingsKeys.append(settingsKeys<Setting, Setting::Id>());
  allSettingsKeys.append(settingsKeys<Prompt, Prompt::Kind>());

  QStringList uniqueSettingsKeys;
  foreach (const QString &settingsKey, allSettingsKeys) {
    QVERIFY2(!uniqueSettingsKeys.contains(settingsKey),
             qPrintable(
                 QString("the settings key '%1' is used for multiple settings")
                     .arg(settingsKey)));
    uniqueSettingsKeys.append(settingsKey);
  }
}

void TestSetting::defaults_diff_presentation_settings() {
  Settings *settings = Settings::instance();
  QCOMPARE(settings->diffMode(), Settings::DiffMode::Inline);
  QVERIFY(!settings->isEdgeWhitespaceIgnored());
}

void TestSetting::persists_diff_mode() {
  Settings *settings = Settings::instance();

  settings->setDiffMode(Settings::DiffMode::Hunk);
  QCOMPARE(settings->diffMode(), Settings::DiffMode::Hunk);
  QCOMPARE(QSettings().value("diff/mode").toInt(),
           static_cast<int>(Settings::DiffMode::Hunk));

  settings->setDiffMode(Settings::DiffMode::Split);
  QCOMPARE(settings->diffMode(), Settings::DiffMode::Split);
  QCOMPARE(QSettings().value("diff/mode").toInt(),
           static_cast<int>(Settings::DiffMode::Split));

  settings->setDiffMode(Settings::DiffMode::Inline);
  QCOMPARE(settings->diffMode(), Settings::DiffMode::Inline);
  QCOMPARE(QSettings().value("diff/mode").toInt(),
           static_cast<int>(Settings::DiffMode::Inline));

  settings->setDiffMode(static_cast<Settings::DiffMode>(99));
  QCOMPARE(settings->diffMode(), Settings::DiffMode::Inline);
  settings->setDiffMode(Settings::DiffMode::Inline);
}

void TestSetting::persists_edge_whitespace() {
  Settings *settings = Settings::instance();

  settings->setEdgeWhitespaceIgnored(true);
  QVERIFY(settings->isEdgeWhitespaceIgnored());
  QVERIFY(QSettings().value("diff/whitespace/ignoreEdge").toBool());

  settings->setEdgeWhitespaceIgnored(false);
  QVERIFY(!settings->isEdgeWhitespaceIgnored());
  QVERIFY(!QSettings().value("diff/whitespace/ignoreEdge").toBool());
}

void TestSetting::resolves_about_documents() {
  const QDir dir = Settings::docDir();
  QVERIFY(QFileInfo::exists(dir.filePath("changelog.html")));
  QVERIFY(QFileInfo::exists(dir.filePath("acknowledgments.html")));
  QVERIFY(QFileInfo::exists(dir.filePath("privacy.html")));
}

int main(int argc, char *argv[]) {
  QTemporaryDir settings;
  if (!settings.isValid())
    return 1;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settings.path());
  return Test::runTest<TestSetting>(argc, argv);
}

#include "Setting.moc"

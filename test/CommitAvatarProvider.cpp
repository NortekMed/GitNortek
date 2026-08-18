//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
#include "conf/Setting.h"
#include "conf/Settings.h"
#include "git/Reference.h"
#include "ui/CommitAvatarProvider.h"
#include <QBuffer>
#include <QImage>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

using namespace QTest;

namespace {

QUrl imageUrl() {
  QImage image(32, 32, QImage::Format_ARGB32);
  image.fill(Qt::red);
  QByteArray data;
  QBuffer buffer(&data);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return QUrl("data:image/png;base64," + QString::fromLatin1(data.toBase64()));
}

} // namespace

class TestCommitAvatarProvider : public QObject {
  Q_OBJECT

private slots:
  void batchingAndCache();
  void disabledAndUnavailable();
  void missingAvatar();
};

void TestCommitAvatarProvider::batchingAndCache() {
  Test::ScratchRepository repo;
  QProcess git;
  git.setWorkingDirectory(repo->workdir().path());
  git.start(GIT_EXECUTABLE, {"commit", "--allow-empty", "-m", "first"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git.start(GIT_EXECUTABLE, {"commit", "--allow-empty", "-m", "second"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);

  git::Commit second = repo->head().target();
  git::Commit first = second.parents().first();
  int calls = 0;
  QStringList requested;
  CommitAvatarProvider provider(
      repo,
      [&calls, &requested](const QStringList &oids, int,
                           const CommitAvatarProvider::LookupCallback &done) {
        ++calls;
        requested = oids;
        CommitAvatarProvider::AvatarMap avatars;
        for (const QString &oid : oids)
          avatars.insert(oid, imageUrl());
        done(avatars);
      });
  Settings::instance()->setValue(Setting::Id::ShowAvatars, true);

  QVERIFY(provider.avatar(first, 16, 1.0).isNull());
  QVERIFY(provider.avatar(second, 16, 1.0).isNull());
  QSignalSpy ready(&provider, &CommitAvatarProvider::avatarReady);
  QTRY_COMPARE(ready.count(), 2);
  QCOMPARE(calls, 1);
  QCOMPARE(requested.size(), 2);

  QPixmap avatar = provider.avatar(first, 16, 1.0);
  QVERIFY(!avatar.isNull());
  QCOMPARE(avatar.size(), QSize(16, 16));
  QImage rendered = avatar.toImage();
  QCOMPARE(rendered.pixelColor(0, 0).alpha(), 0);
  QCOMPARE(rendered.pixelColor(8, 8), QColor(Qt::red));
  QCOMPARE(provider.avatar(first, 16, 1.0).cacheKey(), avatar.cacheKey());
  QPixmap highDpi = provider.avatar(first, 16, 2.0);
  QCOMPARE(highDpi.size(), QSize(32, 32));
  QCOMPARE(highDpi.devicePixelRatio(), 2.0);
  QCOMPARE(calls, 1);
}

void TestCommitAvatarProvider::disabledAndUnavailable() {
  Test::ScratchRepository repo;
  QProcess git;
  git.setWorkingDirectory(repo->workdir().path());
  git.start(GIT_EXECUTABLE, {"commit", "--allow-empty", "-m", "disabled"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git::Commit commit = repo->head().target();
  int calls = 0;
  CommitAvatarProvider provider(
      repo,
      [&calls](const QStringList &, int,
               const CommitAvatarProvider::LookupCallback &) { ++calls; });
  Settings::instance()->setValue(Setting::Id::ShowAvatars, false);
  QVERIFY(provider.avatar(commit, 16, 1.0).isNull());
  qWait(30);
  QCOMPARE(calls, 0);

  CommitAvatarProvider unavailable(repo);
  QVERIFY(!unavailable.isAvailable());
}

void TestCommitAvatarProvider::missingAvatar() {
  Test::ScratchRepository repo;
  QProcess git;
  git.setWorkingDirectory(repo->workdir().path());
  git.start(GIT_EXECUTABLE, {"commit", "--allow-empty", "-m", "missing"});
  QVERIFY(git.waitForFinished());
  QCOMPARE(git.exitCode(), 0);
  git::Commit commit = repo->head().target();

  int calls = 0;
  CommitAvatarProvider provider(
      repo, [&calls](const QStringList &, int,
                     const CommitAvatarProvider::LookupCallback &done) {
        ++calls;
        done({});
      });
  Settings::instance()->setValue(Setting::Id::ShowAvatars, true);
  QSignalSpy ready(&provider, &CommitAvatarProvider::avatarReady);
  QVERIFY(provider.avatar(commit, 16, 1.0).isNull());
  QTRY_COMPARE(ready.count(), 1);
  QVERIFY(provider.avatar(commit, 16, 1.0).isNull());
  qWait(30);
  QCOMPARE(calls, 1);
}

int main(int argc, char *argv[]) {
  QTemporaryDir settings;
  if (!settings.isValid())
    return 1;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     settings.path());
  Application::setInTest();
  int testArgc = argc;
  auto app = Test::createApp(argc, argv);
  TestCommitAvatarProvider test;
  return QTest::qExec(&test, testArgc, argv);
}

#include "CommitAvatarProvider.moc"

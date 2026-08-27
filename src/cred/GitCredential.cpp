//
//          Copyright (c) 2018, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "GitCredential.h"
#include "qtsupport.h"
#include <QStandardPaths>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>
#include <QTextStream>
#include <QUrl>

namespace {

QString host(const QString &url) {
  QString host = QUrl(url).host();
  if (!host.isEmpty())
    return host;

  // Extract hostname from SSH URL.
  int end = url.indexOf(':');
  int begin = url.indexOf('@') + 1;
  return url.mid(begin, end - begin);
}

QString protocol(const QString &url) {
  QString scheme = QUrl(url).scheme();
  return !scheme.isEmpty() ? scheme : "ssh";
}

} // namespace

GitCredential::GitCredential(const QString &name) : mName(name) {}

bool GitCredential::get(const QString &url, QString &username,
                        QString &password) {
  QProcess process;
  QString helper = command();
  process.start(helper, {"get"});
  if (!process.waitForStarted()) {
    log(QString("failed to start credential helper %1: %2")
            .arg(helper, process.errorString()));
    return false;
  }

  QTextStream out(&process);
  out << "protocol=" << protocol(url) << Qt::endl;
  out << "host=" << host(url) << Qt::endl;
  if (!username.isEmpty())
    out << "username=" << username << Qt::endl;
  out << Qt::endl;

  process.closeWriteChannel();
  process.waitForFinished();

  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    log(QString("credential helper %1 get failed: %2")
            .arg(helper, QString::fromUtf8(process.readAllStandardError())));
    return false;
  }

  QString output = process.readAllStandardOutput();
  foreach (const QString &line, output.split('\n')) {
    int pos = line.indexOf('=');
    if (pos < 0)
      continue;

    QString key = line.left(pos);
    QString value = line.mid(pos + 1);
    if (key == "username") {
      username = value;
    } else if (key == "password") {
      password = value;
    }
  }

  return !username.isEmpty() && !password.isEmpty();
}

bool GitCredential::store(const QString &url, const QString &username,
                          const QString &password) {
  QProcess process;
  QString helper = command();
  process.start(helper, {"store"});
  if (!process.waitForStarted()) {
    log(QString("failed to start credential helper %1: %2")
            .arg(helper, process.errorString()));
    return false;
  }

  QTextStream out(&process);
  out << "protocol=" << protocol(url) << Qt::endl;
  out << "host=" << host(url) << Qt::endl;
  out << "username=" << username << Qt::endl;
  out << "password=" << password << Qt::endl;
  out << Qt::endl;

  process.closeWriteChannel();
  process.waitForFinished();

  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    log(QString("credential helper %1 store failed: %2")
            .arg(helper, QString::fromUtf8(process.readAllStandardError())));
    return false;
  }

  return true;
}

QString GitCredential::command() const {
  QString name = QString("git-credential-%1").arg(mName);
  QDir appDir = QCoreApplication::applicationDirPath();
  appDir.cd("credential-helpers");

  // Prefer credential helpers directly installed into GitNortek's app dir
  QString candidate =
      QStandardPaths::findExecutable(name, QStringList(appDir.path()));
  if (!candidate.isEmpty()) {
    return candidate;
  }

  candidate = QStandardPaths::findExecutable(name);
  if (!candidate.isEmpty()) {
    return candidate;
  }

  QProcess git;
  git.start("git", {"--exec-path"});
  if (git.waitForStarted() && git.waitForFinished() &&
      git.exitStatus() == QProcess::NormalExit && git.exitCode() == 0) {
    QString execPath = QString::fromUtf8(git.readAllStandardOutput()).trimmed();
    candidate = QStandardPaths::findExecutable(name, QStringList(execPath));
    if (!candidate.isEmpty()) {
      return candidate;
    }
  }

#ifdef Q_OS_WIN
  // Look for GIT CLI installation path
  QString gitPath = QStandardPaths::findExecutable("git");
  if (!gitPath.isEmpty()) {
    QDir gitDir = QFileInfo(gitPath).dir();
    if (gitDir.dirName() == "cmd" || gitDir.dirName() == "bin") {
      gitDir.cdUp();

#ifdef Q_OS_WIN64
      gitDir.cd("mingw64");
#else
      gitDir.cd("mingw32");
#endif

      gitDir.cd("bin");

      candidate =
          QStandardPaths::findExecutable(name, QStringList(gitDir.path()));
      if (!candidate.isEmpty()) {
        return candidate;
      }
    }
  }
#endif

  return name;
}

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
#include <QProcessEnvironment>
#include <QProcess>
#include <QTextStream>

bool GitCredential::get(const QString &url, QString &username,
                        QString &password) {
  return run("fill", url, username, &password);
}

bool GitCredential::store(const QString &url, const QString &username,
                          const QString &password) {
  QString user(username);
  QString secret(password);
  return run("approve", url, user, &secret);
}

bool GitCredential::run(const QString &action, const QString &url,
                        QString &username, QString *password) const {
  QProcess process;
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  // GitNortek controls interactive authentication. Never let the child Git
  // process invoke an askpass helper or wait for a terminal prompt.
  environment.insert("GIT_TERMINAL_PROMPT", "0");
  environment.remove("GIT_ASKPASS");
  environment.remove("SSH_ASKPASS");
  process.setProcessEnvironment(environment);
  process.start("git", {"credential", action});
  if (!process.waitForStarted()) {
    log(QString("failed to start git credential %1: %2")
            .arg(action, process.errorString()));
    return false;
  }

  QTextStream out(&process);
  out << "url=" << url << Qt::endl;
  if (!username.isEmpty())
    out << "username=" << username << Qt::endl;
  if (action == "approve")
    out << "password=" << *password << Qt::endl;
  out << Qt::endl;

  process.closeWriteChannel();
  process.waitForFinished();

  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    log(QString("git credential %1 failed: %2")
            .arg(action, QString::fromUtf8(process.readAllStandardError())));
    return false;
  }

  if (action == "approve")
    return true;

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
      *password = value;
    }
  }

  return !username.isEmpty() && !password->isEmpty();
}

//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef LOCALWORKSPACES_H
#define LOCALWORKSPACES_H

#include "LocalWorkspace.h"
#include <QList>
#include <QObject>

class QFileSystemWatcher;
class QTimer;

class LocalWorkspaces : public QObject {
  Q_OBJECT

public:
  int count() const;
  const LocalWorkspace *workspace(int index) const;
  const LocalWorkspace *workspace(const QString &id) const;

  bool add(const LocalWorkspace &workspace, QString *error = nullptr);
  bool update(const LocalWorkspace &workspace, QString *error = nullptr);
  bool remove(const QString &id, QString *error = nullptr);

  bool addRepository(const QString &id, const QString &path,
                     QString *error = nullptr);
  bool addRepositories(const QString &id, const QStringList &paths,
                       QStringList *invalidPaths = nullptr,
                       QStringList *duplicatePaths = nullptr,
                       QString *error = nullptr);
  bool removeRepository(const QString &id, const QString &path,
                        QString *error = nullptr);
  bool rescanSynchronizedDirectory(const QString &id,
                                   QString *error = nullptr);

  static LocalWorkspaces *instance();

signals:
  void workspacesChanged();

private:
  LocalWorkspaces(QObject *parent = nullptr);

  LocalWorkspace *find(const QString &id);
  void load();
  void store() const;
  void changed();
  void updateWatchedDirectories();

  QList<LocalWorkspace> mWorkspaces;
  QFileSystemWatcher *mWatcher;
  QTimer *mRescanTimer;
};

#endif

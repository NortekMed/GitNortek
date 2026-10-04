//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef LOCALWORKSPACES_H
#define LOCALWORKSPACES_H

#include "LocalWorkspace.h"
#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <atomic>
#include <memory>

class QFileSystemWatcher;
class QTimer;

class LocalWorkspaces : public QObject {
  Q_OBJECT

public:
  ~LocalWorkspaces() override;

  int count() const;
  const LocalWorkspace *workspace(int index) const;
  const LocalWorkspace *workspace(const QString &id) const;

  bool add(const LocalWorkspace &workspace, QString *error = nullptr);
  bool update(const LocalWorkspace &workspace, QString *error = nullptr);
  bool remove(const QString &id, QString *error = nullptr);

  // These requests perform repository discovery and synchronized-directory
  // scans outside the GUI thread. Completion is delivered on this object's
  // thread.
  void addAsync(const LocalWorkspace &workspace);
  void updateAsync(const LocalWorkspace &workspace);

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
  void addRepositoriesAsync(const QString &id, const QStringList &paths);
  void rescanSynchronizedDirectoryAsync(const QString &id);

  static LocalWorkspaces *instance();

signals:
  void workspacesChanged();
  void workspaceAdded(const QString &id, bool success, const QString &error);
  void workspaceUpdated(const QString &id, bool success, const QString &error);
  void repositoriesAdded(const QString &id, bool success,
                         const QStringList &invalidPaths,
                         const QStringList &duplicatePaths,
                         const QString &error);
  void synchronizedDirectoryRescanned(const QString &id, bool success,
                                      const QString &error);

private:
  struct InitialScanResult {
    QString id;
    QString directory;
    QStringList repositories;
    QStringList watchedDirectories;
    QString error;
    bool success = false;
  };

  struct OperationResult {
    enum class Kind { Add, Update, AddRepositories, Rescan };

    Kind kind = Kind::Add;
    quint64 generation = 0;
    QString id;
    LocalWorkspace workspace;
    QStringList watchedDirectories;
    QStringList invalidPaths;
    QStringList duplicatePaths;
    QString error;
    bool success = false;
  };

  LocalWorkspaces(QObject *parent = nullptr);

  LocalWorkspace *find(const QString &id);
  void load();
  void store() const;
  void changed();
  void startInitialSynchronization();
  void finishInitialSynchronization();
  void startRescanSynchronization();
  void finishRescanSynchronization();
  void finishOperation();
  void applyOperationResult(const OperationResult &result);
  void scheduleRescanSynchronization();
  void setWatchedDirectories(const QStringList &directories);
  void updateWatchedDirectories();

  QList<LocalWorkspace> mWorkspaces;
  QFileSystemWatcher *mWatcher;
  QTimer *mRescanTimer;
  QFutureWatcher<QList<InitialScanResult>> *mInitialScanWatcher;
  std::shared_ptr<std::atomic_bool> mInitialScanCancel;
  quint64 mInitialScanGeneration = 0;
  quint64 mInitialScanRunGeneration = 0;

  QFutureWatcher<QList<InitialScanResult>> *mRescanWatcher;
  std::shared_ptr<std::atomic_bool> mRescanCancel;
  quint64 mRescanGeneration = 0;
  quint64 mRescanRunGeneration = 0;
  bool mRescanPending = false;

  QFutureWatcher<OperationResult> *mOperationWatcher;
  quint64 mOperationGeneration = 0;
};

#endif

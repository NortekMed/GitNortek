//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "LocalWorkspaces.h"
#include "git/Repository.h"
#include "util/PerformanceTrace.h"
#include "util/Path.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QMetaObject>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>
#include <QtConcurrent>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>

namespace {

const QString kLocalWorkspacesKey = "localWorkspaces";

struct DirectoryWatchRegistration {
  bool wait(const std::atomic_bool &cancelled) {
    std::unique_lock<std::mutex> lock(mutex);
    while (!completed && !cancelled.load())
      condition.wait_for(lock, std::chrono::milliseconds(25));
    return completed && !cancelled.load();
  }

  void complete() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      completed = true;
    }
    condition.notify_all();
  }

  std::mutex mutex;
  std::condition_variable condition;
  bool completed = false;
};

struct DirectoryWatchRegistrationCompletion {
  std::shared_ptr<DirectoryWatchRegistration> registration;

  ~DirectoryWatchRegistrationCompletion() { registration->complete(); }
};

void setError(QString *error, const QString &message) {
  if (error)
    *error = message;
}

bool containsPath(const QStringList &paths, const QString &path) {
  return util::containsPath(paths, path);
}

bool repositoryRoot(const QString &path, QString *root) {
  git::Repository repository = git::Repository::open(path, true);
  if (!repository.isValid())
    return false;

  QDir directory = repository.dir(false);
  *root = directory.canonicalPath();
  if (root->isEmpty())
    *root = directory.absolutePath();
  *root = QDir::cleanPath(*root);
  return true;
}

QString normalizedDirectoryPath(const QString &path) {
  QFileInfo directory(path);
  QString normalized = directory.canonicalFilePath();
  if (normalized.isEmpty())
    normalized = directory.absoluteFilePath();
  return QDir::cleanPath(normalized);
}

QStringList normalizedRepositories(const QStringList &repositories) {
  QStringList result;
  for (const QString &path : repositories) {
    QString normalized = path;
    QString root;
    if (repositoryRoot(path, &root))
      normalized = root;
    if (!normalized.isEmpty() && !containsPath(result, normalized))
      result.append(normalized);
  }
  return result;
}

QStringList manualRepositories(const LocalWorkspace &workspace) {
  QStringList result = normalizedRepositories(workspace.manualRepositories);
  for (const QString &path : workspace.repositories) {
    if (!containsPath(workspace.synchronizedRepositories, path) &&
        !containsPath(result, path))
      result.append(path);
  }
  return result;
}

void walkSynchronizedDirectory(
    const QString &path, bool syncDirectory, QStringList *repositories,
    QStringList *watchedDirectories, QSet<QString> *visited,
    const std::atomic_bool *cancelled = nullptr,
    const std::function<void(const QString &)> &repositoryDiscovered = {},
    const std::function<bool(const QString &)> &directoryDiscovered = {}) {
  if (cancelled && cancelled->load())
    return;

  const QString normalized = normalizedDirectoryPath(path);
  if (normalized.isEmpty())
    return;

  const QString key = util::pathCompareKey(normalized);
  if (visited->contains(key))
    return;
  visited->insert(key);

  if (syncDirectory && watchedDirectories &&
      !containsPath(*watchedDirectories, normalized))
    watchedDirectories->append(normalized);

  // Do not descend into repository trees. This also excludes submodules and
  // avoids traversing Git metadata.
  QString root;
  if (repositoryRoot(path, &root)) {
    if (!syncDirectory && util::pathsEqual(normalized, root) && repositories &&
        !containsPath(*repositories, root)) {
      repositories->append(root);
      if (repositoryDiscovered)
        repositoryDiscovered(root);
    }
    return;
  }

  if (!syncDirectory && watchedDirectories &&
      !containsPath(*watchedDirectories, normalized))
    watchedDirectories->append(normalized);
  if (!syncDirectory && directoryDiscovered && !directoryDiscovered(normalized))
    return;
  if (cancelled && cancelled->load())
    return;

  const QFileInfoList children =
      QDir(path).entryInfoList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
                               QDir::Name | QDir::IgnoreCase);
  for (const QFileInfo &child : children)
    walkSynchronizedDirectory(child.absoluteFilePath(), false, repositories,
                              watchedDirectories, visited, cancelled,
                              repositoryDiscovered, directoryDiscovered);
}

bool scanSynchronizedDirectory(
    const QString &path, QStringList *repositories, QString *error,
    QStringList *watchedDirectories = nullptr,
    const std::atomic_bool *cancelled = nullptr,
    const std::function<void(const QString &)> &repositoryDiscovered = {},
    const std::function<bool(const QString &)> &directoryDiscovered = {}) {
  if (watchedDirectories)
    watchedDirectories->clear();

  if (cancelled && cancelled->load()) {
    setError(error, LocalWorkspaces::tr("Workspace scan was canceled."));
    return false;
  }

  if (path.isEmpty()) {
    setError(error, LocalWorkspaces::tr(
                        "Select a directory to enable synchronization."));
    return false;
  }

  QFileInfo syncInfo(path);
  const QString parent = syncInfo.dir().absolutePath();
  if (watchedDirectories && QDir(parent).exists())
    watchedDirectories->append(parent);

  if (cancelled && cancelled->load()) {
    setError(error, LocalWorkspaces::tr("Workspace scan was canceled."));
    return false;
  }

  QDir syncDirectory(path);
  if (!syncDirectory.exists()) {
    setError(error,
             LocalWorkspaces::tr("Synchronized directory does not exist: %1")
                 .arg(path));
    return false;
  }

  repositories->clear();
  QSet<QString> visited;
  walkSynchronizedDirectory(path, true, repositories, watchedDirectories,
                            &visited, cancelled, repositoryDiscovered,
                            directoryDiscovered);
  if (cancelled && cancelled->load()) {
    setError(error, LocalWorkspaces::tr("Workspace scan was canceled."));
    return false;
  }

  return true;
}

void updateRepositories(LocalWorkspace *workspace) {
  workspace->repositories = workspace->manualRepositories;
  for (const QString &path :
       std::as_const(workspace->synchronizedRepositories)) {
    if (!containsPath(workspace->repositories, path))
      workspace->repositories.append(path);
  }
}

bool equal(const LocalWorkspace &lhs, const LocalWorkspace &rhs) {
  return lhs.id == rhs.id && lhs.name == rhs.name &&
         lhs.description == rhs.description && lhs.iconName == rhs.iconName &&
         lhs.color == rhs.color && lhs.syncDirectory == rhs.syncDirectory &&
         lhs.syncEnabled == rhs.syncEnabled &&
         lhs.repositories == rhs.repositories &&
         lhs.manualRepositories == rhs.manualRepositories &&
         lhs.synchronizedRepositories == rhs.synchronizedRepositories;
}

bool sameWorkspaceConfiguration(const LocalWorkspace &lhs,
                                const LocalWorkspace &rhs) {
  return lhs.id == rhs.id && lhs.name == rhs.name &&
         lhs.description == rhs.description && lhs.iconName == rhs.iconName &&
         lhs.color == rhs.color && lhs.syncDirectory == rhs.syncDirectory &&
         lhs.syncEnabled == rhs.syncEnabled;
}

bool sameWorkspaceScanTarget(const LocalWorkspace &lhs,
                             const LocalWorkspace &rhs) {
  return lhs.id == rhs.id &&
         util::pathsEqual(lhs.syncDirectory, rhs.syncDirectory);
}

bool pausedWorkspaceMatchesScan(const LocalWorkspace &workspace,
                                const LocalWorkspace &scanConfiguration) {
  return scanConfiguration.syncEnabled && !workspace.syncEnabled &&
         sameWorkspaceScanTarget(workspace, scanConfiguration);
}

bool namesMatch(const QString &lhs, const QString &rhs) {
  return lhs.trimmed().compare(rhs.trimmed(), Qt::CaseInsensitive) == 0;
}

QString cleanOptionalPath(const QString &path) {
  return path.isEmpty() ? QString() : QDir::cleanPath(path);
}

enum class PreparedOperationKind { Add, Update, AddRepositories, Rescan };

struct PreparedOperation {
  PreparedOperationKind kind = PreparedOperationKind::Add;
  QString id;
  LocalWorkspace workspace;
  QStringList watchedDirectories;
  QStringList invalidPaths;
  QStringList duplicatePaths;
  QString error;
  bool success = false;
  bool syncDirectoryUnchanged = false;
};

const LocalWorkspace *findWorkspace(const QList<LocalWorkspace> &workspaces,
                                    const QString &id) {
  for (const LocalWorkspace &workspace : workspaces) {
    if (workspace.id == id)
      return &workspace;
  }
  return nullptr;
}

bool validateWorkspaceBasics(const LocalWorkspace &workspace,
                             const QList<LocalWorkspace> &existing,
                             bool updating, QString *error,
                             bool validateSyncDirectory = true) {
  if (workspace.id.isEmpty()) {
    setError(error, LocalWorkspaces::tr("Workspace ID must not be empty."));
    return false;
  }
  if (!updating && findWorkspace(existing, workspace.id)) {
    setError(error,
             LocalWorkspaces::tr("A workspace with this ID already exists."));
    return false;
  }
  if (!findWorkspace(existing, workspace.id) && updating) {
    setError(error, LocalWorkspaces::tr("Workspace not found."));
    return false;
  }
  if (workspace.name.trimmed().isEmpty()) {
    setError(error, LocalWorkspaces::tr("Workspace name must not be empty."));
    return false;
  }
  if (validateSyncDirectory && workspace.syncEnabled &&
      workspace.syncDirectory.isEmpty()) {
    setError(error, LocalWorkspaces::tr(
                        "Select a directory to enable synchronization."));
    return false;
  }
  if (validateSyncDirectory && workspace.syncEnabled &&
      !QDir(workspace.syncDirectory).exists()) {
    setError(error,
             LocalWorkspaces::tr("Synchronized directory does not exist: %1")
                 .arg(workspace.syncDirectory));
    return false;
  }
  for (const LocalWorkspace &candidate : existing) {
    if (candidate.id != workspace.id &&
        namesMatch(candidate.name, workspace.name)) {
      setError(error, LocalWorkspaces::tr(
                          "A workspace with this name already exists."));
      return false;
    }
  }
  return true;
}

PreparedOperation
prepareAdd(const LocalWorkspace &request, const QList<LocalWorkspace> &existing,
           const std::atomic_bool *cancelled,
           const std::function<void(const QString &)> &repositoryDiscovered,
           const std::function<bool(const QString &)> &directoryDiscovered) {
  PreparedOperation result;
  result.kind = PreparedOperationKind::Add;
  result.id = request.id;

  LocalWorkspace added = request;
  added.name = added.name.trimmed();
  added.syncDirectory = cleanOptionalPath(added.syncDirectory);
  added.manualRepositories.clear();
  added.synchronizedRepositories.clear();
  added.repositories.clear();
  result.workspace = added;
  if (!validateWorkspaceBasics(request, existing, false, &result.error, false))
    return result;
  for (const QString &path : request.repositories) {
    if (cancelled && cancelled->load())
      break;
    QString root;
    if (!repositoryRoot(path, &root)) {
      if (result.error.isEmpty())
        result.error =
            LocalWorkspaces::tr("Not a valid Git repository: %1").arg(path);
      continue;
    }
    if (!containsPath(added.manualRepositories, root))
      added.manualRepositories.append(root);
  }

  if (added.syncEnabled) {
    QString scanError;
    if (!scanSynchronizedDirectory(added.syncDirectory,
                                   &added.synchronizedRepositories, &scanError,
                                   &result.watchedDirectories, cancelled,
                                   repositoryDiscovered, directoryDiscovered) &&
        result.error.isEmpty())
      result.error = scanError;
  }

  if (cancelled && cancelled->load() && result.error.isEmpty())
    result.error = LocalWorkspaces::tr("Workspace scan was canceled.");

  updateRepositories(&added);
  result.workspace = added;
  result.success = result.error.isEmpty() && !(cancelled && cancelled->load());
  return result;
}

PreparedOperation prepareUpdate(const LocalWorkspace &request,
                                const QList<LocalWorkspace> &existing) {
  PreparedOperation result;
  result.kind = PreparedOperationKind::Update;
  result.id = request.id;
  if (!validateWorkspaceBasics(request, existing, true, &result.error))
    return result;

  LocalWorkspace updated = request;
  updated.name = updated.name.trimmed();
  updated.syncDirectory = cleanOptionalPath(updated.syncDirectory);
  updated.manualRepositories = manualRepositories(request);
  const LocalWorkspace *stored = findWorkspace(existing, request.id);
  result.syncDirectoryUnchanged =
      stored && sameWorkspaceScanTarget(updated, *stored);
  if (updated.syncEnabled) {
    updated.synchronizedRepositories.clear();
    if (!scanSynchronizedDirectory(updated.syncDirectory,
                                   &updated.synchronizedRepositories,
                                   &result.error, &result.watchedDirectories))
      return result;
  } else {
    if (stored)
      updated.synchronizedRepositories = stored->synchronizedRepositories;
  }
  updateRepositories(&updated);
  result.workspace = updated;
  result.success = true;
  return result;
}

PreparedOperation
prepareAddRepositories(const QString &id, const QStringList &paths,
                       const QList<LocalWorkspace> &existing) {
  PreparedOperation result;
  result.kind = PreparedOperationKind::AddRepositories;
  result.id = id;
  const LocalWorkspace *stored = findWorkspace(existing, id);
  if (!stored) {
    result.error = LocalWorkspaces::tr("Workspace not found.");
    return result;
  }

  LocalWorkspace updated = *stored;
  for (const QString &path : paths) {
    QString root;
    if (!repositoryRoot(path, &root)) {
      result.invalidPaths.append(path);
      continue;
    }
    if (containsPath(updated.manualRepositories, root)) {
      result.duplicatePaths.append(root);
      continue;
    }
    updated.manualRepositories.append(root);
    if (!containsPath(updated.repositories, root))
      updated.repositories.append(root);
  }
  result.workspace = updated;
  result.success = true;
  return result;
}

PreparedOperation prepareRescan(const QString &id,
                                const QList<LocalWorkspace> &existing) {
  PreparedOperation result;
  result.kind = PreparedOperationKind::Rescan;
  result.id = id;
  const LocalWorkspace *stored = findWorkspace(existing, id);
  if (!stored) {
    result.error = LocalWorkspaces::tr("Workspace not found.");
    return result;
  }
  if (!stored->syncEnabled || stored->syncDirectory.isEmpty()) {
    result.error =
        LocalWorkspaces::tr("Workspace has no synchronized directory.");
    return result;
  }

  LocalWorkspace updated = *stored;
  if (!scanSynchronizedDirectory(updated.syncDirectory,
                                 &updated.synchronizedRepositories,
                                 &result.error, &result.watchedDirectories))
    return result;
  updateRepositories(&updated);
  result.workspace = updated;
  result.success = true;
  return result;
}

} // namespace

LocalWorkspaces::LocalWorkspaces(QObject *parent)
    : QObject(parent), mWatcher(new QFileSystemWatcher(this)),
      mRescanTimer(new QTimer(this)),
      mInitialScanWatcher(new QFutureWatcher<QList<InitialScanResult>>(this)),
      mRescanWatcher(new QFutureWatcher<QList<InitialScanResult>>(this)),
      mOperationWatcher(new QFutureWatcher<OperationResult>(this)) {
  load();
  mRescanTimer->setSingleShot(true);
  mRescanTimer->setInterval(300);
  connect(mWatcher, &QFileSystemWatcher::directoryChanged, this, [this] {
    for (auto it = mActiveWorkspaceScans.cbegin();
         it != mActiveWorkspaceScans.cend(); ++it)
      mWorkspaceScanDirectoryChanges.insert(it.key());
    mRescanTimer->start();
  });
  connect(mRescanTimer, &QTimer::timeout, this,
          &LocalWorkspaces::startRescanSynchronization);

  connect(mInitialScanWatcher,
          &QFutureWatcher<QList<InitialScanResult>>::finished, this,
          &LocalWorkspaces::finishInitialSynchronization);
  connect(mRescanWatcher, &QFutureWatcher<QList<InitialScanResult>>::finished,
          this, &LocalWorkspaces::finishRescanSynchronization);
  connect(mOperationWatcher, &QFutureWatcher<OperationResult>::finished, this,
          &LocalWorkspaces::finishOperation);

  // Do not discover persisted synchronized workspaces while a main window is
  // being constructed. The scan can walk a large or unavailable filesystem.
  QTimer::singleShot(0, this, &LocalWorkspaces::startInitialSynchronization);
}

LocalWorkspaces::~LocalWorkspaces() {
  if (mWorkspaceAddCancel)
    mWorkspaceAddCancel->store(true);
  if (mInitialScanCancel)
    mInitialScanCancel->store(true);
  if (mRescanCancel)
    mRescanCancel->store(true);
  if (mInitialScanWatcher && mInitialScanWatcher->isRunning())
    mInitialScanWatcher->waitForFinished();
  if (mRescanWatcher && mRescanWatcher->isRunning())
    mRescanWatcher->waitForFinished();
  if (mOperationWatcher && mOperationWatcher->isRunning())
    mOperationWatcher->waitForFinished();
}

int LocalWorkspaces::count() const { return mWorkspaces.size(); }

const LocalWorkspace *LocalWorkspaces::workspace(int index) const {
  return index >= 0 && index < mWorkspaces.size() ? &mWorkspaces.at(index)
                                                  : nullptr;
}

const LocalWorkspace *LocalWorkspaces::workspace(const QString &id) const {
  for (const LocalWorkspace &workspace : mWorkspaces) {
    if (workspace.id == id)
      return &workspace;
  }
  return nullptr;
}

quint64 LocalWorkspaces::workspaceScanGeneration(const QString &id) const {
  return mActiveWorkspaceScans.value(id, 0);
}

bool LocalWorkspaces::add(const LocalWorkspace &workspace, QString *error) {
  setError(error, {});
  if (workspace.id.isEmpty()) {
    setError(error, tr("Workspace ID must not be empty."));
    return false;
  }
  if (find(workspace.id)) {
    setError(error, tr("A workspace with this ID already exists."));
    return false;
  }
  if (workspace.name.trimmed().isEmpty()) {
    setError(error, tr("Workspace name must not be empty."));
    return false;
  }
  if (workspace.syncEnabled && workspace.syncDirectory.isEmpty()) {
    setError(error, tr("Select a directory to enable synchronization."));
    return false;
  }
  if (workspace.syncEnabled && !QDir(workspace.syncDirectory).exists()) {
    setError(error, tr("Synchronized directory does not exist: %1")
                        .arg(workspace.syncDirectory));
    return false;
  }
  for (const LocalWorkspace &current : std::as_const(mWorkspaces)) {
    if (namesMatch(current.name, workspace.name)) {
      setError(error, tr("A workspace with this name already exists."));
      return false;
    }
  }

  LocalWorkspace added = workspace;
  added.name = added.name.trimmed();
  added.syncDirectory = cleanOptionalPath(added.syncDirectory);
  added.manualRepositories.clear();
  added.synchronizedRepositories.clear();
  added.repositories.clear();
  for (const QString &path : workspace.repositories) {
    QString root;
    if (!repositoryRoot(path, &root)) {
      setError(error, tr("Not a valid Git repository: %1").arg(path));
      return false;
    }
    if (!containsPath(added.manualRepositories, root))
      added.manualRepositories.append(root);
  }
  QStringList watchedDirectories;
  if (added.syncEnabled &&
      !scanSynchronizedDirectory(added.syncDirectory,
                                 &added.synchronizedRepositories, error,
                                 &watchedDirectories))
    return false;
  updateRepositories(&added);

  mWorkspaces.append(added);
  addWatchedDirectories(watchedDirectories);
  changed();
  return true;
}

bool LocalWorkspaces::update(const LocalWorkspace &workspace, QString *error) {
  setError(error, {});
  LocalWorkspace *current = find(workspace.id);
  if (!current) {
    setError(error, tr("Workspace not found."));
    return false;
  }
  if (workspace.name.trimmed().isEmpty()) {
    setError(error, tr("Workspace name must not be empty."));
    return false;
  }
  if (workspace.syncEnabled && workspace.syncDirectory.isEmpty()) {
    setError(error, tr("Select a directory to enable synchronization."));
    return false;
  }
  if (workspace.syncEnabled && !QDir(workspace.syncDirectory).exists()) {
    setError(error, tr("Synchronized directory does not exist: %1")
                        .arg(workspace.syncDirectory));
    return false;
  }
  for (const LocalWorkspace &candidate : std::as_const(mWorkspaces)) {
    if (candidate.id != workspace.id &&
        namesMatch(candidate.name, workspace.name)) {
      setError(error, tr("A workspace with this name already exists."));
      return false;
    }
  }

  LocalWorkspace updated = workspace;
  updated.name = updated.name.trimmed();
  updated.syncDirectory = cleanOptionalPath(updated.syncDirectory);
  updated.manualRepositories = manualRepositories(workspace);
  QStringList watchedDirectories;
  if (updated.syncEnabled) {
    updated.synchronizedRepositories.clear();
    if (!scanSynchronizedDirectory(updated.syncDirectory,
                                   &updated.synchronizedRepositories, error,
                                   &watchedDirectories))
      return false;
  } else {
    updated.synchronizedRepositories = current->synchronizedRepositories;
  }
  updateRepositories(&updated);
  if (equal(*current, updated))
    return true;

  *current = updated;
  if (mActiveWorkspaceScans.contains(workspace.id)) {
    addWatchedDirectories(watchedDirectories);
    mPendingWorkspaceWatchDirectories.insert(workspace.id, watchedDirectories);
  } else {
    mPendingWorkspaceWatchDirectories.remove(workspace.id);
  }
  changed();
  return true;
}

void LocalWorkspaces::addAsync(const LocalWorkspace &workspace) {
  if (mOperationWatcher->isRunning()) {
    emit workspaceAdded(workspace.id, false,
                        tr("Another workspace operation is already running."));
    return;
  }

  const QList<LocalWorkspace> existing = mWorkspaces;
  QString error;
  if (!validateWorkspaceBasics(workspace, existing, false, &error, false)) {
    emit workspaceAdded(workspace.id, false, error);
    return;
  }

  const quint64 generation = ++mOperationGeneration;
  LocalWorkspace shell = workspace;
  shell.name = shell.name.trimmed();
  shell.syncDirectory = cleanOptionalPath(shell.syncDirectory);
  shell.repositories.clear();
  shell.manualRepositories.clear();
  shell.synchronizedRepositories.clear();
  mWorkspaces.append(shell);
  mWorkspaceAddGenerations.insert(shell.id, generation);
  mWorkspaceAddConfigurations.insert(shell.id, shell);
  mWorkspaceAddCancel = std::make_shared<std::atomic_bool>(false);
  mWorkspaceAddCancelId = shell.id;
  mWorkspaceAddCancelGeneration = generation;
  const std::shared_ptr<std::atomic_bool> cancelled = mWorkspaceAddCancel;
  const quint64 scanGeneration =
      shell.syncEnabled ? ++mWorkspaceScanGeneration : 0;
  if (scanGeneration) {
    mActiveWorkspaceScans.insert(shell.id, scanGeneration);
    mWorkspaceScanConfigurations.insert(shell.id, shell);
  }

  if (shell.syncEnabled && !shell.syncDirectory.isEmpty()) {
    const QFileInfo syncInfo(shell.syncDirectory);
    const QString parentDirectory = syncInfo.dir().absolutePath();
    if (QDir(parentDirectory).exists())
      applyWorkspaceWatchDirectory(shell.id, scanGeneration, parentDirectory);

    const QString syncDirectory = normalizedDirectoryPath(shell.syncDirectory);
    if (QDir(syncDirectory).exists())
      applyWorkspaceWatchDirectory(shell.id, scanGeneration, syncDirectory);
  }

  store();
  mOperationWatcher->setFuture(QtConcurrent::run([this, workspace, existing,
                                                  generation, scanGeneration,
                                                  cancelled] {
    const auto repositoryDiscovered = [this, id = workspace.id,
                                       scanGeneration](const QString &path) {
      QMetaObject::invokeMethod(
          this,
          [this, id, scanGeneration, path] {
            applyDiscoveredRepository(id, scanGeneration, path);
          },
          Qt::QueuedConnection);
    };
    const auto directoryDiscovered = [this, id = workspace.id, scanGeneration,
                                      cancelled](const QString &path) {
      if (cancelled->load())
        return false;

      const auto registration = std::make_shared<DirectoryWatchRegistration>();
      const bool queued = QMetaObject::invokeMethod(
          this,
          [this, id, scanGeneration, path, cancelled, registration] {
            DirectoryWatchRegistrationCompletion completion{registration};
            if (!cancelled->load())
              applyWorkspaceWatchDirectory(id, scanGeneration, path);
          },
          Qt::QueuedConnection);
      if (!queued) {
        cancelled->store(true);
        registration->complete();
      }
      return registration->wait(*cancelled);
    };
    const PreparedOperation prepared =
        prepareAdd(workspace, existing, cancelled.get(), repositoryDiscovered,
                   directoryDiscovered);
    OperationResult result;
    result.kind = OperationResult::Kind::Add;
    result.generation = generation;
    result.scanGeneration = scanGeneration;
    result.id = workspace.id;
    result.workspace = prepared.workspace;
    result.watchedDirectories = prepared.watchedDirectories;
    result.error = prepared.error;
    result.success = prepared.success;
    return result;
  }));
  emit workspacesChanged();
  if (shell.syncEnabled)
    emit workspaceScanStarted(shell.id, scanGeneration);
}

void LocalWorkspaces::updateAsync(const LocalWorkspace &workspace) {
  if (mOperationWatcher->isRunning()) {
    emit workspaceUpdated(
        workspace.id, false,
        tr("Another workspace operation is already running."));
    return;
  }

  const quint64 generation = ++mOperationGeneration;
  const QList<LocalWorkspace> existing = mWorkspaces;
  mOperationWatcher->setFuture(
      QtConcurrent::run([workspace, existing, generation] {
        const PreparedOperation prepared = prepareUpdate(workspace, existing);
        OperationResult result;
        result.kind = OperationResult::Kind::Update;
        result.generation = generation;
        result.id = workspace.id;
        result.workspace = prepared.workspace;
        result.watchedDirectories = prepared.watchedDirectories;
        result.error = prepared.error;
        result.success = prepared.success;
        result.syncDirectoryUnchanged = prepared.syncDirectoryUnchanged;
        return result;
      }));
}

bool LocalWorkspaces::remove(const QString &id, QString *error) {
  setError(error, {});
  for (int i = 0; i < mWorkspaces.size(); ++i) {
    if (mWorkspaces.at(i).id == id) {
      const quint64 scanGeneration = mActiveWorkspaceScans.take(id);
      if (mWorkspaceAddCancel && mWorkspaceAddCancelId == id &&
          mWorkspaceAddCancelGeneration ==
              mWorkspaceAddGenerations.value(id, 0))
        mWorkspaceAddCancel->store(true);
      mPendingWorkspaceWatchDirectories.remove(id);
      mWorkspaceScanDirectoryChanges.remove(id);
      mWorkspaceAddGenerations.remove(id);
      mWorkspaceAddConfigurations.remove(id);
      mWorkspaceScanConfigurations.remove(id);
      mInitialScanWorkspaceGenerations.remove(id);
      if (scanGeneration)
        emit workspaceScanFinished(id, scanGeneration, false, {});
      mWorkspaces.removeAt(i);
      changed();
      return true;
    }
  }

  setError(error, tr("Workspace not found."));
  return false;
}

bool LocalWorkspaces::addRepository(const QString &id, const QString &path,
                                    QString *error) {
  QStringList invalidPaths;
  if (!addRepositories(id, {path}, &invalidPaths, nullptr, error))
    return false;
  if (!invalidPaths.isEmpty()) {
    setError(error, tr("Not a valid Git repository: %1").arg(path));
    return false;
  }
  return true;
}

bool LocalWorkspaces::addRepositories(const QString &id,
                                      const QStringList &paths,
                                      QStringList *invalidPaths,
                                      QStringList *duplicatePaths,
                                      QString *error) {
  setError(error, {});
  if (invalidPaths)
    invalidPaths->clear();
  if (duplicatePaths)
    duplicatePaths->clear();
  LocalWorkspace *workspace = find(id);
  if (!workspace) {
    setError(error, tr("Workspace not found."));
    return false;
  }

  bool changedWorkspace = false;
  for (const QString &path : paths) {
    QString root;
    if (!repositoryRoot(path, &root)) {
      if (invalidPaths)
        invalidPaths->append(path);
      continue;
    }
    if (containsPath(workspace->manualRepositories, root)) {
      if (duplicatePaths)
        duplicatePaths->append(root);
      continue;
    }
    workspace->manualRepositories.append(root);
    if (!containsPath(workspace->repositories, root))
      workspace->repositories.append(root);
    changedWorkspace = true;
  }
  if (changedWorkspace)
    changed();
  return true;
}

void LocalWorkspaces::addRepositoriesAsync(const QString &id,
                                           const QStringList &paths) {
  if (mOperationWatcher->isRunning()) {
    emit repositoriesAdded(
        id, false, {}, {},
        tr("Another workspace operation is already running."));
    return;
  }

  const quint64 generation = ++mOperationGeneration;
  const QList<LocalWorkspace> existing = mWorkspaces;
  mOperationWatcher->setFuture(
      QtConcurrent::run([id, paths, existing, generation] {
        const PreparedOperation prepared =
            prepareAddRepositories(id, paths, existing);
        OperationResult result;
        result.kind = OperationResult::Kind::AddRepositories;
        result.generation = generation;
        result.id = id;
        result.workspace = prepared.workspace;
        result.invalidPaths = prepared.invalidPaths;
        result.duplicatePaths = prepared.duplicatePaths;
        result.error = prepared.error;
        result.success = prepared.success;
        return result;
      }));
}

bool LocalWorkspaces::removeRepository(const QString &id, const QString &path,
                                       QString *error) {
  setError(error, {});
  LocalWorkspace *workspace = find(id);
  if (!workspace) {
    setError(error, tr("Workspace not found."));
    return false;
  }

  QString comparedPath = path;
  QString root;
  if (repositoryRoot(path, &root))
    comparedPath = root;

  if (containsPath(workspace->synchronizedRepositories, comparedPath)) {
    setError(error,
             tr("Repositories synchronized from a directory cannot be removed "
                "individually."));
    return false;
  }

  for (int i = 0; i < workspace->repositories.size(); ++i) {
    if (util::pathsEqual(workspace->repositories.at(i), comparedPath)) {
      workspace->repositories.removeAt(i);
      for (int manual = workspace->manualRepositories.size() - 1; manual >= 0;
           --manual) {
        if (util::pathsEqual(workspace->manualRepositories.at(manual),
                             comparedPath))
          workspace->manualRepositories.removeAt(manual);
      }
      changed();
      return true;
    }
  }

  setError(error, tr("Repository not found in workspace."));
  return false;
}

bool LocalWorkspaces::rescanSynchronizedDirectory(const QString &id,
                                                  QString *error) {
  setError(error, {});
  LocalWorkspace *workspace = find(id);
  if (!workspace) {
    setError(error, tr("Workspace not found."));
    return false;
  }
  if (!workspace->syncEnabled || workspace->syncDirectory.isEmpty()) {
    setError(error, tr("Workspace has no synchronized directory."));
    return false;
  }

  QStringList synchronizedRepositories;
  QStringList watchedDirectories;
  if (!scanSynchronizedDirectory(workspace->syncDirectory,
                                 &synchronizedRepositories, error,
                                 &watchedDirectories))
    return false;

  addWatchedDirectories(watchedDirectories);

  QStringList repositories = workspace->manualRepositories;
  for (const QString &path : std::as_const(synchronizedRepositories)) {
    if (!containsPath(repositories, path))
      repositories.append(path);
  }

  if (repositories != workspace->repositories ||
      synchronizedRepositories != workspace->synchronizedRepositories) {
    workspace->repositories = repositories;
    workspace->synchronizedRepositories = synchronizedRepositories;
    changed();
  }
  return true;
}

void LocalWorkspaces::rescanSynchronizedDirectoryAsync(const QString &id) {
  if (mOperationWatcher->isRunning()) {
    emit synchronizedDirectoryRescanned(
        id, false, tr("Another workspace operation is already running."));
    return;
  }

  const quint64 generation = ++mOperationGeneration;
  const QList<LocalWorkspace> existing = mWorkspaces;
  mOperationWatcher->setFuture(QtConcurrent::run([id, existing, generation] {
    const PreparedOperation prepared = prepareRescan(id, existing);
    OperationResult result;
    result.kind = OperationResult::Kind::Rescan;
    result.generation = generation;
    result.id = id;
    result.workspace = prepared.workspace;
    result.watchedDirectories = prepared.watchedDirectories;
    result.error = prepared.error;
    result.success = prepared.success;
    return result;
  }));
}

LocalWorkspaces *LocalWorkspaces::instance() {
  static LocalWorkspaces *instance = nullptr;
  if (!instance)
    instance = new LocalWorkspaces(qApp);
  return instance;
}

LocalWorkspace *LocalWorkspaces::find(const QString &id) {
  for (LocalWorkspace &workspace : mWorkspaces) {
    if (workspace.id == id)
      return &workspace;
  }
  return nullptr;
}

void LocalWorkspaces::load() {
  const QVariantList stored = QSettings().value(kLocalWorkspacesKey).toList();
  for (const QVariant &value : stored) {
    const QVariantMap map = value.toMap();
    LocalWorkspace workspace;
    workspace.id = map.value("id").toString();
    if (workspace.id.isEmpty())
      workspace.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (find(workspace.id))
      continue;

    workspace.name = map.value("name").toString();
    workspace.description = map.value("description").toString();
    workspace.iconName = map.value("iconName").toString();
    workspace.color = QColor(map.value("color").toString());
    workspace.syncDirectory = map.value("syncDirectory").toString();
    workspace.syncEnabled = map.contains("syncEnabled")
                                ? map.value("syncEnabled").toBool()
                                : !workspace.syncDirectory.isEmpty();
    workspace.repositories =
        normalizedRepositories(map.value("repositories").toStringList());
    workspace.synchronizedRepositories = normalizedRepositories(
        map.value("synchronizedRepositories").toStringList());
    if (map.contains("manualRepositories")) {
      workspace.manualRepositories = normalizedRepositories(
          map.value("manualRepositories").toStringList());
    } else {
      workspace.manualRepositories = manualRepositories(workspace);
    }
    updateRepositories(&workspace);
    mWorkspaces.append(workspace);
  }

  if (!stored.isEmpty())
    store();
}

bool LocalWorkspaces::store(bool synchronize) const {
  QVariantList stored;
  for (const LocalWorkspace &workspace : mWorkspaces) {
    QVariantMap map;
    map.insert("id", workspace.id);
    map.insert("name", workspace.name);
    map.insert("description", workspace.description);
    map.insert("iconName", workspace.iconName);
    map.insert("color", workspace.color.isValid()
                            ? workspace.color.name(QColor::HexArgb)
                            : QString());
    map.insert("syncDirectory", workspace.syncDirectory);
    map.insert("syncEnabled", workspace.syncEnabled);
    map.insert("repositories", workspace.repositories);
    map.insert("manualRepositories", workspace.manualRepositories);
    map.insert("synchronizedRepositories", workspace.synchronizedRepositories);
    stored.append(map);
  }
  QSettings settings;
  settings.setValue(kLocalWorkspacesKey, stored);
  if (synchronize)
    settings.sync();
  return !synchronize || settings.status() == QSettings::NoError;
}

void LocalWorkspaces::changed() {
  ++mInitialScanGeneration;
  ++mOperationGeneration;
  store();
  scheduleRescanSynchronization();
  emit workspacesChanged();
}

void LocalWorkspaces::applyDiscoveredRepository(const QString &id,
                                                quint64 generation,
                                                const QString &path) {
  if (mActiveWorkspaceScans.value(id, 0) != generation)
    return;
  LocalWorkspace *workspace = find(id);
  const auto configuration = mWorkspaceScanConfigurations.constFind(id);
  if (!workspace || configuration == mWorkspaceScanConfigurations.cend())
    return;

  const bool pausedStartupScan =
      mInitialScanWorkspaceGenerations.value(id, 0) == generation &&
      configuration.value().syncEnabled && !workspace->syncEnabled &&
      sameWorkspaceScanTarget(*workspace, configuration.value());
  if (!sameWorkspaceConfiguration(*workspace, configuration.value()) &&
      !pausedStartupScan)
    return;

  if (containsPath(workspace->synchronizedRepositories, path))
    return;
  workspace->synchronizedRepositories.append(path);
  if (!containsPath(workspace->repositories, path))
    workspace->repositories.append(path);
  emit workspaceRepositoryDiscovered(id, generation, path);
}

void LocalWorkspaces::applyWorkspaceWatchDirectory(const QString &id,
                                                   quint64 generation,
                                                   const QString &path) {
  if (mActiveWorkspaceScans.value(id, 0) != generation || path.isEmpty())
    return;
  LocalWorkspace *workspace = find(id);
  const auto configuration = mWorkspaceScanConfigurations.constFind(id);
  if (!workspace || !workspace->syncEnabled ||
      configuration == mWorkspaceScanConfigurations.cend() ||
      !sameWorkspaceConfiguration(*workspace, configuration.value()))
    return;

  addWatchedDirectories({path});
  QStringList pendingDirectories = mPendingWorkspaceWatchDirectories.value(id);
  if (!containsPath(pendingDirectories, path))
    pendingDirectories.append(path);
  mPendingWorkspaceWatchDirectories.insert(id, pendingDirectories);
}

void LocalWorkspaces::addWatchedDirectories(const QStringList &directories) {
  QStringList toAdd;
  const QStringList watched = mWatcher->directories();
  for (const QString &directory : directories) {
    if (!containsPath(watched, directory) && !containsPath(toAdd, directory))
      toAdd.append(directory);
  }
  if (!toAdd.isEmpty())
    mWatcher->addPaths(toAdd);
}

void LocalWorkspaces::appendPendingWorkspaceWatchDirectories(
    QStringList *directories) const {
  for (auto it = mPendingWorkspaceWatchDirectories.cbegin();
       it != mPendingWorkspaceWatchDirectories.cend(); ++it) {
    const LocalWorkspace *workspace = findWorkspace(mWorkspaces, it.key());
    if (!workspace || !workspace->syncEnabled)
      continue;
    for (const QString &directory : it.value()) {
      if (!containsPath(*directories, directory))
        directories->append(directory);
    }
  }
}

void LocalWorkspaces::clearPendingWorkspaceWatchDirectoriesIfIdle() {
  if (mInitialScanCompletionPending || mRescanCompletionPending)
    return;

  auto it = mPendingWorkspaceWatchDirectories.begin();
  while (it != mPendingWorkspaceWatchDirectories.end()) {
    if (mActiveWorkspaceScans.contains(it.key()))
      ++it;
    else
      it = mPendingWorkspaceWatchDirectories.erase(it);
  }
}

void LocalWorkspaces::finishOperation() {
  const OperationResult result = mOperationWatcher->result();

  if (result.kind == OperationResult::Kind::Add) {
    if (mWorkspaceAddCancelId == result.id &&
        mWorkspaceAddCancelGeneration == result.generation) {
      mWorkspaceAddCancel.reset();
      mWorkspaceAddCancelId.clear();
      mWorkspaceAddCancelGeneration = 0;
    }
    if (mWorkspaceAddGenerations.value(result.id, 0) != result.generation)
      return;

    LocalWorkspace *workspace = find(result.id);
    QString error = result.error;
    bool success = result.success && error.isEmpty() && workspace;
    bool workspaceConfigurationChanged = false;
    if (!workspace) {
      success = false;
      if (error.isEmpty())
        error = tr("Workspace no longer exists.");
    } else if (!sameWorkspaceConfiguration(
                   *workspace, mWorkspaceAddConfigurations.value(result.id))) {
      success = false;
      error = tr("Workspace changed while scanning.");
      workspaceConfigurationChanged = true;
    } else {
      if (result.scanGeneration &&
          mActiveWorkspaceScans.value(result.id, 0) == result.scanGeneration) {
        for (const QString &path :
             std::as_const(result.workspace.synchronizedRepositories))
          applyDiscoveredRepository(result.id, result.scanGeneration, path);
      }

      const QStringList previousRepositories = workspace->repositories;
      const QStringList previousManualRepositories =
          workspace->manualRepositories;
      const QStringList previousSynchronizedRepositories =
          workspace->synchronizedRepositories;
      workspace->manualRepositories = result.workspace.manualRepositories;
      workspace->synchronizedRepositories =
          result.workspace.synchronizedRepositories;
      updateRepositories(workspace);
      if (!store(true)) {
        success = false;
        if (!error.isEmpty())
          error.append(QLatin1Char('\n'));
        error.append(tr("Unable to save workspace settings."));
      }
      addWatchedDirectories(result.watchedDirectories);
      if (mInitialScanCompletionPending || mRescanCompletionPending)
        mPendingWorkspaceWatchDirectories.insert(result.id,
                                                 result.watchedDirectories);
      if (previousRepositories != workspace->repositories ||
          previousManualRepositories != workspace->manualRepositories ||
          previousSynchronizedRepositories !=
              workspace->synchronizedRepositories)
        emit workspacesChanged();
    }

    const bool scanning =
        result.scanGeneration &&
        mActiveWorkspaceScans.value(result.id, 0) == result.scanGeneration;
    const bool directoryChangedDuringScan =
        scanning && mWorkspaceScanDirectoryChanges.remove(result.id) > 0;
    const bool reconcileAfterAdd = scanning && (workspaceConfigurationChanged ||
                                                directoryChangedDuringScan);
    if (scanning) {
      mActiveWorkspaceScans.remove(result.id);
      mWorkspaceScanConfigurations.remove(result.id);
    }
    mWorkspaceAddGenerations.remove(result.id);
    mWorkspaceAddConfigurations.remove(result.id);
    clearPendingWorkspaceWatchDirectoriesIfIdle();
    if (reconcileAfterAdd) {
      mRescanTimer->stop();
      scheduleRescanSynchronization();
    }
    if (scanning)
      emit workspaceScanFinished(result.id, result.scanGeneration, success,
                                 error);
    emit workspaceAdded(result.id, success, error);
    return;
  }
  if (result.generation != mOperationGeneration)
    return;

  switch (result.kind) {
    case OperationResult::Kind::Add:
      break;

    case OperationResult::Kind::Update: {
      LocalWorkspace *workspace = find(result.id);
      bool success = result.success && workspace;
      if (success && !equal(*workspace, result.workspace)) {
        LocalWorkspace updated = result.workspace;
        if (!updated.syncEnabled && result.syncDirectoryUnchanged) {
          for (const QString &path :
               std::as_const(workspace->synchronizedRepositories)) {
            if (!containsPath(updated.synchronizedRepositories, path))
              updated.synchronizedRepositories.append(path);
          }
          updateRepositories(&updated);
        }
        *workspace = updated;
        if (mActiveWorkspaceScans.contains(result.id)) {
          addWatchedDirectories(result.watchedDirectories);
          mPendingWorkspaceWatchDirectories.insert(result.id,
                                                   result.watchedDirectories);
        } else {
          mPendingWorkspaceWatchDirectories.remove(result.id);
        }
        changed();
      }
      emit workspaceUpdated(result.id, success,
                            success ? QString() : result.error);
      break;
    }

    case OperationResult::Kind::AddRepositories: {
      LocalWorkspace *workspace = find(result.id);
      bool success = result.success && workspace;
      if (success && !equal(*workspace, result.workspace)) {
        *workspace = result.workspace;
        changed();
      }
      emit repositoriesAdded(result.id, success, result.invalidPaths,
                             result.duplicatePaths,
                             success ? QString() : result.error);
      break;
    }

    case OperationResult::Kind::Rescan: {
      LocalWorkspace *workspace = find(result.id);
      bool success = result.success && workspace;
      if (success &&
          workspace->syncDirectory == result.workspace.syncDirectory &&
          workspace->syncEnabled) {
        const bool modified = !equal(*workspace, result.workspace);
        *workspace = result.workspace;
        setWatchedDirectories(result.watchedDirectories);
        if (modified) {
          ++mInitialScanGeneration;
          store();
          emit workspacesChanged();
        }
      } else if (success) {
        success = false;
      }
      emit synchronizedDirectoryRescanned(result.id, success,
                                          success ? QString() : result.error);
      break;
    }
  }
}

void LocalWorkspaces::startInitialSynchronization() {
  if (mInitialScanCompletionPending || mInitialScanWatcher->isRunning())
    return;

  QList<InitialScanResult> requests;
  for (const LocalWorkspace &workspace : std::as_const(mWorkspaces)) {
    if (!workspace.syncEnabled || workspace.syncDirectory.isEmpty() ||
        mActiveWorkspaceScans.contains(workspace.id))
      continue;

    InitialScanResult request;
    request.id = workspace.id;
    request.directory = workspace.syncDirectory;
    request.generation = ++mWorkspaceScanGeneration;
    request.cachedRepositories = workspace.repositories;
    request.cachedSynchronizedRepositories = workspace.synchronizedRepositories;
    requests.append(request);
  }

  if (requests.isEmpty())
    return;

  const quint64 generation = ++mInitialScanGeneration;
  mInitialScanRunGeneration = generation;
  mInitialScanCancel = std::make_shared<std::atomic_bool>(false);
  const std::shared_ptr<std::atomic_bool> cancelled = mInitialScanCancel;

  mInitialScanCompletionPending = true;
  mInitialScanWorkspaceGenerations.clear();
  for (const InitialScanResult &request : requests) {
    const LocalWorkspace *workspace = find(request.id);
    if (!workspace)
      continue;

    mActiveWorkspaceScans.insert(request.id, request.generation);
    mWorkspaceScanConfigurations.insert(request.id, *workspace);
    mInitialScanWorkspaceGenerations.insert(request.id, request.generation);

    const QFileInfo syncInfo(request.directory);
    const QString parentDirectory = syncInfo.dir().absolutePath();
    if (QDir(parentDirectory).exists())
      applyWorkspaceWatchDirectory(request.id, request.generation,
                                   parentDirectory);
    const QString syncDirectory = normalizedDirectoryPath(request.directory);
    if (QDir(syncDirectory).exists())
      applyWorkspaceWatchDirectory(request.id, request.generation,
                                   syncDirectory);

    emit workspaceScanStarted(request.id, request.generation);
  }

  mInitialScanWatcher->setFuture(QtConcurrent::run([this, requests, cancelled] {
    QList<InitialScanResult> results;
    results.reserve(requests.size());

    for (const InitialScanResult &request : requests) {
      if (cancelled->load())
        break;

      InitialScanResult result = request;
      PerformanceTrace::Span span(
          "workspace", "initial synchronized directory scan", result.directory);
      const auto repositoryDiscovered =
          [this, id = request.id,
           generation = request.generation](const QString &path) {
            QMetaObject::invokeMethod(
                this,
                [this, id, generation, path] {
                  applyDiscoveredRepository(id, generation, path);
                },
                Qt::QueuedConnection);
          };
      const auto directoryDiscovered = [this, id = request.id,
                                        generation = request.generation,
                                        cancelled](const QString &path) {
        if (cancelled->load())
          return false;

        const auto registration =
            std::make_shared<DirectoryWatchRegistration>();
        const bool queued = QMetaObject::invokeMethod(
            this,
            [this, id, generation, path, cancelled, registration] {
              DirectoryWatchRegistrationCompletion completion{registration};
              if (!cancelled->load())
                applyWorkspaceWatchDirectory(id, generation, path);
            },
            Qt::QueuedConnection);
        if (!queued) {
          cancelled->store(true);
          registration->complete();
        }
        return registration->wait(*cancelled);
      };
      result.success = scanSynchronizedDirectory(
          result.directory, &result.repositories, &result.error,
          &result.watchedDirectories, cancelled.get(), repositoryDiscovered,
          directoryDiscovered);
      results.append(result);
    }

    return results;
  }));
}

void LocalWorkspaces::finishInitialSynchronization() {
  const QList<InitialScanResult> results = mInitialScanWatcher->result();
  mInitialScanCancel.reset();

  QHash<QString, InitialScanResult> resultsById;
  for (const InitialScanResult &result : results)
    resultsById.insert(result.id, result);

  struct ScanCompletion {
    QString id;
    quint64 generation = 0;
    bool success = false;
    QString error;
  };
  QList<ScanCompletion> completions;
  const QList<QString> scanIds = mInitialScanWorkspaceGenerations.keys();
  QStringList watchedDirectories;
  bool workspacesChanged = false;
  bool directoryChangedDuringScan = false;
  bool existingWorkspaceConfigurationChanged = false;
  for (const QString &id : scanIds) {
    const quint64 generation = mInitialScanWorkspaceGenerations.value(id);
    if (mActiveWorkspaceScans.value(id, 0) != generation)
      continue;

    LocalWorkspace *workspace = find(id);
    const auto configuration = mWorkspaceScanConfigurations.constFind(id);
    const auto resultIt = resultsById.constFind(id);
    const bool hasResult =
        resultIt != resultsById.cend() && resultIt->generation == generation;
    bool success = false;
    QString error;
    const bool hasConfiguration =
        configuration != mWorkspaceScanConfigurations.cend();
    const bool exactConfigurationMatch =
        workspace && hasConfiguration &&
        sameWorkspaceConfiguration(*workspace, configuration.value());
    const bool pausedForThisScan =
        workspace && hasConfiguration &&
        mInitialScanWorkspaceGenerations.value(id, 0) == generation &&
        pausedWorkspaceMatchesScan(*workspace, configuration.value());
    const bool configurationMatches =
        exactConfigurationMatch || pausedForThisScan;
    const bool existingWorkspaceConfigurationChangedDuringScan =
        workspace && !configurationMatches;

    if (!configurationMatches) {
      error = tr("Workspace changed while scanning.");
      existingWorkspaceConfigurationChanged =
          existingWorkspaceConfigurationChanged ||
          existingWorkspaceConfigurationChangedDuringScan;
    } else {
      if (hasResult) {
        const InitialScanResult &result = resultIt.value();
        if (!pausedForThisScan) {
          for (const QString &directory : result.watchedDirectories) {
            if (!containsPath(watchedDirectories, directory))
              watchedDirectories.append(directory);
          }
        }

        for (const QString &path : result.repositories)
          applyDiscoveredRepository(id, generation, path);

        if (pausedForThisScan) {
          // Pausing disables future synchronization but retains known
          // repositories and any discoveries from this in-flight scan.
          updateRepositories(workspace);
          success = result.success;
          if (!success)
            error = result.error;
        } else if (result.success) {
          workspace->synchronizedRepositories = result.repositories;
          updateRepositories(workspace);
          success = true;
        } else {
          error = result.error;
        }
      } else {
        if (pausedForThisScan)
          updateRepositories(workspace);
        error = tr("Workspace scan was canceled.");
      }
    }

    if (workspace && hasConfiguration &&
        sameWorkspaceScanTarget(*workspace, configuration.value())) {
      const QStringList &cachedRepositories = hasResult
                                                  ? resultIt->cachedRepositories
                                                  : configuration->repositories;
      const QStringList &cachedSynchronizedRepositories =
          hasResult ? resultIt->cachedSynchronizedRepositories
                    : configuration->synchronizedRepositories;
      if (cachedRepositories != workspace->repositories ||
          cachedSynchronizedRepositories != workspace->synchronizedRepositories)
        workspacesChanged = true;
    }

    if (mWorkspaceScanDirectoryChanges.remove(id) > 0)
      directoryChangedDuringScan = true;
    mActiveWorkspaceScans.remove(id);
    mWorkspaceScanConfigurations.remove(id);
    mInitialScanWorkspaceGenerations.remove(id);
    completions.append({id, generation, success, error});
  }

  appendPendingWorkspaceWatchDirectories(&watchedDirectories);
  const bool currentRun = mInitialScanRunGeneration == mInitialScanGeneration;
  if (currentRun)
    setWatchedDirectories(watchedDirectories);
  else
    addWatchedDirectories(watchedDirectories);
  mInitialScanCompletionPending = false;
  clearPendingWorkspaceWatchDirectoriesIfIdle();

  const bool settingsSaved = !workspacesChanged || store(true);
  if (workspacesChanged)
    emit this->workspacesChanged();

  for (ScanCompletion completion : std::as_const(completions)) {
    if (!settingsSaved) {
      completion.success = false;
      if (!completion.error.isEmpty())
        completion.error.append(QLatin1Char('\n'));
      completion.error.append(tr("Unable to save workspace settings."));
    }
    emit workspaceScanFinished(completion.id, completion.generation,
                               completion.success, completion.error);
  }

  if (!settingsSaved)
    emit workspaceScanPersistenceFailed(
        mInitialScanRunGeneration, tr("Unable to save workspace settings."));

  if (directoryChangedDuringScan || existingWorkspaceConfigurationChanged) {
    mRescanTimer->stop();
    scheduleRescanSynchronization();
  }
}

void LocalWorkspaces::scheduleRescanSynchronization() {
  if (mRescanCompletionPending || mRescanWatcher->isRunning()) {
    ++mRescanGeneration;
    mRescanPending = true;
    if (mRescanCancel)
      mRescanCancel->store(true);
    return;
  }

  startRescanSynchronization();
}

void LocalWorkspaces::startRescanSynchronization() {
  if (mRescanCompletionPending || mRescanWatcher->isRunning()) {
    mRescanPending = true;
    return;
  }

  QList<InitialScanResult> requests;
  for (const LocalWorkspace &workspace : std::as_const(mWorkspaces)) {
    if (!workspace.syncEnabled || workspace.syncDirectory.isEmpty() ||
        mActiveWorkspaceScans.contains(workspace.id))
      continue;

    InitialScanResult request;
    request.id = workspace.id;
    request.directory = workspace.syncDirectory;
    requests.append(request);
  }

  if (requests.isEmpty()) {
    if (mActiveWorkspaceScans.isEmpty()) {
      setWatchedDirectories({});
    } else {
      QStringList watchedDirectories;
      appendPendingWorkspaceWatchDirectories(&watchedDirectories);
      setWatchedDirectories(watchedDirectories);
    }
    return;
  }

  mRescanPending = false;
  const quint64 generation = ++mRescanGeneration;
  mRescanRunGeneration = generation;
  mRescanCancel = std::make_shared<std::atomic_bool>(false);
  const std::shared_ptr<std::atomic_bool> cancelled = mRescanCancel;

  mRescanCompletionPending = true;
  mRescanWatcher->setFuture(QtConcurrent::run([requests, cancelled] {
    QList<InitialScanResult> results;
    results.reserve(requests.size());

    for (const InitialScanResult &request : requests) {
      if (cancelled->load())
        break;

      InitialScanResult result = request;
      PerformanceTrace::Span span("workspace", "synchronized directory rescan",
                                  result.directory);
      result.success = scanSynchronizedDirectory(
          result.directory, &result.repositories, &result.error,
          &result.watchedDirectories, cancelled.get());
      results.append(result);
    }

    return results;
  }));
}

void LocalWorkspaces::finishRescanSynchronization() {
  const QList<InitialScanResult> results = mRescanWatcher->result();
  mRescanCancel.reset();

  if (mRescanRunGeneration == mRescanGeneration) {
    QStringList watchedDirectories;
    bool workspacesModified = false;
    for (const InitialScanResult &result : results) {
      if (mActiveWorkspaceScans.contains(result.id))
        continue;
      LocalWorkspace *workspace = find(result.id);
      if (!workspace || !workspace->syncEnabled ||
          workspace->syncDirectory != result.directory)
        continue;

      for (const QString &directory : result.watchedDirectories) {
        if (!containsPath(watchedDirectories, directory))
          watchedDirectories.append(directory);
      }

      if (!result.success)
        continue;

      if (workspace->synchronizedRepositories != result.repositories) {
        workspace->synchronizedRepositories = result.repositories;
        workspacesModified = true;
      }

      const QStringList repositories = workspace->repositories;
      updateRepositories(workspace);
      if (repositories != workspace->repositories)
        workspacesModified = true;
    }

    appendPendingWorkspaceWatchDirectories(&watchedDirectories);
    setWatchedDirectories(watchedDirectories);
    if (workspacesModified) {
      ++mInitialScanGeneration;
      store();
      emit workspacesChanged();
    }
  } else {
    QStringList pendingDirectories;
    appendPendingWorkspaceWatchDirectories(&pendingDirectories);
    addWatchedDirectories(pendingDirectories);
  }

  mRescanCompletionPending = false;
  clearPendingWorkspaceWatchDirectoriesIfIdle();

  if (mRescanPending) {
    mRescanPending = false;
    startRescanSynchronization();
  }
}

void LocalWorkspaces::setWatchedDirectories(const QStringList &directories) {
  const QStringList watched = mWatcher->directories();

  QStringList toAdd;
  for (const QString &directory : directories) {
    if (!containsPath(watched, directory) && !containsPath(toAdd, directory))
      toAdd.append(directory);
  }

  QStringList toRemove;
  for (const QString &directory : watched) {
    if (!containsPath(directories, directory))
      toRemove.append(directory);
  }

  if (!toAdd.isEmpty())
    mWatcher->addPaths(toAdd);
  if (!toRemove.isEmpty())
    mWatcher->removePaths(toRemove);
}

void LocalWorkspaces::updateWatchedDirectories() {
  QStringList directories;
  QSet<QString> visited;
  for (const LocalWorkspace &workspace : std::as_const(mWorkspaces)) {
    if (!workspace.syncEnabled || workspace.syncDirectory.isEmpty())
      continue;

    const QFileInfo syncInfo(workspace.syncDirectory);
    const QString parent = syncInfo.dir().absolutePath();
    if (QDir(parent).exists() && !containsPath(directories, parent))
      directories.append(parent);

    QDir syncDirectory(workspace.syncDirectory);
    if (!syncDirectory.exists())
      continue;
    walkSynchronizedDirectory(workspace.syncDirectory, true, nullptr,
                              &directories, &visited);
  }
  setWatchedDirectories(directories);
}

//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "LocalWorkspaces.h"
#include "git/Repository.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QSettings>
#include <QTimer>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>

namespace {

const QString kLocalWorkspacesKey = "localWorkspaces";

void setError(QString *error, const QString &message) {
  if (error)
    *error = message;
}

bool pathsMatch(const QString &lhs, const QString &rhs) {
#ifdef Q_OS_WIN
  return lhs.compare(rhs, Qt::CaseInsensitive) == 0;
#else
  return lhs == rhs;
#endif
}

bool containsPath(const QStringList &paths, const QString &path) {
  for (const QString &candidate : paths) {
    if (pathsMatch(candidate, path))
      return true;
  }
  return false;
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

bool scanSynchronizedDirectory(const QString &path, QStringList *repositories,
                               QString *error) {
  QDir syncDirectory(path);
  if (!syncDirectory.exists()) {
    setError(error, LocalWorkspaces::tr(
                        "Synchronized directory does not exist: %1")
                        .arg(path));
    return false;
  }

  repositories->clear();
  const QFileInfoList children = syncDirectory.entryInfoList(
      QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot,
      QDir::Name | QDir::IgnoreCase);
  for (const QFileInfo &child : children) {
    QString root;
    if (!repositoryRoot(child.absoluteFilePath(), &root))
      continue;

    QString childPath = child.canonicalFilePath();
    if (childPath.isEmpty())
      childPath = child.absoluteFilePath();
    if (pathsMatch(QDir::cleanPath(childPath), root))
      repositories->append(root);
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

bool namesMatch(const QString &lhs, const QString &rhs) {
  return lhs.trimmed().compare(rhs.trimmed(), Qt::CaseInsensitive) == 0;
}

QString cleanOptionalPath(const QString &path) {
  return path.isEmpty() ? QString() : QDir::cleanPath(path);
}

} // namespace

LocalWorkspaces::LocalWorkspaces(QObject *parent)
    : QObject(parent), mWatcher(new QFileSystemWatcher(this)),
      mRescanTimer(new QTimer(this)) {
  load();
  updateWatchedDirectories();
  mRescanTimer->setSingleShot(true);
  mRescanTimer->setInterval(300);
  connect(mWatcher, &QFileSystemWatcher::directoryChanged, this,
          [this] { mRescanTimer->start(); });
  connect(mRescanTimer, &QTimer::timeout, this, [this] {
    updateWatchedDirectories();
    QStringList ids;
    for (const LocalWorkspace &workspace : std::as_const(mWorkspaces)) {
      if (workspace.syncEnabled && !workspace.syncDirectory.isEmpty())
        ids.append(workspace.id);
    }
    for (const QString &id : std::as_const(ids))
      rescanSynchronizedDirectory(id);
  });

  QStringList synchronizedIds;
  for (const LocalWorkspace &workspace : std::as_const(mWorkspaces)) {
    if (workspace.syncEnabled && !workspace.syncDirectory.isEmpty())
      synchronizedIds.append(workspace.id);
  }
  for (const QString &id : std::as_const(synchronizedIds))
    rescanSynchronizedDirectory(id);
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
  if (added.syncEnabled &&
      !scanSynchronizedDirectory(added.syncDirectory,
                                 &added.synchronizedRepositories, error))
    return false;
  updateRepositories(&added);

  mWorkspaces.append(added);
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
  if (updated.syncEnabled) {
    updated.synchronizedRepositories.clear();
    if (!scanSynchronizedDirectory(updated.syncDirectory,
                                   &updated.synchronizedRepositories, error))
      return false;
  }
  updateRepositories(&updated);
  if (equal(*current, updated))
    return true;

  *current = updated;
  changed();
  return true;
}

bool LocalWorkspaces::remove(const QString &id, QString *error) {
  setError(error, {});
  for (int i = 0; i < mWorkspaces.size(); ++i) {
    if (mWorkspaces.at(i).id == id) {
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
    if (pathsMatch(workspace->repositories.at(i), comparedPath)) {
      workspace->repositories.removeAt(i);
      for (int manual = workspace->manualRepositories.size() - 1; manual >= 0;
           --manual) {
        if (pathsMatch(workspace->manualRepositories.at(manual), comparedPath))
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
  if (!scanSynchronizedDirectory(workspace->syncDirectory,
                                 &synchronizedRepositories, error))
    return false;

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

void LocalWorkspaces::store() const {
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
    map.insert("synchronizedRepositories",
               workspace.synchronizedRepositories);
    stored.append(map);
  }
  QSettings().setValue(kLocalWorkspacesKey, stored);
}

void LocalWorkspaces::changed() {
  store();
  updateWatchedDirectories();
  emit workspacesChanged();
}

void LocalWorkspaces::updateWatchedDirectories() {
  const QStringList watched = mWatcher->directories();
  if (!watched.isEmpty())
    mWatcher->removePaths(watched);

  QStringList directories;
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
    if (!containsPath(directories, workspace.syncDirectory))
      directories.append(workspace.syncDirectory);
    const QFileInfoList children = syncDirectory.entryInfoList(
        QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QFileInfo &child : children) {
      if (!containsPath(directories, child.absoluteFilePath()))
        directories.append(child.absoluteFilePath());
    }
  }
  if (!directories.isEmpty())
    mWatcher->addPaths(directories);
}

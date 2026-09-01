//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "Submodule.h"
#include "Config.h"
#include "Id.h"
#include "Repository.h"
#include "git2/buffer.h"
#include "git2/graph.h"
#include "git2/index.h"
#include "git2/remote.h"
#include "git2/status.h"
#include "git2/sys/errors.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QPair>
#include <QRegularExpression>

namespace git {

const QString kUrl = "https://github.com/NortekMed/GitNortek";
const QString kUpdateCheckRemote = "gitnortek-submodule-check";

Result fail(const QString &message) {
  git_error_set_str(GIT_ERROR_INVALID, message.toUtf8());
  return -GIT_ERROR_INVALID;
}

void renameConfigSection(Config config, const QString &oldName,
                         const QString &newName) {
  if (!config.isValid() || oldName == newName)
    return;

  const QString oldPrefix = QString("submodule.%1.").arg(oldName);
  const QString newPrefix = QString("submodule.%1.").arg(newName);
  QList<QPair<QString, QString>> entries;

  Config::Iterator it = config.glob(oldPrefix + "*");
  while (Config::Entry entry = it.next()) {
    entries.append(
        {entry.name().mid(oldPrefix.length()), entry.value<QString>()});
  }

  for (const QPair<QString, QString> &entry : entries) {
    config.remove(oldPrefix + entry.first);
    config.setValue(newPrefix + entry.first, entry.second);
  }
}

void setOptionalConfigValue(Config config, const QString &key,
                            const QString &value) {
  if (value.isEmpty())
    config.remove(key);
  else
    config.setValue(key, value);
}

bool removeConfigSection(Config config, const QString &name) {
  if (!config.isValid())
    return false;

  QStringList entries;
  Config::Iterator it =
      config.glob(QString("submodule\\.%1\\..*")
                      .arg(QRegularExpression::escape(name)));
  while (Config::Entry entry = it.next())
    entries.append(entry.name());

  bool removed = false;
  for (const QString &entry : entries)
    removed |= config.remove(entry);
  return removed;
}

QString canonicalOrAbsolutePath(const QString &path) {
  QFileInfo info(path);
  QString canonical = info.canonicalFilePath();
  return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath()
                                             : canonical);
}

bool isPathInside(const QString &parent, const QString &child) {
  const QString base = canonicalOrAbsolutePath(parent);
  const QString path = canonicalOrAbsolutePath(child);
#ifdef Q_OS_WIN
  return path.startsWith(base + QDir::separator(), Qt::CaseInsensitive);
#else
  return path.startsWith(base + QDir::separator());
#endif
}

Result readSubmoduleGitdirTarget(const QString &oldPath, QString *target) {
  QFile file(QDir(oldPath).filePath(".git"));
  if (!file.exists())
    return 0;

  if (!file.open(QFile::ReadOnly))
    return fail(QObject::tr("Failed to read moved submodule .git file."));

  QByteArray data = file.readAll();
  file.close();

  const QByteArray prefix = "gitdir: ";
  if (!data.startsWith(prefix))
    return 0;

  QString gitdir = QString::fromUtf8(data.mid(prefix.size())).trimmed();
  if (QDir::isAbsolutePath(gitdir)) {
    *target = QDir::cleanPath(gitdir);
    return 0;
  }

  *target = QFileInfo(QDir(oldPath).filePath(gitdir)).absoluteFilePath();
  return 0;
}

Result updateMovedSubmoduleGitFile(const QString &newPath,
                                   const QString &absoluteGitdir) {
  if (absoluteGitdir.isEmpty())
    return 0;

  QFile file(QDir(newPath).filePath(".git"));
  QDir newDir(newPath);
  QString relativeGitdir = newDir.relativeFilePath(absoluteGitdir);
  const QByteArray prefix = "gitdir: ";

  if (!file.open(QFile::WriteOnly | QFile::Truncate))
    return fail(QObject::tr("Failed to update moved submodule .git file."));

  file.write(prefix + relativeGitdir.toUtf8() + '\n');
  return 0;
}

void configureFetchCallbacks(git_fetch_options &opts,
                             Remote::Callbacks *callbacks, const QString &url) {
#ifndef USE_SYSTEM_LIBGIT2
  opts.callbacks.connected = &Remote::Callbacks::connected;
  opts.callbacks.about_to_disconnect = &Remote::Callbacks::about_to_disconnect;
#endif
  opts.callbacks.sideband_progress = &Remote::Callbacks::sideband;
  opts.callbacks.credentials = &Remote::Callbacks::credentials;
  opts.callbacks.certificate_check = &Remote::Callbacks::certificate;
  opts.callbacks.transfer_progress = &Remote::Callbacks::transfer;
  opts.callbacks.update_tips = &Remote::Callbacks::update;
  opts.callbacks.remote_ready = &Remote::Callbacks::remoteReady;
  opts.callbacks.payload = callbacks;
}

QString shortBranchName(const QString &name) {
  if (name.startsWith("refs/heads/"))
    return name.mid(QString("refs/heads/").size());

  return name;
}

QString refNameBranch(const QString &branch) {
  return branch.startsWith("refs/heads/")
             ? branch
             : QString("refs/heads/%1").arg(branch);
}

QString localTargetRef(const QString &branch) {
  QString name = shortBranchName(branch);
  name.replace(QRegularExpression("[^A-Za-z0-9._/-]"), "-");
  return QString("refs/remotes/%1/%2").arg(kUpdateCheckRemote, name);
}

Submodule::UpdateStatus statusError(const Submodule &submodule,
                                    const QString &message) {
  Submodule::UpdateStatus status;
  status.state = Submodule::UpdateStatus::Error;
  status.name = submodule.name();
  status.path = submodule.path();
  status.url = submodule.url();
  status.branch = submodule.branch();
  status.pinnedId = submodule.indexId();
  status.message = message;
  return status;
}

QString lastError(const QString &fallback) {
  const git_error *error = git_error_last();
  if (error && error->message)
    return QString::fromUtf8(error->message);

  return fallback.isEmpty() ? QObject::tr("Unknown error.") : fallback;
}

Submodule::Submodule() {}

Submodule::Submodule(git_submodule *submodule)
    : d(submodule, git_submodule_free) {}

Submodule::operator git_submodule *() const { return d.data(); }

bool Submodule::isInitialized() const {
  if (!isValid())
    return false;

  git_repository *repo = nullptr;
  if (git_submodule_open(&repo, d.data())) {
    git_error_clear();
    return false;
  }

  git_repository_free(repo);
  return true;
}

void Submodule::initialize() const { git_submodule_init(d.data(), false); }

void Submodule::deinitialize() const {
  // Remove git config entry.
  Repository repo(git_submodule_owner(d.data()));
  Config config = repo.gitConfig();
  QString regex = QString("submodule\\.%1\\..*").arg(name());
  Config::Iterator it = config.glob(regex);
  while (Config::Entry entry = it.next())
    config.remove(entry.name());

  // Remove submodule workdir.
  QDir dir = repo.workdir();
  if (dir.cd(path()) && dir.removeRecursively())
    dir.mkpath(".");
}

QString Submodule::name() const { return git_submodule_name(d.data()); }

QString Submodule::path() const { return git_submodule_path(d.data()); }

QString Submodule::url() const { return git_submodule_url(d.data()); }

void Submodule::setUrl(const QString &url) {
  if (url == this->url())
    return;

  QByteArray buffer = url.toUtf8();
  const char *data = !buffer.isEmpty() ? buffer.constData() : nullptr;
  git_repository *repo = git_submodule_owner(d.data());
  git_submodule_set_url(repo, git_submodule_name(d.data()), data);
}

QString Submodule::branch() const { return git_submodule_branch(d.data()); }

void Submodule::setBranch(const QString &branch) {
  if (branch == this->branch())
    return;

  QByteArray buffer = branch.toUtf8();
  const char *data = !buffer.isEmpty() ? buffer.constData() : nullptr;
  git_repository *repo = git_submodule_owner(d.data());
  git_submodule_set_branch(repo, git_submodule_name(d.data()), data);
}

Id Submodule::headId() const { return git_submodule_head_id(d.data()); }

Id Submodule::indexId() const { return git_submodule_index_id(d.data()); }

Id Submodule::workdirId() const { return git_submodule_wd_id(d.data()); }

Result Submodule::update(Remote::Callbacks *callbacks, bool init,
                         bool checkout_force) {
  git_submodule_update_options opts = GIT_SUBMODULE_UPDATE_OPTIONS_INIT;
#ifndef USE_SYSTEM_LIBGIT2
  opts.fetch_opts.callbacks.connected = &Remote::Callbacks::connected;
  opts.fetch_opts.callbacks.about_to_disconnect =
      &Remote::Callbacks::about_to_disconnect;
#endif
  opts.fetch_opts.callbacks.sideband_progress = &Remote::Callbacks::sideband;
  opts.fetch_opts.callbacks.credentials = &Remote::Callbacks::credentials;
  opts.fetch_opts.callbacks.certificate_check = &Remote::Callbacks::certificate;
  opts.fetch_opts.callbacks.transfer_progress = &Remote::Callbacks::transfer;
  opts.fetch_opts.callbacks.update_tips = &Remote::Callbacks::update;
  opts.fetch_opts.callbacks.remote_ready = &Remote::Callbacks::remoteReady;
  opts.fetch_opts.callbacks.payload = callbacks;

  if (checkout_force)
    opts.checkout_opts.checkout_strategy |= GIT_CHECKOUT_FORCE;
  // Use a fake URL. Submodule update doesn't have a way to
  // query a different proxy for each submodule remote.
  QByteArray proxy = Remote::proxyUrl(kUrl, opts.fetch_opts.proxy_opts.type);
  opts.fetch_opts.proxy_opts.url = proxy;

  return git_submodule_update(d.data(), init, &opts);
}

Submodule::UpdateStatus
Submodule::checkForUpdates(Remote::Callbacks *callbacks) const {
  UpdateStatus status;
  status.name = name();
  status.path = path();
  status.url = url();
  status.branch = branch();
  status.pinnedId = indexId();

  git_buf resolvedUrl = GIT_BUF_INIT;
  if (!git_submodule_resolve_url(&resolvedUrl, git_submodule_owner(d.data()),
                                 status.url.toUtf8()))
    status.url = QString::fromUtf8(resolvedUrl.ptr);
  git_buf_dispose(&resolvedUrl);

  if (status.pinnedId.isNull()) {
    status.message = QObject::tr("No pinned submodule commit is recorded.");
    return status;
  }

  if (status.branch.isEmpty()) {
    status.state = UpdateStatus::NotTracked;
    status.message = QObject::tr("No submodule branch is configured.");
    return status;
  }

  if (status.branch == ".") {
    status.message = QObject::tr(
        "The special '.' submodule branch is not supported by this check.");
    return status;
  }

  if (!isInitialized())
    return statusError(
        *this, QObject::tr("The submodule repository is not initialized."));

  git_repository *repo = nullptr;
  int error = git_submodule_open(&repo, d.data());
  if (error)
    return statusError(
        *this,
        lastError(QObject::tr("The submodule repository is not initialized.")));

  QSharedPointer<git_repository> repoPtr(repo, git_repository_free);

  git_remote *remote = nullptr;
  error = git_remote_create_anonymous(&remote, repo, status.url.toUtf8());
  if (error)
    return statusError(*this,
                       lastError(QObject::tr("Unable to create remote.")));

  QSharedPointer<git_remote> remotePtr(remote, git_remote_free);

  git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
  configureFetchCallbacks(opts, callbacks, status.url);
  opts.follow_redirects = GIT_REMOTE_REDIRECT_INITIAL;

  QByteArray proxy = Remote::proxyUrl(status.url, opts.proxy_opts.type);
  opts.proxy_opts.url = proxy;

  QString source = refNameBranch(status.branch);
  QString target = localTargetRef(status.branch);
  QByteArray refspec = QString("+%1:%2").arg(source, target).toUtf8();
  char *refspecs[] = {refspec.data()};
  git_strarray array = {refspecs, 1};
  git_error_clear();
  error = git_remote_fetch(remote, &array, &opts, "submodule update check");
  if (error)
    return statusError(*this, lastError(QObject::tr(
                                  "Unable to fetch submodule target branch.")));

  git_reference *ref = nullptr;
  git_error_clear();
  error = git_reference_lookup(&ref, repo, target.toUtf8());
  if (error)
    return statusError(*this, lastError(QObject::tr(
                                  "Unable to read fetched submodule branch.")));

  QSharedPointer<git_reference> refPtr(ref, git_reference_free);
  status.targetId = git_reference_target(ref);

  if (status.targetId == status.pinnedId) {
    status.state = UpdateStatus::UpToDate;
    return status;
  }

  int descendant =
      git_graph_descendant_of(repo, status.targetId, status.pinnedId);
  if (descendant == 1) {
    status.state = UpdateStatus::UpdateAvailable;
  } else if (descendant == 0) {
    status.state = UpdateStatus::DifferentHistory;
    status.message =
        QObject::tr("Remote target is not a descendant of the pinned commit.");
  } else {
    status.state = UpdateStatus::Unknown;
    status.message =
        lastError(QObject::tr("Unable to compare submodule commits."));
  }

  return status;
}

Result Submodule::add(Repository repo, const QString &url, const QString &path,
                      const QString &branch, Remote::Callbacks *callbacks) {
  git_submodule *submodule = nullptr;
  int error = git_submodule_add_setup(&submodule, repo.d->repo, url.toUtf8(),
                                      path.toUtf8(), true);
  if (error)
    return error;

  QSharedPointer<git_submodule> submodulePtr(submodule, git_submodule_free);

  if (!branch.isEmpty()) {
    error = git_submodule_set_branch(
        repo.d->repo, git_submodule_name(submodule), branch.toUtf8());
    if (error)
      return error;
  }

  git_submodule_update_options opts = GIT_SUBMODULE_UPDATE_OPTIONS_INIT;
#ifndef USE_SYSTEM_LIBGIT2
  opts.fetch_opts.callbacks.connected = &Remote::Callbacks::connected;
  opts.fetch_opts.callbacks.about_to_disconnect =
      &Remote::Callbacks::about_to_disconnect;
#endif
  opts.fetch_opts.callbacks.sideband_progress = &Remote::Callbacks::sideband;
  opts.fetch_opts.callbacks.credentials = &Remote::Callbacks::credentials;
  opts.fetch_opts.callbacks.certificate_check = &Remote::Callbacks::certificate;
  opts.fetch_opts.callbacks.transfer_progress = &Remote::Callbacks::transfer;
  opts.fetch_opts.callbacks.update_tips = &Remote::Callbacks::update;
  opts.fetch_opts.callbacks.remote_ready = &Remote::Callbacks::remoteReady;
  opts.fetch_opts.callbacks.payload = callbacks;
  opts.fetch_opts.follow_redirects = GIT_REMOTE_REDIRECT_INITIAL;

  QByteArray proxy = Remote::proxyUrl(url, opts.fetch_opts.proxy_opts.type);
  opts.fetch_opts.proxy_opts.url = proxy;

  git_repository *submoduleRepo = nullptr;
  error = git_submodule_clone(&submoduleRepo, submodule, &opts);
  if (error)
    return error;

  git_repository_free(submoduleRepo);

  error = git_submodule_add_finalize(submodule);
  if (error)
    return error;

  repo.invalidateSubmoduleCache();
  return 0;
}

Result Submodule::modify(Repository repo, const QString &oldName,
                         const QString &newName, const QString &newPath,
                         const QString &newUrl, const QString &newBranch) {
  if (newName.isEmpty())
    return fail(QObject::tr("Submodule name cannot be empty."));

  if (newPath.isEmpty())
    return fail(QObject::tr("Submodule path cannot be empty."));

  if (newUrl.isEmpty())
    return fail(QObject::tr("Submodule URL cannot be empty."));

  Submodule submodule = repo.lookupSubmodule(oldName);
  if (!submodule)
    return fail(QObject::tr("Submodule '%1' was not found.").arg(oldName));

  const QString oldPath = submodule.path();

  foreach (const Submodule &existing, repo.submodules()) {
    if (existing.name() != oldName && existing.name() == newName)
      return fail(
          QObject::tr("A submodule named '%1' already exists.").arg(newName));

    if (existing.name() != oldName && existing.path() == newPath)
      return fail(
          QObject::tr("A submodule already uses path '%1'.").arg(newPath));
  }

  Config modules = Config::open(repo.workdir().filePath(".gitmodules"));
  if (!modules.isValid())
    return fail(QObject::tr("Failed to open .gitmodules."));

  if (oldPath != newPath) {
    QDir workdir = repo.workdir();
    QString oldAbsolutePath =
        QFileInfo(workdir.filePath(oldPath)).absoluteFilePath();
    QString newAbsolutePath =
        QFileInfo(workdir.filePath(newPath)).absoluteFilePath();

    if (QFileInfo::exists(newAbsolutePath))
      return fail(QObject::tr("Path '%1' already exists.").arg(newPath));

    if (QFileInfo::exists(oldAbsolutePath)) {
      QString gitdirTarget;
      Result result = readSubmoduleGitdirTarget(oldAbsolutePath, &gitdirTarget);
      if (!result)
        return result;

      QDir parent = QFileInfo(newAbsolutePath).absoluteDir();
      if (!parent.exists() && !QDir().mkpath(parent.absolutePath()))
        return fail(QObject::tr("Failed to create parent directory for '%1'.")
                        .arg(newPath));

      if (!QDir().rename(oldAbsolutePath, newAbsolutePath))
        return fail(QObject::tr("Failed to move submodule folder to '%1'.")
                        .arg(newPath));

      result = updateMovedSubmoduleGitFile(newAbsolutePath, gitdirTarget);
      if (!result)
        return result;
    }
  }

  renameConfigSection(modules, oldName, newName);
  renameConfigSection(repo.gitConfig(), oldName, newName);

  const QString prefix = QString("submodule.%1.").arg(newName);
  modules.setValue(prefix + "path", newPath);
  modules.setValue(prefix + "url", newUrl);
  setOptionalConfigValue(modules, prefix + "branch", newBranch);

  Config local = repo.gitConfig();
  if (local.isValid()) {
    local.setValue(prefix + "url", newUrl);
    setOptionalConfigValue(local, prefix + "branch", newBranch);
  }

  repo.invalidateSubmoduleCache();
  return 0;
}

Result Submodule::remove(Repository repo, const Submodule &submodule) {
  if (!repo.isValid() || !submodule.isValid())
    return fail(QObject::tr("Invalid submodule."));

  const QString name = submodule.name();
  const QString path = submodule.path();
  if (!repo.lookupSubmodule(name).isValid())
    return fail(QObject::tr("Submodule '%1' was not found.").arg(name));

  const QString worktree = repo.workdir().absolutePath();
  const QString submoduleWorktree = repo.workdir().absoluteFilePath(path);
  if (!isPathInside(worktree, submoduleWorktree))
    return fail(QObject::tr("Submodule path '%1' is outside the repository.")
                    .arg(path));

  const QString modulesFile = repo.workdir().filePath(".gitmodules");
  QFile originalModules(modulesFile);
  if (!originalModules.open(QFile::ReadOnly))
    return fail(QObject::tr("Failed to read .gitmodules."));
  const QByteArray originalModulesData = originalModules.readAll();
  originalModules.close();

  unsigned int modulesStatus = GIT_STATUS_CURRENT;
  int error = git_status_file(&modulesStatus, repo.d->repo, ".gitmodules");
  if (error)
    return error;
  if (modulesStatus != GIT_STATUS_CURRENT)
    return fail(QObject::tr(
        ".gitmodules has existing changes. Commit or discard them first."));

  QString cachePath;
  Result result = readSubmoduleGitdirTarget(submoduleWorktree, &cachePath);
  if (!result)
    return result;
  if (cachePath.isEmpty()) {
    Repository submoduleRepo = submodule.open();
    if (submoduleRepo.isValid())
      cachePath = submoduleRepo.dir().absolutePath();
  }

  if (!cachePath.isEmpty()) {
    const QString modulesRoot = repo.dir().absoluteFilePath("modules");
    if (!isPathInside(modulesRoot, cachePath))
      return fail(QObject::tr("Submodule cache path is outside .git/modules."));
  }

  Config modules = Config::open(modulesFile);
  if (!modules.isValid() || !removeConfigSection(modules, name))
    return fail(QObject::tr("Failed to remove submodule from .gitmodules."));

  git_index *index = nullptr;
  error = git_repository_index(&index, repo.d->repo);
  if (!error)
    error = git_index_remove_bypath(index, path.toUtf8());
  if (!error)
    error = git_index_add_bypath(index, ".gitmodules");
  if (!error)
    error = git_index_write(index);
  git_index_free(index);

  if (error) {
    QFile restore(modulesFile);
    if (restore.open(QFile::WriteOnly | QFile::Truncate))
      restore.write(originalModulesData);
    return error;
  }

  removeConfigSection(repo.gitConfig(), name);
  repo.invalidateSubmoduleCache();
  emit repo.notifier()->indexChanged({".gitmodules", path});

  QFileInfo worktreeInfo(submoduleWorktree);
  if (worktreeInfo.exists() && !QDir(submoduleWorktree).removeRecursively())
    return fail(QObject::tr("Submodule was removed from the project, but its "
                            "working directory could not be deleted."));

  QFileInfo cacheInfo(cachePath);
  if (!cachePath.isEmpty() && cacheInfo.exists() &&
      !QDir(cachePath).removeRecursively())
    return fail(QObject::tr("Submodule was removed from the project, but its "
                            "cached repository could not be deleted."));

  return 0;
}

Repository Submodule::open() const {
  git_repository *repo = nullptr;
  git_submodule_open(&repo, d.data());
  return Repository(repo);
}

} // namespace git

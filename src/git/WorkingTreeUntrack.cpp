//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. See LICENSE.md.
//

#include "WorkingTreeUntrack.h"

#include "Commit.h"
#include "Index.h"
#include "Reference.h"
#include "Repository.h"
#include "Submodule.h"
#include "WorkingTreeStatus.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>
#include <algorithm>

namespace git {

namespace {

QMutex &mutationMutex() {
  static QMutex mutex;
  return mutex;
}

bool isCanceled(const std::shared_ptr<std::atomic_bool> &canceled) {
  return canceled && canceled->load();
}

QString normalizePath(const QString &path) {
  return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

bool isSafeRelativePath(const QString &path) {
  if (path.isEmpty() || QDir::isAbsolutePath(path))
    return false;
  if (path.contains('\n') || path.contains('\r'))
    return false;

  const QString normalized = normalizePath(path);
  return normalized != QStringLiteral(".") &&
         normalized != QStringLiteral("..") &&
         !normalized.startsWith(QStringLiteral("../"));
}

bool pathMatchesRoot(const QString &path, const QString &root) {
  return path == root || path.startsWith(root + QStringLiteral("/"));
}

bool pathMatchesAnyRoot(const QString &path, const QStringList &roots) {
  for (const QString &root : roots) {
    if (pathMatchesRoot(path, root))
      return true;
  }
  return false;
}

void appendUnique(QStringList &paths, const QString &path) {
  if (!path.isEmpty() && !paths.contains(path))
    paths.append(path);
}

QString encode(const QString &value) {
  return QString::fromLatin1(value.toUtf8().toHex());
}

QString hashBytes(const QByteArray &bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString headId(const Repository &repo) {
  const Commit head = repo.head().target();
  return head.isValid() ? head.id().toString() : QString();
}

QByteArray readIgnoreFile(const Repository &repo, bool *exists,
                          QString *error) {
  const QString path = repo.workdir().filePath(QStringLiteral(".gitignore"));
  QFile file(path);
  if (!file.exists()) {
    if (exists)
      *exists = false;
    return QByteArray();
  }

  if (!file.open(QIODevice::ReadOnly)) {
    if (error)
      *error = file.errorString();
    return QByteArray();
  }

  if (exists)
    *exists = true;
  return file.readAll();
}

QString ignoreFingerprint(const Repository &repo, QString *error = nullptr) {
  bool exists = false;
  const QByteArray content = readIgnoreFile(repo, &exists, error);
  if (error && !error->isEmpty())
    return QString();
  return hashBytes(exists ? content : QByteArray("<missing>"));
}

QString statusFingerprint(const WorkingTreeStatusSnapshot &status,
                          const QStringList &roots) {
  QStringList entries;
  for (const WorkingTreeStatusEntry &entry : status.entries()) {
    if (!roots.isEmpty() && !pathMatchesAnyRoot(entry.path, roots) &&
        !pathMatchesAnyRoot(entry.oldPath, roots))
      continue;

    entries.append(QStringList{
        encode(entry.path), encode(entry.oldPath), QString::number(entry.flags),
        QString::number(entry.indexStatus),
        QString::number(entry.workdirStatus), encode(entry.oldId.toString()),
        encode(entry.newId.toString()), QString::number(entry.oldMode),
        QString::number(entry.newMode)}
                       .join('|'));
  }
  std::sort(entries.begin(), entries.end());
  return hashBytes(entries.join('\n').toUtf8());
}

bool hasSymlinkParent(const Repository &repo, const QString &path);

QString fileStateFingerprint(const Repository &repo, const QStringList &paths) {
  QStringList entries;
  QStringList sorted = paths;
  sorted.removeDuplicates();
  std::sort(sorted.begin(), sorted.end());
  for (const QString &path : sorted) {
    const QFileInfo info(repo.workdir().filePath(path));
    entries.append(QStringList{
        encode(path),
        info.exists() ? QStringLiteral("exists") : QStringLiteral("missing"),
        info.isDir() ? QStringLiteral("dir") : QStringLiteral("file"),
        info.isSymLink() ? QStringLiteral("symlink") : QString(),
        hasSymlinkParent(repo, path) ? QStringLiteral("symlink-parent")
                                     : QString(),
        QString::number(info.size()),
        QString::number(info.lastModified().toMSecsSinceEpoch())}
                       .join('|'));
  }
  return hashBytes(entries.join('\n').toUtf8());
}

QStringList indexPaths(const Index &index, const QStringList &roots) {
  QStringList paths = index.pathsUnder(roots);
  paths.removeDuplicates();
  std::sort(paths.begin(), paths.end());
  return paths;
}

QStringList ignoreLines(const QByteArray &content) {
  QStringList lines = QString::fromUtf8(content).split('\n');
  for (QString &line : lines)
    if (line.endsWith('\r'))
      line.chop(1);
  return lines;
}

QString patternFor(const QString &root, bool directory) {
  QString escaped;
  escaped.reserve(root.size());
  for (int i = 0; i < root.size(); ++i) {
    const QChar character = root.at(i);
    if (character == '\\' || character == '*' || character == '?' ||
        character == '[' || character == ']' ||
        ((character == ' ' || character == '\t') && i + 1 == root.size()))
      escaped += '\\';
    escaped += character;
  }

  return QStringLiteral("/") + escaped +
         (directory ? QStringLiteral("/") : QString());
}

bool isRepositoryDirectory(const QString &path) {
  return Repository::open(path, false).isValid();
}

bool hasSymlinkParent(const Repository &repo, const QString &path) {
  QString current = repo.workdir().path();
  const QStringList components =
      normalizePath(path).split('/', Qt::SkipEmptyParts);
  for (int i = 0; i + 1 < components.size(); ++i) {
    current = QDir(current).filePath(components.at(i));
    if (QFileInfo(current).isSymLink())
      return true;
  }
  return false;
}

void collectDirectory(const Repository &repo, const QString &absolutePath,
                      const QString &relativePath, const QSet<QString> &tracked,
                      QStringList &untracked, QStringList &ignored,
                      QStringList &protectedPaths,
                      const std::shared_ptr<std::atomic_bool> &canceled) {
  QDirIterator iterator(absolutePath,
                        QDir::AllEntries | QDir::Hidden | QDir::System |
                            QDir::NoDotAndDotDot,
                        QDirIterator::NoIteratorFlags);
  while (iterator.hasNext()) {
    if (isCanceled(canceled))
      return;

    const QString absolute = iterator.next();
    const QFileInfo info = iterator.fileInfo();
    const QString relative =
        normalizePath(QDir(relativePath).filePath(info.fileName()));

    if (info.isDir() && !info.isSymLink()) {
      if (info.fileName() == QStringLiteral(".git") ||
          repo.lookupSubmodule(relative).isValid() ||
          isRepositoryDirectory(absolute)) {
        appendUnique(protectedPaths, relative);
        continue;
      }
      collectDirectory(repo, absolute, relative, tracked, untracked, ignored,
                       protectedPaths, canceled);
      continue;
    }

    if (tracked.contains(relative))
      continue;
    if (repo.isIgnored(relative))
      appendUnique(ignored, relative);
    else
      appendUnique(untracked, relative);
  }
}

bool validateRoots(const QStringList &roots, QString *error) {
  for (const QString &root : roots) {
    if (isSafeRelativePath(root))
      continue;
    if (error)
      *error = QStringLiteral("The selected path is invalid: %1").arg(root);
    return false;
  }
  return true;
}

bool isProtectedPath(const QString &path, const QStringList &protectedPaths) {
  for (const QString &protectedPath : protectedPaths) {
    if (path == protectedPath ||
        path.startsWith(protectedPath + QStringLiteral("/")))
      return true;
  }
  return false;
}

bool isProtectedRepositoryPath(const Repository &repo, const QString &path,
                               const QStringList &protectedPaths) {
  if (isProtectedPath(path, protectedPaths))
    return true;

  const QStringList components =
      normalizePath(path).split('/', Qt::SkipEmptyParts);
  QString current;
  for (int i = 0; i < components.size(); ++i) {
    current = current.isEmpty() ? components.at(i)
                                : QDir(current).filePath(components.at(i));
    if (repo.lookupSubmodule(current).isValid())
      return true;
    if (i + 1 < components.size() &&
        isRepositoryDirectory(repo.workdir().filePath(current)))
      return true;
  }
  return false;
}

bool appendIgnoreRules(const Repository &repo, const QStringList &patterns,
                       bool *written, QString *error) {
  bool exists = false;
  const QByteArray content = readIgnoreFile(repo, &exists, error);
  if (error && !error->isEmpty())
    return false;

  const QStringList existing = ignoreLines(content);
  QByteArray updated = content;
  bool changed = false;
  for (const QString &pattern : patterns) {
    if (existing.contains(pattern) ||
        QString::fromUtf8(updated).split('\n').contains(pattern))
      continue;

    if (!updated.isEmpty() && !updated.endsWith('\n'))
      updated.append('\n');
    updated.append(pattern.toUtf8());
    updated.append('\n');
    changed = true;
  }

  if (!changed) {
    if (written)
      *written = false;
    return true;
  }

  QSaveFile file(repo.workdir().filePath(QStringLiteral(".gitignore")));
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    if (error)
      *error = file.errorString();
    return false;
  }
  if (file.write(updated) != updated.size() || !file.commit()) {
    if (error)
      *error = file.errorString();
    return false;
  }

  if (written)
    *written = true;
  return true;
}

bool removeWorktreeFile(const Repository &repo, const QString &path,
                        QStringList &failedPaths) {
  if (hasSymlinkParent(repo, path)) {
    failedPaths.append(path);
    return false;
  }

  const QFileInfo info(repo.workdir().filePath(path));
  if (!info.exists() && !info.isSymLink())
    return true;
  if (info.isDir() && !info.isSymLink()) {
    failedPaths.append(path);
    return false;
  }
  if (QFile::remove(info.filePath()))
    return true;
  failedPaths.append(path);
  return false;
}

void removeEmptyParents(const Repository &repo, const QStringList &roots,
                        const QStringList &deletedPaths) {
  QSet<QString> candidates;
  for (const QString &path : deletedPaths) {
    QString current = QFileInfo(repo.workdir().filePath(path)).path();
    while (!current.isEmpty() && current != repo.workdir().path()) {
      candidates.insert(current);
      const QString parent = QFileInfo(current).path();
      if (parent == current)
        break;
      current = parent;
    }
  }

  QStringList ordered = candidates.values();
  std::sort(ordered.begin(), ordered.end(),
            [](const QString &lhs, const QString &rhs) {
              return lhs.count('/') > rhs.count('/');
            });
  for (const QString &path : ordered) {
    bool withinRoot = false;
    const QString relative =
        normalizePath(repo.workdir().relativeFilePath(path));
    for (const QString &root : roots) {
      if (pathMatchesRoot(relative, root)) {
        withinRoot = true;
        break;
      }
    }
    if (withinRoot)
      if (!QFileInfo(path).isSymLink())
        QDir().rmdir(path);
  }
}

} // namespace

WorkingTreeUntrackPreparation
WorkingTreeUntrack::prepare(const QString &repositoryPath,
                            const QStringList &inputRoots,
                            const std::shared_ptr<std::atomic_bool> &canceled) {
  WorkingTreeUntrackPreparation result;
  if (isCanceled(canceled)) {
    result.canceled = true;
    return result;
  }

  const Repository repo = Repository::open(repositoryPath);
  if (!repo.isValid() || repo.isBare()) {
    result.error = QStringLiteral("Unable to open a non-bare repository.");
    return result;
  }

  QStringList roots;
  for (const QString &inputRoot : inputRoots) {
    const QString root = normalizePath(inputRoot);
    if (!roots.contains(root))
      roots.append(root);
  }
  roots.removeAll(QStringLiteral("."));
  std::sort(roots.begin(), roots.end());
  if (roots.isEmpty() || !validateRoots(roots, &result.error))
    return result;

  Index index = repo.index();
  const QStringList tracked = indexPaths(index, roots);
  QSet<QString> trackedSet(tracked.begin(), tracked.end());
  QStringList protectedPaths;
  for (const QString &path : tracked) {
    if (repo.lookupSubmodule(path).isValid())
      appendUnique(protectedPaths, path);
  }

  QStringList untracked;
  QStringList ignored;
  for (const QString &root : roots) {
    if (isCanceled(canceled)) {
      result.canceled = true;
      return result;
    }

    const QString absolute = repo.workdir().filePath(root);
    const QFileInfo info(absolute);
    const bool directory = info.isDir() && !info.isSymLink();
    if (directory) {
      if (isRepositoryDirectory(absolute)) {
        appendUnique(protectedPaths, root);
        continue;
      }
      collectDirectory(repo, absolute, root, trackedSet, untracked, ignored,
                       protectedPaths, canceled);
    } else if (trackedSet.contains(root)) {
      continue;
    } else if (info.exists() || info.isSymLink()) {
      if (repo.isIgnored(root))
        appendUnique(ignored, root);
      else
        appendUnique(untracked, root);
    }
  }

  QStringList filteredTracked;
  for (const QString &path : tracked) {
    if (!isProtectedRepositoryPath(repo, path, protectedPaths))
      filteredTracked.append(path);
  }

  const WorkingTreeStatusSnapshot status = WorkingTreeStatusSnapshot::scan(
      repo.dir(false).path(), WorkingTreeStatusOptions(), canceled.get());
  if (isCanceled(canceled)) {
    result.canceled = true;
    return result;
  }
  if (!status.isValid()) {
    result.error = status.result().errorString(
        QStringLiteral("Unable to read repository status."));
    return result;
  }

  for (const WorkingTreeStatusEntry &entry : status.entries()) {
    if (entry.hasTrackedChange() && (pathMatchesAnyRoot(entry.path, roots) ||
                                     pathMatchesAnyRoot(entry.oldPath, roots)))
      appendUnique(result.plan.modifiedTrackedPaths, entry.path);
  }

  QString ignoreError;
  const QString ignoreHash = ignoreFingerprint(repo, &ignoreError);
  if (!ignoreError.isEmpty()) {
    result.error = ignoreError;
    return result;
  }

  QStringList patterns;
  for (const QString &root : roots) {
    const QFileInfo info(repo.workdir().filePath(root));
    const bool directory =
        (info.isDir() && !info.isSymLink()) ||
        std::any_of(filteredTracked.begin(), filteredTracked.end(),
                    [&root](const QString &path) {
                      return path.startsWith(root + QStringLiteral("/"));
                    });
    patterns.append(patternFor(root, directory));
  }
  patterns.removeDuplicates();

  QStringList preservedTrackedPaths;
  for (const QString &path : filteredTracked) {
    if (path == QStringLiteral(".gitignore"))
      preservedTrackedPaths.append(path);
  }
  QStringList preservedUntrackedPaths;
  for (const QString &path : untracked) {
    if (path == QStringLiteral(".gitignore"))
      preservedUntrackedPaths.append(path);
  }

  result.plan.repositoryPath = repo.dir(false).path();
  result.plan.headId = headId(repo);
  result.plan.statusFingerprint = statusFingerprint(status, roots);
  QStringList fingerprintPaths = filteredTracked;
  fingerprintPaths.append(untracked);
  result.plan.worktreeFingerprint =
      fileStateFingerprint(repo, fingerprintPaths);
  result.plan.ignoreFingerprint = ignoreHash;
  result.plan.roots = roots;
  result.plan.trackedPaths = filteredTracked;
  result.plan.untrackedPaths = untracked;
  result.plan.ignoredPaths = ignored;
  result.plan.protectedPaths = protectedPaths;
  result.plan.preservedTrackedPaths = preservedTrackedPaths;
  result.plan.preservedUntrackedPaths = preservedUntrackedPaths;
  result.plan.ignorePatterns = patterns;

  if (result.plan.trackedPaths.isEmpty()) {
    result.error =
        QStringLiteral("No tracked files were found in the selection.");
    return result;
  }

  return result;
}

WorkingTreeUntrackExecution
WorkingTreeUntrack::execute(const WorkingTreeUntrackPlan &plan,
                            bool deleteTracked, bool deleteUntracked,
                            const std::shared_ptr<std::atomic_bool> &canceled) {
  WorkingTreeUntrackExecution result;
  if (isCanceled(canceled)) {
    result.canceled = true;
    return result;
  }
  if (plan.repositoryPath.isEmpty() || plan.trackedPaths.isEmpty()) {
    result.error = QStringLiteral("The stop-tracking plan is empty.");
    return result;
  }

  QMutexLocker locker(&mutationMutex());
  if (isCanceled(canceled)) {
    result.canceled = true;
    return result;
  }

  const Repository repo = Repository::open(plan.repositoryPath);
  if (!repo.isValid() || repo.isBare()) {
    result.error = QStringLiteral("Unable to reopen the repository.");
    return result;
  }
  if (headId(repo) != plan.headId) {
    result.error =
        QStringLiteral("The repository HEAD changed after confirmation.");
    return result;
  }

  Index index = repo.index();
  QStringList currentTracked;
  for (const QString &path : indexPaths(index, plan.roots)) {
    if (!isProtectedRepositoryPath(repo, path, plan.protectedPaths))
      currentTracked.append(path);
  }
  if (currentTracked != plan.trackedPaths) {
    result.error =
        QStringLiteral("The tracked paths changed after confirmation.");
    return result;
  }

  const WorkingTreeStatusSnapshot status = WorkingTreeStatusSnapshot::scan(
      plan.repositoryPath, WorkingTreeStatusOptions(), canceled.get());
  if (isCanceled(canceled)) {
    result.canceled = true;
    return result;
  }
  if (!status.isValid()) {
    result.error = status.result().errorString(
        QStringLiteral("Unable to recheck repository status."));
    return result;
  }
  if (statusFingerprint(status, plan.roots) != plan.statusFingerprint) {
    result.error =
        QStringLiteral("The selected paths changed after confirmation.");
    return result;
  }

  QStringList fingerprintPaths = plan.trackedPaths;
  fingerprintPaths.append(plan.untrackedPaths);
  if (fileStateFingerprint(repo, fingerprintPaths) !=
      plan.worktreeFingerprint) {
    result.error =
        QStringLiteral("The selected files changed after confirmation.");
    return result;
  }

  QString ignoreError;
  if (ignoreFingerprint(repo, &ignoreError) != plan.ignoreFingerprint) {
    result.error =
        ignoreError.isEmpty()
            ? QStringLiteral(".gitignore changed after confirmation.")
            : ignoreError;
    return result;
  }

  if (isCanceled(canceled)) {
    result.canceled = true;
    return result;
  }

  if (!appendIgnoreRules(repo, plan.ignorePatterns, &result.ignoreWritten,
                         &result.error)) {
    return result;
  }

  if (isCanceled(canceled)) {
    result.canceled = true;
    result.partial = result.ignoreWritten;
    return result;
  }

  if (!index.removePaths(plan.trackedPaths)) {
    result.error =
        QStringLiteral("Unable to remove tracked paths from the index.");
    result.partial = result.ignoreWritten;
    return result;
  }
  result.indexWritten = true;

  if (isCanceled(canceled)) {
    result.canceled = true;
    result.partial = true;
    return result;
  }

  if (deleteTracked) {
    for (const QString &path : plan.trackedPaths) {
      if (isCanceled(canceled)) {
        result.canceled = true;
        result.partial = true;
        return result;
      }
      if (plan.preservedTrackedPaths.contains(path))
        continue;
      if (removeWorktreeFile(repo, path, result.failedPaths))
        result.deletedTrackedPaths.append(path);
    }
  }
  if (deleteUntracked) {
    for (const QString &path : plan.untrackedPaths) {
      if (isCanceled(canceled)) {
        result.canceled = true;
        result.partial = true;
        return result;
      }
      if (plan.preservedUntrackedPaths.contains(path) ||
          isProtectedRepositoryPath(repo, path, plan.protectedPaths))
        continue;
      if (removeWorktreeFile(repo, path, result.failedPaths))
        result.deletedUntrackedPaths.append(path);
    }
  }

  if (isCanceled(canceled)) {
    result.canceled = true;
    result.partial = true;
    return result;
  }

  if (deleteTracked || deleteUntracked) {
    QStringList deleted = result.deletedTrackedPaths;
    deleted.append(result.deletedUntrackedPaths);
    removeEmptyParents(repo, plan.roots, deleted);
  }

  result.partial = !result.failedPaths.isEmpty();
  return result;
}

} // namespace git

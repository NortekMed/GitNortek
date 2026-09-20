//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. See LICENSE.md.
//

#include "WorkingTreeDiscard.h"

#include "Commit.h"
#include "Reference.h"
#include "Repository.h"
#include "Submodule.h"
#include "Tree.h"
#include "WorkingTreeStatus.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <functional>

namespace git {

namespace {

bool isCanceled(const std::shared_ptr<std::atomic_bool> &canceled) {
  return canceled && canceled->load();
}

void appendPath(QStringList &paths, const QString &path) {
  if (!path.isEmpty() && !paths.contains(path))
    paths.append(path);
}

QString prefixedPath(const QString &prefix, const QString &path) {
  return prefix.isEmpty() ? path : prefix + '/' + path;
}

QString headId(const Repository &repo) {
  const Commit head = repo.head().target();
  return head.isValid() ? head.id().toString() : QString();
}

bool isSafeRelativePath(const QString &path) {
  if (path.isEmpty() || QDir::isAbsolutePath(path))
    return false;

  const QString normalized = QDir::cleanPath(QDir::fromNativeSeparators(path));
  return normalized != QStringLiteral(".") &&
         normalized != QStringLiteral("..") &&
         !normalized.startsWith(QStringLiteral("../"));
}

QString errorText(const QString &fallback) {
  return Repository::lastError(fallback);
}

QString encode(const QString &value) {
  return QString::fromLatin1(value.toUtf8().toHex());
}

QString trackedFingerprint(const Repository &repo,
                           const WorkingTreeStatusSnapshot &status,
                           const QStringList &trackedPaths) {
  QStringList entries;
  for (const WorkingTreeStatusEntry &entry : status.entries()) {
    if (!entry.hasTrackedChange())
      continue;

    entries.append(QStringList{
        encode(entry.path), encode(entry.oldPath), QString::number(entry.flags),
        QString::number(entry.indexStatus),
        QString::number(entry.workdirStatus), encode(entry.oldId.toString()),
        encode(entry.newId.toString()), QString::number(entry.oldMode),
        QString::number(entry.newMode)}
                       .join('|'));
  }

  QStringList paths = trackedPaths;
  paths.removeDuplicates();
  std::sort(paths.begin(), paths.end());
  for (const QString &path : paths) {
    const QFileInfo info(repo.workdir().filePath(path));
    QString content;
    if (!info.exists() && !info.isSymLink()) {
      content = QStringLiteral("missing");
    } else if (info.isDir()) {
      content = QStringLiteral("directory");
    } else {
      QFile file(info.filePath());
      QCryptographicHash hash(QCryptographicHash::Sha256);
      if (!file.open(QIODevice::ReadOnly)) {
        content = QStringLiteral("unreadable");
      } else {
        QByteArray buffer(64 * 1024, '\0');
        while (!file.atEnd()) {
          const qint64 count = file.read(buffer.data(), buffer.size());
          if (count < 0) {
            content = QStringLiteral("unreadable");
            break;
          }
          hash.addData(QByteArrayView(buffer.constData(), count));
        }
        if (content.isEmpty())
          content = QString::fromLatin1(hash.result().toHex());
      }
    }
    entries.append(QStringList{encode(path), encode(content)}.join('|'));
  }

  std::sort(entries.begin(), entries.end());
  QByteArray serialized;
  for (const QString &entry : entries) {
    serialized.append(entry.toUtf8());
    serialized.append('\n');
  }
  return QString::fromLatin1(
      QCryptographicHash::hash(serialized, QCryptographicHash::Sha256).toHex());
}

void addStatusPaths(const WorkingTreeStatusSnapshot &status, bool hasHead,
                    WorkingTreeDiscardRepositoryPlan &repository,
                    QStringList *allTracked, QStringList *allUntracked) {
  for (const WorkingTreeStatusEntry &entry : status.entries()) {
    QStringList &paths = (!hasHead || entry.isUntracked())
                             ? repository.untrackedPaths
                             : repository.trackedPaths;
    QStringList *allPaths =
        (!hasHead || entry.isUntracked()) ? allUntracked : allTracked;
    appendPath(paths, entry.path);
    appendPath(*allPaths, prefixedPath(repository.prefix, entry.path));

    if (!entry.isUntracked()) {
      appendPath(paths, entry.oldPath);
      appendPath(*allPaths, prefixedPath(repository.prefix, entry.oldPath));
    }
  }
}

bool validatePaths(const WorkingTreeDiscardRepositoryPlan &repository,
                   QString *error) {
  const auto validate = [error](const QStringList &paths) {
    for (const QString &path : paths) {
      if (isSafeRelativePath(path))
        continue;

      *error = QStringLiteral("The discard plan contains an invalid path: %1")
                   .arg(path);
      return false;
    }
    return true;
  };

  return validate(repository.trackedPaths) &&
         validate(repository.untrackedPaths);
}

bool sameHead(const Repository &repo, const QString &expected) {
  return headId(repo) == expected;
}

bool pathMatches(const QString &path, const QStringList &plannedPaths) {
  for (const QString &plannedPath : plannedPaths) {
    if (path == plannedPath ||
        path.startsWith(plannedPath + QStringLiteral("/")))
      return true;
  }
  return false;
}

bool hasPlannedStatus(const WorkingTreeStatusSnapshot &status,
                      const WorkingTreeDiscardRepositoryPlan &plan) {
  QStringList plannedPaths = plan.trackedPaths;
  plannedPaths.append(plan.untrackedPaths);
  for (const WorkingTreeStatusEntry &entry : status.entries()) {
    if (pathMatches(entry.path, plannedPaths) ||
        pathMatches(entry.oldPath, plannedPaths))
      return true;
  }
  return false;
}

void appendFailure(QString &firstError, const QString &error) {
  if (firstError.isEmpty())
    firstError = error;
}

} // namespace

WorkingTreeDiscardPreparation WorkingTreeDiscard::prepare(
    const QString &repositoryPath, const QString &expectedHeadId,
    const QStringList &trackedPaths, const QStringList &untrackedPaths,
    const std::shared_ptr<std::atomic_bool> &canceled) {
  WorkingTreeDiscardPreparation result;
  if (isCanceled(canceled)) {
    result.canceled = true;
    return result;
  }

  const Repository repo = Repository::open(repositoryPath);
  if (!repo.isValid()) {
    result.error =
        errorText(QStringLiteral("Unable to open the repository for discard."));
    return result;
  }

  if (!sameHead(repo, expectedHeadId)) {
    result.error = QStringLiteral(
        "The repository HEAD changed before discard preparation completed.");
    return result;
  }

  WorkingTreeDiscardRepositoryPlan root;
  root.repositoryPath = repo.dir(false).path();
  root.headId = expectedHeadId;
  root.trackedPaths = trackedPaths;
  root.untrackedPaths = untrackedPaths;

  const WorkingTreeStatusSnapshot rootStatus = WorkingTreeStatusSnapshot::scan(
      root.repositoryPath, WorkingTreeStatusOptions(), canceled.get());
  if (isCanceled(canceled)) {
    result.canceled = true;
    return result;
  }
  if (!rootStatus.isValid()) {
    result.error = rootStatus.result().errorString(
        QStringLiteral("Unable to read repository status."));
    return result;
  }
  if (!sameHead(repo, expectedHeadId)) {
    result.error = QStringLiteral(
        "The repository HEAD changed before discard preparation completed.");
    return result;
  }

  addStatusPaths(rootStatus, !expectedHeadId.isEmpty(), root,
                 &result.plan.trackedPaths, &result.plan.untrackedPaths);
  root.trackedFingerprint =
      trackedFingerprint(repo, rootStatus, root.trackedPaths);
  result.plan.repositoryPath = root.repositoryPath;
  result.plan.headId = expectedHeadId;

  for (const QString &path : trackedPaths)
    appendPath(result.plan.trackedPaths, path);
  for (const QString &path : untrackedPaths)
    appendPath(result.plan.untrackedPaths, path);

  result.plan.repositories.append(root);

  const std::function<bool(const Repository &, const QString &,
                           const QString &)>
      collect = [&](const Repository &parent, const QString &prefix,
                    const QString &desiredHeadId) {
        for (const Submodule &submodule : parent.submodules()) {
          if (isCanceled(canceled)) {
            result.canceled = true;
            return false;
          }

          QString targetId;
          Commit desiredHead = parent.lookupCommit(desiredHeadId);
          if (!desiredHead.isValid())
            desiredHead = parent.head().target();
          if (desiredHead.isValid()) {
            const Id id = desiredHead.tree().id(submodule.path());
            if (id.isValid())
              targetId = id.toString();
          }

          if (!submodule.isInitialized())
            continue;

          const Repository subrepo = submodule.open();
          if (!subrepo.isValid())
            continue;

          WorkingTreeDiscardRepositoryPlan child;
          child.repositoryPath = subrepo.dir(false).path();
          child.prefix = prefixedPath(prefix, submodule.path());
          child.headId = headId(subrepo);
          child.targetId = targetId;

          const WorkingTreeStatusSnapshot status =
              WorkingTreeStatusSnapshot::scan(child.repositoryPath,
                                              WorkingTreeStatusOptions(),
                                              canceled.get());
          if (isCanceled(canceled) || !status.isValid()) {
            result.canceled = isCanceled(canceled);
            if (!result.canceled)
              result.error = status.result().errorString(
                  QStringLiteral("Unable to read submodule status."));
            return false;
          }

          const bool childHasHead = !child.headId.isEmpty();
          addStatusPaths(status, childHasHead, child, &result.plan.trackedPaths,
                         &result.plan.untrackedPaths);
          child.trackedFingerprint =
              trackedFingerprint(subrepo, status, child.trackedPaths);
          result.plan.repositories.append(child);

          const QString nestedHeadId =
              child.targetId.isEmpty() ? child.headId : child.targetId;
          if (!collect(subrepo, child.prefix, nestedHeadId))
            return false;
        }
        return true;
      };

  if (!collect(repo, QString(), expectedHeadId))
    return result;

  for (const WorkingTreeDiscardRepositoryPlan &repository :
       result.plan.repositories) {
    QString pathError;
    if (!validatePaths(repository, &pathError)) {
      result.error = pathError;
      return result;
    }
  }

  if (!result.plan.isDirty())
    result.plan.repositories.clear();

  return result;
}

WorkingTreeDiscardExecution
WorkingTreeDiscard::execute(const WorkingTreeDiscardPlan &plan,
                            const std::shared_ptr<std::atomic_bool> &canceled) {
  WorkingTreeDiscardExecution result;
  if (isCanceled(canceled)) {
    result.canceled = true;
    return result;
  }

  if (plan.repositories.isEmpty() || plan.repositoryPath.isEmpty()) {
    result.error = QStringLiteral("The discard plan is empty.");
    return result;
  }

  struct OpenRepository {
    WorkingTreeDiscardRepositoryPlan plan;
    Repository repository;
  };
  QList<OpenRepository> repositories;
  repositories.reserve(plan.repositories.size());

  // Validate every repository and every planned path before changing any
  // working tree. This prevents a stale plan from partially discarding a
  // repository after a branch or submodule moved.
  for (const WorkingTreeDiscardRepositoryPlan &repositoryPlan :
       plan.repositories) {
    if (isCanceled(canceled)) {
      result.canceled = true;
      return result;
    }

    QString pathError;
    if (!validatePaths(repositoryPlan, &pathError)) {
      result.error = pathError;
      return result;
    }

    const Repository repository =
        Repository::open(repositoryPlan.repositoryPath);
    if (!repository.isValid()) {
      result.error = errorText(
          QStringLiteral("Unable to open a repository from the discard plan."));
      return result;
    }

    if (!sameHead(repository, repositoryPlan.headId)) {
      result.error = QStringLiteral(
          "A repository HEAD changed after discard confirmation.");
      return result;
    }

    const WorkingTreeStatusSnapshot status = WorkingTreeStatusSnapshot::scan(
        repositoryPlan.repositoryPath, WorkingTreeStatusOptions(),
        canceled.get());
    if (isCanceled(canceled)) {
      result.canceled = true;
      return result;
    }
    if (!status.isValid()) {
      result.error = status.result().errorString(
          QStringLiteral("Unable to read repository status."));
      return result;
    }
    if (!sameHead(repository, repositoryPlan.headId)) {
      result.error = QStringLiteral(
          "A repository HEAD changed after discard confirmation.");
      return result;
    }
    if (trackedFingerprint(repository, status, repositoryPlan.trackedPaths) !=
        repositoryPlan.trackedFingerprint) {
      result.error =
          QStringLiteral("Tracked changes changed after discard confirmation.");
      return result;
    }

    if (!repositoryPlan.headId.isEmpty() &&
        !repository.lookupCommit(repositoryPlan.headId).isValid()) {
      result.error = QStringLiteral(
          "The discard plan references a missing repository HEAD.");
      return result;
    }

    if (!repositoryPlan.targetId.isEmpty() &&
        !repository.lookupCommit(repositoryPlan.targetId).isValid()) {
      result.error = QStringLiteral(
          "The discard plan references a missing submodule commit.");
      return result;
    }

    repositories.append({repositoryPlan, repository});
  }

  if (repositories.constFirst().plan.repositoryPath != plan.repositoryPath ||
      repositories.constFirst().plan.headId != plan.headId) {
    result.error = QStringLiteral("The discard plan root repository changed.");
    return result;
  }

  for (int index = 0; index < repositories.size(); ++index) {
    if (isCanceled(canceled)) {
      result.canceled = true;
      return result;
    }

    OpenRepository &openRepository = repositories[index];
    const WorkingTreeDiscardRepositoryPlan &repositoryPlan =
        openRepository.plan;
    Repository &repository = openRepository.repository;
    const bool isRoot = index == 0;
    const bool targetChanged = !repositoryPlan.targetId.isEmpty() &&
                               repositoryPlan.targetId != repositoryPlan.headId;

    const auto cleanPaths = [&]() {
      QStringList pathsToClean = repositoryPlan.untrackedPaths;
      if (repositoryPlan.headId.isEmpty())
        pathsToClean.append(repositoryPlan.trackedPaths);

      for (const QString &path : pathsToClean) {
        if (isCanceled(canceled)) {
          result.canceled = true;
          return false;
        }

        if (repository.clean(path))
          continue;

        const QString absolutePath = repository.workdir().filePath(path);
        if (!QFileInfo::exists(absolutePath))
          continue;

        const QString fullPath = prefixedPath(repositoryPlan.prefix, path);
        appendPath(result.failedPaths, fullPath);
        appendFailure(result.cleanupError,
                      QStringLiteral("Unable to remove '%1'.").arg(fullPath));
      }
      return true;
    };

    if (targetChanged && !cleanPaths())
      return result;

    if (!repositoryPlan.headId.isEmpty()) {
      const Commit head = repository.lookupCommit(repositoryPlan.headId);
      if (!head.isValid() ||
          !head.reset(GIT_RESET_HARD, QStringList(), false)) {
        const QString message = errorText(
            QStringLiteral("Unable to restore the repository from HEAD."));
        if (isRoot)
          appendFailure(result.rootResetError, message);
        else {
          appendPath(result.failedSubmodules, repositoryPlan.prefix);
          appendFailure(result.submoduleError, message);
        }
        return result;
      }
    } else {
      QStringList paths = repositoryPlan.trackedPaths;
      paths.append(repositoryPlan.untrackedPaths);
      if (!paths.isEmpty() && !repository.index().setStaged(paths, false)) {
        result.error = QStringLiteral("Unable to update the repository index.");
        return result;
      }
    }

    if (targetChanged) {
      const Commit target = repository.lookupCommit(repositoryPlan.targetId);
      if (!target.isValid() ||
          !repository.checkout(target, nullptr, QStringList(),
                               GIT_CHECKOUT_FORCE)) {
        appendPath(result.failedSubmodules, repositoryPlan.prefix);
        appendFailure(result.submoduleError,
                      errorText(QStringLiteral(
                          "Unable to checkout the submodule at its recorded "
                          "commit.")));
        return result;
      }
      if (!repository.setHeadDetached(target)) {
        appendPath(result.failedSubmodules, repositoryPlan.prefix);
        appendFailure(result.submoduleError,
                      errorText(QStringLiteral(
                          "Unable to detach the submodule at its recorded "
                          "commit.")));
        return result;
      }
    }

    if (!targetChanged && !cleanPaths())
      return result;
  }

  for (const OpenRepository &openRepository : repositories) {
    if (isCanceled(canceled)) {
      result.canceled = true;
      return result;
    }

    const WorkingTreeStatusSnapshot status = WorkingTreeStatusSnapshot::scan(
        openRepository.plan.repositoryPath, WorkingTreeStatusOptions(),
        canceled.get());
    if (isCanceled(canceled)) {
      result.canceled = true;
      return result;
    }
    if (!status.isValid()) {
      result.error = status.result().errorString(
          QStringLiteral("Unable to verify the discarded repository."));
      return result;
    }
    if (hasPlannedStatus(status, openRepository.plan)) {
      result.error =
          QStringLiteral("Some planned changes could not be discarded.");
      appendPath(result.failedPaths, openRepository.plan.prefix);
      return result;
    }
  }

  return result;
}

} // namespace git

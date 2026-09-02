//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "WorkingTreeStatus.h"
#include "util/Path.h"
#include "util/PerformanceTrace.h"
#include "git2/errors.h"
#include "git2/repository.h"

namespace git {

namespace {

bool isCanceled(const std::atomic_bool *canceled) {
  return canceled && canceled->load();
}

QString deltaPath(const git_diff_delta *delta, bool oldFile) {
  if (!delta)
    return QString();

  const char *path = oldFile ? delta->old_file.path : delta->new_file.path;
  return path ? QString::fromUtf8(path) : QString();
}

git_delta_t deltaStatus(const git_diff_delta *delta) {
  return delta ? delta->status : GIT_DELTA_UNMODIFIED;
}

Id deltaId(const git_diff_delta *delta, bool oldFile) {
  if (!delta)
    return Id();

  return oldFile ? Id(delta->old_file.id) : Id(delta->new_file.id);
}

uint32_t deltaMode(const git_diff_delta *delta, bool oldFile) {
  if (!delta)
    return 0;

  return oldFile ? delta->old_file.mode : delta->new_file.mode;
}

WorkingTreeStatusEntry entryFromStatus(const git_status_entry *status) {
  WorkingTreeStatusEntry entry;
  entry.flags = status->status;

  const git_diff_delta *indexDelta = status->index_to_workdir;
  const git_diff_delta *headDelta = status->head_to_index;
  const git_diff_delta *primaryDelta = indexDelta ? indexDelta : headDelta;

  entry.path = deltaPath(primaryDelta, false);
  if (entry.path.isEmpty())
    entry.path = deltaPath(primaryDelta, true);

  entry.oldPath = deltaPath(primaryDelta, true);
  if (entry.oldPath == entry.path)
    entry.oldPath.clear();

  entry.indexStatus = deltaStatus(headDelta);
  entry.workdirStatus = deltaStatus(indexDelta);
  entry.oldId = deltaId(primaryDelta, true);
  entry.newId = deltaId(primaryDelta, false);
  entry.oldMode = deltaMode(primaryDelta, true);
  entry.newMode = deltaMode(primaryDelta, false);

  return entry;
}

} // namespace

bool WorkingTreeStatusEntry::isConflicted() const {
  return flags & GIT_STATUS_CONFLICTED;
}

bool WorkingTreeStatusEntry::isUntracked() const {
  return flags & GIT_STATUS_WT_NEW;
}

bool WorkingTreeStatusEntry::hasIndexChange() const {
  return flags & (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED |
                  GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED |
                  GIT_STATUS_INDEX_TYPECHANGE);
}

bool WorkingTreeStatusEntry::hasWorkdirChange() const {
  return flags & (GIT_STATUS_WT_NEW | GIT_STATUS_WT_MODIFIED |
                  GIT_STATUS_WT_DELETED | GIT_STATUS_WT_RENAMED |
                  GIT_STATUS_WT_TYPECHANGE);
}

bool WorkingTreeStatusEntry::hasTrackedChange() const {
  return isConflicted() || hasIndexChange() ||
         (hasWorkdirChange() && !isUntracked());
}

WorkingTreeStatusSnapshot WorkingTreeStatusSnapshot::scan(
    const QString &path, const WorkingTreeStatusOptions &options,
    const std::atomic_bool *canceled) {
  PerformanceTrace::Span span("status", "WorkingTreeStatusSnapshot::scan", path);
  WorkingTreeStatusSnapshot snapshot;
  if (isCanceled(canceled)) {
    snapshot.setResult(Result(GIT_EUSER));
    return snapshot;
  }

  git_repository *repo = nullptr;
  int error = 0;
  {
    PerformanceTrace::Span openSpan("status", "raw repository open", path);
    error = git_repository_open_ext(&repo, util::canonicalizePath(path).toUtf8(),
                                    GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr);
  }
  if (error) {
    snapshot.setResult(Result(error));
    return snapshot;
  }

  git_status_options statusOptions = GIT_STATUS_OPTIONS_INIT;
  statusOptions.flags = 0;
  if (options.includeUntracked)
    statusOptions.flags |= GIT_STATUS_OPT_INCLUDE_UNTRACKED;
  if (options.recurseUntrackedDirs)
    statusOptions.flags |= GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
  if (options.detectRenames) {
    statusOptions.flags |= GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
                           GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR;
  }

  git_status_list *list = nullptr;
  {
    PerformanceTrace::Span statusSpan("status", "git_status_list_new", path);
    error = git_status_list_new(&list, repo, &statusOptions);
  }
  if (error) {
    snapshot.setResult(Result(error));
    git_repository_free(repo);
    return snapshot;
  }

  const size_t count = git_status_list_entrycount(list);
  PerformanceTrace::event("status", "entry count", path,
                          {{"entries", qint64(count)}});
  for (size_t i = 0; i < count; ++i) {
    if (isCanceled(canceled)) {
      snapshot.setResult(Result(GIT_EUSER));
      break;
    }

    if (const git_status_entry *status = git_status_byindex(list, i))
      snapshot.append(entryFromStatus(status));
  }

  git_status_list_free(list);
  git_repository_free(repo);
  return snapshot;
}

bool WorkingTreeStatusSnapshot::isDirty() const {
  return mValid && !mEntries.isEmpty();
}

bool WorkingTreeStatusSnapshot::hasTrackedChanges() const {
  if (!mValid)
    return false;

  for (const WorkingTreeStatusEntry &entry : mEntries) {
    if (entry.hasTrackedChange())
      return true;
  }
  return false;
}

QStringList WorkingTreeStatusSnapshot::untrackedPaths() const {
  QStringList paths;
  if (!mValid)
    return paths;

  for (const WorkingTreeStatusEntry &entry : mEntries) {
    if (entry.isUntracked())
      paths.append(entry.path);
  }
  return paths;
}

void WorkingTreeStatusSnapshot::setResult(const Result &result) {
  mResult = result;
  mValid = bool(result);
}

} // namespace git

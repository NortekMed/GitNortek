//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef WORKINGTREESTATUS_H
#define WORKINGTREESTATUS_H

#include "Id.h"
#include "Result.h"
#include "git2/status.h"
#include <QList>
#include <QString>
#include <atomic>

namespace git {

struct WorkingTreeStatusOptions {
  bool includeUntracked = true;
  bool recurseUntrackedDirs = true;
  bool detectRenames = true;
};

struct WorkingTreeStatusEntry {
  QString path;
  QString oldPath;
  unsigned int flags = 0;
  git_delta_t indexStatus = GIT_DELTA_UNMODIFIED;
  git_delta_t workdirStatus = GIT_DELTA_UNMODIFIED;
  Id oldId;
  Id newId;
  uint32_t oldMode = 0;
  uint32_t newMode = 0;

  bool isConflicted() const;
  bool isUntracked() const;
  bool hasIndexChange() const;
  bool hasWorkdirChange() const;
  bool hasTrackedChange() const;
};

class WorkingTreeStatusSnapshot {
public:
  static WorkingTreeStatusSnapshot scan(
      const QString &path, const WorkingTreeStatusOptions &options,
      const std::atomic_bool *canceled = nullptr);

  bool isValid() const { return mValid; }
  explicit operator bool() const { return isValid(); }
  const Result &result() const { return mResult; }
  const QList<WorkingTreeStatusEntry> &entries() const { return mEntries; }

  bool isDirty() const;
  bool hasTrackedChanges() const;
  QStringList untrackedPaths() const;

private:
  void setResult(const Result &result);
  void append(const WorkingTreeStatusEntry &entry) { mEntries.append(entry); }

  QList<WorkingTreeStatusEntry> mEntries;
  Result mResult{0};
  bool mValid = true;
};

} // namespace git

Q_DECLARE_METATYPE(git::WorkingTreeStatusSnapshot)

#endif // WORKINGTREESTATUS_H

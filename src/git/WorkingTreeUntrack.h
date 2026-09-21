//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. See LICENSE.md.
//

#ifndef WORKINGTREEUNTRACK_H
#define WORKINGTREEUNTRACK_H

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>

namespace git {

struct WorkingTreeUntrackPlan {
  QString repositoryPath;
  QString headId;
  QString statusFingerprint;
  QString worktreeFingerprint;
  QString ignoreFingerprint;
  QStringList roots;
  QStringList trackedPaths;
  QStringList modifiedTrackedPaths;
  QStringList untrackedPaths;
  QStringList ignoredPaths;
  QStringList protectedPaths;
  QStringList preservedTrackedPaths;
  QStringList preservedUntrackedPaths;
  QStringList ignorePatterns;
  quint64 generation = 0;

  bool isEmpty() const { return trackedPaths.isEmpty(); }
};

struct WorkingTreeUntrackPreparation {
  WorkingTreeUntrackPlan plan;
  QString error;
  bool canceled = false;

  bool isValid() const { return !canceled && error.isEmpty(); }
};

struct WorkingTreeUntrackExecution {
  QString error;
  QStringList failedPaths;
  QStringList deletedTrackedPaths;
  QStringList deletedUntrackedPaths;
  bool ignoreWritten = false;
  bool indexWritten = false;
  bool partial = false;
  bool canceled = false;

  bool isValid() const {
    return !canceled && error.isEmpty() && failedPaths.isEmpty();
  }
};

class WorkingTreeUntrack {
public:
  static WorkingTreeUntrackPreparation
  prepare(const QString &repositoryPath, const QStringList &roots,
          const std::shared_ptr<std::atomic_bool> &canceled);

  static WorkingTreeUntrackExecution
  execute(const WorkingTreeUntrackPlan &plan, bool deleteTracked,
          bool deleteUntracked,
          const std::shared_ptr<std::atomic_bool> &canceled);
};

} // namespace git

Q_DECLARE_METATYPE(git::WorkingTreeUntrackPlan)
Q_DECLARE_METATYPE(git::WorkingTreeUntrackPreparation)
Q_DECLARE_METATYPE(git::WorkingTreeUntrackExecution)

#endif // WORKINGTREEUNTRACK_H

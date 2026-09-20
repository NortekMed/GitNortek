//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. See LICENSE.md.
//

#ifndef WORKINGTREEDISCARD_H
#define WORKINGTREEDISCARD_H

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>

namespace git {

struct WorkingTreeDiscardRepositoryPlan {
  QString repositoryPath;
  QString prefix;
  QString headId;
  QString targetId;
  QString trackedFingerprint;
  QStringList trackedPaths;
  QStringList untrackedPaths;
};

struct WorkingTreeDiscardPlan {
  QString repositoryPath;
  QString headId;
  quint64 generation = 0;
  QStringList trackedPaths;
  QStringList untrackedPaths;
  QList<WorkingTreeDiscardRepositoryPlan> repositories;

  bool isDirty() const {
    return !trackedPaths.isEmpty() || !untrackedPaths.isEmpty();
  }
};

struct WorkingTreeDiscardPreparation {
  WorkingTreeDiscardPlan plan;
  QString error;
  bool canceled = false;

  bool isValid() const { return !canceled && error.isEmpty(); }
};

struct WorkingTreeDiscardExecution {
  QString error;
  QString rootResetError;
  QString cleanupError;
  QString submoduleError;
  QStringList failedPaths;
  QStringList failedSubmodules;
  bool canceled = false;

  bool isValid() const {
    return !canceled && error.isEmpty() && rootResetError.isEmpty() &&
           cleanupError.isEmpty() && submoduleError.isEmpty();
  }
};

class WorkingTreeDiscard {
public:
  static WorkingTreeDiscardPreparation
  prepare(const QString &repositoryPath, const QString &headId,
          const QStringList &trackedPaths, const QStringList &untrackedPaths,
          const std::shared_ptr<std::atomic_bool> &canceled);

  static WorkingTreeDiscardExecution
  execute(const WorkingTreeDiscardPlan &plan,
          const std::shared_ptr<std::atomic_bool> &canceled);
};

} // namespace git

Q_DECLARE_METATYPE(git::WorkingTreeDiscardPreparation)
Q_DECLARE_METATYPE(git::WorkingTreeDiscardExecution)

#endif // WORKINGTREEDISCARD_H

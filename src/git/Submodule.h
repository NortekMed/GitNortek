//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef SUBMODULE_H
#define SUBMODULE_H

#include "Id.h"
#include "Remote.h"
#include "Result.h"
#include "git2/submodule.h"
#include <QSharedPointer>
#include <QString>

namespace git {

class Id;
class Repository;

class Submodule {
public:
  struct UpdateStatus {
    enum State {
      UpToDate,
      UpdateAvailable,
      DifferentHistory,
      NotTracked,
      Unknown,
      Error
    };

    State state = Unknown;
    QString name;
    QString path;
    QString url;
    QString branch;
    Id pinnedId;
    Id targetId;
    QString message;

    bool canUpdate() const { return state == UpdateAvailable; }
  };

  Submodule();

  bool isValid() const { return !d.isNull(); }
  explicit operator bool() const { return isValid(); }

  bool isInitialized() const;
  void initialize() const;
  void deinitialize() const;

  QString name() const;
  QString path() const;

  QString url() const;
  void setUrl(const QString &url);

  QString branch() const;
  void setBranch(const QString &branch);

  Id headId() const;
  Id indexId() const;
  Id workdirId() const;

  Result update(Remote::Callbacks *callbacks, bool init = false,
                bool checkout_force = false);
  UpdateStatus checkForUpdates(Remote::Callbacks *callbacks) const;

  static Result add(Repository repo, const QString &url, const QString &path,
                    const QString &branch, Remote::Callbacks *callbacks);
  static Result modify(Repository repo, const QString &oldName,
                       const QString &newName, const QString &newPath,
                       const QString &newUrl, const QString &newBranch);

  Repository open() const;

private:
  Submodule(git_submodule *submodule);
  operator git_submodule *() const;

  QSharedPointer<git_submodule> d;

  friend class Index;
  friend class Repository;
};

} // namespace git

Q_DECLARE_METATYPE(git::Submodule);

#endif

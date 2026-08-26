//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef SUBMODULEAVAILABILITY_H
#define SUBMODULEAVAILABILITY_H

#include "Id.h"
#include "Remote.h"
#include <QList>
#include <QString>

namespace git {

class Commit;
class Repository;
class Submodule;

class SubmoduleAvailability {
public:
  struct Issue {
    enum Reason {
      NotAdvertised,
      ComparisonUnavailable,
      RemoteError,
      LocalError
    };

    Reason reason = NotAdvertised;
    QString name;
    QString path;
    QString url;
    Id pinnedId;
    QString message;
  };

  static QList<Issue> check(const Repository &repo, const Commit &parent,
                            Remote::Callbacks *callbacks = nullptr);
  static QList<Issue> check(const Repository &repo, const Commit &parent,
                            const QList<Submodule> &submodules,
                            Remote::Callbacks *callbacks = nullptr);
};

} // namespace git

#endif

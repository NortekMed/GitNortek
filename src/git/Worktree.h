//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef WORKTREE_H
#define WORKTREE_H

#include <QMetaType>
#include <QString>

namespace git {

class Worktree {
public:
  Worktree() = default;
  Worktree(const QString &name, const QString &path, const QString &branch,
           bool main, bool valid, bool current)
      : mName(name), mPath(path), mBranch(branch), mMain(main), mValid(valid),
        mCurrent(current) {}

  QString name() const { return mName; }
  QString path() const { return mPath; }
  QString branch() const { return mBranch; }

  bool isMain() const { return mMain; }
  bool isValid() const { return mValid; }
  bool isCurrent() const { return mCurrent; }

private:
  QString mName;
  QString mPath;
  QString mBranch;
  bool mMain = false;
  bool mValid = false;
  bool mCurrent = false;
};

} // namespace git

Q_DECLARE_METATYPE(git::Worktree);

#endif

//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef FILECONTEXTMENU_H
#define FILECONTEXTMENU_H

#include "git/Id.h"
#include "git/Index.h"
#include "git/Commit.h"
#include <QMenu>

class ExternalTool;
class RepoView;

class FileContextMenu : public QMenu {
  Q_OBJECT

public:
  FileContextMenu(RepoView *view, const QStringList &files,
                  const git::Index &index = git::Index(),
                  QWidget *parent = nullptr,
                  const QStringList &roots = QStringList(),
                  bool workingTreeContext = true);

  QAction *doubleClickAction() { return mDoubleClickAction; }

private slots:
  void ignoreFile();

private:
  QAction *addExternalToolsAction(const QList<ExternalTool *> &tools);
  static bool exportPath(const git::Commit &commit, const QString &folder,
                         const QString &path, bool preservePath = false);
  void handleUncommittedChanges(const git::Index &index,
                                const QStringList &files);
  void handleCommits(const QList<git::Commit> &commits,
                     const QStringList &files);
  void addStopTrackingAction();

  RepoView *mView;
  QStringList mFiles;
  QStringList mIgnoreRoots;
  bool mWorkingTreeContext;
  QAction *mDoubleClickAction;

  friend class TestTreeView;
  friend class TestFileContextMenu;
};

#endif

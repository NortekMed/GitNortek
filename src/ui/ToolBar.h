//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef TOOLBAR_H
#define TOOLBAR_H

#include <QAction>
#include <QPointer>
#include <QToolBar>

class GitHub;
class History;
class MainWindow;
class RepoView;
class SearchField;
class QButtonGroup;
class QToolButton;

class ToolBar : public QToolBar {
  Q_OBJECT

public:
  ToolBar(MainWindow *parent);

  SearchField *searchField() const { return mSearchField; }

private:
  void updateButtons(int ahead, int behind);
  void updateRemote(int ahead, int behind);
  void updateHistory();
  void updateStash();
  void updateView();
  void updateSearch();
  void updateFastIssueAccess();
  void setFastIssueAccount(GitHub *account);
  void openFastIssueDialog();

  RepoView *currentView() const;

  QToolButton *mPrevButton;
  QToolButton *mNextButton;
  QToolButton *mLocalRepoButton;

  QToolButton *mFetchButton;
  QToolButton *mPullButton;
  QToolButton *mPushButton;

  QToolButton *mCheckoutButton;

  QToolButton *mStashButton;
  QToolButton *mStashPopButton;

  QToolButton *mRefreshButton;
  QWidget *mFastIssueSpacer;
  QAction *mFastIssueSpacerAction;
  QToolButton *mFastIssueButton;
  QAction *mFastIssueButtonAction;
  QPointer<GitHub> mFastIssueAccount;
  int mFastIssueGeneration = 0;

  QToolButton *mRebaseContinueButton;
  QToolButton *mRebaseAbortButton;

  QToolButton *mPullRequestButton = nullptr;

  QToolButton *mTerminalButton;
  QToolButton *mFileManagerButton;
  QToolButton *mLogButton;
  const QButtonGroup *mModeGroup;

  QToolButton *mStarButton;
  SearchField *mSearchField;

  QAction *mRepoConfigAction;

  friend class MainWindow;
  friend class RepoView;
  friend class TestMainWindow;
};

#endif

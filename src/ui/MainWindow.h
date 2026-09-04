//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "git/Repository.h"
#include <QMainWindow>
#include <optional>

class RepoView;
class TabWidget;
class ToolBar;
class MenuBar;
class LocalRepositoryManagement;
class QSplitter;
class QStackedWidget;

namespace git {
class Submodule;
}

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  enum class OpenSource { Other, RecentRepository };

  MainWindow(const git::Repository &repo, QWidget *parent = nullptr,
             Qt::WindowFlags flags = Qt::WindowFlags(),
             std::optional<bool> updateSubmodules = std::nullopt,
             bool submoduleTab = false);

  ToolBar *toolBar() const { return mToolBar; }

  bool isSideBarVisible() const;
  void setSideBarVisible(bool visible);

  bool isLocalRepositoryManagementVisible() const;
  void setLocalRepositoryManagementVisible(bool visible);

  TabWidget *tabWidget() const;
  RepoView *addTab(const QString &path,
                   OpenSource source = OpenSource::Other,
                   const QString &tabContext = QString(),
                   std::optional<bool> updateSubmodules = std::nullopt,
                   bool submoduleTab = false);
  RepoView *addTab(const git::Repository &repo,
                   const QString &tabContext = QString(),
                   std::optional<bool> updateSubmodules = std::nullopt,
                   bool submoduleTab = false);
  bool selectTab(const QString &path);

  int count() const;
  RepoView *activeView() const;
  RepoView *currentView() const;
  QString externalToolRepositoryPath() const;
  RepoView *view(int index) const;

  // Get the "active" main window.
  static MainWindow *activeWindow();
  static QList<MainWindow *> windows();

  // Restore previous open window state.
  // Returns true if any windows were opened.
  static bool restoreWindows();

  // Open a new window.
  static MainWindow *open(const QString &path, bool warnOnInvalid = true,
                          OpenSource source = OpenSource::Other,
                          std::optional<bool> updateSubmodules = std::nullopt,
                          bool submoduleTab = false);
  static MainWindow *open(
      const git::Repository &repo = git::Repository(),
      std::optional<bool> updateSubmodules = std::nullopt,
      bool submoduleTab = false);

  // Save window settings on close.
  static void setSaveWindowSettings(bool enabled);

signals:
  void firstPainted();

protected:
  void paintEvent(QPaintEvent *event) override;
  void showEvent(QShowEvent *event) override;
  void closeEvent(QCloseEvent *event) override;
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;

private:
  void updateTabNames();
  void updateInterface();
  void updateWindowTitle(int ahead = -1, int behind = -1);

  static void warnInvalidRepo(const QString &path, const QString &details);
  static void warnInvalidRecentRepo(const QString &path,
                                    const QString &details);

  QStringList paths() const;
  QStringList tabContexts() const;
  QStringList submoduleTabs() const;
  QString windowGroup() const;

  ToolBar *mToolBar;
  MenuBar *mMenuBar;
  QStackedWidget *mCentralStack;
  QSplitter *mRepositorySplitter;
  TabWidget *mTabs;
  LocalRepositoryManagement *mLocalRepositoryManagement;

  bool mFullPath = false;
  bool mIsSideBarVisible = true;

  bool mShown = false;
  bool mFirstPaintTraced = false;
  bool mClosing = false;
  bool mAddingTab = false;
  bool mRestoringTabs = false;

  static bool sSaveWindowSettings;
};

#endif

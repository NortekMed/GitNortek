//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "FileContextMenu.h"
#include "CommitList.h"
#include "RepoView.h"
#include "IgnoreDialog.h"
#include "conf/Settings.h"
#include "Debug.h"
#include "dialogs/SettingsDialog.h"
#include "git/Diff.h"
#include "git/Index.h"
#include "git/Tree.h"
#include "host/Repository.h"
#include "tools/EditTool.h"
#include "tools/ShowTool.h"
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QMessageBox>
#include <QPushButton>
#include <QFileDialog>
#include <QDesktopServices>
#include <QFileInfo>
#include <QSaveFile>
#include <QTimer>
#include <qfileinfo.h>

namespace {

void appendUnique(QStringList &paths, const QString &path) {
  if (!path.isEmpty() && !paths.contains(path))
    paths.append(path);
}

void warnRevisionNotFound(QWidget *parent, const QString &fragment,
                          const QString &file) {
  QString title = FileContextMenu::tr("Revision Not Found");
  QString text =
      FileContextMenu::tr("The selected file doesn't have a %1 revision.")
          .arg(fragment);
  QMessageBox msg(QMessageBox::Warning, title, text, QMessageBox::Ok, parent);
  msg.setInformativeText(file);
  msg.exec();
}

void handlePath(const git::Repository &repo, const QString &path,
                const git::Diff &diff, QStringList &modified,
                QStringList &untracked) {
  auto fullPath = repo.workdir().absoluteFilePath(path);
  Debug("FileContextMenu handlePath()" << path);

  if (QFileInfo(fullPath).isDir()) {
    auto dir = QDir(path);

    for (auto entry : QDir(fullPath).entryList(
             QDir::NoDotAndDotDot | QDir::Hidden | QDir::Dirs | QDir::Files)) {
      handlePath(repo, dir.filePath(entry), diff, modified, untracked);
    }

  } else {
    int index = diff.indexOf(path);
    if (index < 0)
      return;

    switch (diff.status(index)) {
      case GIT_DELTA_DELETED:
      case GIT_DELTA_MODIFIED:
        modified.append(path);
        break;

      case GIT_DELTA_UNTRACKED:
        untracked.append(path);
        break;

      case GIT_DELTA_UNMODIFIED: // fall through
      case GIT_DELTA_ADDED:      // fall through
      case GIT_DELTA_RENAMED:    // fall through
      case GIT_DELTA_COPIED:     // fall through
      case GIT_DELTA_IGNORED:    // fall through
      case GIT_DELTA_TYPECHANGE: // fall through
      case GIT_DELTA_UNREADABLE: // fall through
      case GIT_DELTA_CONFLICTED: // fall through
        break;
    }
  }
}

void handleStatusPath(const git::Repository &repo, const QString &path,
                      const git::WorkingTreeStatusSnapshot &status,
                      QStringList &modified, QStringList &untracked) {
  const QString fullPath = repo.workdir().absoluteFilePath(path);
  const QFileInfo info(fullPath);
  if (info.isDir() && !info.isSymLink()) {
    const QDir dir(fullPath);
    for (const QString &entry : dir.entryList(
             QDir::NoDotAndDotDot | QDir::Hidden | QDir::Dirs | QDir::Files))
      handleStatusPath(repo, QDir(path).filePath(entry), status, modified,
                       untracked);
    return;
  }

  for (const git::WorkingTreeStatusEntry &entry : status.entries()) {
    if (entry.path != path && entry.oldPath != path)
      continue;
    if (entry.isUntracked())
      appendUnique(untracked, path);
    else if (entry.hasTrackedChange())
      appendUnique(modified, path);
    return;
  }
}

void handleIndexPath(const git::Repository &repo, const QString &path,
                     const git::Index &index, QStringList &modified,
                     QStringList &untracked) {
  const QString fullPath = repo.workdir().absoluteFilePath(path);
  const QFileInfo info(fullPath);
  if (info.isDir() && !info.isSymLink()) {
    const QDir dir(fullPath);
    for (const QString &entry : dir.entryList(
             QDir::NoDotAndDotDot | QDir::Hidden | QDir::Dirs | QDir::Files))
      handleIndexPath(repo, QDir(path).filePath(entry), index, modified,
                      untracked);
    return;
  }

  if (!index.isTracked(path)) {
    appendUnique(untracked, path);
    return;
  }

  switch (index.isStaged(path)) {
    case git::Index::Staged:
    case git::Index::Disabled:
      break;
    default:
      appendUnique(modified, path);
      break;
  }
}

enum class TreeEntryKind { Invalid, Blob, Tree };

QStringList pathComponents(const QString &path) {
  const QStringList components = path.split('/', Qt::SkipEmptyParts);
  for (const QString &component : components) {
    if (component == "." || component == "..")
      return QStringList();
  }
  return components;
}

TreeEntryKind findTreeEntry(const git::Tree &root, const QString &path,
                            git::Blob &blob, git::Tree &tree) {
  const QStringList components = pathComponents(path);
  if (!root.isValid() || components.isEmpty())
    return TreeEntryKind::Invalid;

  git::Tree current = root;
  for (int depth = 0; depth < components.size(); ++depth) {
    const QString &component = components.at(depth);
    bool found = false;
    for (int index = 0; index < current.count(); ++index) {
      if (current.name(index) != component)
        continue;
      const git::Object object = current.object(index);
      if (!object.isValid())
        return TreeEntryKind::Invalid;

      if (depth + 1 < components.size()) {
        current = git::Tree(object);
        if (!current.isValid())
          return TreeEntryKind::Invalid;
      } else if (object.type() == GIT_OBJECT_BLOB) {
        blob = git::Blob(object);
        return blob.isValid() ? TreeEntryKind::Blob : TreeEntryKind::Invalid;
      } else if (object.type() == GIT_OBJECT_TREE) {
        tree = git::Tree(object);
        return tree.isValid() ? TreeEntryKind::Tree : TreeEntryKind::Invalid;
      } else {
        return TreeEntryKind::Invalid;
      }
      found = true;
      break;
    }

    if (!found)
      return TreeEntryKind::Invalid;
  }

  return TreeEntryKind::Invalid;
}

bool writeBlob(const git::Blob &blob, const QString &path) {
  if (!blob.isValid())
    return false;

  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return false;

  const QByteArray content = blob.content();
  if (file.write(content) != content.size()) {
    file.cancelWriting();
    return false;
  }

  return file.commit();
}

bool writeTree(const git::Tree &tree, const QString &path) {
  if (!tree.isValid() || !QDir().mkpath(path))
    return false;

  for (int index = 0; index < tree.count(); ++index) {
    const QString name = tree.name(index);
    if (name.isEmpty() || name == "." || name == ".." || name.contains('/'))
      return false;

    const git::Object object = tree.object(index);
    const QString childPath = QDir(path).filePath(name);
    if (object.type() == GIT_OBJECT_BLOB) {
      if (!writeBlob(git::Blob(object), childPath))
        return false;
    } else if (object.type() == GIT_OBJECT_TREE) {
      if (!writeTree(git::Tree(object), childPath))
        return false;
    } else {
      return false;
    }
  }

  return true;
}

} // namespace

FileContextMenu::FileContextMenu(RepoView *view, const QStringList &files,
                                 const git::Index &index, QWidget *parent,
                                 const QStringList &roots,
                                 bool workingTreeContext)
    : QMenu(parent), mView(view), mFiles(files),
      mIgnoreRoots(roots.isEmpty() ? files : roots),
      mWorkingTreeContext(workingTreeContext && view->isWorkingTreeContext()) {
  // Show diff and merge tools for the currently selected diff.
  git::Diff diff = view->diff();
  git::Repository repo = view->repo();

  // Create external tools.
  QList<ExternalTool *> showTools;
  QList<ExternalTool *> editTools;
  QList<ExternalTool *> diffTools;
  QList<ExternalTool *> diffToLocalTools;
  QList<ExternalTool *> mergeTools;
  foreach (const QString &file, files) {
    // Convert to absolute path.
    QString path = repo.workdir().filePath(file);

    // Add show tool.
    showTools.append(new ShowTool(path, this));

    // Add edit tool.
    editTools.append(new EditTool(path, this));

    ExternalTool *tool = nullptr;
    // Add diff to local
    if (tool = ExternalTool::create(file, diff, repo, true, this)) {
      Q_ASSERT(tool->kind() == ExternalTool::Diff);
      diffToLocalTools.append(tool);
      connect(tool, &ExternalTool::error, [this](ExternalTool::Error error) {
        if (error != ExternalTool::BashNotFound)
          return;

        QString title = tr("Bash Not Found");
        QString text = tr("Bash was not found on your PATH.");
        QMessageBox msg(QMessageBox::Warning, title, text, QMessageBox::Ok,
                        this);
        msg.setInformativeText(
            tr("Bash is required to execute external tools."));
        msg.exec();
      });
    }

    // Add diff or merge tool.
    if (tool = ExternalTool::create(file, diff, repo, false, this)) {
      switch (tool->kind()) {
        case ExternalTool::Diff:
          diffTools.append(tool);
          break;

        case ExternalTool::Merge:
          mergeTools.append(tool);
          break;

        case ExternalTool::Show: // fall through
        case ExternalTool::Edit:
          Q_ASSERT(false);
          break;
      }

      connect(tool, &ExternalTool::error, [this](ExternalTool::Error error) {
        if (error != ExternalTool::BashNotFound)
          return;

        QString title = tr("Bash Not Found");
        QString text = tr("Bash was not found on your PATH.");
        QMessageBox msg(QMessageBox::Warning, title, text, QMessageBox::Ok,
                        this);
        msg.setInformativeText(
            tr("Bash is required to execute external tools."));
        msg.exec();
      });
    }
  }

  // Add external tool actions.
  addExternalToolsAction(showTools);
  addExternalToolsAction(editTools);
  addExternalToolsAction(diffTools);
  mDoubleClickAction = addExternalToolsAction(diffToLocalTools);
  addExternalToolsAction(mergeTools);

  if (!isEmpty())
    addSeparator();

  QList<git::Commit> commits = view->commits();
  if (commits.isEmpty()) {
    handleUncommittedChanges(index, files);
  } else {
    handleCommits(commits, files);
  }

  addStopTrackingAction();

  // TODO: moving this into handleWorkingDirChanges()? Because
  // Locking committed files does not make sense or?
  // LFS
  if (repo.lfsIsInitialized()) {
    addSeparator();

    bool locked = false;
    foreach (const QString &file, files) {
      if (repo.lfsIsLocked(file)) {
        locked = true;
        break;
      }
    }

    addAction(locked ? tr("Unlock") : tr("Lock"),
              [view, files, locked] { view->lfsSetLocked(files, !locked); });
  }

  // Add single selection actions.
  if (files.size() == 1) {
    addSeparator();

    // Copy File Name
    QDir dir = repo.workdir();
    QString file = files.first();
    QString rel = QDir::toNativeSeparators(file);
    QString abs = QDir::toNativeSeparators(dir.filePath(file));
    QString name = QFileInfo(file).fileName();
    QMenu *copy = addMenu(tr("Copy File Name"));
    if (!name.isEmpty() && name != file) {
      copy->addAction(name,
                      [name] { QApplication::clipboard()->setText(name); });
    }
    copy->addAction(rel, [rel] { QApplication::clipboard()->setText(rel); });
    copy->addAction(abs, [abs] { QApplication::clipboard()->setText(abs); });

    addSeparator();

    // History
    addAction(tr("Filter History"), [view, file] { view->setPathspec(file); });

    // Navigate
    QMenu *navigate = addMenu(tr("Navigate to"));
    QAction *nextAct = navigate->addAction(tr("Next Revision"));
    connect(nextAct, &QAction::triggered, [view, file] {
      if (git::Commit next = view->nextRevision(file)) {
        view->selectCommit(next, file);
      } else {
        warnRevisionNotFound(view, tr("next"), file);
      }
    });

    QAction *prevAct = navigate->addAction(tr("Previous Revision"));
    connect(prevAct, &QAction::triggered, [view, file] {
      if (git::Commit prev = view->previousRevision(file)) {
        view->selectCommit(prev, file);
      } else {
        warnRevisionNotFound(view, tr("previous"), file);
      }
    });

    if (index.isValid() && index.isStaged(file)) {
      addSeparator();

      // Executable
      git_filemode_t mode = index.mode(file);
      bool exe = (mode == GIT_FILEMODE_BLOB_EXECUTABLE);
      QString exeName = exe ? tr("Unset Executable") : tr("Set Executable");
      QAction *exeAct = addAction(exeName, [view, index, file, exe] {
        if (view->isDiscardAllChangesActive())
          return;
        git::Index(index).setMode(file, exe ? GIT_FILEMODE_BLOB
                                            : GIT_FILEMODE_BLOB_EXECUTABLE);
      });

      exeAct->setEnabled(exe || mode == GIT_FILEMODE_BLOB);
    }
  }
}

void FileContextMenu::handleUncommittedChanges(const git::Index &index,
                                               const QStringList &files) {
  git::Diff diff = mView->diff();
  git::Repository repo = mView->repo();
  const auto view = mView;
  if (index.isValid()) {
    // Stage/Unstage
    QAction *stage = addAction(tr("Stage"), [view, index, files] {
      if (view->isDiscardAllChangesActive())
        return;
      git::Index(index).setStaged(files, true);
    });

    QAction *unstage = addAction(tr("Unstage"), [view, index, files] {
      if (view->isDiscardAllChangesActive())
        return;
      git::Index(index).setStaged(files, false);
    });

    int staged = 0;
    int unstaged = 0;
    foreach (const QString &file, files) {
      switch (index.isStaged(file)) {
        case git::Index::Disabled:
          break;

        case git::Index::Unstaged:
          ++unstaged;
          break;

        case git::Index::PartiallyStaged:
          ++staged;
          ++unstaged;
          break;

        case git::Index::Staged:
          ++staged;
          break;

        case git::Index::Conflicted:
          // FIXME: Resolve conflicts?
          break;
      }
    }

    stage->setEnabled(unstaged > 0);
    unstage->setEnabled(staged > 0);

    addSeparator();
  }

  // Discard
  QStringList modified;
  QStringList untracked;
  // copied from DiffTreeModel.cpp
  // handle submodules
  auto s = repo.submodules();
  QList<git::Submodule> submodules;
  QStringList filePatches;
  for (auto trackedPatch : files) {
    bool is_submodule = false;
    for (auto submodule : s) {
      if (submodule.path() == trackedPatch) {
        is_submodule = true;
        submodules.append(submodule);
        break;
      }
    }
    if (!is_submodule)
      filePatches.append(trackedPatch);
  }

  // handle files not submodules
  const git::WorkingTreeStatusSnapshot status = mView->workingTreeStatus();
  foreach (const QString &file, filePatches) {
    if (diff.isValid())
      handlePath(repo, file, diff, modified, untracked);
    else {
      const int classified = modified.size() + untracked.size();
      if (status.isValid())
        handleStatusPath(repo, file, status, modified, untracked);
      if (classified == modified.size() + untracked.size())
        handleIndexPath(repo, file, index, modified, untracked);
    }
  }

  QAction *discard =
      addAction(tr("Discard Changes"), [view, modified, submodules] {
        QMessageBox *dialog =
            new QMessageBox(QMessageBox::Warning, tr("Discard Changes?"),
                            tr("Are you sure you want to discard changes in "
                               "the selected files?"),
                            QMessageBox::Cancel, view);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setInformativeText(tr("This action cannot be undone."));
        QString detailedText = modified.join('\n');
        for (const auto &s : submodules)
          detailedText += s.path() + " " + tr("(Submodule)") + "\n";
        dialog->setDetailedText(detailedText);

        // Expand the Show Details
        foreach (QAbstractButton *button, dialog->buttons()) {
          if (dialog->buttonRole(button) == QMessageBox::ActionRole) {
            button->click(); // click it to expand the text
            break;
          }
        }

        QString text = tr("Discard Changes");
        QPushButton *discard = dialog->addButton(text, QMessageBox::AcceptRole);
        discard->setObjectName("DiscardButton");
        connect(discard, &QPushButton::clicked, [view, modified, submodules] {
          if (view->isDiscardAllChangesActive())
            return;
          git::Repository repo = view->repo();
          int strategy = GIT_CHECKOUT_FORCE;
          if (modified.count() &&
              !repo.checkout(git::Commit(), nullptr, modified, strategy)) {
            QString text = tr("%1 files").arg(modified.size());
            LogEntry *parent = view->addLogEntry(text, tr("Discard"));
            view->error(parent, tr("discard"), text);
          }
          view->updateSubmodules(submodules, true, false, true);

          if (submodules.isEmpty())
            view->refresh();
        });

        dialog->open();
      });
  discard->setEnabled(!modified.isEmpty() || submodules.count());

  QAction *remove = addAction(tr("Remove Untracked Files"),
                              [view, untracked] { view->clean(untracked); });
  remove->setObjectName("RemoveAction");
  remove->setEnabled(!untracked.isEmpty());

  // Ignore untracked paths.
  QAction *ignore = addAction(tr("Ignore"));
  ignore->setObjectName("IgnoreAction");
  connect(ignore, &QAction::triggered, this, &FileContextMenu::ignoreFile);
  bool ignoreEnabled = true;
  for (const QString &root : mIgnoreRoots) {
    if (!QFileInfo(repo.workdir().filePath(root)).isDir() &&
        modified.contains(root)) {
      ignoreEnabled = false;
      break;
    }
  }
  ignore->setEnabled(ignoreEnabled);
}

void FileContextMenu::addStopTrackingAction() {
  if (!mWorkingTreeContext)
    return;

  const git::Repository repo = mView->repo();
  bool hasTrackedPaths = false;
  for (const QString &path : repo.index().pathsUnder(mIgnoreRoots)) {
    if (!repo.lookupSubmodule(path).isValid()) {
      hasTrackedPaths = true;
      break;
    }
  }
  if (!hasTrackedPaths)
    return;

  QAction *stopTracking = addAction(tr("Stop Tracking and Ignore..."));
  stopTracking->setObjectName("StopTrackingAction");
  connect(stopTracking, &QAction::triggered, this,
          [view = mView, roots = mIgnoreRoots] { view->stopTracking(roots); });
}

void FileContextMenu::handleCommits(const QList<git::Commit> &commits,
                                    const QStringList &files) {
  // because this might not live anymore
  // when the lambdas are handled
  const auto view = mView;
  const git::Commit commit = commits.first();
  const QStringList exportPaths = mIgnoreRoots;
  git::Repository repo = view->repo();

  // Checkout
  QAction *checkout = addAction(tr("Checkout"), [view, commit, files] {
    if (view->isDiscardAllChangesActive())
      return;
    view->checkout(commit, files);
    view->setViewMode(RepoView::DoubleTree);
  });

  // Checkout to ...
  QAction *checkoutTo = addAction(
      tr("Save Selected Version as ..."), [view, commit, exportPaths] {
        QFileDialog d(view);
        d.setFileMode(QFileDialog::FileMode::Directory);
        d.setOption(QFileDialog::ShowDirsOnly);
        d.setWindowTitle(tr("Select new file directory"));
        if (d.exec() && !d.selectedFiles().isEmpty()) {
          const auto folder = d.selectedFiles().first();
          const auto save =
              view->addLogEntry(tr("Saving files"),
                                tr("Saving files of selected version to disk"));
          for (const auto &file : exportPaths) {
            const auto saveFile =
                view->addLogEntry(tr("Save file ") + file, "Save file", save);
            if (!FileContextMenu::exportPath(commit, folder, file,
                                             exportPaths.size() > 1))
              view->error(saveFile, tr("save file"), file,
                          tr("Unable to export selected version."));
          }
          QTimer::singleShot(
              0, view, [view] { view->setViewMode(RepoView::DoubleTree); });
        }
      });

  QAction *open = addAction(tr("Open this version"), [view, commit,
                                                      exportPaths] {
    if (exportPaths.size() != 1)
      return;

    QString folder = QDir::tempPath();
    const QString file = exportPaths.first();
    auto filename = file.split("/").last();

    auto logentry =
        view->addLogEntry(tr("Opening file"), tr("Open ") + filename);

    if (FileContextMenu::exportPath(commit, folder, file))
      QDesktopServices::openUrl(QUrl::fromLocalFile(
          QFileInfo(folder + "/" + filename).absoluteFilePath()));
    else
      view->error(logentry, tr("open file"), filename, tr("Blob is invalid."));
  });

  // should show a dialog to select an application
  // Don't forgett to uncomment "openWith->setEnabled(!isBare);" below
  //	QAction *openWith = addAction(tr("Open this Version with ..."),
  //[this,
  // view, files] { 	  QString folder = QDir::tempPath(); const auto&
  // file = files.first(); 	  auto filename = file.split("/").last();

  //	  auto logentry = view->addLogEntry(tr("Opening file with ..."),
  // tr("Open ") + filename);

  //	  if (FileContextMenu::exportPath(commit, folder, file))
  //		QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(folder
  //+
  //"/"
  //+ filename).absoluteFilePath())); 	  else
  // view->error(logentry, tr("open file"), filename, tr("Blob is
  // invalid."));
  //	});

  auto isBare = view->repo().isBare();
  bool canExport = false;
  bool canOpen = exportPaths.size() == 1;
  for (const QString &file : exportPaths) {
    git::Blob blob;
    git::Tree tree;
    const auto kind = findTreeEntry(commit.tree(), file, blob, tree);
    canExport |= kind == TreeEntryKind::Blob || kind == TreeEntryKind::Tree;
    canOpen &= kind == TreeEntryKind::Blob;
  }
  checkout->setEnabled(!isBare);
  checkout->setToolTip(!isBare ? ""
                               : tr("Unable to checkout bare repositories"));
  checkoutTo->setEnabled(!isBare && canExport);
  checkoutTo->setToolTip(!isBare ? ""
                                 : tr("Unable to checkout bare repositories"));
  open->setEnabled(!isBare && canOpen);
  open->setToolTip(!isBare ? ""
                           : tr("Unable to open files from bare repository"));
  // openWith->setEnabled(!isBare && blob.isValid());

  /* disable checkout if the file is already
   * in the current working directory */
  foreach (const QString &file, files) {
    if (commit.tree().id(file) == repo.workdirId(file)) {
      checkout->setEnabled(false);
      checkout->setToolTip(
          tr("The file is already in the current working directory"));
      break;
    }
  }
}

void FileContextMenu::ignoreFile() {
  if (!mFiles.count())
    return;

  auto d = new IgnoreDialog(mIgnoreRoots.join('\n'), parentWidget());
  d->setAttribute(Qt::WA_DeleteOnClose);

  auto *view = mView;
  connect(d, &QDialog::accepted, [d, view]() {
    auto ignore = d->ignoreText();
    if (!ignore.isEmpty())
      view->ignore(ignore);
  });

  d->open();
}

bool FileContextMenu::exportPath(const git::Commit &commit,
                                 const QString &folder, const QString &path,
                                 bool preservePath) {
  if (!commit.isValid() || folder.isEmpty())
    return false;

  git::Blob blob;
  git::Tree tree;
  const auto kind = findTreeEntry(commit.tree(), path, blob, tree);
  if (kind == TreeEntryKind::Invalid)
    return false;

  const QStringList components = pathComponents(path);
  const QString relativePath = kind == TreeEntryKind::Tree || preservePath
                                   ? components.join('/')
                                   : components.last();
  const QString destination = QDir(folder).filePath(relativePath);
  if (kind == TreeEntryKind::Blob)
    return writeBlob(blob, destination);
  return writeTree(tree, destination);
}

QAction *
FileContextMenu::addExternalToolsAction(const QList<ExternalTool *> &tools) {
  if (tools.isEmpty())
    return nullptr;

  // Add action.
  QAction *action = addAction(tools.first()->name(), [this, tools] {
    foreach (ExternalTool *tool, tools) {
      if (tool->start())
        return;

      QString kind;
      switch (tool->kind()) {
        case ExternalTool::Show:
          return;

        case ExternalTool::Edit:
          kind = tr("edit");
          break;

        case ExternalTool::Diff:
          kind = tr("diff");
          break;

        case ExternalTool::Merge:
          kind = tr("merge");
          break;
      }

      QString title = tr("External Tool Not Found");
      QString text = tr("Failed to execute external %1 tool.");
      QMessageBox::warning(this, title, text.arg(kind), QMessageBox::Ok);
      SettingsDialog::openSharedInstance(SettingsDialog::Tools);
    }
  });

  // Disable if any tools are invalid.
  foreach (ExternalTool *tool, tools) {
    if (!tool->isValid()) {
      action->setEnabled(false);
      break;
    }
  }
  return action->isEnabled() ? action : nullptr;
}

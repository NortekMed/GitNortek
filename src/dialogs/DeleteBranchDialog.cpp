//
//          Copyright (c) 2017, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "DeleteBranchDialog.h"
#include "git/Branch.h"
#include "git/Config.h"
#include "git/Remote.h"
#include "log/LogEntry.h"
#include "ui/RemoteCallbacks.h"
#include "ui/RepoView.h"
#include <QCheckBox>
#include <QFutureWatcher>
#include <QPushButton>
#include <QtConcurrent>

namespace {

const QString kBranchMergeFmt = "branch.%1.merge";

} // namespace

DeleteBranchDialog::DeleteBranchDialog(const git::Branch &branch,
                                       QWidget *parent)
    : QMessageBox(parent) {
  if (branch.isRemoteBranch()) {
    QString name = branch.name();
    git::Remote remote = branch.remote();
    setWindowTitle(tr("Delete Remote Branch?"));
    setText(
        tr("Are you sure you want to delete remote branch '%1'?").arg(name));
    setStandardButtons(QMessageBox::Cancel);

    QPushButton *remove = addButton(tr("Delete"), QMessageBox::AcceptRole);
    remove->setEnabled(remote.isValid());
    connect(remove, &QPushButton::clicked, [this, branch, name, remote] {
      RepoView *view = RepoView::parentView(this);
      git::Repository repo = view->repo();
      QString remoteName = remote.name();
      QString branchName = name.mid(remoteName.size() + 1);
      QString text = tr("delete '%1' from '%2'").arg(branchName, remoteName);
      LogEntry *entry = view->addLogEntry(text, tr("Push"));
      QFutureWatcher<git::Result> *watcher =
          new QFutureWatcher<git::Result>(view);
      RemoteCallbacks *callbacks =
          new RemoteCallbacks(RemoteCallbacks::Send, entry, remote.url(),
                              remoteName, watcher, repo);

      entry->setBusy(true);
      QStringList refspecs(QString(":refs/heads/%1").arg(branchName));
      git::Result (git::Remote::*push)(
          git::Remote::Callbacks *, const QStringList &) = &git::Remote::push;
      watcher->setFuture(QtConcurrent::run(push, remote, callbacks, refspecs));

      connect(watcher, &QFutureWatcher<git::Result>::finished, watcher,
              [branch, entry, watcher, callbacks, remoteName] {
                entry->setBusy(false);
                git::Result result = watcher->result();
                if (callbacks->isCanceled()) {
                  entry->addEntry(LogEntry::Error, tr("Push canceled."));
                } else if (!result) {
                  QString err = result.errorString();
                  QString fmt = tr("Unable to push to %1 - %2");
                  entry->addEntry(LogEntry::Error, fmt.arg(remoteName, err));
                } else if (!callbacks->wasRejected() &&
                           !git::Branch(branch).remove()) {
                  entry->addEntry(
                      LogEntry::Error,
                      tr("Unable to update the local remote-tracking branch."));
                }

                watcher->deleteLater();
              });
    });

    remove->setFocus();
    return;
  }

  QString text = tr("Are you sure you want to delete local branch '%1'?");
  setWindowTitle(tr("Delete Branch?"));
  setText(text.arg(branch.name()));
  setStandardButtons(QMessageBox::Cancel);

  git::Branch upstream = branch.upstream();
  if (upstream.isValid()) {
    QString text = tr("Also delete the upstream branch from its remote");
    setCheckBox(new QCheckBox(text, this));
  }

  QPushButton *remove = addButton(tr("Delete"), QMessageBox::AcceptRole);
  const bool checkedOut = branch.isCheckedOut();
  if (checkedOut) {
    setInformativeText(tr(
        "The branch is checked out in another worktree. Switch that worktree "
        "to another branch before deleting this one."));
    setDefaultButton(QMessageBox::Cancel);
    remove->setEnabled(false);
    button(QMessageBox::Cancel)->setFocus();
  }

  connect(remove, &QPushButton::clicked, [this, branch, upstream] {
    RepoView *view = RepoView::parentView(this);
    const QString name = branch.name();
    LogEntry *entry = view->addLogEntry(name, tr("Delete Branch"));

    if (!git::Branch(branch).remove()) {
      const QString reason =
          git::Branch(branch).isCheckedOut()
              ? tr("The branch is checked out in another worktree.")
              : tr("The branch may not be fully merged or could not be "
                   "deleted.");
      view->error(entry, tr("delete branch"), name, reason);
      return;
    }

    if (upstream.isValid() && checkBox()->isChecked()) {
      git::Repository repo = view->repo();

      QString name = upstream.name().section('/', 1);
      QString key = kBranchMergeFmt.arg(branch.name());
      QString upstreamName = repo.gitConfig().value<QString>(key);

      git::Remote remote = upstream.remote();
      QString remoteName = remote.name();
      QString text = tr("delete '%1' from '%2'").arg(name, remoteName);
      LogEntry *entry = view->addLogEntry(text, tr("Push"));
      QFutureWatcher<git::Result> *watcher =
          new QFutureWatcher<git::Result>(view);
      RemoteCallbacks *callbacks =
          new RemoteCallbacks(RemoteCallbacks::Send, entry, remote.url(),
                              remoteName, watcher, repo);

      entry->setBusy(true);
      QStringList refspecs(QString(":%1").arg(upstreamName));
      git::Result (git::Remote::*push)(
          git::Remote::Callbacks *, const QStringList &) = &git::Remote::push;
      watcher->setFuture(QtConcurrent::run(push, remote, callbacks, refspecs));

      connect(watcher, &QFutureWatcher<git::Result>::finished, watcher,
              [entry, watcher, callbacks, remoteName] {
                entry->setBusy(false);
                git::Result result = watcher->result();
                if (callbacks->isCanceled()) {
                  entry->addEntry(LogEntry::Error, tr("Push canceled."));
                } else if (!result) {
                  QString err = result.errorString();
                  QString fmt = tr("Unable to push to %1 - %2");
                  entry->addEntry(LogEntry::Error, fmt.arg(remoteName, err));
                }

                watcher->deleteLater();
              });
    }
  });

  if (!checkedOut)
    remove->setFocus();

  if (!checkedOut && !branch.isMerged()) {
    setInformativeText(tr("The branch is not fully merged. Deleting "
                          "it may cause some commits to be lost."));
    setDefaultButton(QMessageBox::Cancel);
  }
}

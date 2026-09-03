// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Michael WERLE
//

#include "RenameBranchDialog.h"
#include "git/Branch.h"
#include "git/Remote.h"
#include "log/LogEntry.h"
#include "ui/ExpandButton.h"
#include "ui/RemoteCallbacks.h"
#include "ui/ReferenceList.h"
#include "ui/RepoView.h"
#include "util/WaitCursor.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent>

RenameBranchDialog::RenameBranchDialog(const git::Repository &repo,
                                       const git::Branch &branch,
                                       QWidget *parent)
    : QDialog(parent) {
  Q_ASSERT(branch.isValid() && branch.isBranch());
  setAttribute(Qt::WA_DeleteOnClose);

  bool remoteBranch = branch.isRemoteBranch();
  git::Remote remote = remoteBranch ? branch.remote() : git::Remote();
  QString remoteName = remote.name();
  QString branchName =
      remoteBranch
          ? (remote.isValid() ? branch.name().mid(remoteName.size() + 1)
                              : branch.name().section('/', 1))
          : branch.name();

  mName = new QLineEdit(branchName, this);
  mName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  mName->setMinimumWidth(QFontMetrics(mName->font()).averageCharWidth() * 40);

  QFormLayout *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->addRow(tr("Name:"), mName);

  QDialogButtonBox *buttons = new QDialogButtonBox(this);
  buttons->addButton(QDialogButtonBox::Cancel);
  QPushButton *rename =
      buttons->addButton(tr("Rename Branch"), QDialogButtonBox::AcceptRole);
  rename->setEnabled(false);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(buttons);

  // Update button when name text changes.
  connect(
      mName, &QLineEdit::textChanged,
      [repo, rename, remoteBranch, remote, remoteName](const QString &text) {
        QString name =
            remoteBranch ? QString("%1/%2").arg(remoteName, text) : text;
        git_branch_t type = remoteBranch ? GIT_BRANCH_REMOTE : GIT_BRANCH_LOCAL;
        rename->setEnabled((!remoteBranch || remote.isValid()) &&
                           git::Branch::isNameValid(text) &&
                           !repo.lookupBranch(name, type).isValid());
      });

  // Perform the rename when the button is clicked
  connect(
      rename, &QPushButton::clicked,
      [this, branch, remoteBranch, remote, remoteName, branchName] {
        QString newName = mName->text();
        if (!remoteBranch) {
          git::Branch(branch).rename(newName);
          return;
        }

        RepoView *view = RepoView::parentView(this);
        git::Repository repo = view->repo();
        QString text = tr("rename '%1' to '%2' on '%3'")
                           .arg(branchName, newName, remoteName);
        LogEntry *entry = view->addLogEntry(text, tr("Push"));
        QFutureWatcher<git::Result> *createWatcher =
            new QFutureWatcher<git::Result>(view);
        RemoteCallbacks *createCallbacks =
            new RemoteCallbacks(RemoteCallbacks::Send, entry, remote.url(),
                                remoteName, createWatcher, repo);

        entry->setBusy(true);
        QString createRefspec =
            QString("%1:refs/heads/%2").arg(branch.qualifiedName(), newName);
        QStringList createRefspecs(createRefspec);
        git::Result (git::Remote::*push)(
            git::Remote::Callbacks *, const QStringList &) = &git::Remote::push;
        WaitCursor::track(createWatcher);
        createWatcher->setFuture(
            QtConcurrent::run(push, remote, createCallbacks, createRefspecs));

        connect(
            createWatcher, &QFutureWatcher<git::Result>::finished,
            createWatcher,
            [branch, branchName, newName, remote, remoteName, repo, view, entry,
             createWatcher, createCallbacks, push] {
              git::Result result = createWatcher->result();
              if (createCallbacks->isCanceled()) {
                entry->setBusy(false);
                entry->addEntry(LogEntry::Error, tr("Push canceled."));
              } else if (!result) {
                entry->setBusy(false);
                QString fmt = tr("Unable to push to %1 - %2");
                entry->addEntry(LogEntry::Error,
                                fmt.arg(remoteName, result.errorString()));
              } else if (createCallbacks->wasRejected()) {
                entry->setBusy(false);
              } else {
                createCallbacks->storeDeferredCredentials();
                QFutureWatcher<git::Result> *deleteWatcher =
                    new QFutureWatcher<git::Result>(view);
                RemoteCallbacks *deleteCallbacks = new RemoteCallbacks(
                    RemoteCallbacks::Send, entry, remote.url(), remoteName,
                    deleteWatcher, repo);
                QStringList deleteRefspecs(
                    QString(":refs/heads/%1").arg(branchName));
                WaitCursor::track(deleteWatcher);
                deleteWatcher->setFuture(QtConcurrent::run(
                    push, remote, deleteCallbacks, deleteRefspecs));

                connect(
                    deleteWatcher, &QFutureWatcher<git::Result>::finished,
                    deleteWatcher,
                    [branch, branchName, newName, remoteName, entry,
                     deleteWatcher, deleteCallbacks] {
                      entry->setBusy(false);
                      git::Result result = deleteWatcher->result();
                      bool deleted = !deleteCallbacks->isCanceled() && result &&
                                     !deleteCallbacks->wasRejected();
                      if (deleteCallbacks->isCanceled()) {
                        entry->addEntry(LogEntry::Error, tr("Push canceled."));
                      } else if (!result) {
                        QString fmt = tr("Unable to push to %1 - %2");
                        entry->addEntry(
                            LogEntry::Error,
                            fmt.arg(remoteName, result.errorString()));
                      }

                      if (deleted) {
                        deleteCallbacks->storeDeferredCredentials();
                        git::Branch(branch).remove();
                      } else {
                        QString fmt =
                            tr("Remote branch '%1/%2' was created, but '%1/%3' "
                               "could not be deleted.");
                        entry->addEntry(
                            LogEntry::Warning,
                            fmt.arg(remoteName, newName, branchName));
                      }

                      deleteWatcher->deleteLater();
                    });
              }

              createWatcher->deleteLater();
            });
      });
}

QString RenameBranchDialog::name() const { return mName->text(); }

//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef PUBLISHREPOSITORYDIALOG_H
#define PUBLISHREPOSITORYDIALOG_H

#include "git/Repository.h"
#include "host/Account.h"
#include <QDialog>

class QCloseEvent;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;

class PublishRepositoryDialog : public QDialog {
  Q_OBJECT

public:
  explicit PublishRepositoryDialog(const git::Repository &repo,
                                   QWidget *parent = nullptr);

  Account *account() const;
  QString namespaceId() const;
  QString repositoryName() const;
  QString remoteName() const;
  QString description() const;
  bool isPublic() const;
  Repository::Protocol protocol() const;

  void setBusy(bool busy);
  void showError(const QString &error);
  void complete();
  void reject() override;

protected:
  void closeEvent(QCloseEvent *event) override;

signals:
  void publishRequested();
  void addRemoteRequested();

private:
  void populateAccounts();
  void refreshNamespaces();
  void updateButtons();
  bool validate(QString *error) const;

  git::Repository mRepo;
  QComboBox *mAccounts;
  QComboBox *mNamespaces;
  QLineEdit *mRepositoryName;
  QLineEdit *mRemoteName;
  QPlainTextEdit *mDescription;
  QComboBox *mVisibility;
  QComboBox *mProtocol;
  QLabel *mNamespaceStatus;
  QLabel *mVisibilityStatus;
  QLabel *mError;
  QDialogButtonBox *mButtons;
  QPushButton *mAddAccount;
  QPushButton *mAddRemote;
  QPushButton *mPublish;
  bool mLoadingNamespaces = false;
  bool mBusy = false;
  quint64 mNamespaceRequest = 0;
};

#endif

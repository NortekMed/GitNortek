//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef REMOTECONFIGDIALOG_H
#define REMOTECONFIGDIALOG_H

#include "git/Remote.h"
#include "host/Account.h"
#include <QDialog>

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

class RemoteConfigDialog : public QDialog {
  Q_OBJECT

public:
  RemoteConfigDialog(const git::Repository &repo,
                     const git::Remote &remote = git::Remote(),
                     const QString &name = QString(),
                     QWidget *parent = nullptr);

  QString name() const;
  QString fetchUrl() const;
  QString pushUrl() const;
  QStringList fetchRefspecs() const;
  QStringList pushRefspecs() const;
  int service() const { return mService; }
  Repository::Protocol protocol() const;

  void setCreatedRepositoryUrls(const QString &httpsUrl, const QString &sshUrl,
                                Repository::Protocol protocol);

public slots:
  void accept() override;

signals:
  void createRepositoryRequested(int service, int protocol);

private:
  void setService(int service);
  void updateUrlExamples();
  void updateAccountMenu();
  void setRepositoryUrls(const QString &httpsUrl, const QString &sshUrl);
  QString exampleUrl(bool push) const;
  void updateSaveButton();
  QString defaultFetchRefspec(const QString &name) const;

  git::Repository mRepo;
  git::Remote mRemote;
  bool mEditing = false;
  bool mFetchRefspecEdited = false;
  QString mGeneratedFetchRefspec;
  int mService = -1;
  bool mHasRepositoryUrls = false;
  bool mFetchUrlEdited = false;
  bool mPushUrlEdited = false;
  QString mHttpsUrl;
  QString mSshUrl;
  QString mGeneratedFetchUrl;
  QString mGeneratedPushUrl;
  QButtonGroup *mServiceButtons = nullptr;
  QComboBox *mProtocol = nullptr;
  QLabel *mFetchUrlExample = nullptr;
  QLabel *mPushUrlExample = nullptr;
  QLabel *mRepositoryStatus = nullptr;
  QToolButton *mAccountMenu = nullptr;
  QLineEdit *mName;
  QLineEdit *mFetchUrl;
  QLineEdit *mPushUrl;
  QPlainTextEdit *mFetchRefspecs;
  QPlainTextEdit *mPushRefspecs;
  QPushButton *mSave;
};

#endif

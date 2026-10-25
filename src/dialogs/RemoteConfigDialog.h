//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef REMOTECONFIGDIALOG_H
#define REMOTECONFIGDIALOG_H

#include "git/Remote.h"
#include <QDialog>

class QLineEdit;
class QPlainTextEdit;
class QPushButton;

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

public slots:
  void accept() override;

private:
  void updateSaveButton();
  QString defaultFetchRefspec(const QString &name) const;

  git::Repository mRepo;
  git::Remote mRemote;
  bool mEditing = false;
  bool mFetchRefspecEdited = false;
  QString mGeneratedFetchRefspec;
  QLineEdit *mName;
  QLineEdit *mFetchUrl;
  QLineEdit *mPushUrl;
  QPlainTextEdit *mFetchRefspecs;
  QPlainTextEdit *mPushRefspecs;
  QPushButton *mSave;
};

#endif

//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "RemoteConfigDialog.h"
#include "git/Result.h"
#include "ui/ExpandButton.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {

QStringList lines(const QPlainTextEdit *edit) {
  QStringList result;
  for (const QString &line : edit->toPlainText().split('\n')) {
    QString value = line.trimmed();
    if (!value.isEmpty())
      result.append(value);
  }
  return result;
}

} // namespace

RemoteConfigDialog::RemoteConfigDialog(const git::Repository &repo,
                                       const git::Remote &remote,
                                       const QString &name, QWidget *parent)
    : QDialog(parent), mRepo(repo), mRemote(remote),
      mEditing(remote.isValid()) {
  setAttribute(Qt::WA_DeleteOnClose);
  setWindowTitle(mEditing ? tr("Configure Remote") : tr("Add Remote"));

  mName = new QLineEdit(mEditing ? remote.name() : name, this);
  mName->setObjectName(QStringLiteral("RemoteName"));
  mName->setPlaceholderText(tr("e.g. origin"));
  mFetchUrl = new QLineEdit(mEditing ? remote.url() : QString(), this);
  mFetchUrl->setObjectName(QStringLiteral("RemoteFetchUrl"));
  mFetchUrl->setPlaceholderText(
      tr("e.g. https://github.com/owner/repository.git"));
  mPushUrl = new QLineEdit(mEditing ? remote.pushUrl() : QString(), this);
  mPushUrl->setObjectName(QStringLiteral("RemotePushUrl"));
  mPushUrl->setPlaceholderText(tr("e.g. git@github.com:owner/repository.git"));

  auto *pushUrlHint =
      new QLabel(tr("Optional; leave blank to use the fetch URL."), this);
  pushUrlHint->setObjectName(QStringLiteral("RemotePushUrlHint"));
  pushUrlHint->setWordWrap(true);

  auto *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  form->addRow(tr("Name:"), mName);
  form->addRow(tr("Fetch URL:"), mFetchUrl);
  form->addRow(tr("Push URL:"), mPushUrl);
  form->addRow(QString(), pushUrlHint);

  auto *expand = new ExpandButton(this);
  expand->setObjectName(QStringLiteral("RemoteRefspecsToggle"));
  expand->setText(tr("Refspecs"));
  expand->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  auto *advanced = new QWidget(this);
  advanced->setObjectName(QStringLiteral("RemoteAdvancedSettings"));
  advanced->setVisible(false);

  mFetchRefspecs = new QPlainTextEdit(advanced);
  mFetchRefspecs->setObjectName(QStringLiteral("RemoteFetchRefspecs"));
  mFetchRefspecs->setTabChangesFocus(true);
  mFetchRefspecs->setPlaceholderText(
      tr("One fetch refspec per line, for example:\n"
         "+refs/heads/*:refs/remotes/origin/*"));
  mPushRefspecs = new QPlainTextEdit(advanced);
  mPushRefspecs->setObjectName(QStringLiteral("RemotePushRefspecs"));
  mPushRefspecs->setTabChangesFocus(true);
  mPushRefspecs->setPlaceholderText(
      tr("One push refspec per line, for example:\n"
         "refs/heads/main:refs/heads/main"));

  auto *advancedForm = new QFormLayout(advanced);
  advancedForm->setContentsMargins(0, 0, 0, 0);
  advancedForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  advancedForm->addRow(tr("Fetch refspecs:"), mFetchRefspecs);
  advancedForm->addRow(tr("Push refspecs:"), mPushRefspecs);
  connect(expand, &QToolButton::toggled, advanced, &QWidget::setVisible);

  if (mEditing) {
    const QStringList fetchRefspecs = remote.fetchRefspecs();
    const QStringList pushRefspecs = remote.pushRefspecs();
    mFetchRefspecs->setPlainText(fetchRefspecs.join('\n'));
    mPushRefspecs->setPlainText(pushRefspecs.join('\n'));
    expand->setChecked(!fetchRefspecs.isEmpty() || !pushRefspecs.isEmpty());
  } else {
    mGeneratedFetchRefspec = defaultFetchRefspec(name);
    mFetchRefspecs->setPlainText(mGeneratedFetchRefspec);
    connect(mName, &QLineEdit::textChanged, this, [this] {
      if (!mFetchRefspecEdited) {
        mGeneratedFetchRefspec = defaultFetchRefspec(mName->text());
        QSignalBlocker blocker(mFetchRefspecs);
        mFetchRefspecs->setPlainText(mGeneratedFetchRefspec);
      }
      updateSaveButton();
    });
    connect(mFetchRefspecs, &QPlainTextEdit::textChanged, this, [this] {
      mFetchRefspecEdited =
          mFetchRefspecs->toPlainText() != mGeneratedFetchRefspec;
    });
  }

  auto *buttons = new QDialogButtonBox(this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  buttons->addButton(QDialogButtonBox::Cancel);
  mSave = buttons->addButton(mEditing ? tr("Save Remote") : tr("Add Remote"),
                             QDialogButtonBox::AcceptRole);
  mSave->setObjectName(QStringLiteral("SaveRemote"));
  connect(mSave, &QPushButton::clicked, this, &RemoteConfigDialog::accept);

  auto *layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(expand);
  layout->addWidget(advanced);
  layout->addWidget(buttons);

  connect(mFetchUrl, &QLineEdit::textChanged, this,
          &RemoteConfigDialog::updateSaveButton);
  connect(mName, &QLineEdit::textChanged, this,
          &RemoteConfigDialog::updateSaveButton);
  updateSaveButton();
}

QString RemoteConfigDialog::name() const { return mName->text().trimmed(); }

QString RemoteConfigDialog::fetchUrl() const {
  return mFetchUrl->text().trimmed();
}

QString RemoteConfigDialog::pushUrl() const {
  return mPushUrl->text().trimmed();
}

QStringList RemoteConfigDialog::fetchRefspecs() const {
  return lines(mFetchRefspecs);
}

QStringList RemoteConfigDialog::pushRefspecs() const {
  return lines(mPushRefspecs);
}

void RemoteConfigDialog::accept() {
  if (name().isEmpty() || fetchUrl().isEmpty())
    return;

  git::Result result;
  if (mEditing) {
    result = mRemote.configure(name(), fetchUrl(), pushUrl(), fetchRefspecs(),
                               pushRefspecs());
  } else {
    mRemote = mRepo.addRemote(name(), fetchUrl(), &result);
    if (mRemote) {
      result = mRemote.configure(name(), fetchUrl(), pushUrl(), fetchRefspecs(),
                                 pushRefspecs());
      if (result.error()) {
        mRepo.deleteRemote(name());
        mRemote = git::Remote();
      }
    }
  }

  if (result.error()) {
    QMessageBox::warning(
        this, tr("Unable to Save Remote"),
        result.errorString(tr("The remote configuration could not be saved.")));
    return;
  }

  QDialog::accept();
}

void RemoteConfigDialog::updateSaveButton() {
  mSave->setEnabled(!name().isEmpty() && !fetchUrl().isEmpty());
}

QString
RemoteConfigDialog::defaultFetchRefspec(const QString &remoteName) const {
  QString name = remoteName.trimmed();
  if (name.isEmpty())
    name = QStringLiteral("origin");
  return QStringLiteral("+refs/heads/*:refs/remotes/%1/*").arg(name);
}

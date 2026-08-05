//
//          Copyright (c) 2026 NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "ModifySubmoduleDialog.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

void setSearchFieldHeight(QLineEdit *input) { input->setFixedHeight(24); }

} // namespace

ModifySubmoduleDialog::ModifySubmoduleDialog(const git::Submodule &submodule,
                                             QWidget *parent)
    : QDialog(parent) {
  setWindowTitle(tr("Modify Submodule"));

  mName = new QLineEdit(submodule.name(), this);
  setSearchFieldHeight(mName);
  connect(mName, &QLineEdit::textChanged, this, &ModifySubmoduleDialog::update);

  mPath = new QLineEdit(submodule.path(), this);
  setSearchFieldHeight(mPath);
  connect(mPath, &QLineEdit::textChanged, this, &ModifySubmoduleDialog::update);

  mUrl = new QLineEdit(submodule.url(), this);
  setSearchFieldHeight(mUrl);
  connect(mUrl, &QLineEdit::textChanged, this, &ModifySubmoduleDialog::update);

  mBranch = new QLineEdit(submodule.branch(), this);
  setSearchFieldHeight(mBranch);

  QLabel *hint = new QLabel(
      tr("Name is the submodule config section. Path is the folder location in "
         "the repository. Changing the path moves the folder when it exists."),
      this);
  hint->setWordWrap(true);

  QDialogButtonBox *buttons = new QDialogButtonBox(this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  buttons->addButton(QDialogButtonBox::Cancel);
  mSave = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);

  QFormLayout *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
  form->setVerticalSpacing(20);
  form->addRow(tr("Name:"), mName);
  form->addRow(tr("Path:"), mPath);
  form->addRow(tr("URL:"), mUrl);
  form->addRow(tr("Branch:"), mBranch);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setSpacing(12);
  layout->addLayout(form);
  layout->addWidget(hint);
  layout->addWidget(buttons);

  update();
  resize(sizeHint().width() * 3, sizeHint().height());
}

QString ModifySubmoduleDialog::name() const { return mName->text(); }

QString ModifySubmoduleDialog::path() const { return mPath->text(); }

QString ModifySubmoduleDialog::url() const { return mUrl->text(); }

QString ModifySubmoduleDialog::branch() const { return mBranch->text(); }

void ModifySubmoduleDialog::update() {
  mSave->setEnabled(!name().isEmpty() && !path().isEmpty() && !url().isEmpty());
}

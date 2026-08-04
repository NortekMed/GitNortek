//
//          Copyright (c) 2026 NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "AddSubmoduleDialog.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

void setSearchFieldHeight(QLineEdit *input) {
  input->setFixedHeight(24);
}

} // namespace

AddSubmoduleDialog::AddSubmoduleDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Add Submodule"));

  mUrl = new QLineEdit(this);
  setSearchFieldHeight(mUrl);
  connect(mUrl, &QLineEdit::textChanged, this, &AddSubmoduleDialog::update);

  mPath = new QLineEdit(this);
  setSearchFieldHeight(mPath);
  connect(mPath, &QLineEdit::textChanged, this, &AddSubmoduleDialog::update);

  mBranch = new QLineEdit(this);
  setSearchFieldHeight(mBranch);

  QLabel *hint = new QLabel(tr("Only submodules with a branch configured can "
                               "be checked for updates."),
                            this);
  hint->setWordWrap(true);

  QDialogButtonBox *buttons = new QDialogButtonBox(this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  buttons->addButton(QDialogButtonBox::Cancel);
  mAdd = buttons->addButton(tr("Add Submodule"), QDialogButtonBox::AcceptRole);

  QFormLayout *form = new QFormLayout;
  form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
  form->setVerticalSpacing(20);
  form->addRow(tr("URL:"), mUrl);
  form->addRow(tr("Path:"), mPath);
  form->addRow(tr("Branch:"), mBranch);

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setSpacing(12);
  layout->addLayout(form);
  layout->addWidget(hint);
  layout->addWidget(buttons);

  update();
}

QString AddSubmoduleDialog::url() const { return mUrl->text(); }

QString AddSubmoduleDialog::path() const { return mPath->text(); }

QString AddSubmoduleDialog::branch() const { return mBranch->text(); }

void AddSubmoduleDialog::update() {
  mAdd->setEnabled(!url().isEmpty() && !path().isEmpty());
}

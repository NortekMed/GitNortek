#include "StopTrackingDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QVBoxLayout>

StopTrackingDialog::StopTrackingDialog(const git::WorkingTreeUntrackPlan &plan,
                                       QWidget *parent)
    : QDialog(parent), mPlan(plan) {
  setWindowTitle(tr("Stop Tracking and Ignore"));

  auto *summary = new QLabel(this);
  summary->setWordWrap(true);
  summary->setText(
      tr("The selected tracked paths will be removed from the Git index and "
         "kept on disk unless deletion is selected. The next commit will "
         "remove them from the repository."));

  auto *details = new QTextEdit(this);
  details->setReadOnly(true);
  const int deletableTrackedCount =
      mPlan.trackedPaths.size() - mPlan.preservedTrackedPaths.size();
  QStringList preservedPaths = mPlan.preservedTrackedPaths;
  preservedPaths.append(mPlan.preservedUntrackedPaths);
  QStringList deletableUntrackedPaths = mPlan.untrackedPaths;
  for (const QString &path : mPlan.preservedUntrackedPaths)
    deletableUntrackedPaths.removeAll(path);
  details->setPlainText(
      tr("Ignore rule(s):\n%1\n\nTracked paths to remove from the index "
         "(%2):\n%3\n\nGenerated ignore file(s) retained on disk "
         "(%4):\n%5\n\nUntracked files eligible for deletion (%6):\n%7\n\n"
         "Ignored files will remain untouched (%8).\n\nProtected paths "
         "will remain untouched (%9).")
          .arg(mPlan.ignorePatterns.join('\n'))
          .arg(mPlan.trackedPaths.size())
          .arg(mPlan.trackedPaths.join('\n'))
          .arg(preservedPaths.size())
          .arg(preservedPaths.join('\n'))
          .arg(deletableUntrackedPaths.size())
          .arg(deletableUntrackedPaths.join('\n'))
          .arg(mPlan.ignoredPaths.size())
          .arg(mPlan.protectedPaths.size()));
  details->setMinimumSize(480, 260);

  mDeleteTracked =
      new QCheckBox(tr("Also delete the %1 tracked file(s) from disk")
                        .arg(deletableTrackedCount),
                    this);
  mDeleteTracked->setObjectName("DeleteTrackedCheckBox");
  if (deletableTrackedCount == 0) {
    mDeleteTracked->setEnabled(false);
    mDeleteTracked->setToolTip(
        tr("There are no selected tracked files that can be deleted."));
  } else if (!mPlan.modifiedTrackedPaths.isEmpty()) {
    mDeleteTracked->setEnabled(false);
    mDeleteTracked->setToolTip(
        tr("Deletion is disabled because selected tracked paths have "
           "uncommitted changes."));
  }

  mDeleteUntracked = new QCheckBox(
      tr("Also delete the %1 untracked file(s) under the selection")
          .arg(deletableUntrackedPaths.size()),
      this);
  mDeleteUntracked->setObjectName("DeleteUntrackedCheckBox");
  mDeleteUntracked->setEnabled(!deletableUntrackedPaths.isEmpty());
  mDeleteUntracked->setToolTip(
      tr("Ignored files and protected nested repositories are not deleted."));

  mButtonBox = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  auto *confirm = mButtonBox->button(QDialogButtonBox::Ok);
  confirm->setObjectName("StopTrackingButton");

  auto *layout = new QVBoxLayout(this);
  layout->addWidget(summary);
  layout->addWidget(details);
  layout->addWidget(mDeleteTracked);
  layout->addWidget(mDeleteUntracked);
  layout->addWidget(mButtonBox);

  connect(mDeleteTracked, &QCheckBox::toggled, this,
          &StopTrackingDialog::updateConfirmationText);
  connect(mDeleteUntracked, &QCheckBox::toggled, this,
          &StopTrackingDialog::updateConfirmationText);
  connect(mButtonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(mButtonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

  updateConfirmationText();
}

bool StopTrackingDialog::deleteTracked() const {
  return mDeleteTracked->isChecked();
}

bool StopTrackingDialog::deleteUntracked() const {
  return mDeleteUntracked->isChecked();
}

void StopTrackingDialog::updateConfirmationText() {
  QString text = tr("Stop Tracking and Ignore");
  if (deleteTracked() || deleteUntracked())
    text = tr("Stop Tracking, Ignore, and Delete");
  mButtonBox->button(QDialogButtonBox::Ok)->setText(text);
}

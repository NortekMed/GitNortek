#ifndef STOPTRACKINGDIALOG_H
#define STOPTRACKINGDIALOG_H

#include "git/WorkingTreeUntrack.h"
#include <QDialog>

class QCheckBox;
class QDialogButtonBox;
class QTextEdit;

class StopTrackingDialog : public QDialog {
  Q_OBJECT

public:
  StopTrackingDialog(const git::WorkingTreeUntrackPlan &plan,
                     QWidget *parent = nullptr);

  bool deleteTracked() const;
  bool deleteUntracked() const;

private:
  void updateConfirmationText();

  const git::WorkingTreeUntrackPlan mPlan;
  QCheckBox *mDeleteTracked{nullptr};
  QCheckBox *mDeleteUntracked{nullptr};
  QDialogButtonBox *mButtonBox{nullptr};
};

#endif // STOPTRACKINGDIALOG_H

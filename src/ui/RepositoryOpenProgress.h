//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. See LICENSE.md.
//

#ifndef REPOSITORYOPENPROGRESS_H
#define REPOSITORYOPENPROGRESS_H

#include <QPointer>
#include <QWidget>

class QTimer;

class RepositoryOpenProgress : public QWidget {
  Q_OBJECT

public:
  explicit RepositoryOpenProgress(QWidget *target);

  void start();
  void finish();

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  void showDelayed();

  QPointer<QWidget> mTarget;
  QTimer *mShowTimer;
  QTimer *mFallbackTimer;
  bool mActive = false;
};

#endif

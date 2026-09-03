//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. See LICENSE.md.
//

#include "RepositoryOpenProgress.h"
#include "util/PerformanceTrace.h"
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int kShowDelayMs = 250;
constexpr int kFallbackTimeoutMs = 60000;

} // namespace

RepositoryOpenProgress::RepositoryOpenProgress(QWidget *target)
    : QWidget(target->window()), mTarget(target) {
  setObjectName("RepositoryOpenProgress");
  setAttribute(Qt::WA_TransparentForMouseEvents);
  hide();

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 12, 12, 12);
  layout->setAlignment(Qt::AlignCenter);

  QFrame *panel = new QFrame(this);
  panel->setFrameShape(QFrame::NoFrame);
  panel->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
  QVBoxLayout *panelLayout = new QVBoxLayout(panel);
  panelLayout->setContentsMargins(12, 8, 12, 8);
  panelLayout->setSpacing(4);

  QLabel *label = new QLabel(tr("Loading repository..."), panel);
  label->setWordWrap(true);
  label->setAlignment(Qt::AlignCenter);
  panelLayout->addWidget(label);

  QProgressBar *progress = new QProgressBar(panel);
  progress->setObjectName("RepositoryOpenProgressBar");
  progress->setRange(0, 0);
  progress->setFixedWidth(150);
  panelLayout->addWidget(progress, 0, Qt::AlignHCenter);
  layout->addWidget(panel, 0, Qt::AlignCenter);

  mShowTimer = new QTimer(this);
  mShowTimer->setObjectName("RepositoryOpenProgressShowTimer");
  mShowTimer->setSingleShot(true);
  mShowTimer->setInterval(kShowDelayMs);
  connect(mShowTimer, &QTimer::timeout, this,
          &RepositoryOpenProgress::showDelayed);

  mFallbackTimer = new QTimer(this);
  mFallbackTimer->setObjectName("RepositoryOpenProgressFallbackTimer");
  mFallbackTimer->setSingleShot(true);
  mFallbackTimer->setInterval(kFallbackTimeoutMs);
  connect(mFallbackTimer, &QTimer::timeout, this, [this] {
    if (mActive)
      PerformanceTrace::event("startup", "repository-open-timeout");
    finish();
  });

  mTarget->installEventFilter(this);
  parentWidget()->installEventFilter(this);
  connect(mTarget, &QObject::destroyed, this,
          &RepositoryOpenProgress::deleteLater);
}

void RepositoryOpenProgress::start() {
  if (mActive)
    return;

  mActive = true;
  mCursor = WaitCursor::acquire();
  mShowTimer->start();
  mFallbackTimer->start();
}

void RepositoryOpenProgress::finish() {
  if (!mActive)
    return;

  mActive = false;
  mShowTimer->stop();
  mFallbackTimer->stop();
  hide();
  mCursor.reset();
}

bool RepositoryOpenProgress::eventFilter(QObject *watched, QEvent *event) {
  if (watched == mTarget) {
      switch (event->type()) {
      case QEvent::Resize:
        setGeometry(parentWidget()->rect());
        break;
      case QEvent::Show:
        if (mActive && !mShowTimer->isActive())
          showDelayed();
        break;
      case QEvent::Hide:
        hide();
        break;
      default:
        break;
    }
  } else if (watched == parentWidget() && event->type() == QEvent::Resize &&
             mTarget) {
    setGeometry(parentWidget()->rect());
  }

  return QWidget::eventFilter(watched, event);
}

void RepositoryOpenProgress::showDelayed() {
  if (!mActive || !mTarget || !mTarget->isVisible())
    return;

  setGeometry(parentWidget()->rect());
  show();
  raise();
}

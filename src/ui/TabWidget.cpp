//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "TabWidget.h"
#include "MenuBar.h"
#include "RepositoryTabStrip.h"
#include <QMenu>
#include "app/Application.h"
#include "dialogs/AccountDialog.h"
#include "dialogs/CloneDialog.h"
#include "host/Account.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {

const QString kLinkFmt = "<a href='%1'>%2</a>";
const QString kSupportLink = "https://github.com/NortekMed/GitNortek/issues";

class DefaultWidget : public QFrame {
  Q_OBJECT

public:
  DefaultWidget(QWidget *parent = nullptr) : QFrame(parent) {
    setFrameShape(QFrame::Box);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Base);

    QPushButton *clone =
        addButton(QIcon(":/clone.png"), tr("Clone repository"));
    connect(clone, &QPushButton::clicked, [this] {
      CloneDialog *dialog = new CloneDialog(CloneDialog::Clone, this);
      connect(dialog, &CloneDialog::accepted, [dialog] {
        if (MainWindow *window =
                MainWindow::open(dialog->path(), true,
                                 MainWindow::OpenSource::Other,
                                 dialog->updateSubmodules()))
          window->currentView()->addLogEntry(dialog->message(),
                                             dialog->messageTitle());
      });
      dialog->open();
    });

    QPushButton *open =
        addButton(QIcon(":/open.png"), tr("Open existing repository"));
    connect(open, &QPushButton::clicked, [this] {
      // FIXME: Filter out non-git dirs.
      QFileDialog *dialog =
          new QFileDialog(this, tr("Open Repository"), QDir::homePath());
      dialog->setAttribute(Qt::WA_DeleteOnClose);
      dialog->setFileMode(QFileDialog::Directory);
      dialog->setOption(QFileDialog::ShowDirsOnly);
      connect(dialog, &QFileDialog::fileSelected,
              [](const QString &path) { MainWindow::open(path); });
      dialog->open();
    });

    QPushButton *init =
        addButton(QIcon(":/new.png"), tr("Initialize new repository"));
    connect(init, &QPushButton::clicked, [this] {
      CloneDialog *dialog = new CloneDialog(CloneDialog::Init, this);
      connect(dialog, &CloneDialog::accepted, [dialog] {
        if (MainWindow *window =
                MainWindow::open(dialog->path(), true,
                                 MainWindow::OpenSource::Other,
                                 dialog->updateSubmodules()))
          window->currentView()->addLogEntry(dialog->message(),
                                             dialog->messageTitle());
      });
      dialog->open();
    });

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->addWidget(clone);
    layout->addWidget(open);
    layout->addWidget(init);
    layout->addWidget(addSeparator());

    for (int i = 0; i < Account::NUM_KINDS; ++i) {
      Account::Kind kind = static_cast<Account::Kind>(i);
      QString text = tr("Add %1 account").arg(Account::name(kind));
      QPushButton *account = addButton(Account::icon(kind), text);
      connect(account, &QPushButton::clicked, [this, kind] {
        AccountDialog *dialog = new AccountDialog(nullptr, this);
        dialog->setKind(kind);
        dialog->open();
      });

      layout->addWidget(account);
    }

    layout->addWidget(addSeparator());
    layout->addWidget(addLink(tr("Report an issue"), kSupportLink));
  }

private:
  QPushButton *addButton(const QIcon &icon, const QString &text) {
    QPushButton *button = new QPushButton(icon, text, this);
    button->setStyleSheet("color: palette(bright-text); text-align: left");
    button->setIconSize(QSize(32, 32));
    button->setFlat(true);

    QFont font = button->font();
    font.setPointSize(font.pointSize() + 10);
    button->setFont(font);

    return button;
  }

  QLabel *addLink(const QString &text, const QString &link = QString()) {
    QLabel *label = new QLabel(kLinkFmt.arg(link, text), this);
    label->setOpenExternalLinks(true);

    QFont font = label->font();
    font.setPointSize(font.pointSize() + 3);
    label->setFont(font);

    return label;
  }

  QFrame *addSeparator() {
    QFrame *separator = new QFrame(this);
    separator->setStyleSheet("border: 2px solid palette(dark)");
    separator->setFrameShape(QFrame::HLine);
    return separator;
  }
};

} // namespace

TabWidget::TabWidget(QWidget *parent) : QWidget(parent) {
  mTabStrip = new RepositoryTabStrip(this);
  mStack = new QStackedWidget(this);

  QWidget *defaultPage = new QWidget(mStack);
  auto *defaultWidget = new DefaultWidget(defaultPage);
  auto *defaultLayout = new QVBoxLayout(defaultPage);
  defaultLayout->addStretch();
  defaultLayout->addWidget(defaultWidget, 0, Qt::AlignHCenter);
  defaultLayout->addStretch();
  mStack->addWidget(defaultPage);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(mTabStrip);
  layout->addWidget(mStack, 1);

  connect(mTabStrip, &RepositoryTabStrip::currentChanged, this,
          &TabWidget::setCurrentIndex);
  connect(mTabStrip, &RepositoryTabStrip::closeRequested, this,
          QOverload<int>::of(&TabWidget::closeTab));
  connect(mTabStrip, &RepositoryTabStrip::contextMenuRequested, this,
          [this](int index, const QPoint &position) {
            QMenu menu;
            QAction *left = menu.addAction(tr("Close tabs on left"));
            QAction *right = menu.addAction(tr("Close tabs on right"));
            QAction *others = menu.addAction(tr("Close others tabs"));
            QAction *all = menu.addAction(tr("Close all tabs"));
            left->setEnabled(index > 0);
            right->setEnabled(index + 1 < count());
            others->setEnabled(count() > 1);
            all->setEnabled(count() > 0);
            QAction *selected = menu.exec(position);
            if (selected == left)
              closeTabs(index, TabCloseScope::Left);
            else if (selected == right)
              closeTabs(index, TabCloseScope::Right);
            else if (selected == others)
              closeTabs(index, TabCloseScope::Others);
            else if (selected == all)
              closeTabs(index, TabCloseScope::All);
          });
  connect(mTabStrip, &RepositoryTabStrip::tabMoved, this,
          &TabWidget::moveTabFromStrip);
}

TabWidget::~TabWidget() { mDestroying = true; }

int TabWidget::addTab(QWidget *widget, const QIcon &icon, const QString &text) {
  if (!widget)
    return -1;
  if (int index = indexOf(widget); index >= 0)
    return index;

  mWidgets.append(widget);
  mStack->addWidget(widget);
  int index = mTabStrip->addTab(icon, text);
  connect(widget, &QObject::destroyed, this, &TabWidget::removeTab);

  MenuBar::instance(this)->updateWindow();
  emit tabInserted();
  return index;
}

int TabWidget::count() const { return mWidgets.size(); }

QWidget *TabWidget::widget(int index) const {
  return (index >= 0 && index < count()) ? mWidgets.at(index) : nullptr;
}

int TabWidget::indexOf(QWidget *widget) const {
  return mWidgets.indexOf(widget);
}

int TabWidget::currentIndex() const { return mCurrentIndex; }

QWidget *TabWidget::currentWidget() const { return widget(mCurrentIndex); }

void TabWidget::setCurrentIndex(int index) {
  if (index < 0 || index >= count() || index == mCurrentIndex)
    return;

  mCurrentIndex = index;
  mStack->setCurrentWidget(widget(index));
  mTabStrip->setCurrentIndex(index);
  emit currentChanged(index);
}

void TabWidget::setCurrentWidget(QWidget *widget) {
  setCurrentIndex(indexOf(widget));
}

QIcon TabWidget::tabIcon(int index) const { return mTabStrip->tabIcon(index); }

void TabWidget::setTabIcon(int index, const QIcon &icon) {
  mTabStrip->setTabIcon(index, icon);
}

bool TabWidget::tabBusy(int index) const { return mTabStrip->tabBusy(index); }

void TabWidget::setTabBusy(int index, bool busy) {
  mTabStrip->setTabBusy(index, busy);
}

QString TabWidget::tabText(int index) const {
  return mTabStrip->tabText(index);
}

void TabWidget::setTabText(int index, const QString &text) {
  mTabStrip->setTabText(index, text);
}

QString TabWidget::tabToolTip(int index) const {
  return mTabStrip->tabToolTip(index);
}

void TabWidget::setTabToolTip(int index, const QString &toolTip) {
  mTabStrip->setTabToolTip(index, toolTip);
}

bool TabWidget::closeTab(int index) { return closeTab(widget(index)); }

bool TabWidget::closeTab(QWidget *widget) {
  if (!widget || indexOf(widget) < 0)
    return false;

  emit tabAboutToBeRemoved();
  if (widget->close()) {
    detachTab(widget);
    return true;
  }

  emit tabRemovalCancelled();
  return false;
}

bool TabWidget::closeTabs(int anchorIndex, TabCloseScope scope) {
  if (count() == 0 || (scope != TabCloseScope::All &&
                       (anchorIndex < 0 || anchorIndex >= count())))
    return false;

  QVector<QWidget *> targets;
  for (int index = count() - 1; index >= 0; --index) {
    bool target = scope == TabCloseScope::All ||
                  (scope == TabCloseScope::Left && index < anchorIndex) ||
                  (scope == TabCloseScope::Right && index > anchorIndex) ||
                  (scope == TabCloseScope::Others && index != anchorIndex);
    if (target)
      targets.append(widget(index));
  }

  for (QWidget *target : targets) {
    if (!closeTab(target))
      return false;
  }
  return true;
}

void TabWidget::detachTab(QWidget *page) {
  int index = mWidgets.indexOf(page);
  if (index < 0)
    return;

  int oldCurrent = mCurrentIndex;
  bool removingCurrent = index == oldCurrent;
  mWidgets.removeAt(index);
  mStack->removeWidget(page);
  page->hide();
  mTabStrip->removeTab(index);

  if (mWidgets.isEmpty()) {
    mCurrentIndex = -1;
    mStack->setCurrentIndex(0);
  } else if (index < oldCurrent) {
    mCurrentIndex = oldCurrent - 1;
    mStack->setCurrentWidget(widget(mCurrentIndex));
  } else if (removingCurrent) {
    mCurrentIndex = qMin(index, count() - 1);
    mStack->setCurrentWidget(widget(mCurrentIndex));
  }

  if (removingCurrent || index < oldCurrent || mCurrentIndex < 0)
    emit currentChanged(mCurrentIndex);
  MenuBar::instance(this)->updateWindow();
  emit tabRemoved();
}

void TabWidget::moveTab(int from, int to) { mTabStrip->moveTab(from, to); }

void TabWidget::moveTabFromStrip(int from, int to) {
  if (from < 0 || from >= count() || to < 0 || to >= count() || from == to)
    return;

  QWidget *currentPage = currentWidget();
  QWidget *page = mWidgets.takeAt(from);
  mWidgets.insert(to, page);
  mStack->removeWidget(page);
  mStack->insertWidget(to + 1, page);
  mStack->setCurrentWidget(currentPage);

  int current = indexOf(currentPage);
  if (current != mCurrentIndex) {
    mCurrentIndex = current;
    emit currentChanged(current);
  }
}

void TabWidget::removeTab(QObject *object) {
  if (mDestroying)
    return;

  int index = mWidgets.indexOf(static_cast<QWidget *>(object));
  if (index < 0)
    return;

  int oldCurrent = mCurrentIndex;
  bool removingCurrent = index == oldCurrent;
  mWidgets.removeAt(index);
  mStack->removeWidget(static_cast<QWidget *>(object));
  mTabStrip->removeTab(index);

  if (mWidgets.isEmpty()) {
    mCurrentIndex = -1;
    mStack->setCurrentIndex(0);
  } else if (index < oldCurrent) {
    mCurrentIndex = oldCurrent - 1;
    mStack->setCurrentWidget(widget(mCurrentIndex));
  } else if (removingCurrent) {
    mCurrentIndex = qMin(index, count() - 1);
    mStack->setCurrentWidget(widget(mCurrentIndex));
  }

  if (removingCurrent || index < oldCurrent || mCurrentIndex < 0)
    emit currentChanged(mCurrentIndex);

  MenuBar::instance(this)->updateWindow();
  emit tabRemoved();
}

#include "TabWidget.moc"

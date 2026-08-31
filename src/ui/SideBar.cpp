//
//          Copyright (c) 2018, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "SideBar.h"
#include "Footer.h"
#include "MainWindow.h"
#include "RepositoryNavigator.h"
#include "RepoView.h"
#include "TabWidget.h"
#include "dialogs/CloneDialog.h"
#include <QFileDialog>
#include <QMenu>
#include <QVBoxLayout>

namespace {

const QString kStyleSheet = "QTreeView {"
                            "  background: palette(window);"
                            "  border: none"
                            "}"
                            "Footer {"
                            "  border-left: none;"
                            "  border-right: none"
                            "}";

} // namespace

SideBar::SideBar(TabWidget *tabs, MainWindow *window, QWidget *parent)
    : QWidget(parent) {
  setStyleSheet(kStyleSheet);

  RepositoryNavigator *navigator = new RepositoryNavigator(this);
  auto bindNavigator = [tabs, navigator](int index) {
    navigator->setRepoView(qobject_cast<RepoView *>(tabs->widget(index)));
  };
  connect(tabs, &TabWidget::currentChanged, navigator, bindNavigator);
  connect(tabs, QOverload<>::of(&TabWidget::tabRemoved), navigator,
          [tabs, navigator] {
            navigator->setRepoView(
                qobject_cast<RepoView *>(tabs->currentWidget()));
          });
  connect(navigator, &RepositoryNavigator::openRepositoryRequested, window,
          [window](const QString &path) { window->addTab(path); });
  connect(navigator, &RepositoryNavigator::selectRepositoryRequested, window,
          &MainWindow::selectTab);
  bindNavigator(tabs->currentIndex());

  Footer *footer = new Footer(this);
  footer->setObjectName("RepositoryFooter");
  footer->setMinusVisible(false);

  QMenu *plusMenu = new QMenu(this);
  footer->setPlusMenu(plusMenu);

  QAction *clone = plusMenu->addAction(tr("Clone Repository"));
  connect(clone, &QAction::triggered, [this] {
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

  QAction *open = plusMenu->addAction(tr("Open Existing Repository"));
  connect(open, &QAction::triggered, [this] {
    QFileDialog *dialog =
        new QFileDialog(this, tr("Open Repository"), QDir::homePath());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setFileMode(QFileDialog::Directory);
    dialog->setOption(QFileDialog::ShowDirsOnly);
    connect(dialog, &QFileDialog::fileSelected,
            [](const QString &path) { MainWindow::open(path); });
    dialog->open();
  });

  QAction *init = plusMenu->addAction(tr("Initialize New Repository"));
  connect(init, &QAction::triggered, [this] {
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
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  layout->addWidget(navigator);
  layout->addWidget(footer);

  footer->setMinimumWidth(footer->sizeHint().width());
}

QSize SideBar::sizeHint() const { return QSize(280, 0); }

QSize SideBar::minimumSizeHint() const { return QSize(0, 0); }

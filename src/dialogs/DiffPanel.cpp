//
//          Copyright (c) 2017, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "DiffPanel.h"
#include "ConfigDialog.h"
#include "conf/Settings.h"
#include "dialogs/GlobalGitConfigChange.h"
#include "git/Config.h"
#include "ui/MainWindow.h"
#include "ui/RepoView.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSpinBox>
#include <QSignalBlocker>

DiffPanel::DiffPanel(const git::Repository &repo, QWidget *parent)
    : QWidget(parent),
      mConfig(repo ? repo.gitConfig() : git::Config::global()) {
  const bool globalConfig = !repo.isValid();

  // diff context
  QSpinBox *context = new QSpinBox(this);
  context->setObjectName("GlobalGitDiffContext");
  QLabel *contextLabel = new QLabel(tr("lines"), this);
  QHBoxLayout *contextLayout = new QHBoxLayout;
  contextLayout->addWidget(context);
  contextLayout->addWidget(contextLabel);
  contextLayout->addStretch();
  context->setValue(mConfig.value<int>("diff.context", 3));

  auto refreshViews = [] {
    foreach (MainWindow *window, MainWindow::windows()) {
      for (int i = 0; i < window->count(); ++i)
        window->view(i)->refresh();
    }
  };
  if (globalConfig) {
    connect(context, &QSpinBox::editingFinished, this,
            [this, context, refreshViews] {
              const int oldValue = mConfig.value<int>("diff.context", 3);
              const int newValue = context->value();
              if (oldValue == newValue)
                return;

              bool applied = applyGlobalGitConfigChange(
                  this, "diff.context", QString::number(oldValue),
                  QString::number(newValue), [newValue](git::Config &global) {
                    return global.setValue("diff.context", newValue);
                  });
              if (!applied) {
                QSignalBlocker blocker(context);
                context->setValue(oldValue);
                return;
              }
              refreshViews();
            });
  } else {
    auto contextSignal = QOverload<int>::of(&QSpinBox::valueChanged);
    connect(context, contextSignal, this,
            [this, context, refreshViews](int value) {
              if (!mConfig.setValue("diff.context", value)) {
                QSignalBlocker blocker(context);
                context->setValue(mConfig.value<int>("diff.context", 3));
                return;
              }
              refreshViews();
            });
  }

  // encoding

  static std::array encodings{
      "Utf8",  "Utf16",   "Utf16LE", "Utf16BE",
      "Utf32", "Utf32LE", "Utf32BE", "Latin1",
  };

  QComboBox *encoding = new QComboBox(this);
  encoding->setObjectName("GlobalGitEncoding");
  encoding->addItem(tr("System Locale"), -1);
  encoding->insertSeparator(encoding->count());
  for (int i = 0; i < encodings.size(); i++) {
    encoding->addItem(encodings[i], i);
  }
  QString name = mConfig.value<QString>("gui.encoding");
  if (!name.isEmpty())
    encoding->setCurrentIndex(encoding->findText(name));

  auto encodingSignal = QOverload<int>::of(&QComboBox::currentIndexChanged);
  connect(encoding, encodingSignal, this,
          [this, encoding, globalConfig, refreshViews](int index) {
            const QString oldValue = mConfig.value<QString>("gui.encoding");
            const QString newValue = encoding->itemData(index).toInt() < 0
                                         ? QString()
                                         : encoding->itemText(index);
            if (oldValue == newValue)
              return;

            bool applied = true;
            if (globalConfig) {
              applied = applyGlobalGitConfigChange(
                  this, "gui.encoding", oldValue, newValue,
                  [newValue](git::Config &global) {
                    return newValue.isEmpty()
                               ? global.remove("gui.encoding")
                               : global.setValue("gui.encoding", newValue);
                  });
            } else if (newValue.isEmpty()) {
              applied = mConfig.remove("gui.encoding");
            } else {
              applied = mConfig.setValue("gui.encoding", newValue);
            }
            if (!applied) {
              QSignalBlocker blocker(encoding);
              encoding->setCurrentIndex(
                  oldValue.isEmpty() ? 0 : encoding->findText(oldValue));
              return;
            }

            foreach (MainWindow *window, MainWindow::windows()) {
              for (int i = 0; i < window->count(); ++i)
                window->view(i)->refresh();
            }
          });

  // Wrap lines
  QCheckBox *wrapLines = new QCheckBox(tr("Wrap lines"), this);
  wrapLines->setChecked(Settings::instance()->isTextEditorWrapLines());
  connect(wrapLines, &QCheckBox::toggled, [](bool wrap) {
    Settings::instance()->setTextEditorWrapLines(wrap);
  });

  QFormLayout *layout = new QFormLayout(this);
  layout->addRow(tr("Context lines:"), contextLayout);
  layout->addRow(tr("Wrap lines:"), wrapLines);
  layout->addRow(tr("Character Encoding:"), encoding);

  // Remaining settings are strictly global.
  if (qobject_cast<ConfigDialog *>(parent))
    return;

  // ignore whitespace
  // The ignore whitespace option is global because it's
  // not a config setting. It's a flag (-w) to git diff.
  QCheckBox *ignoreWs = new QCheckBox(tr("Ignore Whitespace (-w)"), this);
  ignoreWs->setChecked(Settings::instance()->isWhitespaceIgnored());
  connect(ignoreWs, &QCheckBox::toggled, [](bool checked) {
    Settings::instance()->setWhitespaceIgnored(checked);
  });

  // auto collapse
  Settings *settings = Settings::instance();
  QCheckBox *collapseAdded = new QCheckBox(tr("Added files"), this);
  collapseAdded->setChecked(
      settings->value(Setting::Id::AutoCollapseAddedFiles).toBool());
  connect(collapseAdded, &QCheckBox::toggled, [settings](bool checked) {
    settings->setValue(Setting::Id::AutoCollapseAddedFiles, checked);
  });

  QCheckBox *collapseDeleted = new QCheckBox(tr("Deleted files"), this);
  collapseDeleted->setChecked(
      settings->value(Setting::Id::AutoCollapseDeletedFiles).toBool());
  connect(collapseDeleted, &QCheckBox::toggled, [settings](bool checked) {
    settings->setValue(Setting::Id::AutoCollapseDeletedFiles, checked);
  });

  layout->addRow(tr("Whitespace:"), ignoreWs);
  layout->addRow(tr("Auto Collapse:"), collapseAdded);
  layout->addRow(QString(), collapseDeleted);
}

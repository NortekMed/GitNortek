//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef LOCALWORKSPACEDIALOG_H
#define LOCALWORKSPACEDIALOG_H

#include "conf/LocalWorkspace.h"
#include <QDialog>
#include <optional>

class QCheckBox;
class QColor;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;

class LocalWorkspaceDialog : public QDialog {
  Q_OBJECT

public:
  explicit LocalWorkspaceDialog(QWidget *parent);
  explicit LocalWorkspaceDialog(
      std::optional<LocalWorkspace> workspace = std::nullopt,
      QWidget *parent = nullptr);

  LocalWorkspace workspace() const;

private:
  void browseRepository();
  void browseSyncDirectory();
  void chooseColor();
  void updateColorButton();
  void updateState();

  LocalWorkspace mWorkspace;
  QLineEdit *mName;
  QComboBox *mIcon;
  QPushButton *mColor;
  QPlainTextEdit *mDescription;
  QListWidget *mRepositories;
  QPushButton *mRemoveRepository;
  QCheckBox *mSync;
  QLineEdit *mSyncDirectory;
  QPushButton *mBrowseSyncDirectory;
  QPushButton *mSave;
};

#endif

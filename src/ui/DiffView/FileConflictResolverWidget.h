#ifndef FILECONFLICTRESOLVERWIDGET_H
#define FILECONFLICTRESOLVERWIDGET_H

#include "git/Index.h"
#include "git/Patch.h"
#include <QWidget>

class QLabel;
class QToolButton;

class FileConflictResolverWidget : public QWidget {
  Q_OBJECT

public:
  explicit FileConflictResolverWidget(const git::Patch &patch,
                                      const git::Index::Conflict &conflict,
                                      QWidget *parent = nullptr);

  git::Patch::ConflictResolution resolution() const;

signals:
  void resolutionChanged();

private:
  QWidget *createSide(const QString &title, const git::Id &id,
                      git_filemode_t mode,
                      git::Patch::ConflictResolution resolution,
                      const QString &objectName);
  void select(git::Patch::ConflictResolution resolution);
  void updateUi();

  git::Patch mPatch;
  git::Index::Conflict mConflict;
  git::Patch::ConflictResolution mResolution = git::Patch::Unresolved;
  QToolButton *mCurrent = nullptr;
  QToolButton *mIncoming = nullptr;
  QLabel *mResult = nullptr;
};

#endif // FILECONFLICTRESOLVERWIDGET_H

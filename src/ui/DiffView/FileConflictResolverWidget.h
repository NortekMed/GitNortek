#ifndef FILECONFLICTRESOLVERWIDGET_H
#define FILECONFLICTRESOLVERWIDGET_H

#include "git/Index.h"
#include "git/Patch.h"
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QToolButton;

class FileConflictResolverWidget : public QWidget {
  Q_OBJECT

public:
  explicit FileConflictResolverWidget(const git::Patch &patch,
                                      const git::Index::Conflict &conflict,
                                      QWidget *parent = nullptr);

  git::Patch::ConflictResolution resolution() const;
  bool outputDeleted() const { return mOutputKind == DeletedOutput; }
  bool outputUsesBlob() const {
    return mOutputKind != DeletedOutput && mOutputId.isValid();
  }
  git::Id outputId() const { return mOutputId; }
  git_filemode_t outputMode() const { return mOutputMode; }
  bool hasUnsavedOutput() const;
  QByteArray output() const;
  void acceptCurrent();

signals:
  void resolutionChanged();

private:
  QWidget *createSide(const QString &title, const git::Id &id,
                      git_filemode_t mode,
                      git::Patch::ConflictResolution resolution,
                      const QString &objectName);
  void select(git::Patch::ConflictResolution resolution);
  void setOutput(const git::Id &id, git_filemode_t mode);
  void updateUi();

  enum OutputKind { TextOutput, BlobOutput, DeletedOutput };

  git::Patch mPatch;
  git::Index::Conflict mConflict;
  git::Patch::ConflictResolution mResolution = git::Patch::Unresolved;
  QToolButton *mCurrent = nullptr;
  QToolButton *mIncoming = nullptr;
  QPlainTextEdit *mOutputText = nullptr;
  QLabel *mOutputInfo = nullptr;
  git::Id mOutputId;
  git_filemode_t mOutputMode = GIT_FILEMODE_UNREADABLE;
  OutputKind mOutputKind = TextOutput;
  bool mUpdatingOutput = false;
  OutputKind mInitialOutputKind = TextOutput;
  git_filemode_t mInitialOutputMode = GIT_FILEMODE_UNREADABLE;
  git::Id mInitialOutputId;
  QByteArray mInitialOutput;
};

#endif // FILECONFLICTRESOLVERWIDGET_H

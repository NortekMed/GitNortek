//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#ifndef BLAMEEDITOR_H
#define BLAMEEDITOR_H

#include "FindWidget.h"
#include "git/Blame.h"
#include "git/Blob.h"
#include "git/Commit.h"
#include "git/Repository.h"
#include <QFutureWatcher>
#include <QHash>
#include <QPointer>
#include <QSharedPointer>
#include <QWidget>

class BlameMargin;
class TextEditor;

class BlameEditor : public QWidget, public EditorProvider {
  Q_OBJECT

public:
  BlameEditor(const git::Repository &repo = git::Repository(),
              QWidget *parent = nullptr, bool annotationOnly = false);

  void setEditor(TextEditor *editor, bool preserveBlame = false);
  bool isAnnotationOnly() const { return mAnnotationOnly; }

  QString name() const;
  QString path() const;
  QString revision() const;

  TextEditor *editor() const;
  QList<TextEditor *> editors() override;
  void ensureVisible(TextEditor *editor, int pos) override {}

  bool load(const QString &name, const git::Blob &blob, git::Commit commit);

  void setBlameVisible(bool visible);
  bool isBlameVisible() const { return mBlameVisible; }

  void startBlame();
  void cancelBlame();

  void save();
  void clear();

  void find();
  void findNext();
  void findPrevious();

signals:
  void saved();
  void linkActivated(const QString &link);

protected:
  void resizeEvent(QResizeEvent *event) override;

private:
  void adjustLineMarginWidth();
  void editorLinesAdded();
  void editorScrolled();
  void blameFinished();
  void requestVisibleBlame();
  void updateAnnotationGeometry();

  git::Repository mRepo;

  QPointer<TextEditor> mEditor;
  FindWidget *mFind{nullptr};
  BlameMargin *mMargin;
  std::optional<git::Commit> mPendingBlameCommit;

  QString mName;
  QString mRevision;
  git::Commit mBlameCommit;
  bool mBlameVisible{true};
  bool mAnnotationOnly{false};

  QSharedPointer<git::Blame::Callbacks> mCallbacks;
  QFutureWatcher<git::Blame> mBlame;
  QHash<QString, git::Blame> mBlameCache;
  int mBlameGeneration{0};
  int mActiveBlameGeneration{0};
  QString mActiveBlameCacheKey;
  int mLoadedBlameMinLine{0};
  int mLoadedBlameMaxLine{0};
  int mLoadedEditorLineCount{0};
  int mActiveEditorLineCount{0};
  int mActiveBlameMinLine{0};
  int mActiveBlameMaxLine{0};
};

#endif

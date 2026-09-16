#ifndef COMPLETEFILEDIFFWIDGET_H
#define COMPLETEFILEDIFFWIDGET_H

#include "conf/Settings.h"
#include "git/Diff.h"
#include "git/Patch.h"
#include <QHash>
#include <QPair>
#include <QWidget>

class Editor;
class DiffView;
class HunkWidget;
class QEvent;
class TextEditor;
class QToolButton;

class CompleteFileDiffWidget : public QWidget {
  Q_OBJECT

public:
  using Target = QPair<int, int>;

  CompleteFileDiffWidget(const git::Diff &diff, const git::Patch &patch,
                         const QList<HunkWidget *> &hunks,
                         Settings::DiffMode mode, QWidget *parent = nullptr,
                         DiffView *view = nullptr);

  QList<TextEditor *> editors() const;
  bool containsEditor(TextEditor *editor) const;
  void reload();

signals:
  void stageLinesRequested(const QList<Target> &targets, bool staged);

protected:
  bool event(QEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  struct Row {
    QString oldText;
    QString newText;
    int oldLine{-1};
    int newLine{-1};
    Target oldTarget{-1, -1};
    Target newTarget{-1, -1};
    bool deletion{false};
    bool addition{false};
  };

  QList<Row> rows() const;
  void loadEditor(Editor *editor, bool oldSide, const QList<Row> &rows);
  QList<Target> targets(Editor *editor, int start, int end) const;
  void createNavigation();
  void updateModifiedBlocks();
  void updateNavigationButtons();
  void updateLineNumberHighlight();
  void updateNavigationGeometry();
  void navigateModifiedBlock(int direction);

  git::Diff mDiff;
  git::Patch mPatch;
  QList<HunkWidget *> mHunks;
  Settings::DiffMode mMode;
  DiffView *mView{nullptr};
  Editor *mInline{nullptr};
  Editor *mOld{nullptr};
  Editor *mNew{nullptr};
  QWidget *mNavigationSlot{nullptr};
  QWidget *mNavigation{nullptr};
  QToolButton *mPreviousBlock{nullptr};
  QToolButton *mNextBlock{nullptr};
  QList<Row> mRows;
  QHash<Editor *, QList<QList<Target>>> mEditorTargets;
  QList<QPair<int, int>> mModifiedBlocks;
  int mCurrentBlock{-1};
  int mHighlightedLine{-1};
};

#endif // COMPLETEFILEDIFFWIDGET_H

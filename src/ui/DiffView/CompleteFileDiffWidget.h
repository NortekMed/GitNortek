#ifndef COMPLETEFILEDIFFWIDGET_H
#define COMPLETEFILEDIFFWIDGET_H

#include "conf/Settings.h"
#include "git/Diff.h"
#include "git/Patch.h"
#include <QHash>
#include <QPair>
#include <QWidget>

class Editor;
class HunkWidget;
class TextEditor;

class CompleteFileDiffWidget : public QWidget {
  Q_OBJECT

public:
  using Target = QPair<int, int>;

  CompleteFileDiffWidget(const git::Diff &diff, const git::Patch &patch,
                         const QList<HunkWidget *> &hunks,
                         Settings::DiffMode mode, QWidget *parent = nullptr);

  QList<TextEditor *> editors() const;
  bool containsEditor(TextEditor *editor) const;
  void reload();

signals:
  void stageLinesRequested(const QList<Target> &targets, bool staged);

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

  git::Diff mDiff;
  git::Patch mPatch;
  QList<HunkWidget *> mHunks;
  Settings::DiffMode mMode;
  Editor *mInline{nullptr};
  Editor *mOld{nullptr};
  Editor *mNew{nullptr};
  QList<Row> mRows;
  QHash<Editor *, QList<QList<Target>>> mEditorTargets;
};

#endif // COMPLETEFILEDIFFWIDGET_H

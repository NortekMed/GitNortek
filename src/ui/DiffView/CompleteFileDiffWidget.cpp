#include "CompleteFileDiffWidget.h"
#include "Editor.h"
#include "HunkWidget.h"
#include "git/Blob.h"
#include "git/Repository.h"
#include <QFile>
#include <QHBoxLayout>
#include <QRegularExpression>

namespace {

QStringList splitLines(const QByteArray &content, const git::Repository &repo) {
  QString text = repo.decode(content);
  text.replace("\r\n", "\n");
  if (text.endsWith('\n'))
    text.chop(1);
  return text.isEmpty() ? QStringList() : text.split('\n');
}

QString withoutLineEnd(const QByteArray &content,
                       const git::Repository &repo) {
  QString text = repo.decode(content);
  if (text.endsWith('\n'))
    text.chop(1);
  if (text.endsWith('\r'))
    text.chop(1);
  return text;
}

QString edgeTrimmed(const QString &text) {
  static const QRegularExpression leading("^[\\t ]+");
  static const QRegularExpression trailing("[\\t ]+$");
  QString result = text;
  result.remove(leading);
  result.remove(trailing);
  return result;
}

} // namespace

CompleteFileDiffWidget::CompleteFileDiffWidget(
    const git::Diff &diff, const git::Patch &patch,
    const QList<HunkWidget *> &hunks, Settings::DiffMode mode, QWidget *parent)
    : QWidget(parent), mDiff(diff), mPatch(patch), mHunks(hunks), mMode(mode) {
  setObjectName(mode == Settings::DiffMode::Split ? "SplitFileDiff"
                                                  : "InlineFileDiff");
  QHBoxLayout *layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(mode == Settings::DiffMode::Split ? 1 : 0);

  auto createEditor = [this, layout] {
    Editor *editor = new Editor(this);
    editor->setObjectName("CompleteFileEditor");
    editor->setLexer(mPatch.name());
    editor->setCaretStyle(CARETSTYLE_INVISIBLE);
    editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    editor->setStatusDiff(mDiff.isStatusDiff());
    editor->setDiscardSelectedEnabled(false);
    editor->setMarginWidthN(TextEditor::Margin::Staged, 0);
    connect(editor, &TextEditor::stageSelectedSignal, this,
            [this, editor](int start, int end) {
              emit stageLinesRequested(targets(editor, start, end), true);
            });
    connect(editor, &TextEditor::unstageSelectedSignal, this,
            [this, editor](int start, int end) {
              emit stageLinesRequested(targets(editor, start, end), false);
            });
    layout->addWidget(editor, 1);
    return editor;
  };

  if (mode == Settings::DiffMode::Split) {
    mOld = createEditor();
    mNew = createEditor();
  } else {
    mInline = createEditor();
  }
  reload();
}

QList<TextEditor *> CompleteFileDiffWidget::editors() const {
  QList<TextEditor *> result;
  if (mInline)
    result.append(mInline);
  if (mOld)
    result.append(mOld);
  if (mNew)
    result.append(mNew);
  return result;
}

bool CompleteFileDiffWidget::containsEditor(TextEditor *editor) const {
  return editor == mInline || editor == mOld || editor == mNew;
}

void CompleteFileDiffWidget::reload() {
  mRows = rows();
  if (mInline)
    loadEditor(mInline, false, mRows);
  if (mOld)
    loadEditor(mOld, true, mRows);
  if (mNew)
    loadEditor(mNew, false, mRows);
}

QList<CompleteFileDiffWidget::Row> CompleteFileDiffWidget::rows() const {
  const git::Repository repo = mPatch.repo();
  QByteArray oldContent;
  if (git::Blob blob = mPatch.blob(git::Diff::OldFile); blob.isValid())
    oldContent = blob.content();

  QByteArray newContent;
  if (git::Blob blob = mPatch.blob(git::Diff::NewFile); blob.isValid()) {
    newContent = blob.content();
  } else if (mDiff.isStatusDiff()) {
    QFile file(repo.workdir().filePath(mPatch.name()));
    if (file.open(QFile::ReadOnly))
      newContent = file.readAll();
  }

  const QStringList oldLines = splitLines(oldContent, repo);
  const QStringList newLines = splitLines(newContent, repo);
  QList<Row> result;
  int oldCursor = 1;
  int newCursor = 1;

  auto appendUnchanged = [&](int oldEnd, int newEnd) {
    while (oldCursor < oldEnd || newCursor < newEnd) {
      Row row;
      if (oldCursor < oldEnd && oldCursor <= oldLines.size()) {
        row.oldText = oldLines.at(oldCursor - 1);
        row.oldLine = oldCursor++;
      }
      if (newCursor < newEnd && newCursor <= newLines.size()) {
        row.newText = newLines.at(newCursor - 1);
        row.newLine = newCursor++;
      }
      result.append(row);
    }
  };

  for (int hunkIndex = 0; hunkIndex < mPatch.count(); ++hunkIndex) {
    const git_diff_hunk *header = mPatch.header_struct(hunkIndex);
    appendUnchanged(header->old_start, header->new_start);

    QList<Row> pendingDeletions;
    QList<Row> pendingAdditions;
    auto flushChanges = [&] {
      const int count = qMax(pendingDeletions.size(), pendingAdditions.size());
      for (int i = 0; i < count; ++i) {
        Row row;
        if (i < pendingDeletions.size()) {
          const Row old = pendingDeletions.at(i);
          row.oldText = old.oldText;
          row.oldLine = old.oldLine;
          row.oldTarget = old.oldTarget;
          row.deletion = true;
        }
        if (i < pendingAdditions.size()) {
          const Row added = pendingAdditions.at(i);
          row.newText = added.newText;
          row.newLine = added.newLine;
          row.newTarget = added.newTarget;
          row.addition = true;
        }

        const bool ignored = Settings::instance()->isEdgeWhitespaceIgnored() &&
                             row.deletion && row.addition &&
                             edgeTrimmed(row.oldText) ==
                                 edgeTrimmed(row.newText);
        if (ignored) {
          row.deletion = false;
          row.addition = false;
          row.oldTarget = {-1, -1};
          row.newTarget = {-1, -1};
        }
        result.append(row);
      }
      pendingDeletions.clear();
      pendingAdditions.clear();
    };

    int editorLine = 0;
    for (int patchLine = 0; patchLine < mPatch.lineCount(hunkIndex);
         ++patchLine) {
      const char origin = mPatch.lineOrigin(hunkIndex, patchLine);
      if (origin == GIT_DIFF_LINE_CONTEXT_EOFNL ||
          origin == GIT_DIFF_LINE_ADD_EOFNL ||
          origin == GIT_DIFF_LINE_DEL_EOFNL)
        continue;

      const QString text =
          withoutLineEnd(mPatch.lineContent(hunkIndex, patchLine), repo);
      if (origin == GIT_DIFF_LINE_DELETION) {
        Row row;
        row.oldText = text;
        row.oldLine = oldCursor++;
        row.oldTarget = {hunkIndex, editorLine++};
        pendingDeletions.append(row);
      } else if (origin == GIT_DIFF_LINE_ADDITION) {
        Row row;
        row.newText = text;
        row.newLine = newCursor++;
        row.newTarget = {hunkIndex, editorLine++};
        pendingAdditions.append(row);
      } else {
        flushChanges();
        Row row;
        row.oldText = text;
        row.newText = text;
        row.oldLine = oldCursor++;
        row.newLine = newCursor++;
        result.append(row);
        ++editorLine;
      }
    }
    flushChanges();
  }

  appendUnchanged(oldLines.size() + 1, newLines.size() + 1);
  return result;
}

void CompleteFileDiffWidget::loadEditor(Editor *editor, bool oldSide,
                                        const QList<Row> &rows) {
  editor->setUpdatesEnabled(false);
  QStringList content;
  QList<QList<Target>> editorTargets;
  content.reserve(rows.size());
  for (const Row &row : rows) {
    if (mMode == Settings::DiffMode::Inline) {
      if (row.deletion) {
        content.append(row.oldText);
        editorTargets.append({row.oldTarget});
      }
      if (row.addition || !row.deletion) {
        content.append(row.newText);
        editorTargets.append(row.addition ? QList<Target>{row.newTarget}
                                          : QList<Target>());
      }
    } else {
      content.append(oldSide ? row.oldText : row.newText);
      const Target target = oldSide ? row.oldTarget : row.newTarget;
      editorTargets.append(target.first >= 0 ? QList<Target>{target}
                                             : QList<Target>());
    }
  }
  mEditorTargets.insert(editor, editorTargets);

  editor->setReadOnly(false);
  editor->setText(content.join('\n'));
  editor->markerDeleteAll(-1);

  int editorLine = 0;
  for (const Row &row : rows) {
    auto apply = [&](bool deletion, bool addition, int oldLine, int newLine,
                     const Target &target) {
      if (deletion)
        editor->markerAdd(editorLine, TextEditor::Deletion);
      if (addition)
        editor->markerAdd(editorLine, TextEditor::Addition);
      editor->marginSetText(
          editorLine,
          QString("%1 %2")
              .arg(oldLine >= 0 ? QString::number(oldLine) : QString())
              .arg(newLine >= 0 ? QString::number(newLine) : QString()));
      editor->marginSetStyle(editorLine, STYLE_LINENUMBER);
      if (target.first >= 0 && target.first < mHunks.size()) {
        TextEditor *source = mHunks.at(target.first)->editor();
        const int markers = source->markers(target.second);
        if (markers & (1 << TextEditor::StagedMarker))
          editor->markerAdd(editorLine, TextEditor::StagedMarker);
        else
          editor->markerAdd(editorLine, TextEditor::UnstagedMarker);

        const int sourceStart = source->positionFromLine(target.second);
        const int sourceEnd = source->lineEndPosition(target.second);
        const int destinationStart = editor->positionFromLine(editorLine);
        for (int indicator : {TextEditor::WordDeletion,
                              TextEditor::WordAddition}) {
          int position = sourceStart;
          while (position < sourceEnd) {
            if (!source->indicatorValueAt(indicator, position)) {
              ++position;
              continue;
            }

            const int start =
                qMax(sourceStart, source->indicatorStart(indicator, position));
            const int end =
                qMin(sourceEnd, source->indicatorEnd(indicator, position));
            editor->setIndicatorCurrent(indicator);
            editor->indicatorFillRange(destinationStart + start - sourceStart,
                                       end - start);
            position = qMax(position + 1, end);
          }
        }

        const QString annotation = source->annotationText(target.second);
        if (!annotation.isEmpty()) {
          QByteArray styles(annotation.toUtf8().size(), 0);
          source->annotationStyles(target.second, styles.data());
          editor->annotationSetText(editorLine, annotation);
          editor->annotationSetStyles(editorLine, styles);
          editor->annotationSetVisible(ANNOTATION_STANDARD);
        }
      }
      ++editorLine;
    };

    if (mMode == Settings::DiffMode::Inline) {
      if (row.deletion)
        apply(true, false, row.oldLine, -1, row.oldTarget);
      if (row.addition)
        apply(false, true, -1, row.newLine, row.newTarget);
      if (!row.deletion && !row.addition)
        apply(false, false, row.oldLine, row.newLine, {-1, -1});
    } else if (oldSide) {
      apply(row.deletion, false, row.oldLine, -1, row.oldTarget);
    } else {
      apply(false, row.addition, -1, row.newLine, row.newTarget);
    }
  }

  editor->setReadOnly(true);
  const QByteArray marginSample =
      QByteArray::number(qMax(1, qMax(mRows.size(), editor->lineCount()))) +
      " " + QByteArray::number(qMax(1, qMax(mRows.size(), editor->lineCount())));
  editor->setMarginWidthN(TextEditor::LineNumber,
                          editor->textWidth(STYLE_LINENUMBER,
                                            marginSample.constData()) +
                              8);
  editor->setMarginWidthN(TextEditor::Margin::Staged, 0);
  editor->updateGeometry();
  editor->setUpdatesEnabled(true);
}

QList<CompleteFileDiffWidget::Target>
CompleteFileDiffWidget::targets(Editor *editor, int start, int end) const {
  QList<Target> result;
  const QList<QList<Target>> mapping = mEditorTargets.value(editor);
  for (int line = qMax(0, start); line < qMin(end, mapping.size()); ++line) {
    for (const Target &target : mapping.at(line)) {
      if (target.first >= 0 && !result.contains(target))
        result.append(target);
    }
  }
  return result;
}

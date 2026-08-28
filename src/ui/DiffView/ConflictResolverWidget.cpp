#include "ConflictResolverWidget.h"
#include "git/Commit.h"
#include "git/Index.h"
#include "git/Reference.h"
#include "git/Repository.h"
#include <QCheckBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QStringConverter>
#include <functional>

namespace {

constexpr int kContextLines = 3;

class SelectionBubble : public QToolButton {
public:
  explicit SelectionBubble(QWidget *parent = nullptr) : QToolButton(parent) {
    setFixedSize(18, 18);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    refresh();
  }

  void setSelected(bool selected) {
    mSelected = selected;
    refresh();
  }

  void setHovered(bool hovered) {
    mHovered = hovered;
    refresh();
  }

private:
  void refresh() {
    if (!mSelected && !mHovered) {
      setText(QString());
      setStyleSheet("QToolButton { border: 0; background: transparent; }");
      return;
    }

    const bool removing = mSelected && mHovered;
    setText(mHovered ? (removing ? "-" : "+") : QString());
    setStyleSheet(QString("QToolButton { border: 0; border-radius: 9px; color: "
                          "white; font-weight: 700; background: %1; }")
                      .arg(removing ? "#cf222e" : "#2da44e"));
  }

  bool mSelected = false;
  bool mHovered = false;
};

} // namespace

class SourceLineRow : public QWidget {
public:
  SourceLineRow(const QString &text, bool selectable,
                const std::function<void()> &toggle,
                const QString &proposalColor = {}, QWidget *parent = nullptr)
      : QWidget(parent), mSelectable(selectable) {
    setMouseTracking(true);
    setFixedHeight(fontMetrics().height() + 6);
    if (selectable)
      setStyleSheet(QString("background: %1;").arg(proposalColor));

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 1, 6, 1);
    layout->setSpacing(5);

    mBubble = new SelectionBubble(this);
    mBubble->setEnabled(selectable);
    layout->addWidget(mBubble);

    QLabel *label = new QLabel(text, this);
    label->setTextFormat(Qt::PlainText);
    label->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(label, 1);

    if (selectable)
      connect(mBubble, &QToolButton::clicked, toggle);
  }

  void setSelected(bool selected) { mBubble->setSelected(selected); }

protected:
  void enterEvent(QEnterEvent *event) override {
    if (mSelectable)
      mBubble->setHovered(true);
    QWidget::enterEvent(event);
  }

  void leaveEvent(QEvent *event) override {
    if (mSelectable)
      mBubble->setHovered(false);
    QWidget::leaveEvent(event);
  }

private:
  SelectionBubble *mBubble = nullptr;
  bool mSelectable = false;
};

class ConflictSourcePanel : public QWidget {
public:
  ConflictSourcePanel(const QString &title, const QString &objectName,
                      const QString &headerColor, QWidget *parent = nullptr)
      : QWidget(parent), mProposalColor(headerColor) {
    setObjectName(objectName);
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QWidget *header = new QWidget(this);
    header->setStyleSheet(QString("background: %1;").arg(headerColor));
    QHBoxLayout *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 5, 10, 5);
    mMaster = new QCheckBox(title, header);
    mMaster->setObjectName(objectName + "Master");
    mMaster->setTristate(true);
    mMaster->setStyleSheet("font-weight: 700;");
    headerLayout->addWidget(mMaster);
    layout->addWidget(header);

    mScroll = new QScrollArea(this);
    mScroll->setWidgetResizable(true);
    mScroll->setFrameShape(QFrame::NoFrame);
    mContent = new QWidget(mScroll);
    mRows = new QVBoxLayout(mContent);
    mRows->setContentsMargins(0, 0, 0, 0);
    mRows->setSpacing(0);
    mRows->addStretch();
    mScroll->setWidget(mContent);
    layout->addWidget(mScroll, 1);
  }

  QCheckBox *addBlockHeader(const QString &text,
                            const std::function<void()> &toggle,
                            const QString &objectName) {
    QWidget *row = new QWidget(mContent);
    row->setStyleSheet(QString("background: %1;").arg(mProposalColor));
    QHBoxLayout *layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 4, 8, 4);
    QCheckBox *check = new QCheckBox(text, row);
    check->setObjectName(objectName);
    layout->addWidget(check);
    layout->addStretch();
    connect(check, &QCheckBox::clicked, toggle);
    mRows->insertWidget(mRows->count() - 1, row);
    return check;
  }

  SourceLineRow *addLine(const QString &text, bool selectable,
                         const std::function<void()> &toggle = {}) {
    SourceLineRow *row =
        new SourceLineRow(text, selectable, toggle, mProposalColor, mContent);
    mRows->insertWidget(mRows->count() - 1, row);
    return row;
  }

  void addOmittedLines(int count) {
    SourceLineRow *row =
        addLine(ConflictResolverWidget::tr("%n unchanged line(s) omitted",
                                           nullptr, count),
                false);
    row->setObjectName("ConflictSourceOmittedLines");
  }

  QScrollBar *verticalScrollBar() const { return mScroll->verticalScrollBar(); }

  void connectMaster(const std::function<void()> &toggle) {
    connect(mMaster, &QCheckBox::clicked, toggle);
  }

  void setMasterState(Qt::CheckState state) { mMaster->setCheckState(state); }

private:
  QScrollArea *mScroll = nullptr;
  QWidget *mContent = nullptr;
  QVBoxLayout *mRows = nullptr;
  QCheckBox *mMaster = nullptr;
  QString mProposalColor;
};

ConflictResolverWidget::ConflictResolverWidget(const git::Patch &patch,
                                               QWidget *parent)
    : QWidget(parent), mPatch(patch) {
  setObjectName("ConflictResolver");
  setMinimumHeight(560);

  const QList<git::Patch::ConflictBlock> blocks = patch.conflictBlocks();
  const int conflictCount = blocks.size();
  const git::Repository repo = patch.repo();
  QString currentId = repo.head().target().shortId();
  if (currentId.isEmpty())
    currentId = tr("unknown");
  QString incomingId = incomingCommit();

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  QSplitter *vertical = new QSplitter(Qt::Vertical, this);
  QSplitter *sources = new QSplitter(Qt::Horizontal, vertical);
  const QString title = tr("Commit %1 (%2 conflicts)");
  mCurrentPanel = new ConflictSourcePanel(
      tr("Current - %1").arg(title.arg(currentId).arg(conflictCount)),
      "ConflictCurrentPanel", "rgba(45, 164, 78, 0.18)", sources);
  mIncomingPanel = new ConflictSourcePanel(
      tr("Incoming - %1").arg(title.arg(incomingId).arg(conflictCount)),
      "ConflictIncomingPanel", "rgba(9, 105, 218, 0.18)", sources);
  mCurrentPanel->connectMaster([this] { toggleAll(Current); });
  mIncomingPanel->connectMaster([this] { toggleAll(Incoming); });
  sources->addWidget(mCurrentPanel);
  sources->addWidget(mIncomingPanel);
  sources->setSizes({500, 500});

  QWidget *resultPanel = new QWidget(vertical);
  QVBoxLayout *resultLayout = new QVBoxLayout(resultPanel);
  resultLayout->setContentsMargins(0, 0, 0, 0);
  resultLayout->setSpacing(0);
  QLabel *resultHeader = new QLabel(tr("Result - editable"), resultPanel);
  resultHeader->setContentsMargins(10, 7, 10, 7);
  resultHeader->setStyleSheet(
      "font-weight: 700; background: rgba(191, 135, 0, 0.20);");
  resultLayout->addWidget(resultHeader);
  mResult = new QPlainTextEdit(resultPanel);
  mResult->setObjectName("ConflictResult");
  mResult->setLineWrapMode(QPlainTextEdit::NoWrap);
  mResult->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  resultLayout->addWidget(mResult, 1);

  vertical->addWidget(sources);
  vertical->addWidget(resultPanel);
  vertical->setSizes({340, 260});
  layout->addWidget(vertical);

  connect(mCurrentPanel->verticalScrollBar(), &QScrollBar::valueChanged,
          mIncomingPanel->verticalScrollBar(), &QScrollBar::setValue);
  connect(mIncomingPanel->verticalScrollBar(), &QScrollBar::valueChanged,
          mCurrentPanel->verticalScrollBar(), &QScrollBar::setValue);

  const QList<QByteArray> fileLines = patch.conflictFileLines();
  bool hasCrLf = false;
  bool hasBareLf = false;
  for (const QByteArray &line : fileLines) {
    if (line.endsWith("\r\n"))
      hasCrLf = true;
    else if (line.endsWith('\n'))
      hasBareLf = true;
  }
  mCrLf = hasCrLf && !hasBareLf;

  QString resultText;
  int fileLine = 0;
  auto addUnchangedLines = [&](int end) {
    const int count = end - fileLine;
    const int contextCount = 2 * kContextLines;
    auto addLine = [&](int line) {
      const QString text = lineText(fileLines.at(line));
      mCurrentPanel->addLine(text, false);
      mIncomingPanel->addLine(text, false);
    };

    if (count <= contextCount + 1) {
      while (fileLine < end)
        addLine(fileLine++);
      return;
    }

    for (int line = fileLine; line < fileLine + kContextLines; ++line)
      addLine(line);
    mCurrentPanel->addOmittedLines(count - contextCount);
    mIncomingPanel->addOmittedLines(count - contextCount);
    for (int line = end - kContextLines; line < end; ++line)
      addLine(line);
    fileLine = end;
  };

  for (int blockIndex = 0; blockIndex < blocks.size(); ++blockIndex) {
    const git::Patch::ConflictBlock &block = blocks.at(blockIndex);
    const int unchangedStart = fileLine;
    while (fileLine < block.startMarker) {
      resultText.append(resultLine(fileLines.at(fileLine)));
      ++fileLine;
    }
    fileLine = unchangedStart;
    addUnchangedLines(block.startMarker);

    BlockState state;
    state.block = block;
    state.resultStart = resultText.size();
    for (const QByteArray &line : block.ancestorLines)
      resultText.append(resultLine(line));
    state.resultEnd = resultText.size();
    mBlocks.append(state);

    BlockState &stored = mBlocks.last();
    stored.currentCheck = mCurrentPanel->addBlockHeader(
        tr("Select current block"),
        [this, blockIndex] { toggleBlock(blockIndex, Current); },
        QString("ConflictCurrentBlock_%1").arg(blockIndex));
    stored.incomingCheck = mIncomingPanel->addBlockHeader(
        tr("Select incoming block"),
        [this, blockIndex] { toggleBlock(blockIndex, Incoming); },
        QString("ConflictIncomingBlock_%1").arg(blockIndex));

    const int rowCount =
        qMax(block.currentLines.size(), block.incomingLines.size());
    for (int line = 0; line < rowCount; ++line) {
      if (line < block.currentLines.size()) {
        SourceLineRow *row =
            mCurrentPanel->addLine(lineText(block.currentLines.at(line)), true,
                                   [this, blockIndex, line] {
                                     toggleLine(blockIndex, Current, line);
                                   });
        row->setObjectName(
            QString("ConflictCurrentLine_%1_%2").arg(blockIndex).arg(line));
        row->findChild<QToolButton *>()->setObjectName(
            QString("ConflictCurrentBubble_%1_%2").arg(blockIndex).arg(line));
        stored.currentRows.append(row);
      } else {
        mCurrentPanel->addLine(QString(), false);
      }

      if (line < block.incomingLines.size()) {
        SourceLineRow *row =
            mIncomingPanel->addLine(lineText(block.incomingLines.at(line)),
                                    true, [this, blockIndex, line] {
                                      toggleLine(blockIndex, Incoming, line);
                                    });
        row->setObjectName(
            QString("ConflictIncomingLine_%1_%2").arg(blockIndex).arg(line));
        row->findChild<QToolButton *>()->setObjectName(
            QString("ConflictIncomingBubble_%1_%2").arg(blockIndex).arg(line));
        stored.incomingRows.append(row);
      } else {
        mIncomingPanel->addLine(QString(), false);
      }
    }

    fileLine = block.endMarker + 1;
  }

  const int unchangedStart = fileLine;
  while (fileLine < fileLines.size()) {
    resultText.append(resultLine(fileLines.at(fileLine)));
    ++fileLine;
  }
  fileLine = unchangedStart;
  addUnchangedLines(fileLines.size());

  mResult->setPlainText(resultText);
  connect(mResult->document(), &QTextDocument::contentsChange, this,
          [this](int pos, int removed, int added) {
            if (mUpdatingResult)
              return;

            const int oldLength =
                mResult->document()->characterCount() - 1 - added + removed;
            if ((pos == 0 && removed >= oldLength) || !mResultRangesValid) {
              mResultRangesValid = false;
              for (BlockState &state : mBlocks)
                state.origins.clear();
              updateResultHighlights();
              return;
            }

            const int removedEnd = pos + removed;
            int touchedBlocks = 0;
            for (const BlockState &state : mBlocks) {
              const bool touched =
                  removed == 0
                      ? pos >= state.resultStart && pos <= state.resultEnd
                      : pos < state.resultEnd && removedEnd > state.resultStart;
              if (touched)
                ++touchedBlocks;
            }
            if (touchedBlocks > 1) {
              mResultRangesValid = false;
              for (BlockState &state : mBlocks)
                state.origins.clear();
              updateResultHighlights();
              return;
            }

            const int delta = added - removed;
            for (BlockState &state : mBlocks) {
              if ((removed == 0 && pos < state.resultStart) ||
                  (removed > 0 && removedEnd <= state.resultStart)) {
                state.resultStart += delta;
                state.resultEnd += delta;
              } else if (pos <= state.resultEnd &&
                         removedEnd >= state.resultStart) {
                state.origins.clear();
                if (pos < state.resultStart)
                  state.resultStart = pos + added;
                state.resultEnd = removedEnd < state.resultEnd
                                      ? state.resultEnd + delta
                                      : qMax(state.resultStart, pos + added);
              }
            }
            updateResultHighlights();
          });

  mBulkUpdating = true;
  for (int block = 0; block < mBlocks.size(); ++block) {
    const git::Patch::ConflictResolution resolution =
        mPatch.conflictResolution(block);
    if (resolution == git::Patch::Ours || resolution == git::Patch::Both) {
      for (int line = 0; line < mBlocks.at(block).block.currentLines.size();
           ++line)
        mBlocks[block].selections.append({Current, line});
      if (mBlocks.at(block).block.currentLines.isEmpty())
        mBlocks[block].selections.append({Current, -1});
    }
    if (resolution == git::Patch::Theirs || resolution == git::Patch::Both) {
      for (int line = 0; line < mBlocks.at(block).block.incomingLines.size();
           ++line)
        mBlocks[block].selections.append({Incoming, line});
      if (mBlocks.at(block).block.incomingLines.isEmpty())
        mBlocks[block].selections.append({Incoming, -1});
    }
    if (!mBlocks.at(block).selections.isEmpty())
      replaceResultBlock(block);
    updateBlockUi(block);
  }
  mBulkUpdating = false;
  updateResultHighlights();
  updateMasterUi();
  emit completenessChanged(isComplete());
  mInitialOutput = result();
}

bool ConflictResolverWidget::isComplete() const { return !mBlocks.isEmpty(); }

bool ConflictResolverWidget::hasUnsavedOutput() const {
  return result() != mInitialOutput;
}

int ConflictResolverWidget::untouchedBlockCount() const {
  int count = 0;
  for (const BlockState &state : mBlocks) {
    if (state.selections.isEmpty())
      ++count;
  }
  return count;
}

QByteArray ConflictResolverWidget::result() const {
  return encodeResult(mResult->toPlainText());
}

QByteArray ConflictResolverWidget::resultWithConflictChunks() const {
  if (!mResultRangesValid)
    return {};

  QString text = mResult->toPlainText();
  for (int block = mBlocks.size() - 1; block >= 0; --block) {
    const BlockState &state = mBlocks.at(block);
    if (!state.selections.isEmpty())
      continue;
    text.replace(state.resultStart, state.resultEnd - state.resultStart,
                 conflictChunk(state));
  }
  return encodeResult(text);
}

QByteArray ConflictResolverWidget::encodeResult(QString text) const {
  if (mCrLf)
    text.replace('\n', "\r\n");
  return QStringEncoder{mPatch.repo().encoding()}.encode(text);
}

QString ConflictResolverWidget::conflictChunk(const BlockState &state) const {
  QString text = "<<<<<<< Current\n";
  for (const QByteArray &line : state.block.currentLines)
    text.append(lineText(line)).append('\n');
  text.append("||||||| Base\n");
  for (const QByteArray &line : state.block.ancestorLines)
    text.append(lineText(line)).append('\n');
  text.append("=======\n");
  for (const QByteArray &line : state.block.incomingLines)
    text.append(lineText(line)).append('\n');
  text.append(">>>>>>> Incoming\n");
  return text;
}

void ConflictResolverWidget::toggleBlock(int block, Side side) {
  BlockState &state = mBlocks[block];
  const bool remove = allSelected(state, side);
  int first = state.selections.size();
  for (int i = state.selections.size() - 1; i >= 0; --i) {
    if (state.selections.at(i).side == side) {
      first = qMin(first, i);
      state.selections.removeAt(i);
    }
  }

  if (!remove) {
    const QList<QByteArray> &lines = (side == Current)
                                         ? state.block.currentLines
                                         : state.block.incomingLines;
    if (lines.isEmpty()) {
      state.selections.insert(first, {side, -1});
    } else {
      for (int line = 0; line < lines.size(); ++line)
        state.selections.insert(first + line, {side, line});
    }
  }

  if (mResultRangesValid)
    replaceResultBlock(block);
  updateBlockUi(block);
}

void ConflictResolverWidget::toggleLine(int block, Side side, int line) {
  BlockState &state = mBlocks[block];
  for (int i = 0; i < state.selections.size(); ++i) {
    const Selection &selection = state.selections.at(i);
    if (selection.side == side && selection.line == line) {
      state.selections.removeAt(i);
      if (mResultRangesValid)
        replaceResultBlock(block);
      updateBlockUi(block);
      return;
    }
  }

  state.selections.append({side, line});
  if (mResultRangesValid)
    replaceResultBlock(block);
  updateBlockUi(block);
}

void ConflictResolverWidget::toggleAll(Side side) {
  bool all = !mBlocks.isEmpty();
  for (const BlockState &state : std::as_const(mBlocks))
    all = all && allSelected(state, side);
  setAllSelected(side, !all);
}

void ConflictResolverWidget::setAllSelected(Side side, bool selected) {
  QTextCursor editCursor(mResult->document());
  mUpdatingResult = true;
  editCursor.beginEditBlock();
  mBulkUpdating = true;
  for (int block = 0; block < mBlocks.size(); ++block) {
    BlockState &state = mBlocks[block];
    for (int i = state.selections.size() - 1; i >= 0; --i) {
      if (state.selections.at(i).side == side)
        state.selections.removeAt(i);
    }
    if (selected) {
      const QList<QByteArray> &lines = side == Current
                                           ? state.block.currentLines
                                           : state.block.incomingLines;
      if (lines.isEmpty()) {
        state.selections.append({side, -1});
      } else {
        for (int line = 0; line < lines.size(); ++line)
          state.selections.append({side, line});
      }
    }
    if (mResultRangesValid)
      replaceResultBlock(block);
    updateBlockUi(block);
  }
  mBulkUpdating = false;
  editCursor.endEditBlock();
  mUpdatingResult = false;
  updateResultHighlights();
  updateMasterUi();
  emit completenessChanged(isComplete());
}

void ConflictResolverWidget::replaceResultBlock(int block) {
  BlockState &state = mBlocks[block];
  QString text;
  state.origins.clear();
  if (state.selections.isEmpty()) {
    for (const QByteArray &line : state.block.ancestorLines)
      text.append(resultLine(line));
  }
  for (const Selection &selection : state.selections) {
    if (selection.line < 0)
      continue;
    const QList<QByteArray> &lines = selection.side == Current
                                         ? state.block.currentLines
                                         : state.block.incomingLines;
    const int start = text.size();
    text.append(resultLine(lines.at(selection.line)));
    if (!state.origins.isEmpty() &&
        state.origins.last().side == selection.side &&
        state.origins.last().end == start) {
      state.origins.last().end = text.size();
    } else {
      state.origins.append(
          {selection.side, start, static_cast<int>(text.size())});
    }
  }

  QTextCursor cursor(mResult->document());
  cursor.setPosition(state.resultStart);
  cursor.setPosition(state.resultEnd, QTextCursor::KeepAnchor);
  const int oldLength = state.resultEnd - state.resultStart;
  const bool updatingResult = mUpdatingResult;
  mUpdatingResult = true;
  cursor.insertText(text);
  mUpdatingResult = updatingResult;

  const int delta = text.size() - oldLength;
  state.resultEnd = state.resultStart + text.size();
  for (int i = block + 1; i < mBlocks.size(); ++i) {
    mBlocks[i].resultStart += delta;
    mBlocks[i].resultEnd += delta;
  }
  if (!mBulkUpdating)
    updateResultHighlights();
}

void ConflictResolverWidget::updateResultHighlights() {
  QList<QTextEdit::ExtraSelection> highlights;
  const QColor currentColor(45, 164, 78, 46);
  const QColor incomingColor(9, 105, 218, 46);
  const int documentEnd = mResult->document()->characterCount() - 1;
  for (const BlockState &state : std::as_const(mBlocks)) {
    for (const OriginSpan &origin : state.origins) {
      const int start = state.resultStart + origin.start;
      const int end = qMin(state.resultStart + origin.end, documentEnd);
      if (start < 0 || start > end || start > documentEnd)
        continue;

      QTextEdit::ExtraSelection highlight;
      highlight.cursor = QTextCursor(mResult->document());
      highlight.cursor.setPosition(start);
      highlight.cursor.setPosition(end, QTextCursor::KeepAnchor);
      highlight.format.setBackground(origin.side == Current ? currentColor
                                                            : incomingColor);
      highlight.format.setProperty(QTextFormat::FullWidthSelection, true);
      highlights.append(highlight);
    }
  }
  mResult->setExtraSelections(highlights);
}

void ConflictResolverWidget::updateBlockUi(int block) {
  BlockState &state = mBlocks[block];
  state.currentCheck->setChecked(allSelected(state, Current));
  state.incomingCheck->setChecked(allSelected(state, Incoming));
  QSet<int> currentSelections;
  QSet<int> incomingSelections;
  for (const Selection &selection : std::as_const(state.selections)) {
    (selection.side == Current ? currentSelections : incomingSelections)
        .insert(selection.line);
  }
  for (int line = 0; line < state.currentRows.size(); ++line)
    state.currentRows.at(line)->setSelected(currentSelections.contains(line));
  for (int line = 0; line < state.incomingRows.size(); ++line)
    state.incomingRows.at(line)->setSelected(incomingSelections.contains(line));

  git::Patch::ConflictResolution resolution = git::Patch::Unresolved;
  const bool current = allSelected(state, Current);
  const bool incoming = allSelected(state, Incoming);
  if (current && incoming)
    resolution = git::Patch::Both;
  else if (current)
    resolution = git::Patch::Ours;
  else if (incoming)
    resolution = git::Patch::Theirs;
  mPatch.setConflictResolution(block, resolution);
  if (!mBulkUpdating) {
    updateMasterUi();
    emit completenessChanged(isComplete());
  }
}

void ConflictResolverWidget::updateMasterUi() {
  auto stateFor = [this](Side side) {
    bool any = false;
    bool all = !mBlocks.isEmpty();
    for (const BlockState &state : std::as_const(mBlocks)) {
      any = any || anySelected(state, side);
      all = all && allSelected(state, side);
    }
    return all ? Qt::Checked : (any ? Qt::PartiallyChecked : Qt::Unchecked);
  };
  mCurrentPanel->setMasterState(stateFor(Current));
  mIncomingPanel->setMasterState(stateFor(Incoming));
}

bool ConflictResolverWidget::contains(const BlockState &state, Side side,
                                      int line) const {
  for (const Selection &selection : state.selections) {
    if (selection.side == side && selection.line == line)
      return true;
  }
  return false;
}

bool ConflictResolverWidget::allSelected(const BlockState &state,
                                         Side side) const {
  const QList<QByteArray> &lines =
      side == Current ? state.block.currentLines : state.block.incomingLines;
  int selected = 0;
  for (const Selection &selection : state.selections) {
    if (selection.side != side)
      continue;
    if (lines.isEmpty())
      return selection.line == -1;
    ++selected;
  }
  return !lines.isEmpty() && selected == lines.size();
}

bool ConflictResolverWidget::anySelected(const BlockState &state,
                                         Side side) const {
  for (const Selection &selection : state.selections) {
    if (selection.side == side)
      return true;
  }
  return false;
}

QString ConflictResolverWidget::resultLine(const QByteArray &line) const {
  QString text = mPatch.repo().decode(line);
  if (text.endsWith("\r\n")) {
    text.chop(2);
    text.append('\n');
  }
  return text;
}

QString ConflictResolverWidget::lineText(const QByteArray &line) const {
  QString text = resultLine(line);
  if (text.endsWith('\n'))
    text.chop(1);
  return text;
}

QString ConflictResolverWidget::incomingCommit() const {
  const git::Repository repo = mPatch.repo();
  const QStringList refs = {"MERGE_HEAD", "REBASE_HEAD", "CHERRY_PICK_HEAD",
                            "REVERT_HEAD"};
  for (const QString &name : refs) {
    const git::Reference ref = repo.lookupRef(name);
    if (ref.isValid()) {
      const QString id = ref.target().shortId();
      if (!id.isEmpty())
        return id;
    }
  }

  const git::Index::Conflict conflict = repo.index().conflict(mPatch.name());
  return conflict.theirs.isValid() ? conflict.theirs.shortId() : tr("unknown");
}

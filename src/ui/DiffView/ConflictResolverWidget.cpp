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
#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QStringConverter>
#include <functional>

namespace {

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
                const std::function<void()> &toggle, QWidget *parent = nullptr)
      : QWidget(parent), mSelectable(selectable) {
    setMouseTracking(true);
    setFixedHeight(fontMetrics().height() + 6);

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
      : QWidget(parent) {
    setObjectName(objectName);
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    QLabel *header = new QLabel(title, this);
    header->setContentsMargins(10, 7, 10, 7);
    header->setStyleSheet(
        QString("font-weight: 700; background: %1;").arg(headerColor));
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
    row->setStyleSheet("background: palette(alternate-base);");
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
    SourceLineRow *row = new SourceLineRow(text, selectable, toggle, mContent);
    mRows->insertWidget(mRows->count() - 1, row);
    return row;
  }

  QScrollBar *verticalScrollBar() const { return mScroll->verticalScrollBar(); }

private:
  QScrollArea *mScroll = nullptr;
  QWidget *mContent = nullptr;
  QVBoxLayout *mRows = nullptr;
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
  for (int blockIndex = 0; blockIndex < blocks.size(); ++blockIndex) {
    const git::Patch::ConflictBlock &block = blocks.at(blockIndex);
    while (fileLine < block.startMarker) {
      const QString text = lineText(fileLines.at(fileLine));
      mCurrentPanel->addLine(text, false);
      mIncomingPanel->addLine(text, false);
      resultText.append(repo.decode(fileLines.at(fileLine)));
      ++fileLine;
    }

    BlockState state;
    state.block = block;
    state.resultStart = resultText.size();
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

  while (fileLine < fileLines.size()) {
    const QString text = lineText(fileLines.at(fileLine));
    mCurrentPanel->addLine(text, false);
    mIncomingPanel->addLine(text, false);
    resultText.append(repo.decode(fileLines.at(fileLine)));
    ++fileLine;
  }

  mResult->setPlainText(resultText);
  connect(mResult->document(), &QTextDocument::contentsChange, this,
          [this](int pos, int removed, int added) {
            if (mUpdatingResult)
              return;

            const int oldLength =
                mResult->document()->characterCount() - 1 - added + removed;
            if ((pos == 0 && removed >= oldLength) || !mResultRangesValid) {
              mResultRangesValid = false;
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
                if (pos < state.resultStart)
                  state.resultStart = pos + added;
                state.resultEnd = removedEnd < state.resultEnd
                                      ? state.resultEnd + delta
                                      : qMax(state.resultStart, pos + added);
              }
            }
          });

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
    replaceResultBlock(block);
    updateBlockUi(block);
  }
}

bool ConflictResolverWidget::isComplete() const { return !mBlocks.isEmpty(); }

int ConflictResolverWidget::untouchedBlockCount() const {
  int count = 0;
  for (const BlockState &state : mBlocks) {
    if (state.selections.isEmpty())
      ++count;
  }
  return count;
}

QByteArray ConflictResolverWidget::result() const {
  QString text = mResult->toPlainText();
  if (mCrLf)
    text.replace('\n', "\r\n");
  return QStringEncoder{mPatch.repo().encoding()}.encode(text);
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

void ConflictResolverWidget::replaceResultBlock(int block) {
  BlockState &state = mBlocks[block];
  QString text;
  for (const Selection &selection : state.selections) {
    if (selection.line < 0)
      continue;
    const QList<QByteArray> &lines = selection.side == Current
                                         ? state.block.currentLines
                                         : state.block.incomingLines;
    text.append(mPatch.repo().decode(lines.at(selection.line)));
  }

  QTextCursor cursor(mResult->document());
  cursor.setPosition(state.resultStart);
  cursor.setPosition(state.resultEnd, QTextCursor::KeepAnchor);
  const int oldLength = state.resultEnd - state.resultStart;
  mUpdatingResult = true;
  cursor.insertText(text);
  mUpdatingResult = false;

  const int delta = text.size() - oldLength;
  state.resultEnd = state.resultStart + text.size();
  for (int i = block + 1; i < mBlocks.size(); ++i) {
    mBlocks[i].resultStart += delta;
    mBlocks[i].resultEnd += delta;
  }
}

void ConflictResolverWidget::updateBlockUi(int block) {
  BlockState &state = mBlocks[block];
  state.currentCheck->setChecked(allSelected(state, Current));
  state.incomingCheck->setChecked(allSelected(state, Incoming));
  for (int line = 0; line < state.currentRows.size(); ++line)
    state.currentRows.at(line)->setSelected(contains(state, Current, line));
  for (int line = 0; line < state.incomingRows.size(); ++line)
    state.incomingRows.at(line)->setSelected(contains(state, Incoming, line));

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
  emit completenessChanged(isComplete());
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
  if (lines.isEmpty())
    return contains(state, side, -1);
  for (int line = 0; line < lines.size(); ++line) {
    if (!contains(state, side, line))
      return false;
  }
  return true;
}

QString ConflictResolverWidget::lineText(const QByteArray &line) const {
  QString text = mPatch.repo().decode(line);
  if (text.endsWith('\n'))
    text.chop(1);
  if (text.endsWith('\r'))
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

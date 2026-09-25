#include "CompleteFileDiffWidget.h"
#include "DiffView.h"
#include "Editor.h"
#include "HunkWidget.h"
#include "app/Application.h"
#include "git/Blob.h"
#include "git/Repository.h"
#include <QEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <QtMath>
#include <functional>
#include <utility>

namespace {

constexpr int kOverviewWidth = 32;
constexpr int kOverviewMinimumThumbHeight = 12;
constexpr int kNavigationWidth = 40;
constexpr int kNavigationButtonSize = 20;
constexpr int kNavigationHeight = 50;
constexpr qreal kNavigationTopAnchor = 0.20;
constexpr qreal kNavigationBottomAnchor = 0.80;
const QColor kOverviewThumb(128, 128, 128, 96);
const QColor kOverviewThumbBorder(96, 96, 96, 144);
const QColor kNavigationHighlight(240, 160, 32);
const QColor kNavigationHighlightText(60, 37, 0);

const QString kNavigationStyle = QStringLiteral("QToolButton {"
                                                "  background-color: #f0a020;"
                                                "  border: 1px solid #d88e14;"
                                                "  border-radius: 3px;"
                                                "  padding: 2px;"
                                                "}"
                                                "QToolButton:hover {"
                                                "  background-color: #ffb52e;"
                                                "}"
                                                "QToolButton:pressed {"
                                                "  background-color: #d98200;"
                                                "}"
                                                "QToolButton:disabled {"
                                                "  background-color: #c9af82;"
                                                "  border-color: #b59a6d;"
                                                "}");

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

class DiffOverviewBar final : public QWidget {
public:
  using Navigate = std::function<void(qreal)>;

  explicit DiffOverviewBar(QWidget *parent = nullptr) : QWidget(parent) {
    setObjectName("DiffOverviewBar");
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setCursor(Qt::PointingHandCursor);
    setMouseTracking(true);
  }

  QSize sizeHint() const override { return QSize(kOverviewWidth, 100); }

  void setChanges(const QVector<QColor> &left, const QVector<QColor> &right) {
    mLeftChanges = left;
    mRightChanges = right;
    mLineCount = qMax(mLeftChanges.size(), mRightChanges.size());
    update();
  }

  void setNavigateHandler(Navigate navigate) {
    mNavigate = std::move(navigate);
  }

  void setViewportRange(qreal start, qreal end) {
    mViewportStart = qBound(qreal(0), start, qreal(1));
    mViewportEnd = qBound(mViewportStart, end, qreal(1));
    update();
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Base));

    const int leftWidth = width() / 2;
    const int rightWidth = width() - leftWidth;
    paintChanges(painter, mLeftChanges, 0, leftWidth);
    paintChanges(painter, mRightChanges, leftWidth, rightWidth);

    if (rightWidth > 0) {
      QColor divider = palette().color(QPalette::Mid);
      divider.setAlpha(80);
      painter.fillRect(QRect(leftWidth, 0, 1, height()), divider);
    }

    const QRect thumb = viewportThumb();
    if (!thumb.isEmpty()) {
      painter.fillRect(thumb, kOverviewThumb);
      painter.setPen(kOverviewThumbBorder);
      painter.setBrush(Qt::NoBrush);
      painter.drawRect(thumb.adjusted(0, 0, -1, -1));
    }
  }

  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton) {
      QWidget::mousePressEvent(event);
      return;
    }

    const QRect thumb = viewportThumb();
    if (!thumb.isEmpty() && thumb.contains(event->position().toPoint())) {
      mDragging = true;
      mDragOffset = event->position().y() - thumb.top();
      grabMouse();
    } else {
      navigateTo(event->position().y() / qMax(1, height()));
    }
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    if (mDragging) {
      const QRect thumb = viewportThumb();
      const qreal visualSpan =
          height() > 0 ? qreal(thumb.height()) / height() : 1.0;
      const qreal visualAvailable =
          qMax(qreal(0), qreal(1) - visualSpan);
      const qreal visualStart =
          height() > 0 ? (event->position().y() - mDragOffset) / height() : 0;
      const qreal fraction =
          visualAvailable > 0
              ? qBound(qreal(0), visualStart, visualAvailable) /
                    visualAvailable
              : 0;
      const qreal span = qBound(qreal(0), mViewportEnd - mViewportStart,
                                qreal(1));
      if (mNavigate)
        mNavigate(fraction * qMax(qreal(0), qreal(1) - span));
      event->accept();
      return;
    }
    QWidget::mouseMoveEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && mDragging) {
      mDragging = false;
      releaseMouse();
      event->accept();
      return;
    }
    QWidget::mouseReleaseEvent(event);
  }

private:
  void paintChanges(QPainter &painter, const QVector<QColor> &changes, int x,
                    int width) const {
    if (mLineCount <= 0 || width <= 0 || height() <= 0)
      return;

    for (int line = 0; line < changes.size(); ++line) {
      const QColor &color = changes.at(line);
      if (!color.isValid())
        continue;

      const int top = qFloor(qreal(line) * height() / mLineCount);
      int bottom = qCeil(qreal(line + 1) * height() / mLineCount);
      if (bottom <= top)
        bottom = top + 1;
      if (top >= height())
        continue;
      bottom = qMin(bottom, height());
      painter.fillRect(QRect(x, top, width, bottom - top), color);
    }
  }

  QRect viewportThumb() const {
    if (height() <= 0 || mViewportEnd - mViewportStart >= 0.999)
      return QRect();

    int top = qRound(mViewportStart * height());
    int bottom = qRound(mViewportEnd * height());
    const int minimum = qMin(kOverviewMinimumThumbHeight, height());
    if (bottom - top < minimum) {
      const int center = (top + bottom) / 2;
      top = qBound(0, center - minimum / 2, height() - minimum);
      bottom = top + minimum;
    }
    return QRect(0, top, width(), bottom - top);
  }

  void navigateTo(qreal position) {
    if (!mNavigate)
      return;
    const qreal span =
        qBound(qreal(0), mViewportEnd - mViewportStart, qreal(1));
    const qreal maximum = qMax(qreal(0), qreal(1) - span);
    mNavigate(qBound(qreal(0), position - span / 2, maximum));
  }

  QVector<QColor> mLeftChanges;
  QVector<QColor> mRightChanges;
  Navigate mNavigate;
  int mLineCount{0};
  qreal mViewportStart{0};
  qreal mViewportEnd{1};
  bool mDragging{false};
  qreal mDragOffset{0};
};

CompleteFileDiffWidget::CompleteFileDiffWidget(const git::Diff &diff,
                                               const git::Patch &patch,
                                               const QList<HunkWidget *> &hunks,
                                               Settings::DiffMode mode,
                                               QWidget *parent, DiffView *view)
    : QWidget(parent), mDiff(diff), mPatch(patch), mHunks(hunks), mMode(mode),
      mView(view) {
  setObjectName(mode == Settings::DiffMode::Split ? "SplitFileDiff"
                                                  : "InlineFileDiff");
  QHBoxLayout *layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(mode == Settings::DiffMode::Split ? 1 : 0);

  auto createEditor = [this] {
    Editor *editor = new Editor(this);
    editor->setObjectName("CompleteFileEditor");
    editor->setLexer(mPatch.name());
    editor->setMarginTypeN(TextEditor::Margin::LineNumber, SC_MARGIN_RTEXT);
    auto updateLineNumberStyle = [editor] {
      editor->styleSetFont(TextEditor::ModifiedBlockLineNumber,
                           editor->styleFont(STYLE_LINENUMBER));
      editor->styleSetFore(TextEditor::ModifiedBlockLineNumber,
                           kNavigationHighlightText);
      editor->styleSetBack(TextEditor::ModifiedBlockLineNumber,
                           kNavigationHighlight);
    };
    updateLineNumberStyle();
    connect(editor, &TextEditor::settingsChanged, this, updateLineNumberStyle);
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
    connect(editor, &TextEditor::settingsChanged, this,
            [this] { updateOverview(); });
    return editor;
  };

  if (mode == Settings::DiffMode::Split) {
    mOld = createEditor();
    mNavigationSlot = new QWidget(this);
    mNavigationSlot->setObjectName("ModifiedBlockNavigationSlot");
    mNavigationSlot->setFixedWidth(kNavigationWidth);
    mNavigationSlot->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    mNew = createEditor();
    mOverviewSlot = new QWidget(this);
    mOverviewSlot->setObjectName("DiffOverviewSlot");
    mOverviewSlot->setFixedWidth(kOverviewWidth);
    mOverviewSlot->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    layout->addWidget(mOld, 1);
    layout->addWidget(mNavigationSlot);
    layout->addWidget(mNew, 1);
    layout->addWidget(mOverviewSlot);
  } else {
    mNavigationSlot = new QWidget(this);
    mNavigationSlot->setObjectName("ModifiedBlockNavigationSlot");
    mNavigationSlot->setFixedWidth(kNavigationWidth);
    mNavigationSlot->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    mInline = createEditor();
    mOverviewSlot = new QWidget(this);
    mOverviewSlot->setObjectName("DiffOverviewSlot");
    mOverviewSlot->setFixedWidth(kOverviewWidth);
    mOverviewSlot->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    layout->addWidget(mNavigationSlot);
    layout->addWidget(mInline, 1);
    layout->addWidget(mOverviewSlot);
  }

  createOverview();
  createNavigation();
  reload();
  QTimer::singleShot(0, this, [this] { updateOverviewGeometry(); });
}

CompleteFileDiffWidget::~CompleteFileDiffWidget() { delete mOverview; }

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
  if (mNavigation)
    updateModifiedBlocks();
  if (mInline)
    loadEditor(mInline, false, mRows);
  if (mOld)
    loadEditor(mOld, true, mRows);
  if (mNew)
    loadEditor(mNew, false, mRows);
  setMinimumHeight(0);
  int contentHeight = 0;
  for (Editor *editor : {mInline, mOld, mNew})
    if (editor)
      contentHeight = qMax(contentHeight, editor->sizeHint().height());
  if (contentHeight > 0)
    setMinimumHeight(contentHeight);
  updateGeometry();
  updateOverview();
  updateNavigationGeometry();
}

void CompleteFileDiffWidget::createOverview() {
  if (!mOverviewSlot)
    return;

  mOverviewInViewport = mView && mView->isAncestorOf(this);
  mOverview = new DiffOverviewBar(mOverviewSlot);
  mOverview->setNavigateHandler(
      [this](qreal position) { navigateOverview(position); });

  if (mView) {
    connect(mView->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] {
              updateOverviewGeometry();
              updateNavigationBlockForScroll();
            });
    connect(mView->verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this] {
              updateOverviewGeometry();
              updateNavigationBlockForScroll();
            });
    connect(mView->horizontalScrollBar(), &QScrollBar::valueChanged, this,
            [this] { updateOverviewGeometry(); });
    connect(mView->horizontalScrollBar(), &QScrollBar::rangeChanged, this,
            [this] { updateOverviewGeometry(); });
    mView->viewport()->installEventFilter(this);
    mView->installEventFilter(this);
    mView->verticalScrollBar()->installEventFilter(this);
    mView->horizontalScrollBar()->installEventFilter(this);
  }
}

void CompleteFileDiffWidget::createNavigation() {
  if (!mNavigationSlot)
    return;

  mNavigation = new QWidget(mNavigationSlot);
  mNavigation->setObjectName("ModifiedBlockNavigation");
  mNavigation->setFixedSize(kNavigationWidth, kNavigationHeight);

  QVBoxLayout *layout = new QVBoxLayout(mNavigation);
  layout->setContentsMargins(4, 4, 4, 4);
  layout->setSpacing(2);

  auto createButton = [this](const char *objectName, const QString &name,
                             QStyle::StandardPixmap icon) {
    QToolButton *button = new QToolButton(mNavigation);
    button->setObjectName(objectName);
    button->setAccessibleName(name);
    button->setToolTip(name);
    button->setIcon(style()->standardIcon(icon));
    button->setIconSize(QSize(16, 16));
    button->setFixedSize(kNavigationButtonSize, kNavigationButtonSize);
    button->setStyleSheet(kNavigationStyle);
    button->setAutoRaise(false);
    return button;
  };

  mPreviousBlock =
      createButton("PreviousModifiedBlock", tr("Previous modified block"),
                   QStyle::SP_ArrowUp);
  mNextBlock = createButton("NextModifiedBlock", tr("Next modified block"),
                            QStyle::SP_ArrowDown);
  layout->addWidget(mPreviousBlock);
  layout->addWidget(mNextBlock);

  connect(mPreviousBlock, &QToolButton::clicked, this,
          [this] { navigateModifiedBlock(-1); });
  connect(mNextBlock, &QToolButton::clicked, this,
          [this] { navigateModifiedBlock(1); });
}

void CompleteFileDiffWidget::updateModifiedBlocks() {
  mModifiedBlocks.clear();
  mCurrentBlock = -1;
  mPreviousNavigationBlock = -1;
  mNextNavigationBlock = -1;
  mNavigationPositionKnown = false;
  updateLineNumberHighlight();

  int editorLine = 0;
  int blockStart = -1;
  auto addLine = [&](bool modified) {
    if (modified) {
      if (blockStart < 0)
        blockStart = editorLine;
    } else if (blockStart >= 0) {
      mModifiedBlocks.append({blockStart, editorLine - 1});
      blockStart = -1;
    }
    ++editorLine;
  };

  for (const Row &row : mRows) {
    if (mMode == Settings::DiffMode::Inline) {
      if (row.deletion)
        addLine(true);
      if (row.addition)
        addLine(true);
      if (!row.deletion && !row.addition)
        addLine(false);
    } else {
      addLine(row.deletion || row.addition);
    }
  }
  if (blockStart >= 0)
    mModifiedBlocks.append({blockStart, editorLine - 1});

  updateNavigationButtons();
}

void CompleteFileDiffWidget::updateNavigationButtons() {
  if (!mPreviousBlock || !mNextBlock)
    return;
  if (mCurrentBlock >= 0) {
    mPreviousBlock->setEnabled(mCurrentBlock > 0);
    mNextBlock->setEnabled(mCurrentBlock + 1 < mModifiedBlocks.size());
  } else if (!mNavigationPositionKnown) {
    mPreviousBlock->setEnabled(false);
    mNextBlock->setEnabled(!mModifiedBlocks.isEmpty());
  } else {
    mPreviousBlock->setEnabled(mPreviousNavigationBlock >= 0);
    mNextBlock->setEnabled(mNextNavigationBlock >= 0);
  }
}

void CompleteFileDiffWidget::updateNavigationBlockForScroll() {
  if (!mView || !mView->widget() || mModifiedBlocks.isEmpty()) {
    updateNavigationButtons();
    return;
  }

  Editor *editor = mInline ? mInline : mNew;
  if (!editor)
    return;

  const int viewportTop = mView->verticalScrollBar()->value();
  const int viewportBottom = viewportTop + mView->viewport()->height();
  const int editorTop = editor->mapTo(mView->widget(), QPoint()).y();
  int previousBlock = -1;
  int nextBlock = -1;
  bool currentBlockVisible = false;
  for (int i = 0; i < mModifiedBlocks.size(); ++i) {
    const int startLine = mModifiedBlocks.at(i).first;
    const int startY =
        editorTop +
        editor->pointFromPosition(editor->positionFromLine(startLine)).y();
    if (startY < viewportTop)
      previousBlock = i;
    else if (nextBlock < 0)
      nextBlock = i;
    if (i == mCurrentBlock && startY >= viewportTop &&
        startY < viewportBottom)
      currentBlockVisible = true;
  }

  if (mCurrentBlock >= 0 && !currentBlockVisible) {
    mCurrentBlock = -1;
    updateLineNumberHighlight();
  }
  mPreviousNavigationBlock = previousBlock;
  mNextNavigationBlock = nextBlock;
  mNavigationPositionKnown = true;
  updateNavigationButtons();
}

void CompleteFileDiffWidget::updateLineNumberHighlight() {
  const int line = mCurrentBlock >= 0 && mCurrentBlock < mModifiedBlocks.size()
                       ? mModifiedBlocks.at(mCurrentBlock).first
                       : -1;
  if (line == mHighlightedLine)
    return;

  for (Editor *editor : {mInline, mOld, mNew}) {
    if (!editor)
      continue;
    if (mHighlightedLine >= 0 && mHighlightedLine < editor->lineCount())
      editor->marginSetStyle(mHighlightedLine, STYLE_LINENUMBER);
    if (line >= 0 && line < editor->lineCount())
      editor->marginSetStyle(line, TextEditor::ModifiedBlockLineNumber);
  }
  mHighlightedLine = line;
}

void CompleteFileDiffWidget::updateOverview() {
  if (!mOverview)
    return;

  const QColor addition = Application::theme()->diff(Theme::Diff::Addition);
  const QColor deletion = Application::theme()->diff(Theme::Diff::Deletion);
  const QColor noChange;
  QVector<QColor> left;
  QVector<QColor> right;
  left.reserve(mRows.size());
  right.reserve(mRows.size());

  for (const Row &row : mRows) {
    if (mMode == Settings::DiffMode::Inline) {
      if (row.deletion) {
        left.append(deletion);
        right.append(noChange);
      }
      if (row.addition || !row.deletion) {
        left.append(noChange);
        right.append(row.addition ? addition : noChange);
      }
    } else {
      left.append(row.deletion ? deletion : noChange);
      right.append(row.addition ? addition : noChange);
    }
  }

  mOverview->setChanges(left, right);
  updateOverviewGeometry();
}

void CompleteFileDiffWidget::updateOverviewGeometry() {
  if (!mOverview) {
    initializeModifiedBlockNavigation();
    return;
  }

  if (!isVisible()) {
    mOverview->hide();
    updateNavigationGeometry();
    initializeModifiedBlockNavigation();
    return;
  }

  if (mOverviewInViewport && mView) {
    QWidget *viewport = mView->viewport();
    const QRect fileRect(this->mapTo(viewport, QPoint()), size());
    if (viewport->height() <= 0 || !fileRect.intersects(viewport->rect())) {
      mOverview->hide();
      updateNavigationGeometry();
      initializeModifiedBlockNavigation();
      return;
    }

    mOverview->setGeometry(mOverviewSlot->rect());

    const qreal fileHeight = qMax(1, height());
    const qreal visibleStart =
        qBound(qreal(0), -qreal(fileRect.top()) / fileHeight, qreal(1));
    const qreal visibleEnd = qBound(
        qreal(0), qreal(viewport->height() - fileRect.top()) / fileHeight,
        qreal(1));
    mOverview->setViewportRange(visibleStart, visibleEnd);
  } else {
    mOverview->setGeometry(mOverviewSlot->rect());
    mOverview->setViewportRange(0, 1);
  }

  mOverview->show();
  mOverview->raise();
  updateNavigationGeometry();
  initializeModifiedBlockNavigation();
}

void CompleteFileDiffWidget::updateNavigationGeometry() {
  if (!mNavigation || !mNavigationSlot)
    return;

  if (!isVisible()) {
    mNavigation->hide();
    return;
  }

  const QRect slot = mNavigationSlot->rect();
  int y = (slot.height() - mNavigation->height()) / 2;
  if (mView && mView->isAncestorOf(this)) {
    QWidget *viewport = mView->viewport();
    const QRect fileRect(this->mapTo(viewport, QPoint()), size());
    if (viewport->height() <= 0 || !fileRect.intersects(viewport->rect())) {
      mNavigation->hide();
      return;
    }

    y = mNavigationSlot->mapFrom(viewport,
                                 QPoint(0, viewport->height() / 2))
            .y() -
        mNavigation->height() / 2;
    y = qBound(0, y, qMax(0, slot.height() - mNavigation->height()));
  }

  mNavigation->setGeometry((slot.width() - mNavigation->width()) / 2, y,
                           mNavigation->width(), mNavigation->height());
  mNavigation->show();
  mNavigation->raise();
}

void CompleteFileDiffWidget::navigateOverview(qreal position) {
  if (!mOverviewInViewport || !mView || !mView->widget() || height() <= 0)
    return;

  const int fileTop = mapTo(mView->widget(), QPoint()).y();
  const int viewportHeight = mView->viewport()->height();
  const int maxStart = qMax(0, height() - viewportHeight);
  const int offset = qBound(0, qRound(position * height()), maxStart);
  mView->verticalScrollBar()->setValue(fileTop + offset);
}

void CompleteFileDiffWidget::initializeModifiedBlockNavigation() {
  if (!mInitialNavigationPending)
    return;
  if (mModifiedBlocks.isEmpty()) {
    mInitialNavigationPending = false;
    return;
  }
  if (!mView || !mView->isAncestorOf(this) || !isVisible() ||
      !mView->widget())
    return;

  QWidget *viewport = mView->viewport();
  if (viewport->height() <= 0 || height() <= 0)
    return;

  const QRect fileRect(this->mapTo(viewport, QPoint()), size());
  if (!fileRect.intersects(viewport->rect()))
    return;

  Editor *editor = mInline ? mInline : mNew;
  if (!editor || editor->height() <= 0)
    return;

  QScrollBar *scrollBar = mView->verticalScrollBar();
  if (height() > viewport->height() && scrollBar->maximum() <= 0)
    return;

  mInitialNavigationPending = false;
  navigateModifiedBlock(1);
}

void CompleteFileDiffWidget::navigateModifiedBlock(int direction) {
  if (mModifiedBlocks.isEmpty())
    return;

  const bool initialSelection = mCurrentBlock < 0;
  int target = initialSelection ? -1 : mCurrentBlock + direction;
  if (initialSelection)
    target = direction > 0
                 ? (mNextNavigationBlock >= 0 ? mNextNavigationBlock : 0)
                 : mPreviousNavigationBlock;

  if (target < 0 || target >= mModifiedBlocks.size())
    return;

  mCurrentBlock = target;
  const int line = mModifiedBlocks.at(target).first;
  for (Editor *editor : {mInline, mOld, mNew}) {
    if (editor)
      editor->gotoLine(line);
  }

  Editor *editor = mInline ? mInline : mNew;
  if (editor && mView && mView->isAncestorOf(this) && mView->widget()) {
    QScrollBar *scrollBar = mView->verticalScrollBar();
    const int viewportHeight = mView->viewport()->height();
    if (viewportHeight > 0) {
      const int linePosition = editor->positionFromLine(line);
      const int lineY =
          editor->mapTo(mView->widget(), QPoint()).y() +
          editor->pointFromPosition(linePosition).y();
      const int lineViewportY = lineY - scrollBar->value();
      const int topAnchor = qRound(viewportHeight * kNavigationTopAnchor);
      const int bottomAnchor =
          qRound(viewportHeight * kNavigationBottomAnchor);
      int scrollValue = scrollBar->value();

      if (initialSelection) {
        scrollValue = lineY - topAnchor;
      } else if (direction > 0 && lineViewportY > bottomAnchor) {
        scrollValue = lineY - bottomAnchor;
      } else if (direction < 0 && lineViewportY < topAnchor) {
        scrollValue = lineY - topAnchor;
      }

      scrollBar->setValue(qBound(scrollBar->minimum(), scrollValue,
                                 scrollBar->maximum()));
    }
  }

  updateNavigationButtons();
  updateLineNumberHighlight();
}

bool CompleteFileDiffWidget::eventFilter(QObject *watched, QEvent *event) {
  const bool viewGeometryChanged =
      mView && (watched == mView || watched == mView->viewport() ||
                watched == mView->verticalScrollBar() ||
                watched == mView->horizontalScrollBar());
  if (viewGeometryChanged &&
      (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
       event->type() == QEvent::Show || event->type() == QEvent::Hide ||
       event->type() == QEvent::LayoutRequest))
    updateOverviewGeometry();
  return QWidget::eventFilter(watched, event);
}

bool CompleteFileDiffWidget::event(QEvent *event) {
  const bool result = QWidget::event(event);
  if (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
      event->type() == QEvent::Show || event->type() == QEvent::Hide)
    QTimer::singleShot(0, this, [this] { updateOverviewGeometry(); });
  return result;
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
  QVector<int> blameLines;
  content.reserve(rows.size());
  for (const Row &row : rows) {
    if (mMode == Settings::DiffMode::Inline) {
      if (row.deletion) {
        content.append(row.oldText);
        editorTargets.append({row.oldTarget});
        blameLines.append(-1);
      }
      if (row.addition || !row.deletion) {
        content.append(row.newText);
        editorTargets.append(row.addition ? QList<Target>{row.newTarget}
                                          : QList<Target>());
        blameLines.append(row.newLine);
      }
    } else {
      content.append(oldSide ? row.oldText : row.newText);
      const Target target = oldSide ? row.oldTarget : row.newTarget;
      editorTargets.append(target.first >= 0 ? QList<Target>{target}
                                             : QList<Target>());
      blameLines.append(oldSide ? row.oldLine : row.newLine);
    }
  }
  mEditorTargets.insert(editor, editorTargets);

  editor->setReadOnly(false);
  editor->setText(content.join('\n'));
  editor->setBlameLineMapping(blameLines);
  editor->markerDeleteAll(-1);

  int editorLine = 0;
  for (const Row &row : rows) {
    auto apply = [&](bool deletion, bool addition, int oldLine, int newLine,
                     const Target &target) {
      if (deletion)
        editor->markerAdd(editorLine, TextEditor::Deletion);
      if (addition)
        editor->markerAdd(editorLine, TextEditor::Addition);
      editor->marginSetText(editorLine, QString::number(editorLine + 1));
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
      QByteArray::number(qMax(1, qMax(mRows.size(), editor->lineCount())));
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

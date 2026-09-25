//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "BlameMargin.h"
#include "ProgressIndicator.h"
#include "app/Application.h"
#include "conf/Settings.h"
#include "editor/TextEditor.h"
#include "git/Commit.h"
#include "git/Repository.h"
#include "git/RevWalk.h"
#include "git/Signature.h"
#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyleOption>
#include <QTextLayout>
#include <QToolTip>
#include <QUrl>
#include <QUrlQuery>

namespace {

const QString kNowrapFmt = "<span style='white-space: nowrap'>%1</span><br>";
const QString kStyleSheet = "BlameMargin {"
                            "  background-color: palette(base);"
                            "  border-right: 1px solid palette(mid)"
                            "}";

} // namespace

BlameMargin::BlameMargin(TextEditor *editor, QWidget *parent)
    : QWidget(parent), mEditor(nullptr), mIndex(-1), mProgress(0) {
  setStyleSheet(kStyleSheet);

  if (editor) {
    mEditor = editor;
    if (QScrollBar *scrollBar = mEditor->verticalScrollBar())
      connect(scrollBar, &QScrollBar::valueChanged, [this] { update(); });
    connect(mEditor, &TextEditor::linesAdded, this, &BlameMargin::updateBlame);
  }

  // Connect progress timer.
  connect(&mTimer, &QTimer::timeout, [this] {
    ++mProgress;
    update();
  });
}

void BlameMargin::setEditor(TextEditor *editor) {
  if (mEditor == editor)
    return;
  mEditor = editor;
  if (!mEditor) {
    update();
    return;
  }

  // Can't connect directly because of different parameter types.
  if (QScrollBar *scrollBar = mEditor->verticalScrollBar())
    connect(scrollBar, &QScrollBar::valueChanged, [this] { update(); });
  connect(mEditor, &TextEditor::linesAdded, this, &BlameMargin::updateBlame);
  updateGeometry();
  update();
}

void BlameMargin::startBlame(const QString &name) {
  mName = name;
  mProgress = 0;
  if (!mBlame.isValid())
    mTimer.start(50);
}

void BlameMargin::setBlame(const git::Repository &repo,
                           const git::Blame &blame) {
  if (Settings::instance()
          ->value(Setting::Id::ShowHeatmapInBlameMargin)
          .toBool()) {
    git::Commit first = repo.walker(GIT_SORT_TIME | GIT_SORT_REVERSE).next();
    git::Commit last = repo.walker(GIT_SORT_TIME).next();
    mMinTime = first ? first.committer().date().toSecsSinceEpoch() : -1;
    mMaxTime = last ? last.committer().date().toSecsSinceEpoch() : -1;
  }

  mTimer.stop();
  mSource = blame;
  updateBlame();
}

void BlameMargin::clear() {
  mTimer.stop();
  mName = QString();
  mBlame = git::Blame();
  mSource = git::Blame();

  mIndex = -1;
  mSelection = git::Id();

  mMinTime = -1;
  mMaxTime = -1;

  // Repaint.
  update();
}

QSize BlameMargin::minimumSizeHint() const { return QSize(92, 0); }

bool BlameMargin::event(QEvent *event) {
  if (event->type() != QEvent::ToolTip || !mBlame.isValid())
    return QWidget::event(event);

  QHelpEvent *help = static_cast<QHelpEvent *>(event);
  int index = this->index(help->y());
  if (index >= 0) {
    QStringList lines;

    QString name = this->name(index);
    if (!name.isEmpty())
      name = QString("<b>%1</b>").arg(name);

    QString email, date;
    git::Signature signature = mBlame.signature(index);
    if (signature.isValid()) {
      email = QString("&lt;%1&gt;").arg(signature.email());
      date = QLocale().toString(signature.date(), QLocale::LongFormat);
    }

    if (!name.isEmpty())
      lines << kNowrapFmt.arg(QString("%1 %2").arg(name, email));

    if (!date.isEmpty())
      lines << kNowrapFmt.arg(date);

    lines << mBlame.id(index).toString();

    QString msg = mBlame.message(index);
    if (!msg.isEmpty())
      lines << QString("<p>%1</p>").arg(msg);

    QToolTip::showText(help->globalPos(), lines.join('\n'), this);

  } else {
    QToolTip::hideText();
    event->ignore();
  }

  return true;
}

void BlameMargin::mousePressEvent(QMouseEvent *event) {
  mIndex = mBlame.isValid() ? index(event->position().y()) : -1;
}

void BlameMargin::mouseReleaseEvent(QMouseEvent *event) {
  if (mBlame.isValid() && mIndex >= 0 &&
      mIndex == index(event->position().y())) {
    // Update selection.
    git::Id id = mBlame.id(mIndex);
    mSelection = (mSelection != id) ? id : git::Id();
    update();
  }

  mIndex = -1;
}

void BlameMargin::mouseDoubleClickEvent(QMouseEvent *event) {
  if (!mBlame.isValid())
    return;

  int index = this->index(event->position().y());
  if (index < 0)
    return;

  QUrlQuery query;
  query.addQueryItem("file", mName);

  QUrl url;
  url.setScheme("id");
  url.setPath(mBlame.id(index).toString());
  url.setQuery(query);

  emit linkActivated(url.toString());
}

void BlameMargin::paintEvent(QPaintEvent *event) {
  // Draw background.
  QStyleOption opt;
  opt.initFrom(this);
  QPainter painter(this);
  style()->drawPrimitive(QStyle::PE_Widget, &opt, &painter, this);

  // Draw busy indicator.
  if (!mBlame.isValid()) {
    QRect rect(0, 10, width(), ProgressIndicator::size().height());
    ProgressIndicator::paint(&painter, rect, "#808080", mProgress);
    return;
  }

  // Draw items.
  painter.setRenderHints(QPainter::Antialiasing);

  if (!mEditor)
    return;

  int size = mEditor->styleFont(STYLE_DEFAULT).pointSize();
  QFont regular = font();
  regular.setPointSize(size);
  QFontMetricsF regularMetrics(regular);

  QFont bold = font();
  bold.setBold(true);
  bold.setPointSize(size);
  QFontMetricsF boldMetrics(bold);

  QDate today = QDate::currentDate();

  const int lh = mEditor->textHeight(0);
  const int lineCount = mEditor->lineCount();
  if (lh <= 0 || lineCount <= 0)
    return;

  const int editorFirstDisplayLine = mEditor->firstVisibleLine();
  const QRect editorTextRect = mEditor->textRectangle();
  const int top = editorTop();
  const int editorBottom = top + editorTextRect.height();
  const int visibleTop = qMax(0, top);
  const int visibleBottom = qMin(height(), editorBottom);
  if (visibleBottom <= visibleTop)
    return;

  const auto roundDownLines = [lh](int pixels) {
    return pixels > 0 ? pixels / lh : 0;
  };
  const auto roundUpLines = [lh](int pixels) {
    return pixels > 0 ? (pixels + lh - 1) / lh : 0;
  };
  const int firstDisplayLine = qBound(
      0, editorFirstDisplayLine + roundDownLines(visibleTop - top), lineCount);
  const int lastDisplayLine = qBound(
      firstDisplayLine,
      editorFirstDisplayLine + roundUpLines(visibleBottom - top), lineCount);
  const int first = firstDisplayLine + 1;
  const int last = lastDisplayLine + 1;
  const bool hasLineMapping = mEditor->hasBlameLineMapping();

  painter.setClipRect(QRect(0, visibleTop, width(), visibleBottom - visibleTop),
                      Qt::IntersectClip);

  int count = mBlame.count();
  int index = hasLineMapping ? 0 : mBlame.index(first);
  while (index < count && (hasLineMapping || mBlame.line(index) < last)) {
    // Combine adjacent lines with the same id.
    int line = mBlame.line(index);
    git::Id id = mBlame.id(index);
    if (id.isNull()) {
      // This can happen when the commit is for some reason not valid, for
      // example if the email address is not specified
      index++;
      continue;
    }
    while (index + 1 < count && mBlame.id(index + 1) == id)
      ++index;

    bool invalid = false;
    while (index + 1 < count) {
      const auto idNext = mBlame.id(index + 1);
      if (idNext.isNull()) {
        invalid = true;
        break;
      } else if (idNext == id) {
        index++;
      } else {
        break;
      }
    }
    if (invalid) {
      index++;
      continue;
    }

    // Calculate outer rectangle.
    const int next =
        (index + 1 < count) ? mBlame.line(index + 1) : lineCount + 1;
    int blockFirstDisplayLine = firstDisplayLine;
    int blockLastDisplayLine = lastDisplayLine;
    if (hasLineMapping) {
      blockFirstDisplayLine = -1;
      blockLastDisplayLine = -1;
      const int end = qMin(mEditor->lineCount(), lastDisplayLine);
      for (int displayLine = firstDisplayLine; displayLine < end;
           ++displayLine) {
        const int blameLine = mEditor->blameLine(displayLine);
        if (blameLine < line || blameLine >= next)
          continue;
        if (blockFirstDisplayLine < 0)
          blockFirstDisplayLine = displayLine;
        blockLastDisplayLine = displayLine + 1;
      }
      if (blockFirstDisplayLine < 0) {
        ++index;
        continue;
      }
    } else {
      blockFirstDisplayLine = qMax(firstDisplayLine, line - 1);
      blockLastDisplayLine = qMin(lastDisplayLine, next - 1);
      if (blockLastDisplayLine <= blockFirstDisplayLine) {
        ++index;
        continue;
      }
    }

    const int blockLineCount = blockLastDisplayLine - blockFirstDisplayLine;
    const int blockTop =
        top + (blockFirstDisplayLine - editorFirstDisplayLine) * lh;
    QRectF rect(0, blockTop, width() - 1, blockLineCount * lh);

    // Get short date.
    QString date;
    int time = -1;
    git::Signature signature = mBlame.signature(index);
    if (signature.isValid()) {
      QDateTime dateTime = signature.date();
      date = (dateTime.date() == today)
                 ? QLocale().toString(dateTime.time(), QLocale::ShortFormat)
                 : QLocale().toString(dateTime.date(), QLocale::ShortFormat);
      time = dateTime.toSecsSinceEpoch();
    }

    // Draw background.
    if (id == mSelection) {
      painter.fillRect(rect, palette().highlight());
    } else if (time >= 0 && mMinTime >= 0 && mMaxTime >= 0 &&
               mMinTime != mMaxTime) {

      // Translate to zero.
      qreal val = (time - mMinTime);
      qreal max = (mMaxTime - mMinTime);
      qreal mid = max / 2;

      QColor color;
      if (val > mid) { // hot
        color = Application::theme()->heatMap(Theme::HeatMap::Hot);
        color.setAlphaF(val / max);
      } else { // cold
        color = Application::theme()->heatMap(Theme::HeatMap::Cold);
        color.setAlphaF(1 - val / mid);
      }

      painter.fillRect(rect, color);
    }

    // Draw separator line wholly within the current cell.
    QPointF pt1 = rect.bottomLeft() + QPointF(0.5, -0.5);
    QPointF pt2 = rect.bottomRight() + QPointF(-0.5, -0.5);
    painter.setPen(QColor("#E6E6E6"));
    painter.drawLine(pt1, pt2);

    // Calculate inner rectangle.
    rect.adjust(4, 0, -4, 0);

    painter.setPen(palette().color(QPalette::Text));

    // Draw name or initials.
    QRectF nameRect;
    QString name = this->name(index);
    if (!name.isEmpty()) {
      nameRect = boldMetrics.boundingRect(name);
      QRectF dateRect = regularMetrics.boundingRect(date);
      if (nameRect.width() + dateRect.width() + 4 > rect.width())
        name = git::Signature::initials(name);

      painter.setFont(bold);
      painter.drawText(rect, Qt::AlignLeft, name);
    }

    painter.setFont(regular);

    // Draw date.
    if (signature.isValid()) {
      // Get long date.
      QDateTime dateTime = signature.date();
      QString longDate =
          (dateTime.date() == today)
              ? QLocale().toString(dateTime.time(), QLocale::LongFormat)
              : QLocale().toString(dateTime.date(), QLocale::LongFormat);

      QRectF dateRect = regularMetrics.boundingRect(longDate);
      if (nameRect.width() + dateRect.width() + 4 <= rect.width())
        date = longDate;

      painter.drawText(rect, Qt::AlignRight, date);
    }

    // Draw message.
    if (blockLineCount > 1) {
      rect.setY(rect.y() + lh);

      painter.setPen(palette().color(QPalette::BrightText));

      // Layout text.
      qreal y = 0;
      QFontMetricsF fm(painter.font());
      qreal lineSpacing = fm.lineSpacing();
      QString text = mBlame.message(index);
      QTextLayout layout(text, painter.font());
      layout.beginLayout();
      forever {
        QTextLine line = layout.createLine();
        if (!line.isValid())
          break;

        line.setLineWidth(rect.width());
        qreal nextLineY = y + lineSpacing;
        if (rect.height() >= nextLineY + lineSpacing) {
          line.draw(&painter, QPointF(0, rect.y() + y));
          y = nextLineY;
        } else {
          // Draw the last line elided.
          QString substr = text.mid(line.textStart());
          QString elided = fm.elidedText(substr, Qt::ElideRight, rect.width());
          painter.drawText(QPointF(0, rect.y() + y + fm.ascent()), elided);
          break;
        }
      }
      layout.endLayout();
    }

    ++index;
  }
}

void BlameMargin::wheelEvent(QWheelEvent *event) {
  // Forward to the editor.
  if (mEditor)
    mEditor->wheelEvent(event);
  else
    QWidget::wheelEvent(event);
}

void BlameMargin::updateBlame() {
  if (!mEditor || !mSource.isValid())
    return;

#if 0
  char *data = reinterpret_cast<char *>(mEditor->characterPointer());
  mBlame = mSource.updated(QByteArray::fromRawData(data, mEditor->length()));
#else
  mBlame = mSource;
#endif

  update();
}

int BlameMargin::index(int y) const {
  if (!mEditor)
    return -1;
  const int top = editorTop();
  const int bottom = top + mEditor->textRectangle().height();
  if (y < top || y >= bottom)
    return -1;
  const int lineHeight = mEditor->textHeight(0);
  if (lineHeight <= 0)
    return -1;
  int line = mEditor->firstVisibleLine() + ((y - top) / lineHeight);
  if (line < 0 || line >= mEditor->lineCount())
    return -1;
  const int blameLine = mEditor->blameLine(line);
  return blameLine > 0 ? mBlame.index(blameLine) : -1;
}

int BlameMargin::editorTop() const {
  if (!mEditor)
    return 0;
  const QRect textRect = mEditor->textRectangle();
  return mapFromGlobal(mEditor->mapToGlobal(textRect.topLeft())).y();
}

QString BlameMargin::name(int index) const {
  if (!mBlame.isCommitted(index))
    return tr("Not Committed");

  git::Signature signature = mBlame.signature(index);
  return signature.isValid() ? signature.name() : tr("Invalid Signature");
}

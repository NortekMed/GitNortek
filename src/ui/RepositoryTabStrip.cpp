//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "RepositoryTabStrip.h"
#include <QAbstractButton>
#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QFrame>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScreen>
#include <QStyle>
#include <QStyleOption>
#include <QStyleOptionFocusRect>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTextDocumentFragment>
#include <QVBoxLayout>

namespace {

constexpr int kMaximumRows = 4;
constexpr int kMinimumTitleColumns = 8;
constexpr int kHorizontalPadding = 10;
constexpr int kElementSpacing = 6;
constexpr int kIconExtent = 16;
constexpr int kCloseExtent = 16;
constexpr int kOverflowWidth = 52;

class RepositoryTabButton : public QAbstractButton {
  Q_OBJECT

public:
  explicit RepositoryTabButton(QWidget *parent = nullptr)
      : QAbstractButton(parent) {
    setObjectName("RepositoryTabButton");
    setCheckable(true);
    setAutoExclusive(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAttribute(Qt::WA_StyledBackground, true);

    mClose = new QToolButton(this);
    mClose->setObjectName("RepositoryTabClose");
    mClose->setAutoRaise(true);
    mClose->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    connect(mClose, &QToolButton::clicked, this,
            &RepositoryTabButton::closeClicked);
  }

  void setTabText(const QString &text) {
    setText(text);
    mClose->setAccessibleName(tr("Close %1").arg(text));
    mClose->setToolTip(tr("Close %1").arg(text));
  }

  void setTabIcon(const QIcon &icon) {
    mIcon = icon;
    update();
  }

signals:
  void closeClicked();
  void navigate(int offset);
  void dragMoved(const QPoint &position);

protected:
  void keyPressEvent(QKeyEvent *event) override {
    switch (event->key()) {
      case Qt::Key_Left:
      case Qt::Key_Up:
        emit navigate(-1);
        return;
      case Qt::Key_Right:
      case Qt::Key_Down:
        emit navigate(1);
        return;
      case Qt::Key_Delete:
        emit closeClicked();
        return;
      default:
        QAbstractButton::keyPressEvent(event);
    }
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    if (event->buttons() & Qt::LeftButton) {
      if (!mDragging &&
          (event->position().toPoint() - mPressPosition).manhattanLength() >=
              QApplication::startDragDistance())
        mDragging = true;

      if (mDragging)
        emit dragMoved(mapToParent(event->position().toPoint()));
    }
    QAbstractButton::mouseMoveEvent(event);
  }

  void mousePressEvent(QMouseEvent *event) override {
    mPressPosition = event->position().toPoint();
    mDragging = false;
    QAbstractButton::mousePressEvent(event);
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::MiddleButton) {
      setDown(false);
      emit closeClicked();
      return;
    }

    if (mDragging) {
      setDown(false);
      mDragging = false;
      return;
    }

    QAbstractButton::mouseReleaseEvent(event);
  }

  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    QColor background =
        isChecked() ? palette().window().color() : palette().dark().color();
    if (underMouse() && !isChecked())
      background = background.lighter(110);
    painter.fillRect(rect(), background);

    QStyleOption option;
    option.initFrom(this);
    if (isChecked())
      option.state |= QStyle::State_On;
    style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);

    int left = kHorizontalPadding;
    if (!mIcon.isNull()) {
      QRect iconRect(left, (height() - kIconExtent) / 2, kIconExtent,
                     kIconExtent);
      mIcon.paint(&painter, iconRect);
      left = iconRect.right() + 1 + kElementSpacing;
    }

    QRect textRect(left, 0,
                   qMax(0, closeRect().left() - kElementSpacing - left),
                   height());
    painter.setPen(palette().buttonText().color());
    painter.drawText(
        textRect, Qt::AlignVCenter | Qt::AlignLeft,
        fontMetrics().elidedText(text(), Qt::ElideMiddle, textRect.width()));

    if (hasFocus()) {
      QStyleOptionFocusRect focus;
      focus.initFrom(this);
      focus.rect = rect().adjusted(2, 2, -2, -2);
      style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
    }
  }

  void resizeEvent(QResizeEvent *event) override {
    QAbstractButton::resizeEvent(event);
    mClose->setGeometry(closeRect());
  }

private:
  QRect closeRect() const {
    int extent = qMin(kCloseExtent, qMin(width(), height()));
    return QRect(qMax(0, width() - kHorizontalPadding - extent),
                 (height() - extent) / 2, extent, extent);
  }

  QIcon mIcon;
  QToolButton *mClose;
  QPoint mPressPosition;
  bool mDragging = false;
};

class OverflowDelegate : public QStyledItemDelegate {
  Q_OBJECT

public:
  explicit OverflowDelegate(QObject *parent = nullptr)
      : QStyledItemDelegate(parent) {}

signals:
  void closeRequested(int index);

protected:
  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyledItemDelegate::paint(painter, option, index);
    option.widget->style()
        ->standardIcon(QStyle::SP_TitleBarCloseButton)
        .paint(painter, closeRect(option.rect));
  }

  bool editorEvent(QEvent *event, QAbstractItemModel *,
                   const QStyleOptionViewItem &option,
                   const QModelIndex &index) override {
    if (event->type() != QEvent::MouseButtonRelease)
      return false;

    auto *mouseEvent = static_cast<QMouseEvent *>(event);
    if (mouseEvent->button() != Qt::LeftButton ||
        !closeRect(option.rect).contains(mouseEvent->position().toPoint()))
      return false;

    emit closeRequested(index.data(Qt::UserRole).toInt());
    return true;
  }

private:
  QRect closeRect(const QRect &rect) const {
    return QRect(rect.right() - kHorizontalPadding - kCloseExtent + 1,
                 rect.center().y() - kCloseExtent / 2, kCloseExtent,
                 kCloseExtent);
  }
};

class OverflowPopup : public QFrame {
  Q_OBJECT

public:
  struct Item {
    int index;
    QIcon icon;
    QString text;
    QString toolTip;
  };

  explicit OverflowPopup(const QVector<Item> &items, QWidget *parent = nullptr)
      : QFrame(parent, Qt::Popup), mItems(items) {
    setObjectName("RepositoryTabOverflowPopup");
    setFrameShape(QFrame::StyledPanel);
    setMinimumWidth(320);

    mSearch = new QLineEdit(this);
    mSearch->setObjectName("RepositoryTabSearch");
    mSearch->setAccessibleName(tr("Search hidden repositories"));
    mSearch->setPlaceholderText(tr("Search hidden repositories"));
    mSearch->setClearButtonEnabled(true);

    mList = new QListWidget(this);
    mList->setObjectName("RepositoryTabOverflowList");
    mList->setAccessibleName(tr("Hidden repository tabs"));
    mList->setAccessibleDescription(
        tr("Select a repository tab or press Delete to close it"));
    mList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *delegate = new OverflowDelegate(mList);
    mList->setItemDelegate(delegate);
    connect(delegate, &OverflowDelegate::closeRequested, this,
            &OverflowPopup::closeRequested);
    connect(mSearch, &QLineEdit::textChanged, this, &OverflowPopup::refilter);
    connect(mSearch, &QLineEdit::returnPressed, this, [this] {
      if (QListWidgetItem *item = firstVisibleItem())
        activate(item);
    });
    connect(mList, &QListWidget::itemActivated, this, &OverflowPopup::activate);
    mSearch->installEventFilter(this);
    mList->installEventFilter(this);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addWidget(mSearch);
    layout->addWidget(mList);

    refilter();
    mSearch->setFocus();
  }

signals:
  void activated(int index);
  void closeRequested(int index);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() != QEvent::KeyPress)
      return QFrame::eventFilter(watched, event);

    auto *keyEvent = static_cast<QKeyEvent *>(event);
    if (watched == mList && keyEvent->key() == Qt::Key_Delete &&
        mList->currentItem()) {
      emit closeRequested(mList->currentItem()->data(Qt::UserRole).toInt());
      return true;
    }
    if (watched == mSearch && keyEvent->key() == Qt::Key_Down) {
      mList->setFocus();
      if (QListWidgetItem *item = firstVisibleItem())
        mList->setCurrentItem(item);
      return true;
    }
    return QFrame::eventFilter(watched, event);
  }

private slots:
  void refilter() {
    mList->clear();
    const QString filter = mSearch->text();
    for (const Item &item : mItems) {
      if (!filter.isEmpty() &&
          !item.text.contains(filter, Qt::CaseInsensitive) &&
          !item.toolTip.contains(filter, Qt::CaseInsensitive))
        continue;

      auto *listItem = new QListWidgetItem(item.icon, item.text, mList);
      listItem->setData(Qt::UserRole, item.index);
      listItem->setToolTip(item.toolTip);
    }
  }

  void activate(QListWidgetItem *item) {
    if (!item)
      return;
    emit activated(item->data(Qt::UserRole).toInt());
  }

private:
  QListWidgetItem *firstVisibleItem() const {
    return mList->count() ? mList->item(0) : nullptr;
  }

  QVector<Item> mItems;
  QLineEdit *mSearch;
  QListWidget *mList;
};

} // namespace

class AccessibleRepositoryTab : public QAccessibleWidget {
public:
  explicit AccessibleRepositoryTab(QWidget *widget)
      : QAccessibleWidget(widget, QAccessible::PageTab) {}

  QAccessible::State state() const override {
    QAccessible::State state = QAccessibleWidget::state();
    auto *button = qobject_cast<QAbstractButton *>(object());
    state.selectable = true;
    state.selected = button && button->isChecked();
    state.checkable = true;
    state.checked = state.selected;
    return state;
  }

  QStringList actionNames() const override {
    QStringList actions = QAccessibleWidget::actionNames();
    if (!actions.contains(QAccessibleActionInterface::pressAction()))
      actions.prepend(QAccessibleActionInterface::pressAction());
    return actions;
  }

  void doAction(const QString &actionName) override {
    if (actionName == QAccessibleActionInterface::pressAction()) {
      if (auto *button = qobject_cast<QAbstractButton *>(object()))
        button->click();
      return;
    }
    QAccessibleWidget::doAction(actionName);
  }
};

QAccessibleInterface *repositoryTabAccessibleFactory(const QString &className,
                                                     QObject *object) {
  if (className == "RepositoryTabButton")
    return new AccessibleRepositoryTab(static_cast<QWidget *>(object));
  if (className == "RepositoryTabStrip")
    return new QAccessibleWidget(static_cast<QWidget *>(object),
                                 QAccessible::PageTabList);
  return nullptr;
}

RepositoryTabStrip::RepositoryTabStrip(QWidget *parent) : QWidget(parent) {
  static bool accessibleFactoryInstalled = [] {
    QAccessible::installFactory(repositoryTabAccessibleFactory);
    return true;
  }();
  Q_UNUSED(accessibleFactoryInstalled)

  setObjectName("RepositoryTabStrip");
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  mOverflow = new QToolButton(this);
  mOverflow->setObjectName("RepositoryTabOverflow");
  mOverflow->setAccessibleName(tr("Show hidden repository tabs"));
  mOverflow->setToolTip(tr("Show hidden repository tabs"));
  mOverflow->hide();
  connect(mOverflow, &QToolButton::clicked, this,
          &RepositoryTabStrip::showOverflow);

  setFixedHeight(rowHeight());
}

int RepositoryTabStrip::addTab(const QIcon &icon, const QString &text) {
  auto *button = new RepositoryTabButton(this);
  button->setTabText(text);
  button->setTabIcon(icon);
  button->setAccessibleName(text);
  connect(button, &QAbstractButton::clicked, this,
          [this, button] { setCurrentIndex(indexOf(button)); });
  connect(button, &RepositoryTabButton::closeClicked, this,
          [this, button] { emit closeRequested(indexOf(button)); });
  connect(button, &RepositoryTabButton::navigate, this, [this](int offset) {
    if (!count())
      return;
    setCurrentIndex((mCurrentIndex + offset + count()) % count());
    mTabs.at(mCurrentIndex).button->setFocus();
  });
  connect(button, &RepositoryTabButton::dragMoved, this,
          [this, button](const QPoint &position) {
            int from = indexOf(button);
            for (int to = 0; to < mTabs.size(); ++to) {
              if (to != from && mTabs.at(to).button->isVisible() &&
                  mTabs.at(to).button->geometry().contains(position)) {
                moveTab(from, to);
                break;
              }
            }
          });

  mTabs.append({button, icon, text, QString()});
  if (mCurrentIndex < 0)
    setCurrentIndex(0);
  relayout();
  return mTabs.size() - 1;
}

void RepositoryTabStrip::removeTab(int index) {
  if (index < 0 || index >= count())
    return;

  QAbstractButton *button = mTabs.takeAt(index).button;
  button->deleteLater();

  if (mTabs.isEmpty()) {
    mCurrentIndex = -1;
  } else if (index < mCurrentIndex) {
    --mCurrentIndex;
  } else if (index == mCurrentIndex) {
    mCurrentIndex = qMin(index, count() - 1);
  }

  for (int i = 0; i < count(); ++i)
    mTabs.at(i).button->setChecked(i == mCurrentIndex);
  relayout();
}

int RepositoryTabStrip::count() const { return mTabs.size(); }

int RepositoryTabStrip::currentIndex() const { return mCurrentIndex; }

void RepositoryTabStrip::setCurrentIndex(int index) {
  if (index < 0 || index >= count())
    return;
  if (index == mCurrentIndex) {
    mTabs.at(index).button->setChecked(true);
    return;
  }

  mCurrentIndex = index;
  for (int i = 0; i < count(); ++i)
    mTabs.at(i).button->setChecked(i == index);
  relayout();
  emit currentChanged(index);
}

QIcon RepositoryTabStrip::tabIcon(int index) const {
  return (index >= 0 && index < count()) ? mTabs.at(index).icon : QIcon();
}

void RepositoryTabStrip::setTabIcon(int index, const QIcon &icon) {
  if (index < 0 || index >= count())
    return;
  mTabs[index].icon = icon;
  static_cast<RepositoryTabButton *>(mTabs.at(index).button)->setTabIcon(icon);
  relayout();
}

QString RepositoryTabStrip::tabText(int index) const {
  return (index >= 0 && index < count()) ? mTabs.at(index).text : QString();
}

void RepositoryTabStrip::setTabText(int index, const QString &text) {
  if (index < 0 || index >= count())
    return;
  mTabs[index].text = text;
  static_cast<RepositoryTabButton *>(mTabs.at(index).button)->setTabText(text);
  mTabs.at(index).button->setAccessibleName(text);
  relayout();
}

QString RepositoryTabStrip::tabToolTip(int index) const {
  return (index >= 0 && index < count()) ? mTabs.at(index).toolTip : QString();
}

void RepositoryTabStrip::setTabToolTip(int index, const QString &toolTip) {
  if (index < 0 || index >= count())
    return;
  mTabs[index].toolTip = toolTip;
  mTabs.at(index).button->setToolTip(toolTip);
  mTabs.at(index).button->setAccessibleDescription(
      QTextDocumentFragment::fromHtml(toolTip).toPlainText());
}

int RepositoryTabStrip::rowCount() const { return mRowCount; }

int RepositoryTabStrip::visibleTabCount() const { return mVisibleCount; }

int RepositoryTabStrip::minimumTabWidth() const {
  QFontMetrics metrics(font());
  return 2 * kHorizontalPadding + kIconExtent + kElementSpacing +
         kMinimumTitleColumns * metrics.averageCharWidth() +
         metrics.horizontalAdvance(QChar(0x2026)) + kElementSpacing +
         kCloseExtent;
}

QRect RepositoryTabStrip::tabRect(int index) const {
  if (index < 0 || index >= count() || mTabs.at(index).button->isHidden())
    return QRect();
  return mTabs.at(index).button->geometry();
}

void RepositoryTabStrip::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  relayout();
}

int RepositoryTabStrip::indexOf(const QObject *object) const {
  for (int i = 0; i < count(); ++i) {
    if (mTabs.at(i).button == object)
      return i;
  }
  return -1;
}

int RepositoryTabStrip::rowHeight() const {
  return fontMetrics().lineSpacing() + 12;
}

int RepositoryTabStrip::naturalTabWidth(int index) const {
  const Tab &tab = mTabs.at(index);
  int fixed = 2 * kHorizontalPadding + kElementSpacing + kCloseExtent;
  if (!tab.icon.isNull())
    fixed += kIconExtent + kElementSpacing;
  return qMax(minimumTabWidth(),
              fixed + fontMetrics().horizontalAdvance(tab.text));
}

void RepositoryTabStrip::moveTab(int from, int to) {
  if (from < 0 || from >= count() || to < 0 || to >= count() || from == to)
    return;

  Tab tab = mTabs.takeAt(from);
  mTabs.insert(to, tab);
  if (mCurrentIndex == from) {
    mCurrentIndex = to;
  } else if (from < mCurrentIndex && to >= mCurrentIndex) {
    --mCurrentIndex;
  } else if (from > mCurrentIndex && to <= mCurrentIndex) {
    ++mCurrentIndex;
  }
  relayout();
  emit tabMoved(from, to);
}

void RepositoryTabStrip::relayout() {
  if (width() <= 0)
    return;

  const int minWidth = minimumTabWidth();
  const int fullCapacity = qMax(1, width() / minWidth);
  const int fourRowCapacity = fullCapacity * kMaximumRows;
  bool overflowing = count() > fourRowCapacity;
  int capacity = fourRowCapacity;
  int overflowWidth = qMin(kOverflowWidth, width());
  if (overflowing) {
    int lastRowCapacity = qMax(0, (width() - overflowWidth) / minWidth);
    capacity = fullCapacity * (kMaximumRows - 1) + lastRowCapacity;
  }

  mVisibleCount = qMin(count(), capacity);
  mFirstVisible = qBound(0, mFirstVisible, qMax(0, count() - mVisibleCount));
  if (mCurrentIndex >= 0 && mCurrentIndex < mFirstVisible)
    mFirstVisible = mCurrentIndex;
  if (mCurrentIndex >= mFirstVisible + mVisibleCount)
    mFirstVisible = mCurrentIndex - mVisibleCount + 1;

  mRowCount = qMax(
      1, qMin(kMaximumRows, (mVisibleCount + fullCapacity - 1) / fullCapacity));
  if (overflowing)
    mRowCount = kMaximumRows;

  setFixedHeight(mRowCount * rowHeight());
  for (Tab &tab : mTabs)
    tab.button->hide();

  int index = mFirstVisible;
  int remaining = mVisibleCount;
  for (int row = 0; row < mRowCount && remaining; ++row) {
    int rowsLeft = mRowCount - row;
    int rowCount = overflowing ? qMin(fullCapacity, remaining)
                               : (remaining + rowsLeft - 1) / rowsLeft;
    int rowWidth = width();
    if (overflowing && row == mRowCount - 1) {
      rowWidth -= overflowWidth;
      rowCount = qMin(rowCount, qMax(0, rowWidth / minWidth));
    }

    if (!rowCount)
      continue;

    QVector<int> widths;
    int renderedMinimum = qMin(minWidth, rowWidth / rowCount);
    int minimumSum = 0;
    int desiredSum = 0;
    for (int offset = 0; offset < rowCount; ++offset) {
      int desired = naturalTabWidth(index + offset);
      widths.append(desired);
      minimumSum += renderedMinimum;
      desiredSum += desired;
    }

    if (desiredSum > rowWidth) {
      int spare = qMax(0, rowWidth - minimumSum);
      int desiredExtra = desiredSum - minimumSum;
      for (int offset = 0; offset < widths.size(); ++offset) {
        int extra = widths.at(offset) - renderedMinimum;
        widths[offset] = renderedMinimum +
                         (desiredExtra ? (spare * extra) / desiredExtra : 0);
      }
    }

    int x = 0;
    for (int offset = 0; offset < widths.size(); ++offset) {
      Tab &tab = mTabs[index + offset];
      tab.button->setGeometry(x, row * rowHeight(), widths.at(offset),
                              rowHeight());
      tab.button->show();
      x += widths.at(offset);
    }
    index += rowCount;
    remaining -= rowCount;
  }

  mOverflow->setVisible(overflowing);
  if (overflowing) {
    mOverflow->setGeometry(width() - overflowWidth,
                           (mRowCount - 1) * rowHeight(), overflowWidth,
                           rowHeight());
    updateOverflow();
  }

  for (int i = 0; i < count(); ++i) {
    QString description = tr("Repository tab %1 of %2").arg(i + 1).arg(count());
    QString details =
        QTextDocumentFragment::fromHtml(mTabs.at(i).toolTip).toPlainText();
    if (!details.isEmpty())
      description += tr(". %1").arg(details);
    mTabs.at(i).button->setAccessibleDescription(description);
  }
}

void RepositoryTabStrip::showOverflow() {
  QVector<OverflowPopup::Item> items;
  int lastVisible = mFirstVisible + mVisibleCount;
  for (int i = 0; i < count(); ++i) {
    if (i >= mFirstVisible && i < lastVisible)
      continue;
    const Tab &tab = mTabs.at(i);
    items.append({i, tab.icon, tab.text, tab.toolTip});
  }

  auto *popup = new OverflowPopup(items, this);
  popup->setAttribute(Qt::WA_DeleteOnClose);
  connect(popup, &OverflowPopup::activated, this, [this, popup](int index) {
    popup->close();
    setCurrentIndex(index);
  });
  connect(popup, &OverflowPopup::closeRequested, this,
          [this, popup](int index) {
            popup->close();
            emit closeRequested(index);
          });
  QPoint anchor = mOverflow->mapToGlobal(mOverflow->rect().center());
  QScreen *screen = QGuiApplication::screenAt(anchor);
  if (!screen)
    screen = QGuiApplication::primaryScreen();
  QRect available = screen->availableGeometry();

  popup->adjustSize();
  QSize size(qMin(qMax(popup->sizeHint().width(), 320), available.width()),
             qMin(popup->sizeHint().height(), qMin(420, available.height())));
  popup->setMinimumWidth(qMin(320, available.width()));
  popup->resize(size);
  QPoint position = mOverflow->mapToGlobal(
      QPoint(mOverflow->width() - size.width(), mOverflow->height()));
  if (position.y() + size.height() > available.bottom() + 1)
    position.setY(mOverflow->mapToGlobal(QPoint()).y() - size.height());
  position.setX(qBound(available.left(), position.x(),
                       available.right() - size.width() + 1));
  position.setY(qBound(available.top(), position.y(),
                       available.bottom() - size.height() + 1));
  popup->move(position);
  popup->show();
}

void RepositoryTabStrip::updateOverflow() {
  int hidden = count() - mVisibleCount;
  mOverflow->setText(tr("+%1").arg(hidden));
  mOverflow->setAccessibleDescription(
      hidden == 1 ? tr("1 hidden repository tab")
                  : tr("%1 hidden repository tabs").arg(hidden));
}

#include "RepositoryTabStrip.moc"

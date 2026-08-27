#include "FileConflictResolverWidget.h"
#include "git/Blob.h"
#include "git/Repository.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

FileConflictResolverWidget::FileConflictResolverWidget(
    const git::Patch &patch, const git::Index::Conflict &conflict,
    QWidget *parent)
    : QWidget(parent), mPatch(patch), mConflict(conflict) {
  setObjectName("FileConflictResolver");

  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  QWidget *sides = new QWidget(this);
  QHBoxLayout *sideLayout = new QHBoxLayout(sides);
  sideLayout->setContentsMargins(0, 0, 0, 0);
  sideLayout->setSpacing(4);
  sideLayout->addWidget(createSide(tr("Current"), conflict.ours,
                                   conflict.oursMode, git::Patch::Ours,
                                   "ConflictFileCurrentChoice"));
  sideLayout->addWidget(createSide(tr("Incoming"), conflict.theirs,
                                   conflict.theirsMode, git::Patch::Theirs,
                                   "ConflictFileIncomingChoice"));
  layout->addWidget(sides, 1);

  mResult = new QLabel(this);
  mResult->setObjectName("FileConflictResult");
  mResult->setContentsMargins(10, 8, 10, 8);
  mResult->setStyleSheet(
      "font-weight: 700; background: rgba(191, 135, 0, 0.20);");
  layout->addWidget(mResult);

  if (conflict.ours.isNull() && conflict.theirs.isNull())
    mResult->setText(tr("Result: Delete file"));
  else
    updateUi();
}

git::Patch::ConflictResolution FileConflictResolverWidget::resolution() const {
  return mResolution;
}

QWidget *FileConflictResolverWidget::createSide(
    const QString &title, const git::Id &id, git_filemode_t mode,
    git::Patch::ConflictResolution resolution, const QString &objectName) {
  QFrame *card = new QFrame(this);
  card->setFrameShape(QFrame::StyledPanel);
  QVBoxLayout *layout = new QVBoxLayout(card);

  QLabel *header = new QLabel(title, card);
  header->setStyleSheet("font-weight: 700;");
  layout->addWidget(header);

  if (id.isNull()) {
    QLabel *deleted = new QLabel(tr("Deleted"), card);
    deleted->setAlignment(Qt::AlignCenter);
    deleted->setStyleSheet("font-size: 18px; color: palette(mid);");
    layout->addWidget(deleted, 1);
  } else {
    const git::Blob blob = mPatch.repo().lookupBlob(id);
    if (!blob.isValid()) {
      QLabel *unavailable = new QLabel(
          tr("Content unavailable\nMode %1").arg(QString::number(mode, 8)),
          card);
      unavailable->setAlignment(Qt::AlignCenter);
      layout->addWidget(unavailable, 1);
    } else if (!blob.isBinary()) {
      const QByteArray content = blob.content();
      QPlainTextEdit *preview = new QPlainTextEdit(card);
      preview->setReadOnly(true);
      preview->setLineWrapMode(QPlainTextEdit::NoWrap);
      preview->setPlainText(mPatch.repo().decode(content));
      layout->addWidget(preview, 1);
    } else {
      const QByteArray content = blob.content();
      QLabel *binary =
          new QLabel(tr("Binary file\n%1\nMode %2")
                         .arg(locale().formattedDataSize(content.size()),
                              QString::number(mode, 8)),
                     card);
      binary->setAlignment(Qt::AlignCenter);
      layout->addWidget(binary, 1);
    }
  }

  QToolButton *choose = new QToolButton(card);
  choose->setObjectName(objectName);
  choose->setText(id.isNull() ? tr("Choose deletion")
                              : tr("Choose %1").arg(title));
  choose->setCheckable(true);
  choose->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  connect(choose, &QToolButton::clicked, this,
          [this, resolution] { select(resolution); });
  layout->addWidget(choose);

  if (resolution == git::Patch::Ours)
    mCurrent = choose;
  else
    mIncoming = choose;
  return card;
}

void FileConflictResolverWidget::select(
    git::Patch::ConflictResolution resolution) {
  mResolution = resolution;
  updateUi();
  emit resolutionChanged();
}

void FileConflictResolverWidget::updateUi() {
  mCurrent->setChecked(mResolution == git::Patch::Ours);
  mIncoming->setChecked(mResolution == git::Patch::Theirs);
  if (mResolution == git::Patch::Unresolved) {
    mResult->setText(tr("Result: Choose Current or Incoming"));
    return;
  }

  const bool current = mResolution == git::Patch::Ours;
  const git::Id &id = current ? mConflict.ours : mConflict.theirs;
  if (id.isNull())
    mResult->setText(tr("Result: Delete file"));
  else
    mResult->setText(
        tr("Result: Keep %1").arg(current ? tr("Current") : tr("Incoming")));
}

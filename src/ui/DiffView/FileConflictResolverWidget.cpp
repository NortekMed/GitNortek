#include "FileConflictResolverWidget.h"
#include "git/Blob.h"
#include "git/Repository.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include <QStringConverter>

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

  QFrame *output = new QFrame(this);
  output->setFrameShape(QFrame::StyledPanel);
  QVBoxLayout *outputLayout = new QVBoxLayout(output);
  QLabel *outputHeader = new QLabel(tr("Output"), output);
  outputHeader->setStyleSheet(
      "font-weight: 700; background: rgba(191, 135, 0, 0.20);");
  outputHeader->setContentsMargins(10, 7, 10, 7);
  outputLayout->addWidget(outputHeader);
  mOutputText = new QPlainTextEdit(output);
  mOutputText->setObjectName("FileConflictOutput");
  mOutputText->setLineWrapMode(QPlainTextEdit::NoWrap);
  outputLayout->addWidget(mOutputText, 1);
  mOutputInfo = new QLabel(output);
  mOutputInfo->setObjectName("FileConflictOutputInfo");
  mOutputInfo->setAlignment(Qt::AlignCenter);
  outputLayout->addWidget(mOutputInfo, 1);
  layout->addWidget(output, 1);

  connect(mOutputText, &QPlainTextEdit::textChanged, this, [this] {
    if (mUpdatingOutput)
      return;
    mOutputKind = TextOutput;
    mOutputId = {};
    mResolution = git::Patch::Unresolved;
    updateUi();
    emit resolutionChanged();
  });

  if (!conflict.ancestor.isNull()) {
    setOutput(conflict.ancestor, conflict.ancestorMode);
  } else if (conflict.ours.isNull() && conflict.theirs.isNull()) {
    mOutputKind = DeletedOutput;
    updateUi();
  } else {
    mOutputKind = TextOutput;
    mOutputMode = conflict.oursMode != GIT_FILEMODE_UNREADABLE
                      ? conflict.oursMode
                      : conflict.theirsMode;
    mUpdatingOutput = true;
    mOutputText->clear();
    mUpdatingOutput = false;
    updateUi();
  }
  mInitialOutputKind = mOutputKind;
  mInitialOutputMode = mOutputMode;
  mInitialOutputId = mOutputId;
  if (mOutputKind == TextOutput)
    mInitialOutput = this->output();
}

git::Patch::ConflictResolution FileConflictResolverWidget::resolution() const {
  return mResolution;
}

bool FileConflictResolverWidget::hasUnsavedOutput() const {
  if (mOutputKind != mInitialOutputKind || mOutputMode != mInitialOutputMode)
    return true;
  if (mOutputKind == TextOutput)
    return output() != mInitialOutput;
  return mOutputId != mInitialOutputId;
}

QByteArray FileConflictResolverWidget::output() const {
  return QStringEncoder{mPatch.repo().encoding()}.encode(
      mOutputText->toPlainText());
}

void FileConflictResolverWidget::acceptCurrent() { select(git::Patch::Ours); }

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
  if (resolution == git::Patch::Ours)
    setOutput(mConflict.ours, mConflict.oursMode);
  else
    setOutput(mConflict.theirs, mConflict.theirsMode);
  updateUi();
  emit resolutionChanged();
}

void FileConflictResolverWidget::setOutput(const git::Id &id,
                                           git_filemode_t mode) {
  mOutputId = id;
  mOutputMode = mode;
  if (id.isNull()) {
    mOutputKind = DeletedOutput;
    updateUi();
    return;
  }

  const git::Blob blob = mPatch.repo().lookupBlob(id);
  if (!blob.isValid() || blob.isBinary()) {
    mOutputKind = BlobOutput;
    updateUi();
    return;
  }

  mOutputKind = TextOutput;
  mUpdatingOutput = true;
  mOutputText->setPlainText(mPatch.repo().decode(blob.content()));
  mUpdatingOutput = false;
  updateUi();
}

void FileConflictResolverWidget::updateUi() {
  mCurrent->setChecked(mResolution == git::Patch::Ours);
  mIncoming->setChecked(mResolution == git::Patch::Theirs);
  mOutputText->setVisible(mOutputKind == TextOutput);
  mOutputInfo->setVisible(mOutputKind != TextOutput);
  if (mOutputKind == DeletedOutput) {
    mOutputInfo->setText(tr("Deleted file"));
  } else if (mOutputKind == BlobOutput) {
    const git::Blob blob = mPatch.repo().lookupBlob(mOutputId);
    mOutputInfo->setText(
        blob.isValid()
            ? tr("Binary output\n%1\nMode %2")
                  .arg(locale().formattedDataSize(blob.content().size()),
                       QString::number(mOutputMode, 8))
            : tr("Output object unavailable\nMode %1")
                  .arg(QString::number(mOutputMode, 8)));
  }
}

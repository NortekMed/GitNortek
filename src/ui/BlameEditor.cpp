//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "BlameEditor.h"
#include "BlameMargin.h"
#include "FindWidget.h"
#include "MenuBar.h"
#include "RepoView.h"
#include "conf/Constants.h"
#include "editor/TextEditor.h"
#include "git/Blame.h"
#include "git/Blob.h"
#include "git/Buffer.h"
#include "git/Commit.h"
#include "git/Index.h"
#include "git/Repository.h"
#include <QCloseEvent>
#include <QFile>
#include <QFileDialog>
#include <QSaveFile>
#include <QShortcut>
#include <QSplitter>
#include <QTextStream>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <atomic>

namespace {

const QString kSplitterKey = QString("blamesplitter");

class BlameCallbacks : public git::Blame::Callbacks {
public:
  void setCanceled(bool canceled) { mCanceled.store(canceled); }

  bool progress() override { return !mCanceled.load(); }

private:
  std::atomic_bool mCanceled = false;
};

} // namespace

BlameEditor::BlameEditor(const git::Repository &repo, QWidget *parent,
                         bool annotationOnly)
    : QWidget(parent), mRepo(repo), mAnnotationOnly(annotationOnly) {
  // Create editor.
  if (!mAnnotationOnly) {
    mEditor = new TextEditor(this);
    connect(mEditor, &TextEditor::linesAdded, this,
            &BlameEditor::adjustLineMarginWidth);
    connect(mEditor, &TextEditor::settingsChanged, this,
            &BlameEditor::adjustLineMarginWidth);
    connect(mEditor, &TextEditor::onVisible, this, &BlameEditor::startBlame);
  }

  // Create blame margin.
  mMargin = new BlameMargin(mEditor, this);
  connect(mMargin, &BlameMargin::linkActivated, this,
          &BlameEditor::linkActivated);

  // Add find widget.
  if (!mAnnotationOnly) {
    mFind = new FindWidget(this, this);
    mFind->hide(); // Start hidden.
  }

  // Add widgets.
  QVBoxLayout *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  if (mFind)
    layout->addWidget(mFind);
  if (mAnnotationOnly) {
    layout->addWidget(mMargin, 1);
  } else {
    QSplitter *splitter = new QSplitter(this);
    splitter->setHandleWidth(1);
    splitter->addWidget(mMargin);
    splitter->addWidget(mEditor);
    splitter->setStretchFactor(1, 1);
    connect(splitter, &QSplitter::splitterMoved, this, [splitter] {
      QSettings().setValue(kSplitterKey, splitter->saveState());
    });
    splitter->restoreState(QSettings().value(kSplitterKey).toByteArray());
    layout->addWidget(splitter, 1);
  }

  // Margin starts hidden by default.
  mMargin->setVisible(false);
  if (mAnnotationOnly) {
    const int width = mMargin->minimumSizeHint().width() * 3;
    mMargin->setMinimumWidth(width);
    mMargin->setMaximumWidth(width);
  }
  connect(&mBlame, &QFutureWatcher<git::Blame>::finished, this,
          &BlameEditor::blameFinished);
}

void BlameEditor::setEditor(TextEditor *editor) {
  if (!mAnnotationOnly || mEditor == editor)
    return;
  cancelBlame();
  mMargin->clear();
  mMargin->setVisible(false);
  mEditor = editor;
  mMargin->setEditor(editor);
  if (!mEditor)
    return;
  connect(mEditor, &TextEditor::linesAdded, this,
          &BlameEditor::adjustLineMarginWidth, Qt::UniqueConnection);
  connect(mEditor, &TextEditor::linesAdded, this,
          &BlameEditor::editorLinesAdded, Qt::UniqueConnection);
  connect(mEditor, &TextEditor::settingsChanged, this,
          &BlameEditor::adjustLineMarginWidth, Qt::UniqueConnection);
  connect(mEditor, &TextEditor::onVisible, this, &BlameEditor::startBlame,
          Qt::UniqueConnection);
}

TextEditor *BlameEditor::editor() const { return mEditor.data(); }

QList<TextEditor *> BlameEditor::editors() { return {mEditor.data()}; }

QString BlameEditor::name() const {
  return !mName.isEmpty() ? mName : tr("Untitled");
}

QString BlameEditor::path() const {
  if (mName.isEmpty())
    return QString();

  bool abs = QDir::isAbsolutePath(mName);
  Q_ASSERT(abs || mRepo.isValid());

  return abs ? mName : mRepo.workdir().filePath(mName);
}

QString BlameEditor::revision() const {
  return !mRevision.isEmpty() ? mRevision : tr("Not Tracked");
}

bool BlameEditor::load(const QString &name, const git::Blob &blob,
                       git::Commit commit) {
  // Clear content.
  clear();

  // Remember name.
  mName = name;

  if (mAnnotationOnly) {
    mRevision = commit.isValid() ? commit.shortId() : tr("HEAD");
    mBlameCommit = commit;
    if (mBlameVisible && mRepo.isValid() && mEditor) {
      mMargin->setVisible(mEditor->length() > 0);
      mMargin->startBlame(name);
      mPendingBlameCommit = commit;
      if (mEditor->length() > 0)
        startBlame();
    }
    return true;
  }

  // Load content.
  QByteArray content;
  if (blob.isValid()) {
    if (blob.isBinary())
      return false;

    content = blob.content();
    mRevision = commit.isValid() ? commit.shortId() : tr("HEAD");

  } else {
    if (mRepo.isValid() && mRepo.index().isTracked(name))
      mRevision = tr("Working Copy");

    QFile file(path());
    if (!file.open(QFile::ReadOnly))
      return false;

    // Limit the read to kMaxReadBinary to determine if the file is binary
    content = file.read(kMaxReadBinary);
    git::Buffer buffer(content.constData(), content.length());
    if (buffer.isBinary())
      return false;
    // Okay, not a binary file. Now we need to grab the rest if needed
    else if (content.length() == kMaxReadBinary)
      content = file.readAll();
  }

  // Set editor text.
  mEditor->setReadOnly(false);
  mEditor->load(name, mRepo.isValid() ? mRepo.decode(content) : content);
  mEditor->setReadOnly(blob.isValid());

  mBlameCommit = commit;
  mMargin->setVisible(mBlameVisible && mRepo.isValid() && !content.isEmpty());

  // Calculate blame.
  if (mBlameVisible && mRepo.isValid() && !content.isEmpty()) {
    mMargin->startBlame(name);
    mPendingBlameCommit = commit;
    startBlame();
  }

  return true;
}

void BlameEditor::setBlameVisible(bool visible) {
  if (mBlameVisible == visible)
    return;

  mBlameVisible = visible;
  if (!visible) {
    cancelBlame();
    mMargin->clear();
    mMargin->setVisible(false);
    return;
  }

  if (!mRepo.isValid() || !mEditor || mEditor->length() == 0 || mName.isEmpty())
    return;

  mMargin->setVisible(true);
  mMargin->startBlame(mName);
  mPendingBlameCommit = mBlameCommit;
  startBlame();
}

void BlameEditor::startBlame() {
  if (mPendingBlameCommit.has_value()) {
    const git::Commit commit = mPendingBlameCommit.value();
    const QString name = mName;
    const QString cacheKey =
        commit.isValid() ? name + QStringLiteral("\n") + commit.id().toString()
                         : QString();
    if (!cacheKey.isEmpty() && mBlameCache.contains(cacheKey)) {
      mMargin->setBlame(mRepo, mBlameCache.value(cacheKey));
      mPendingBlameCommit = std::nullopt;
      return;
    }

    mCallbacks = QSharedPointer<BlameCallbacks>::create();
    const QSharedPointer<git::Blame::Callbacks> callbacks = mCallbacks;
    const git::Repository repo = mRepo;
    const int generation = ++mBlameGeneration;
    mActiveBlameGeneration = generation;
    mActiveBlameCacheKey = cacheKey;
    mBlame.setFuture(QtConcurrent::run([repo, name, commit, callbacks] {
      return repo.blame(name, commit, callbacks.data());
    }));
    mPendingBlameCommit = std::nullopt;
  }
}

void BlameEditor::blameFinished() {
  if (mActiveBlameGeneration == 0 || mActiveBlameGeneration != mBlameGeneration)
    return;

  QFuture<git::Blame> future = mBlame.future();
  mActiveBlameGeneration = 0;
  if (future.resultCount() == 0)
    return;

  git::Blame blame = future.result();
  if (!mActiveBlameCacheKey.isEmpty()) {
    if (mBlameCache.size() >= 32)
      mBlameCache.erase(mBlameCache.begin());
    mBlameCache.insert(mActiveBlameCacheKey, blame);
  }
  mMargin->setBlame(mRepo, blame);
  mMargin->setVisible(mBlameVisible && blame.isValid());
}

void BlameEditor::editorLinesAdded() {
  if (!mAnnotationOnly || !mBlameVisible || !mEditor ||
      mEditor->length() == 0 || mName.isEmpty() ||
      !mPendingBlameCommit.has_value())
    return;

  mMargin->setVisible(true);
  startBlame();
}

void BlameEditor::cancelBlame() {
  ++mBlameGeneration;
  mActiveBlameGeneration = 0;
  mActiveBlameCacheKey.clear();
  if (mCallbacks)
    static_cast<BlameCallbacks *>(mCallbacks.data())->setCanceled(true);
  mBlame.setFuture(QFuture<git::Blame>());
  mCallbacks.reset();
  mPendingBlameCommit = std::nullopt;
}

void BlameEditor::save() {
  QString path = this->path();
  if (path.isEmpty()) {
    QDir dir = mRepo.isValid() ? mRepo.workdir() : QDir();
    path = QFileDialog::getSaveFileName(this, tr("Save File"), dir.path());
    if (path.isEmpty())
      return;

    // Set editor lexer.
    mEditor->setLexer(path);
    mEditor->startStyling(0);
  }

  QSaveFile file(path);
  if (!file.open(QFile::WriteOnly))
    return;

  QTextStream out(&file);
  if (mRepo.isValid())
    out.setEncoding(mRepo.encoding());
  out << mEditor->text();
  file.commit();

  mEditor->setSavePoint();

  // Remember the name.
  if (mName.isEmpty())
    mName = path;

  emit saved();
}

void BlameEditor::clear() {
  // Cancel find and blame.
  if (mFind)
    mFind->hide();
  cancelBlame();

  // Clear margin and editor.
  mMargin->clear();
  mMargin->setVisible(false);

  if (mEditor && !mAnnotationOnly) {
    mEditor->setReadOnly(false);
    mEditor->clearAll();
    mEditor->setReadOnly(true);
  }

  mName = QString();
  mRevision = QString();
  mBlameCommit = git::Commit();
}

void BlameEditor::find() {
  if (mFind && mEditor && mEditor->length() > 0)
    mFind->showAndSetFocus();
}

void BlameEditor::findNext() {
  if (mFind)
    mFind->find();
}

void BlameEditor::findPrevious() {
  if (mFind)
    mFind->find(FindWidget::Backward);
}

void BlameEditor::adjustLineMarginWidth() {
  if (!mEditor)
    return;
  // Enable dynamic line margin width by tracking document changes.
  QByteArray lines = QByteArray::number(static_cast<int>(mEditor->lineCount()));
  int width = mEditor->textWidth(STYLE_LINENUMBER, lines.constData());
  int marginWidth = (mEditor->length() > 0) ? width + 8 : 0;
  mEditor->setMarginWidthN(TextEditor::LineNumber, marginWidth);
}

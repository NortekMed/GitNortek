//
//          Copyright (c) 2016, Scientific Toolworks, Inc.
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//
// Author: Jason Haslam
//

#include "RepoView.h"
#include "BlameEditor.h"
#include "CommitList.h"
#include "CommitAvatarProvider.h"
#include "CommitToolBar.h"
#include "DetailView.h"
#include "EditorWindow.h"
#include "FontUtils.h"
#include "History.h"
#include "MainWindow.h"
#include "MenuBar.h"
#include "PathspecWidget.h"
#include "qtsupport.h"
#include "ReferenceWidget.h"
#include "RepositoryOpenProgress.h"
#include "RepositoryNavigator.h"
#include "RemoteCallbacks.h"
#include "SearchField.h"
#include "DoubleTreeWidget.h"
#include "ToolBar.h"
#include "Debug.h"
#include "app/Application.h"
#include "conf/Settings.h"
#include "dialogs/AmendDialog.h"
#include "dialogs/CheckoutDialog.h"
#include "dialogs/CommitDialog.h"
#include "dialogs/DeleteBranchDialog.h"
#include "dialogs/DeleteTagDialog.h"
#include "dialogs/MergeDialog.h"
#include "dialogs/ModifySubmoduleDialog.h"
#include "dialogs/NewBranchDialog.h"
#include "dialogs/RebaseConflictDialog.h"
#include "dialogs/RemoteDialog.h"
#include "dialogs/RenameBranchDialog.h"
#include "dialogs/SettingsDialog.h"
#include "dialogs/TagDialog.h"
#include "editor/TextEditor.h"
#include "git/Config.h"
#include "git/Id.h"
#include "git/Index.h"
#include "git/Rebase.h"
#include "git/RevWalk.h"
#include "git/Signature.h"
#include "git/SubmoduleAvailability.h"
#include "git/TagRef.h"
#include "git/Tree.h"
#include "git/Signature.h"
#include "git2/merge.h"
#include "util/PerformanceTrace.h"
#include "host/Accounts.h"
#include "index/Index.h"
#include "log/LogEntry.h"
#include "log/LogView.h"
#include "tools/ShowTool.h"
#include "watcher/RepositoryWatcher.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMenu>
#include <QtNetwork>
#include <QPaintEvent>
#include <QPushButton>
#include <QProcess>
#include <QStandardPaths>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QTimeLine>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <algorithm>
#include <QVBoxLayout>
#include <QtConcurrent>

#if defined(Q_OS_WIN)
#include <Windows.h>
#include <memory>
#endif

namespace {

const QString kSplitterKey = "reposplitter";
const QString kMsgFmt = "%1 - <span style='color: gray'>%2</span>";

QString msg(const git::Commit &commit) {
  QString summary = commit.summary(git::Commit::SubstituteEmoji);
  return kMsgFmt.arg(commit.link(), summary);
}

QString submoduleUpdateStateText(git::Submodule::UpdateStatus::State state) {
  using State = git::Submodule::UpdateStatus::State;

  switch (state) {
    case State::UpToDate:
      return QCoreApplication::translate("RepoView", "up-to-date");
    case State::UpdateAvailable:
      return QCoreApplication::translate("RepoView", "update available");
    case State::DifferentHistory:
      return QCoreApplication::translate("RepoView", "different history");
    case State::NotTracked:
      return QCoreApplication::translate("RepoView", "not branch-tracked");
    case State::Error:
      return QCoreApplication::translate("RepoView", "error");
    case State::Unknown:
      return QCoreApplication::translate("RepoView", "unknown");
  }

  return QString();
}

QString shortId(const git::Id &id) {
  return id.isValid() ? id.shortId() : QString();
}

class CheckoutCallbacks : public QObject,
                          public git::Repository::CheckoutCallbacks {
  Q_OBJECT

public:
  CheckoutCallbacks(LogEntry *log, int flags, QObject *parent = nullptr)
      : QObject(parent), mLog(log), mFlags(flags) {
    // Connect with automatic type.
    connect(this, &CheckoutCallbacks::queueNotify, this,
            &CheckoutCallbacks::notifyImpl);
  }

  QStringList conflicts() const { return mConflicts; }

  int flags() const override { return mFlags | GIT_CHECKOUT_NOTIFY_CONFLICT; }

  bool notify(char status, const QString &path) override {
    emit queueNotify(status, path);
    return true;
  }

  void progress(const QString &path, int current, int total) override {
    Q_UNUSED(path)

    // Add entries all at once.
    if (mLog && current == total && !mEntries.isEmpty())
      mLog->addEntries(mEntries);
  }

signals:
  void queueNotify(char status, const QString &path);

private:
  void notifyImpl(char status, const QString &path) {
    if (mLog) {
      LogEntry *entry = new LogEntry(LogEntry::File, path, QString());
      entry->setStatus(status);
      mEntries.append(entry);
    }

    if (status == '!')
      mConflicts.append(path);
  }

  LogEntry *mLog;
  int mFlags;

  QStringList mConflicts;
  QList<LogEntry *> mEntries;
};

class ScopedCollapse {
public:
  ScopedCollapse(LogView *view) : mView(view) {
    mView->setCollapseEnabled(false);
  }

  ~ScopedCollapse() { mView->setCollapseEnabled(true); }

private:
  LogView *mView;
};

} // namespace

RepoView::RepoView(const git::Repository &repo, MainWindow *parent)
    : QSplitter(Qt::Vertical, parent), mRepo(repo) {
  PerformanceTrace::Span span("startup", "RepoView constructor",
                              repo.dir(false).path());
  setHandleWidth(0);
  setAttribute(Qt::WA_DeleteOnClose);
  mCloseCleanupTimer.setSingleShot(true);
  connect(&mCloseCleanupTimer, &QTimer::timeout, this,
          &RepoView::finishClosing);
  mActivityTimer.setInterval(100);
  connect(&mActivityTimer, &QTimer::timeout, this, &RepoView::updateActivity);
  mActivityTimer.start();

  // Start (or restart) indexing after the initial status check has completed.
  git::RepositoryNotifier *notifier = repo.notifier();
  connect(this, &RepoView::statusChanged, this,
          [this] { startIndexing(); });

  MenuBar *menuBar = MenuBar::instance(parent);
  connect(this, &RepoView::statusChanged, menuBar, &MenuBar::updateStash);
  connect(notifier, &git::RepositoryNotifier::stateChanged, menuBar,
          &MenuBar::updateBranch);
  connect(notifier, &git::RepositoryNotifier::rebaseInitError, this,
          &RepoView::rebaseInitError);
  connect(notifier, &git::RepositoryNotifier::rebaseAboutToRebase, this,
          &RepoView::rebaseAboutToRebase);
  connect(notifier, &git::RepositoryNotifier::rebaseCommitInvalid, this,
          &RepoView::rebaseCommitInvalid);
  connect(notifier, &git::RepositoryNotifier::rebaseFinished, this,
          &RepoView::rebaseFinished);
  connect(notifier, &git::RepositoryNotifier::rebaseCommitSuccess, this,
          &RepoView::rebaseCommitSuccess);
  connect(notifier, &git::RepositoryNotifier::rebaseConflict, this,
          &RepoView::rebaseConflict);

  ToolBar *toolBar = parent->toolBar();
  connect(this, &RepoView::statusChanged, toolBar, &ToolBar::updateStash);

  // Initialize index.
  mIndex = new Index(repo, this);
  SearchField *searchField = toolBar->searchField();
  connect(&mIndexer, &QProcess::started, this, [this, searchField] {
    QJsonObject fields;
    fields["pid"] = mIndexer.processId();
    PerformanceTrace::event("indexer", "started", mRepo.dir(false).path(),
                            fields);
    searchField->setPlaceholderText(tr("Indexing..."));
  });
  using Signal = void (QProcess::*)(int, QProcess::ExitStatus);
  auto signal = static_cast<Signal>(&QProcess::finished);
  connect(&mIndexer, signal, this,
           [this, searchField](int code, QProcess::ExitStatus status) {
             QJsonObject fields;
             fields["exitCode"] = code;
             fields["crashed"] = status == QProcess::CrashExit;
             PerformanceTrace::event("indexer", "finished",
                                     mRepo.dir(false).path(), fields);

             searchField->setPlaceholderText(tr("Search"));
            if (status == QProcess::CrashExit) {
              QString text =
                  tr("The indexer worker process crashed. If this problem "
                     "persists please contact us at <TODO: "
                     "replace.support@gitahead.com>.");
              addLogEntry(text, tr("Indexer Crashed"));
            }

            if (mRestartIndexer) {
              mRestartIndexer = false;
              startIndexing();
            }
          });

  // Forward indexer stderr. Read from stdout.
  mIndexer.setProcessChannelMode(QProcess::ForwardedErrorChannel);
  mIndexResetTimer.setSingleShot(true);
  mIndexResetTimer.setInterval(200);
  connect(&mIndexResetTimer, &QTimer::timeout, mIndex, &Index::reset);
  connect(&mIndexer, &QProcess::readyReadStandardOutput, this, [this] {
    mIndexer.readAllStandardOutput();
    // Index updates arrive in bursts. Reload once after the burst instead of
    // rereading the complete index for every notification.
    mIndexResetTimer.start();
  });

  // Initialize history.
  mHistory = new History(this);
  connect(mHistory, &History::changed, toolBar, &ToolBar::updateHistory);
  connect(mHistory, &History::changed, menuBar, &MenuBar::updateHistory);
  connect(this, &RepoView::statusChanged, [this](bool dirty) {
    if (!dirty)
      mHistory->clean();
  });

  mSideBar = new QWidget(this);
  QVBoxLayout *sidebarLayout = new QVBoxLayout(mSideBar);
  sidebarLayout->setContentsMargins(0, 0, 0, 0);
  sidebarLayout->setSpacing(0);

  QWidget *header = new QWidget(mSideBar);
  QVBoxLayout *headerLayout = new QVBoxLayout(header);
  headerLayout->setContentsMargins(4, 4, 4, 4);
  headerLayout->setSpacing(4);
  sidebarLayout->addWidget(header);

  // Hide references when commit list is filtered.
  connect(searchField, &QLineEdit::textChanged, header,
          [header](const QString &text) {
            header->setVisible(text.simplified().isEmpty());
          });

  // Create header tool bar.
  CommitToolBar *commitToolBar = new CommitToolBar(header);
  headerLayout->addWidget(commitToolBar);

  // Create reference list.
  mRefs = new ReferenceWidget(repo, ReferenceView::AllRefs, header);
  headerLayout->addWidget(mRefs);

  connect(mRefs, &ReferenceWidget::referenceChanged, menuBar,
          &MenuBar::updateBranch);

  // Select HEAD branch when it changes.
  connect(notifier, &git::RepositoryNotifier::referenceUpdated, this,
          [this](const git::Reference &ref, bool, bool refreshStatus) {
            if (ref.isValid() && ref.isHead()) {
              mCommits->suppressResetWalker(true);
              mRefs->select(ref, false);
              mCommits->suppressResetWalker(false);
              if (refreshStatus)
                mRepo.invalidateSubmoduleCache();
            }
          });

  // Create pathspec chooser.
  mPathspec = new PathspecWidget(repo, header);
  headerLayout->addWidget(mPathspec);

  mAvatarProvider = new CommitAvatarProvider(repo, this);

  // Create commit list.
  mCommits = new CommitList(mIndex, mAvatarProvider, mSideBar);
  sidebarLayout->addWidget(mCommits);

  connect(commitToolBar, &CommitToolBar::settingsChanged, mCommits,
          &CommitList::resetSettings);
  connect(mRefs, &ReferenceWidget::referenceChanged, mCommits,
          &CommitList::setReference);
  connect(mRefs, &ReferenceWidget::referenceChanged, this,
          &RepoView::referenceChanged);
  connect(mRefs, &ReferenceWidget::referenceSelected, this,
          &RepoView::referenceSelected);
  connect(mRefs, &ReferenceWidget::referenceSelected, mCommits,
          &CommitList::selectReference);
  connect(mCommits, &CommitList::statusChanged, this, &RepoView::statusChanged);
  connect(this, &RepoView::statusChanged, this, &RepoView::finishInitialLoad);
  connect(mCommits, &CommitList::statusSelected, this,
          &RepoView::statusSelected);
  connect(mCommits, &CommitList::selectedRangeChanged, this,
          [this](const QString &range) {
            if (!mSelectingPendingCheckoutStatus &&
                !mPendingCheckoutRef.isEmpty() && range != "status")
              mPendingCheckoutRef.clear();
          });
  connect(mCommits, &CommitList::statusError, this,
           [this](const QString &error) {
             finishInitialLoad();
             LogEntry *entry = addLogEntry(QString(), tr("Status"));
            entry->addEntry(LogEntry::Error, error.toHtmlEscaped());
            setLogVisible(true);
          });

  // Respond to pathspec change.
  connect(mPathspec, &PathspecWidget::pathspecChanged, this,
          [this](const QString &pathspec) {
            git::Config config = mRepo.appConfig();
            mCommits->setPathspec(pathspec,
                                  config.value<bool>("index.enable", true));
          });

  // Respond to search query change.
  connect(searchField, &SearchField::textChanged, mCommits,
          &CommitList::setFilter);
  connect(mIndex, &Index::indexReset, this,
          [this, searchField] { mCommits->setFilter(searchField->text()); });

  mPrimaryView = new QStackedWidget(this);
  mPrimaryView->setObjectName("RepositoryPrimaryView");
  mPrimaryView->addWidget(mSideBar);

  mDetails = new DetailView(repo, mAvatarProvider, this);

  QFont bodyFont = mCommits->font();
  bodyFont.setPointSize(8);
  auto applyBodyFont = [&bodyFont](QWidget *widget) {
    if (!widget)
      return;
    const int pointSize = FontUtils::pointSize(bodyFont);
    widget->setStyleSheet(widget->styleSheet() +
                          QString("\nfont-size: %1pt;").arg(pointSize));
    QList<QWidget *> widgets = widget->findChildren<QWidget *>();
    widgets.prepend(widget);
    for (QWidget *child : widgets)
      child->setFont(FontUtils::copySize(child->font(), bodyFont));
    for (TextEditor *editor : widget->findChildren<TextEditor *>())
      editor->setFontPointSizeOverride(pointSize);
  };
  applyBodyFont(mDetails);
  applyBodyFont(mFileInspectionWidget);
  menuBar->setBodyFont(bodyFont);
  if (RepositoryNavigator *navigator =
          parent->findChild<RepositoryNavigator *>())
    navigator->setBodyFont(bodyFont);

  // Respond to diff/tree mode change.
  connect(mDetails, &DetailView::viewModeChanged, this,
          [this](ViewMode mode, bool spontaneous) {
            if (mode != DoubleTree) {
              if (mMaximized && !mDetails->isVisible()) {
                MenuBar *menuBar = MenuBar::instance(this);
                if (menuBar && menuBar->isMaximized())
                  menuBar->setMaximized(false);
                else
                  detailSplitterMaximize(false);
              }
              setFileInspectionVisible(false);
            }

            // Update interface.
            this->toolBar()->updateView();
            MenuBar::instance(this)->updateView();

            // Fake a commit list selection change.
            mCommits->resetSelection(spontaneous);
          });

  // Respond to commit list selection change.
  connect(mCommits, &CommitList::diffSelected, this, &RepoView::diffSelected,
          Qt::ConnectionType::DirectConnection);

  // Refresh the diff when a whole directory is added to the index.
  // FIXME: This is a workaround.
  connect(notifier, &git::RepositoryNotifier::directoryStaged, this,
          QOverload<>::of(&RepoView::refresh), Qt::QueuedConnection);
  connect(notifier, &git::RepositoryNotifier::directoryAboutToBeStaged, this,
          [this](const QString &dir, int count, bool &allow) {
            if (!Settings::instance()->prompt(Prompt::Kind::Directories))
              return;

            QString title = tr("Stage Directory?");
            QString text = tr("Are you sure you want to stage '%1'?");
            QString info = tr("This will result in the addition of %1 files.");
            QString arg =
                (count < 0) ? tr("more than 100") : QString::number(count);
            QMessageBox dialog(QMessageBox::Question, title, text.arg(dir),
                               QMessageBox::Cancel, this);
            dialog.setInformativeText(info.arg(arg));
            QPushButton *button = dialog.addButton(tr("Stage Directory"),
                                                   QMessageBox::AcceptRole);

            QString cbText = tr("Stop prompting to stage directories");
            QCheckBox *cb = new QCheckBox(cbText, &dialog);
            dialog.setCheckBox(cb);

            dialog.exec();
            allow = (dialog.clickedButton() == button);
            if (cb->isChecked())
              Settings::instance()->setPrompt(Prompt::Kind::Directories, false);
          });

  // large file size warning
  connect(notifier, &git::RepositoryNotifier::largeFileAboutToBeStaged, this,
          [this](const QString &file, int size, bool &allow) {
            if (!Settings::instance()->prompt(Prompt::Kind::LargeFiles))
              return;

            QString title = tr("Stage Large File?");
            QString fmt =
                tr("Are you sure you want to stage '%1' with a size of %2?");
            QString text = fmt.arg(file, locale().formattedDataSize(size));
            QMessageBox dialog(QMessageBox::Question, title, text,
                               QMessageBox::Cancel, this);
            QPushButton *stage =
                dialog.addButton(tr("Stage"), QMessageBox::AcceptRole);

            QPushButton *track = nullptr;
            if (this->repo().lfsIsInitialized()) {
              track = dialog.addButton(tr("Track with LFS"),
                                       QMessageBox::RejectRole);
              dialog.setInformativeText(
                  tr("This repository has LFS enabled. Do you "
                     "want to track the file with LFS instead?"));
            }

            QString cbText = tr("Stop prompting to stage large files");
            QCheckBox *cb = new QCheckBox(cbText, &dialog);
            dialog.setCheckBox(cb);

            dialog.exec();
            allow = (dialog.clickedButton() == stage);
            if (cb->isChecked())
              Settings::instance()->setPrompt(Prompt::Kind::LargeFiles, false);

            if (dialog.clickedButton() == track)
              configureSettings(ConfigDialog::Lfs);
          });

  // Refresh when the workdir changes.
  RepositoryWatcher *watcher = new RepositoryWatcher(repo, this);
  connect(notifier, &git::RepositoryNotifier::workdirChanged, this,
          [this] {
            mCommits->preserveSelectionOnRefresh();
            refresh(true);
          });
  connect(notifier, &git::RepositoryNotifier::referenceUpdated, watcher,
          &RepositoryWatcher::cancelPendingNotification);

  mDetailSplitter = new QSplitter(Qt::Horizontal, this);
  mDetailSplitter->setChildrenCollapsible(false);
  mDetailSplitter->setHandleWidth(0);
  mDetailSplitter->addWidget(mPrimaryView);
  mDetailSplitter->addWidget(mDetails);
  mDetailSplitter->setStretchFactor(0, 1);
  mDetailSplitter->setStretchFactor(1, 2);
  connect(mDetailSplitter, &QSplitter::splitterMoved, this, [this] {
    QSettings().setValue(kSplitterKey, mDetailSplitter->saveState());
  });

  // Create log.
  mLogRoot = new LogEntry(this);
  mLogPanel = new QWidget(this);
  mLogPanel->setObjectName("RepositoryLogPanel");
  mLogHeader = new QFrame(mLogPanel);
  mLogHeader->setObjectName("RepositoryLogHeader");
  mLogHeader->setFixedHeight(24);
  mLogHeader->setFrameShape(QFrame::StyledPanel);

  QLabel *logTitle = new QLabel(tr("Log"), mLogHeader);
  mLogToggle = new QToolButton(mLogHeader);
  mLogToggle->setObjectName("RepositoryLogToggle");
  mLogToggle->setAutoRaise(true);
  connect(mLogToggle, &QToolButton::clicked, this,
          [this] { setLogVisible(!isLogVisible()); });

  QHBoxLayout *logHeaderLayout = new QHBoxLayout(mLogHeader);
  logHeaderLayout->setContentsMargins(8, 0, 4, 0);
  logHeaderLayout->addWidget(logTitle);
  logHeaderLayout->addStretch();
  logHeaderLayout->addWidget(mLogToggle);

  mLogView = new LogView(mLogRoot, mLogPanel);
  connect(mLogView, &LogView::linkActivated, this, &RepoView::visitLink);
  connect(mLogView, &LogView::operationCanceled, this,
          &RepoView::cancelRemoteTransfer);

  QVBoxLayout *logLayout = new QVBoxLayout(mLogPanel);
  logLayout->setContentsMargins(0, 0, 0, 0);
  logLayout->setSpacing(0);
  logLayout->addWidget(mLogHeader);
  logLayout->addWidget(mLogView);
  mLogPanel->setMinimumHeight(mLogHeader->height());

  QShortcut *esc = new QShortcut(tr("Esc"), mLogView);
  esc->setContext(Qt::WidgetWithChildrenShortcut);
  connect(esc, &QShortcut::activated, mLogView,
          [this] { setLogVisible(false); });

  connect(notifier, &git::RepositoryNotifier::indexStageError, this,
          [this] { error(mLogRoot, tr("stage")); });

  QObject *context = new QObject(this);
  connect(notifier, &git::RepositoryNotifier::lfsNotFound, context,
          [this, context] {
            QString text = tr("Git LFS was not found on the PATH. "
                              "<a href='https://git-lfs.github.com'>"
                              "Install Git LFS</a> to use LFS integration.");
            mLogRoot->addEntry(LogEntry::Error, text);
            delete context; // Disconnect after the first error.
          });

  addWidget(mDetailSplitter);
  addWidget(mLogPanel);
  setCollapsible(0, false);
  setCollapsible(1, false);
  setStretchFactor(0, 1);
  mIsLogVisible = false;
  mLogContentHeight = mLogView->sizeHint().height();
  updateLogToggle();
  setSizes({1, mLogHeader->height()});

  connect(this, &QSplitter::splitterMoved, this, [this] {
    int contentHeight = sizes().last() - mLogHeader->height();
    bool visible = contentHeight > 0;
    if (visible)
      mLogContentHeight = contentHeight;
    if (visible == mIsLogVisible)
      return;

    mIsLogVisible = visible;
    updateLogToggle();
    this->toolBar()->updateView();
    MenuBar::instance(this)->updateView();
  });

  // Restore splitter state.
  mDetailSplitter->restoreState(QSettings().value(kSplitterKey).toByteArray());

  // Connect automatic fetch timer.
  connect(&mFetchTimer, &QTimer::timeout, this,
          [this] { fetch(git::Remote(), false, false); });
}

void RepoView::diffSelected(const git::Diff diff, const QString &file,
                            bool spontaneous) {
  git::Diff diff2 = diff;
  mHistory->update(diff.isValid() ? location() : Location(),
                   spontaneous); // TODO: why this changes diff?
  mDetails->setDiff(diff2, file, mPathspec->pathspec());
}

void RepoView::statusSelected(const git::WorkingTreeStatusSnapshot status,
                              const QString &file, bool spontaneous) {
  mHistory->update(location(), spontaneous);
  mDetails->setWorkingTreeStatus(status, file);
}

RepoView::~RepoView() {
  cancelIndexing();

  // Work around crash caused by clearing focus from the commit list
  // when it's destroyed. If it gets destroyed after the detail view
  // then the focus change may trigger the menu bar to query the mode
  // index from the already destroyed detail view.
  mCommits->clearFocus();
}

void RepoView::clean(const QStringList &untracked) {
  QString singular = tr("untracked file");
  QString plural = tr("untracked files");
  QString phrase = (untracked.count() == 1) ? singular : plural;
  QMessageBox *mb = new QMessageBox(
      QMessageBox::Warning, tr("Remove Untracked Files"),
      tr("Remove %1 %2?").arg(QString::number(untracked.count()), phrase),
      QMessageBox::Cancel, this);
  mb->setAttribute(Qt::WA_DeleteOnClose);
  mb->setInformativeText(tr("This action cannot be undone."));
  mb->setDetailedText(untracked.join('\n'));

  QPushButton *remove = mb->addButton(tr("Remove"), QMessageBox::AcceptRole);
  remove->setObjectName("RemoveButton");
  mb->setDefaultButton(remove);

  connect(remove, &QPushButton::clicked, [this, untracked] {
    for (const QString &name : untracked)
      repo().clean(name);
  });

  mb->show();
}

void RepoView::selectHead() { mRefs->select(mRepo.head()); }

void RepoView::selectFirstCommit() { mCommits->selectFirstCommit(); }

void RepoView::commit(bool force) { mDetails->commit(force); }

bool RepoView::isCommitEnabled() const { return mDetails->isCommitEnabled(); }

void RepoView::stage() { mDetails->stage(); }

bool RepoView::isStageEnabled() const { return mDetails->isStageEnabled(); }

void RepoView::unstage() { mDetails->unstage(); }

bool RepoView::isUnstageEnabled() const { return mDetails->isUnstageEnabled(); }

RepoView::ViewMode RepoView::viewMode() const { return mDetails->viewMode(); }

void RepoView::setViewMode(ViewMode mode) { mDetails->setViewMode(mode, true); }

void RepoView::setFileInspectionWidget(QWidget *widget) {
  if (!widget || mFileInspectionWidget)
    return;

  mFileInspectionWidget = widget;
  mPrimaryView->addWidget(widget);
}

void RepoView::setFileInspectionVisible(bool visible) {
  if (!mFileInspectionWidget)
    return;

  if (visible && mMaximized && !mPrimaryView->isVisible()) {
    MenuBar *menuBar = MenuBar::instance(this);
    if (menuBar && menuBar->isMaximized())
      menuBar->setMaximized(false);
    else
      detailSplitterMaximize(false);
  }
  mPrimaryView->setCurrentWidget(visible ? mFileInspectionWidget : mSideBar);
}

bool RepoView::isFileInspectionVisible() const {
  return mFileInspectionWidget &&
         mPrimaryView->currentWidget() == mFileInspectionWidget;
}

bool RepoView::isWorkingDirectoryDirty() const {
  // FIXME: Add option to stash untracked files?
  return mCommits->hasTrackedStatusChanges();
}

git::Reference RepoView::reference() const { return mRefs->currentReference(); }

void RepoView::selectReference(const git::Reference &ref) {
  mRefs->select(ref);
}

void RepoView::navigateToReference(const git::Reference &ref) {
  mPendingCheckoutRef.clear();
  mSelectingPendingCheckoutStatus = false;
  mRefs->select(ref);
  mCommits->selectReference(ref);
  emit referenceSelected(ref);
}

void RepoView::selectStash(int index) {
  mPendingCheckoutRef.clear();
  mSelectingPendingCheckoutStatus = false;
  QList<git::Commit> stashes = mRepo.stashes();
  if (index < 0 || index >= stashes.size())
    return;
  mRefs->select(mRepo.stashRef());
  mCommits->selectRange(stashes.at(index).id().toString());
}

QList<git::Commit> RepoView::commits() const {
  return mCommits->selectedCommits();
}

git::Diff RepoView::diff() const { return mCommits->selectedDiff(); }

git::Tree RepoView::tree() const {
  QList<git::Commit> commits = mCommits->selectedCommits();
  if (!commits.isEmpty())
    return commits.first().tree();

  git::Diff diff = mCommits->selectedDiff();
  return diff.isValid() ? mRepo.index().writeTree() : git::Tree();
}

void RepoView::cancelRemoteTransfer() {
  if (!mCallbacks && !mSubmoduleUpdateCallbacks)
    return;

  if (mCallbacks)
    mCallbacks->setCanceled(true);
  if (mSubmoduleUpdateCallbacks)
    mSubmoduleUpdateCallbacks->setCanceled(true);
}

void RepoView::cancelBackgroundTasks() {
  cancelIndexing();
  cancelRemoteTransfer();
  mCommits->cancelStatus();
  mDetails->cancelBackgroundTasks();
}

void RepoView::visitLink(const QString &link) {
  ScopedCollapse collapse(mLogView);
  (void)collapse;

  QUrl url(link);
  QUrlQuery query(url.query());

  if (url.scheme() == "http" || url.scheme() == "https")
    QDesktopServices::openUrl(url);

  // Lookup reference.
  git::Reference ref;
  QString refName = query.queryItemValue("ref");
  if (!refName.isEmpty())
    ref = mRepo.lookupRef(refName);

  // commit id
  if (url.scheme() == "id") {
    if (ref.isValid())
      mRefs->select(ref);
    mCommits->selectRange(url.path(), query.queryItemValue("file"), true);
    return;
  }

  // submodule
  if (url.scheme() == "submodule") {
    openSubmodule(mRepo.lookupSubmodule(url.path()));
    return;
  }

  // actions
  if (url.scheme() != "action")
    return;

  QString action = url.path();
  if (action == "pull") {
    pull();
    return;
  }

  if (action == "push") {
    git::Remote remote;

    QString value = query.queryItemValue("to");
    if (!value.isEmpty())
      remote = mRepo.lookupRemote(value);

    if (query.queryItemValue("force") == "true") {
      promptToForcePush(remote, ref);
    } else {
      bool setUpstream = query.queryItemValue("set-upstream") == "true";
      push(remote, ref, QString(), setUpstream);
    }

    return;
  }

  if (action == "push-to") {
    RemoteDialog *dialog = new RemoteDialog(RemoteDialog::Push, this);
    dialog->open();
  }

  if (action == "add-remote") {
    ConfigDialog *dialog = configureSettings(ConfigDialog::Remotes);
    dialog->addRemote(query.queryItemValue("name"));
    return;
  }

  if (action == "stash") {
    promptToStash();
    return;
  }

  if (action == "unstash") {
    popStash();
    return;
  }

  if (action == "checkout") {
    if (ref.isValid()) {
      checkout(ref, query.queryItemValue("detach") == "true");
    } else {
      promptToCheckout();
    }

    return;
  }

  if (action == "fast-forward") {
    merge(FastForward, ref);
    return;
  }

  // Check for no-ff flag.
  MergeFlags flags;
  if (query.queryItemValue("no-ff") == "true")
    flags |= NoFastForward;

  if (action == "merge") {
    merge(flags | Merge, ref);
    return;
  }

  if (action == "rebase") {
    merge(flags | Rebase, ref);
    return;
  }

  if (action == "config") {
    if (query.queryItemValue("global") == "true") {
      SettingsDialog::openSharedInstance();
    } else {
      configureSettings(ConfigDialog::General);
    }

    return;
  }

  if (action == "amend") {
    amendCommit();
    return;
  }

  if (action == "revert") {
    revert(mRepo.lookupCommit(query.queryItemValue("id")));
    return;
  }

  if (action == "cherry-pick") {
    cherryPick(mRepo.lookupCommit(query.queryItemValue("id")));
    return;
  }

  if (action == "abort") {
    mergeAbort();
    return;
  }

  if (action == "sslverifyrepo") {
    if (mRepo.isValid()) {
      git::Config config = mRepo.gitConfig();
      config.setValue<bool>("http.sslVerify", false);
      QMessageBox msg(QMessageBox::Icon::Information, tr("Certificate Error"),
                      tr("SSL verification disabled for this repository"),
                      QMessageBox::Button::Ok);
      msg.setDetailedText(tr("[http]\n"
                             "  sslVerify = false\n\n"
                             "was added to %1/config")
                              .arg(mRepo.commonDir().path()));
      msg.exec();
    }
    return;
  }

  if (action == "sslverifygit") {
    git::Config config = git::Config::global();
    if (config.isValid()) {
      config.setValue<bool>("http.sslVerify", false);
      QMessageBox msg(QMessageBox::Icon::Information, tr("Certificate Error"),
                      tr("SSL verification disabled for all git repositories"),
                      QMessageBox::Button::Ok);
      msg.setDetailedText(tr("[http]\n"
                             "  sslVerify = false\n\n"
                             "was added to %1")
                              .arg(config.globalPath()));
      msg.exec();
    }
    return;
  }
}

Repository *RepoView::remoteRepo() {
  if (mRemoteRepoCached)
    return mRemoteRepo;

  // Look up remote account repository.
  git::Remote remote = mRepo.defaultRemote();
  mRemoteRepo = remote ? Accounts::instance()->lookup(remote.url()) : nullptr;
  mRemoteRepoCached = true;

  if (mRemoteRepo) {
    auto err =
        connect(mRemoteRepo->account(), &Account::pullRequestError, this,
                [this](const QString &name, const QString &message) {
                  LogEntry *parent =
                      addLogEntry(tr("Pull Request"), tr("Create"));
                  error(parent, tr("create pull request"), name, message);
                });

    connect(mRemoteRepo, &Repository::destroyed, this, [this, err] {
      disconnect(err);
      mRemoteRepo = nullptr;
      mRemoteRepoCached = false;
    });
  }

  return mRemoteRepo;
}

void RepoView::lfsInitialize() {
  LogEntry *entry = addLogEntry(tr("Git LFS"), tr("Initialize"));
  if (!mRepo.lfsInitialize()) {
    error(entry, tr("initialize"));
    return;
  }

  entry->addEntry(LogEntry::File, tr("Git LFS initialized."));
}

void RepoView::lfsDeinitialize() {
  LogEntry *entry = addLogEntry(tr("Git LFS"), tr("Deinitialize"));
  if (!mRepo.lfsDeinitialize()) {
    error(entry, tr("deinitialize"));
    return;
  }

  entry->addEntry(LogEntry::File, tr("Git LFS Deinitialized."));
}

bool RepoView::lfsSetLocked(const QStringList &paths, bool lock) {
  QStringList errors;
  QString verb = lock ? tr("Lock") : tr("Unlock");

  foreach (const QString &path, paths) {
    if (!repo().lfsSetLocked(path, lock))
      errors.append(
          tr("Unable to %1 '%2' - %3")
              .arg(verb.toLower(), path, git::Repository::lastError()));
  }

  if (errors.isEmpty())
    return true;

  LogEntry *entry = addLogEntry(tr("Git LFS"), verb);
  foreach (const QString &error, errors)
    entry->addEntry(LogEntry::Error, error);

  return false;
}

Location RepoView::location() const {
  git::Reference ref = mRefs->currentReference();
  if (!ref.isValid())
    return Location();

  QString name = ref.qualifiedName();
  RepoView::ViewMode mode = mDetails->viewMode();
  return Location(mode, name, mCommits->selectedRange(), mDetails->file());
}

void RepoView::setLocation(const Location &location) {
  mDetails->setViewMode(location.mode(), false);
  mCommits->selectRange(location.id(), location.file());

  if (git::Reference ref = mRepo.lookupRef(location.ref())) {
    if (git::Reference current = mRefs->currentReference()) {
      if (ref.qualifiedName() != current.qualifiedName())
        mRefs->select(ref);
    }
  }
}

void RepoView::find() { mDetails->find(); }

void RepoView::findNext() { mDetails->findNext(); }

void RepoView::findPrevious() { mDetails->findPrevious(); }

void RepoView::startIndexing() {
  if (mClosing)
    return;

  if (!mRepo.appConfig().value<bool>("index.enable", true))
    return;

  if (mIndexer.state() != QProcess::NotRunning) {
    mRestartIndexer = true;
    return;
  }

  QStringList args = {"--notify", "--background", mRepo.dir().path()};
  if (Index::isLoggingEnabled())
    args.prepend("--log");

  QDir dir(QCoreApplication::applicationDirPath());
#ifdef WIN32
  auto indexer_cmd = dir.filePath("gitnortek-indexer.exe");
#else
  auto indexer_cmd = dir.filePath("gitnortek-indexer");
#endif
  QFileInfo check_file(indexer_cmd);
  if (!check_file.isFile()) {
    Debug("No indexer found: " << indexer_cmd);
  }
  PerformanceTrace::event("indexer", "start requested", mRepo.dir(false).path());
  mIndexer.start(indexer_cmd, args);
}

void RepoView::cancelIndexing() {
  mRestartIndexer = false;
  if (mIndexer.state() == QProcess::NotRunning)
    return;

  const qint64 processId = mIndexer.processId();
  QJsonObject fields;
  fields["pid"] = processId;
  PerformanceTrace::event("indexer", "terminate requested",
                          mRepo.dir(false).path(), fields);
  mIndexer.terminate();
  QTimer::singleShot(1000, &mIndexer, [this, processId] {
    if (mIndexer.state() != QProcess::NotRunning &&
        mIndexer.processId() == processId) {
      QJsonObject fields;
      fields["pid"] = processId;
      PerformanceTrace::event("indexer", "kill requested",
                              mRepo.dir(false).path(), fields);
      mIndexer.kill();
    }
  });
}

bool RepoView::isLogVisible() const { return mIsLogVisible; }

void RepoView::updateLogToggle() {
  mLogToggle->setArrowType(mIsLogVisible ? Qt::DownArrow : Qt::UpArrow);
  QString text = mIsLogVisible ? tr("Hide Log") : tr("Show Log");
  mLogToggle->setAccessibleName(text);
  mLogToggle->setToolTip(text);
}

void RepoView::setLogVisible(bool visible) {
  if (visible == mIsLogVisible)
    return;

  int currentHeight = sizes().last();
  int headerHeight = mLogHeader->height();
  if (!visible && currentHeight > headerHeight)
    mLogContentHeight = currentHeight - headerHeight;

  mIsLogVisible = visible;
  updateLogToggle();

  // Update interface.
  toolBar()->updateView();
  MenuBar::instance(this)->updateView();

  int targetHeight = headerHeight + (visible ? qMax(1, mLogContentHeight) : 0);

  QTimeLine *timeline = new QTimeLine(250, this);
  timeline->setEasingCurve(QEasingCurve(QEasingCurve::Linear));
  timeline->setUpdateInterval(20);

  connect(timeline, &QTimeLine::valueChanged, this,
          [this, currentHeight, targetHeight](qreal value) {
            int height =
                currentHeight +
                static_cast<int>((targetHeight - currentHeight) * value);
            setSizes({1, height});
          });

  connect(timeline, &QTimeLine::finished, this, [this, timeline, targetHeight] {
    setSizes({1, targetHeight});
    timeline->deleteLater();
  });

  timeline->start();
}

LogEntry *RepoView::addLogEntry(const QString &text, const QString &title,
                                LogEntry *parent) {
  LogEntry *root = parent ? parent : mLogRoot;
  return root->addEntry(text, title);
}

void RepoView::reportDiagnostics() {
  struct Command {
    QString program;
    QStringList arguments;
    QString display;
  };

  const QList<Command> commands = {
      {"pwd", {}, "pwd"},
      {"git",
       {"rev-parse", "--show-toplevel"},
       "git rev-parse --show-toplevel"},
      {"git",
       {"status", "--porcelain=v1", "--branch"},
       "git status --porcelain=v1 --branch"},
      {"git", {"rev-parse", "HEAD"}, "git rev-parse HEAD"},
  };

  LogEntry *root = addLogEntry(mRepo.workdir().absolutePath().toHtmlEscaped(),
                               tr("Repository Diagnostics"));
  const QString workdir = mRepo.workdir().absolutePath();
  for (const Command &command : commands) {
    LogEntry *entry = root->addEntry(QString(), "$ " + command.display);
    const QString executable = QStandardPaths::findExecutable(command.program);
    if (executable.isEmpty()) {
      entry->addEntry(LogEntry::Error,
                      tr("Unable to find %1").arg(command.program));
      continue;
    }

    QProcess process;
    process.setWorkingDirectory(workdir);
    process.start(executable, command.arguments);
    if (!process.waitForStarted(5000)) {
      entry->addEntry(LogEntry::Error, process.errorString().toHtmlEscaped());
      continue;
    }

    if (!process.waitForFinished(10000)) {
      process.kill();
      process.waitForFinished();
      entry->addEntry(LogEntry::Error, tr("Command timed out"));
      continue;
    }

    auto appendOutput = [entry](const QByteArray &output, LogEntry::Kind kind) {
      QString text = QString::fromLocal8Bit(output);
      const QStringList lines = text.split('\n');
      for (const QString &line : lines) {
        if (!line.isEmpty())
          entry->addEntry(kind, line.toHtmlEscaped());
      }
    };
    appendOutput(process.readAllStandardOutput(), LogEntry::File);
    appendOutput(process.readAllStandardError(), LogEntry::Error);

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode()) {
      entry->addEntry(LogEntry::Error,
                      tr("Exited with code %1").arg(process.exitCode()));
    }
  }

  setLogVisible(true);
}

LogEntry *RepoView::error(LogEntry *parent, const QString &action,
                          const QString &name, const QString &defaultError) {
  QString detail = git::Repository::lastError(defaultError);
  QString text = name.isEmpty()
                     ? tr("Unable to %1 - %2").arg(action, detail)
                     : tr("Unable to %1 '%2' - %3").arg(action, name, detail);

  QStringList items = text.split("\\n", Qt::KeepEmptyParts);
  if (items.last() == "\n")
    items.removeLast();

  LogEntry *root = parent->addEntry(LogEntry::Error, items.takeFirst());
  foreach (const QString &item, items)
    root->addEntry(LogEntry::File, item);

  return root;
}

void RepoView::startFetchTimer() {
  mFetchTimer.stop();

  // Read defaults from global settings.
  Settings *settings = Settings::instance();
  bool enable = settings->value(Setting::Id::FetchAutomatically).toBool();
  int minutes =
      settings->value(Setting::Id::AutomaticFetchPeriodInMinutes).toInt();

  git::Config config = mRepo.appConfig();
  if (!config.value<bool>("autofetch.enable", enable))
    return;

  bool prune = settings->value(Setting::Id::PruneAfterFetch).toBool();
  fetch(git::Remote(), false, false, nullptr, nullptr,
        config.value<bool>("autoprune.enable", prune));

  mFetchTimer.start(config.value<int>("autofetch.minutes", minutes) * 60000);
}

void RepoView::fetchAll() {
  QList<git::Remote> remotes = mRepo.remotes();
  if (remotes.isEmpty())
    return;

  if (remotes.size() == 1) {
    fetch();
    return;
  }

  // Queue up all remotes to fetch them serially.
  QString text = tr("%1 remotes").arg(remotes.size());
  LogEntry *entry = addLogEntry(text, tr("Fetch All"));
  foreach (const git::Remote &remote, remotes)
    fetch(remote, false, true, entry);
}

QFuture<git::Result> RepoView::fetch(const git::Remote &rmt, bool tags,
                                     bool interactive, LogEntry *parent,
                                     QStringList *submodules) {
  bool prune =
      Settings::instance()->value(Setting::Id::PruneAfterFetch).toBool();
  return fetch(rmt, tags, interactive, parent, submodules,
               mRepo.appConfig().value<bool>("autoprune.enable", prune));
}

QFuture<git::Result> RepoView::fetch(const git::Remote &rmt, bool tags,
                                     bool interactive, LogEntry *parent,
                                     QStringList *submodules, bool prune) {
  if (mClosing)
    return QFuture<git::Result>();

  if (mWatcher) {
    // Queue fetch.
    connect(mWatcher, &QFutureWatcher<git::Result>::finished, mWatcher,
            [this, rmt, tags, interactive, parent, submodules, prune] {
              fetch(rmt, tags, interactive, parent, submodules, prune);
            });

    return QFuture<git::Result>();
  }

  // Fetch if there's a valid remote, even if HEAD is detached.
  QString title = tr("Fetch");
  git::Remote remote = rmt.isValid() ? rmt : mRepo.defaultRemote();
  QString text = remote.isValid() ? remote.name() : tr("<i>no remote</i>");
  LogEntry *entry = interactive ? addLogEntry(text, title, parent)
                                : new LogEntry(LogEntry::Entry, text,
                                               title); // Create unparented.

  if (!remote.isValid()) {
    QString err =
        tr("Unable to fetch. No upstream is configured for the current "
           "branch, and there isn't a remote called 'origin'.");
    entry->addEntry(LogEntry::Error, err);
    return QFuture<git::Result>();
  }

  mWatcher = new QFutureWatcher<git::Result>(this);
  connect(
      mWatcher, &QFutureWatcher<git::Result>::finished, mWatcher,
      [this, remote, entry, interactive, parent] {
        entry->setBusy(false);

        git::Result result = mWatcher->result();
        if (mCallbacks->isCanceled()) {
          entry->addEntry(LogEntry::Error, tr("Fetch canceled."));
        } else if (!result) {
          error(entry, tr("fetch from"), remote.name(), result.errorString());
          // Add ssl hint.
          if (result.error() == -GIT_ERROR_SSL) {
            git::Config config =
                mRepo.isValid() ? mRepo.gitConfig() : git::Config::global();
            if (config.value<bool>("http.sslVerify", true)) {
              QString ssl =
                  tr("You may disable ssl verification <a "
                     "href='action:sslverifyrepo'>for this repository</a> "
                     "or overall disable ssl verification <a "
                     "href='action:sslverifygit'>for all repositories</a>.");
              entry->addEntry(LogEntry::Hint, ssl);
            }
          }
        } else {
          mCallbacks->storeDeferredCredentials();
          if (entry->entries().isEmpty()) {
            entry->addEntry(tr("Everything up-to-date."));
          } else if (!interactive) {
            LogEntry *root = parent ? parent : mLogRoot;
            root->addEntries({entry});
          }
        }

        if (!entry->parent())
          delete entry;

        mWatcher->deleteLater();
        mWatcher = nullptr;
        mCallbacks = nullptr;
      });

  QString url = remote.url();
  mCallbacks = new RemoteCallbacks(RemoteCallbacks::Receive, entry, url,
                                   remote.name(), mWatcher, mRepo);
  connect(mCallbacks, &RemoteCallbacks::referenceUpdated, this,
          &RepoView::notifyReferenceUpdated);

  entry->setBusy(true);
  mWatcher->setFuture(
      QtConcurrent::run([this, remote, tags, submodules, prune] {
        git::Result result = git::Remote(remote).fetch(mCallbacks, tags, prune);

        if (result && submodules) {
          // Scan for unmodified submodules on the fetch thread.
          foreach (const git::Submodule &submodule, mRepo.submodules()) {
            if (GIT_SUBMODULE_STATUS_IS_UNMODIFIED(
                    mRepo.submoduleStatus(submodule.name())))
              submodules->append(submodule.name());
          }
        }

        return result;
      }));

  return mWatcher->future();
}

void RepoView::pull(MergeFlags flags, const git::Remote &rmt, bool tags,
                    bool prune) {
  if (mWatcher) {
    // Queue pull.
    connect(mWatcher, &QFutureWatcher<git::Result>::finished, mWatcher,
            [this, flags, rmt, tags] { pull(flags, rmt, tags); });

    return;
  }

  // Use default remote for fetch.
  git::Branch head = mRepo.head();
  git::Remote remote = rmt.isValid() ? rmt : mRepo.defaultRemote();

  QString headName = head.isValid() ? head.name() : tr("<i>no branch</i>");
  QString name = remote.isValid() ? remote.name() : tr("<i>no remote</i>");
  QString text = tr("%1 from %2").arg(headName, name);
  LogEntry *entry = addLogEntry(text, tr("Pull"));

  // Read submodule setting.
  QStringList *submodules = nullptr;
  Settings *settings = Settings::instance();
  bool enable =
      settings->value(Setting::Id::UpdateSubmodulesAfterPullAndClone).toBool();
  if (mRepo.appConfig().value<bool>("autoupdate.enable", enable))
    submodules = new QStringList;

  QFutureWatcher<git::Result> *watcher = new QFutureWatcher<git::Result>(this);
  connect(watcher, &QFutureWatcher<git::Result>::finished, watcher,
          [this, flags, entry, watcher, submodules] {
            // Copy submodule names.
            QStringList names;
            if (submodules) {
              names = *submodules;
              delete submodules;
            }

            watcher->deleteLater();
            if (!watcher->result())
              return;

            // Create callback to update submodules.
            std::function<void()> callback;
            if (!names.isEmpty()) {
              callback = [this, entry, names] {
                QList<git::Submodule> modules;
                foreach (const QString &name, names)
                  modules.append(mRepo.lookupSubmodule(name));
                updateSubmodules(modules, true, false, false, entry);
              };
            }

            // Merge the upstream of the HEAD branch.
            MergeFlags mf = flags;
            if (flags == Default) {
              // Read pull.rebase from config.
              git::Config config = mRepo.gitConfig();
              bool rebase = config.value<bool>("pull.rebase");

              // Read branch.<name>.rebase from config.
              if (git::Branch head = mRepo.head()) {
                QString key = QString("branch.%1.rebase").arg(head.name());
                rebase = config.value<bool>(key, rebase);
              }

              mf = rebase ? Rebase : Merge;
            }

            merge(mf, git::Reference(), git::AnnotatedCommit(), entry,
                  callback);
          });

  watcher->setFuture(fetch(remote, tags, true, entry, submodules, prune));
  if (watcher->isCanceled()) {
    delete watcher;
    delete submodules;
  }
}

void RepoView::merge(MergeFlags flags, const git::Reference &ref,
                     const git::AnnotatedCommit &commit, LogEntry *parent,
                     const std::function<void()> &callback) {
  DebugRefresh("");
  git::Reference head = mRepo.head();

  git::AnnotatedCommit upstream;
  QString upstreamName = tr("<i>no upstream</i>");
  if (commit.isValid()) {
    upstream = commit;
    upstreamName = commit.commit().link();
  } else if (ref.isValid()) {
    upstream = ref.annotatedCommit();
    upstreamName = ref.name();
  } else if (head.isValid() && head.isBranch()) {
    git::Branch headBranch = head;
    upstream = headBranch.annotatedCommitFromFetchHead();
    git::Branch up = headBranch.upstream();
    if (up.isValid())
      upstreamName = up.name();
  }

  int analysis =
      upstream.isValid() ? upstream.analysis() : GIT_MERGE_ANALYSIS_NONE;
  bool ff = (analysis & GIT_MERGE_ANALYSIS_FASTFORWARD);
  bool noff = (flags & NoFastForward);
  bool ffonly = (flags & FastForward);
  bool squashflag = (flags & Squash);

  // Write log entry.
  QString title = tr("Merge");
  QString textFmt = tr("%1 into %2");
  if ((ffonly || (!noff && ff)) && !squashflag) {
    title = tr("Fast-forward");
    textFmt = tr("%2 to %1");
  } else if (flags & Rebase) {
    title = tr("Rebase");
    textFmt = tr("%2 on %1");
  }

  QString headName = head.isValid() ? head.name() : tr("<i>no branch</i>");
  QString text = textFmt.arg(upstreamName, headName);
  LogEntry *entry = addLogEntry(text, title, parent);

  // Empty repository.
  if (!head.isValid()) {
    entry->addEntry(LogEntry::Error, tr("The repository is empty."));
    return;
  }

  // Validate inputs.
  if (!upstream.isValid()) {
    entry->addEntry(
        LogEntry::Error,
        tr("The current branch '%1' has no upstream branch.").arg(headName));
    return;
  }

  // Choose strategy.
  if (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) {
    entry->addEntry(tr("Already up-to-date."));
    return;
  }

  if (!ff && ffonly && !squashflag) {
    entry->addEntry(LogEntry::Error, tr("Unable to fast-forward."));
    return;
  }

  if (ff && !noff && !squashflag) {
    fastForward(ref, upstream, entry, callback);
    return;
  }

  // FIXME: Handle unborn?
  if (!(analysis & GIT_MERGE_ANALYSIS_NORMAL))
    return;

  if (flags & Rebase) {
    rebase(upstream, entry);
    return;
  }

  if (squashflag) {
    squash(upstream, entry);
    return;
  }

  merge(flags, upstream, entry, callback);
}

void RepoView::fastForward(const git::Reference &ref,
                           const git::AnnotatedCommit &upstream,
                           LogEntry *parent,
                           const std::function<void()> &callback) {
  git::Reference head = mRepo.head();
  Q_ASSERT(head.isValid());

  git::Commit commit = upstream.commit();
  CheckoutCallbacks callbacks(parent, GIT_CHECKOUT_NOTIFY_UPDATED);
  if (!mRepo.checkout(commit, &callbacks)) {
    LogEntry *err = error(parent, tr("fast-forward"), head.name());
    foreach (const QString &path, callbacks.conflicts())
      err->addEntry(LogEntry::File, path)->setStatus('!');

    QUrlQuery query;
    if (ref.isValid())
      query.addQueryItem("ref", ref.qualifiedName());

    QUrl url("action:fast-forward");
    url.setQuery(query);

    // Add stash hint.
    QString stash =
        tr("You may be able to reconcile your changes with the conflicting "
           "files by <a href='action:stash'>stashing</a> before you "
           "<a href='%1'>fast-forward</a>. Then "
           "<a href='action:unstash'>unstash</a> to restore your changes.");
    err->addEntry(LogEntry::Hint, stash.arg(url.toString()));

    query.addQueryItem("no-ff", "true");
    url.setPath("merge");
    url.setQuery(query);

    // Add merge hint.
    QString merge =
        tr("If you want to create a new merge commit instead of fast-"
           "forwarding, you can <a href='%1'>merge without fast-forwarding "
           "</a> instead.");
    err->addEntry(LogEntry::Hint, merge.arg(url.toString()));

    return;
  }

  // Point head branch at the new commit.
  if (head.setTarget(commit, "pull: fast-forward").isValid() && callback)
    callback();
}

void RepoView::merge(MergeFlags flags, const git::AnnotatedCommit &upstream,
                     LogEntry *parent, const std::function<void()> &callback) {
  git::Reference head = mRepo.head();
  Q_ASSERT(head.isValid());

  // Try to merge.
  if (!mRepo.merge(upstream)) {
    LogEntry *err = error(parent, tr("merge"), head.name());

    // Add stash hint if the failure was because of uncommitted changes.
    QString msg = git::Repository::lastError();
    int kind = git::Repository::lastErrorKind();
    if (kind == GIT_ERROR_MERGE && msg.contains("overwritten by merge")) {
      QString text =
          tr("You may be able to rebase by <a href='action:stash'>stashing</a> "
             "before trying to <a href='action:merge'>merge</a>. Then "
             "<a href='action:unstash'>unstash</a> to restore your changes.");
      err->addEntry(LogEntry::Hint, text);
    }

    return;
  }

  // Check for conflicts.
  if (checkForConflicts(parent, tr("merge")))
    return;

  if (flags & NoCommit) {
    refresh(false);
    return;
  }

  // Read default message.
  QString msg = mRepo.message();
  if (Settings::instance()->prompt(Prompt::Kind::Merge)) {
    // Prompt to edit message.
    CommitDialog *dialog = new CommitDialog(msg, Prompt::Kind::Merge, this);
    connect(dialog, &QDialog::rejected, this,
            [this, parent] { mergeAbort(parent); });
    connect(dialog, &QDialog::accepted, this,
            [this, dialog, upstream, parent, callback] {
              if (commit(dialog->message(), upstream, parent) && callback)
                callback();
            });

    dialog->open();
    return;
  }

  // Automatically commit with the default message.
  if (commit(msg, upstream, parent) && callback)
    callback();
}

void RepoView::mergeAbort(LogEntry *parent) {
  // Make sure that the we're still merging.
  if (mRepo.state() == GIT_REPOSITORY_STATE_NONE)
    return;

  git::Reference head = mRepo.head();
  if (!head.isValid())
    return;

  git::Commit commit = head.target();
  if (!commit.isValid())
    return;

  bool ignoreWhitespace = Settings::instance()->isWhitespaceIgnored();

  QSet<QString> paths;
  git::Diff index =
      mRepo.diffTreeToIndex(commit.tree(), git::Index(), ignoreWhitespace);
  for (int i = 0; i < index.count(); ++i)
    paths.insert(index.name(i));

  QStringList conflicts;
  git::Diff workdir =
      mRepo.diffIndexToWorkdir(git::Index(), nullptr, ignoreWhitespace);
  for (int i = 0; i < workdir.count(); ++i) {
    QString name = workdir.name(i);
    if (workdir.status(i) != GIT_DELTA_CONFLICTED && paths.contains(name))
      conflicts.append(name);
  }

  if (!conflicts.isEmpty()) {
    LogEntry *parent = addLogEntry(tr("merge"), tr("Abort"));
    QString err = tr("Some merged files have unstaged changes");
    LogEntry *entry = error(parent, tr("abort merge"), QString(), err);
    foreach (const QString &conflict, conflicts)
      entry->addEntry(LogEntry::File, conflict)->setStatus('M');
    return;
  }

  int state = mRepo.state();
  if (!commit.reset(GIT_RESET_HARD, paths.values()))
    return;

  QString text = tr("merge");
  switch (state) {
    case GIT_REPOSITORY_STATE_REVERT:
    case GIT_REPOSITORY_STATE_REVERT_SEQUENCE:
      text = tr("revert");
      break;

    case GIT_REPOSITORY_STATE_CHERRYPICK:
    case GIT_REPOSITORY_STATE_CHERRYPICK_SEQUENCE:
      text = tr("cherry-pick");
      break;

    case GIT_REPOSITORY_STATE_REBASE:
    case GIT_REPOSITORY_STATE_REBASE_INTERACTIVE:
    case GIT_REPOSITORY_STATE_REBASE_MERGE:
      text = tr("rebase");
      break;
  }

  addLogEntry(text, tr("Abort"), parent);
  mDetails->setCommitMessage(QString());
  refresh(false);
}

void RepoView::abortRebase() {
  mRepo.rebaseAbort();
  mRebase = nullptr;
  refresh(false);
}

void RepoView::continueRebase() {
  if (!mRebase) {
    // Rebase operation was started externally so before going on with rebasing,
    // create a log entry
    mRebase = addLogEntry(tr(""), tr("Continue ongoing rebase"));
  }
  mRepo.rebaseContinue(mDetails->commitMessage());
}

void RepoView::rebase(const git::AnnotatedCommit &upstream, LogEntry *parent) {
  git::Branch head = mRepo.head();
  if (!head.isValid()) {
    addLogEntry(tr("Invalid head."), tr("Abort"), parent);
    return;
  }

  mRebase = parent;

  mRepo.rebase(upstream, mDetails->overrideUser(), mDetails->overrideEmail());
}

void RepoView::rebaseInitError() {
  const git::Branch head = mRepo.head();
  Q_ASSERT(head.isValid());
  LogEntry *err = error(mRebase, tr("rebase"), head.name());
  // Add stash hint if the failure was because of uncommitted changes.
  QString msg = git::Repository::lastError();
  int kind = git::Repository::lastErrorKind();
  if (kind == GIT_ERROR_REBASE && msg.contains("changes exist")) {
    QString text =
        tr("You may be able to rebase by <a href='action:stash'>stashing</a> "
           "before trying to <a href='action:rebase'>rebase</a>. Then "
           "<a href='action:unstash'>unstash</a> to restore your changes.");
    err->addEntry(LogEntry::Hint, text);
  }
  mRebase = nullptr;
}

void RepoView::rebaseCommitInvalid(const git::Rebase rebase) {
  const git::Branch head = mRepo.head();
  error(mRebase, tr("rebase"), head.name());
}

void RepoView::rebaseAboutToRebase(const git::Rebase rebase,
                                   const git::Commit before, int currIndex) {
  QString beforeText = before.link();
  const qulonglong totalOps = static_cast<qulonglong>(rebase.count());
  QString step = tr("%1/%2").arg(currIndex).arg(QString::number(totalOps));
  QString text = tr("%1 - %2").arg(step, beforeText);
  mRebase->addEntry(text, tr("Apply"));
}

void RepoView::rebaseConflict(const git::Rebase rebase) {
  if (mRebase) {
    mRebase->addEntry(tr("Please resolve conflicts before continue"),
                      tr("Conflict"));
    mDetails->setCommitMessage(rebase.commitToRebase()
                                   .message(git::Commit::SubstituteEmoji)
                                   .trimmed());
  }
  refresh(false);
}

void RepoView::rebaseCommitSuccess(const git::Rebase rebase,
                                   const git::Commit before,
                                   const git::Commit after, int currIndex) {
  QString beforeText = before.link();
  const qulonglong totalOps = static_cast<qulonglong>(rebase.count());
  QString step = tr("%1/%2").arg(currIndex).arg(QString::number(totalOps));
  auto *lastEntry = mRebase->lastEntry();
  if (lastEntry) {
    lastEntry->setText(
        (after == before)
            ? tr("%1 - %2 <i>already applied</i>").arg(step, beforeText)
            : tr("%1 - %2 as %3").arg(step, beforeText, msg(after)));
  }

  // Yield to the main event loop.
  // So the status of the rebase is shown
  // Without it, the rebase status will be shown at the end of the
  // rebase when the event loop will be processed
  QCoreApplication::processEvents();
}

void RepoView::rebaseFinished(const git::Rebase rebase) {
  QString text = tr("Rebase finished");
  mRebase->addEntry(text, tr("Rebase"));
  mRebase = nullptr;
}

void RepoView::squash(const git::AnnotatedCommit &upstream, LogEntry *parent) {
  git::Branch head = mRepo.head();
  Q_ASSERT(head.isValid());

  // Try to merge.
  if (!mRepo.merge(upstream)) {
    LogEntry *err = error(parent, tr("squash"), head.name());

    // Add stash hint if the failure was because of uncommitted changes.
    QString msg = git::Repository::lastError();
    int kind = git::Repository::lastErrorKind();
    if (kind == GIT_ERROR_MERGE && msg.contains("overwritten by merge")) {
      QString text =
          tr("You may be able to rebase by <a href='action:stash'>stashing</a> "
             "before trying to <a href='action:merge'>merge</a>. Then "
             "<a href='action:unstash'>unstash</a> to restore your changes.");
      err->addEntry(LogEntry::Hint, text);
    }

    return;
  }

  // Make squash effect.
  mRepo.cleanupState();

  // Check for conflicts.
  checkForConflicts(parent, tr("squash"));
}

void RepoView::revert(const git::Commit &commit) {
  if (!commit.isValid())
    return;

  QString link = commit.link();
  LogEntry *parent = addLogEntry(link, tr("Revert"));

  // FIXME: Report which files conflicted?
  if (!commit.revert()) {
    error(parent, tr("revert"), link);
    return;
  }

  // Check for conflicts.
  if (checkForConflicts(parent, tr("revert")))
    return;

  git::Signature committer = mRepo.defaultSignature(
      nullptr, mDetails->overrideUser(), mDetails->overrideEmail());

  QString id = commit.id().toString();
  QString summary = commit.summary();
  QString msg = tr("Revert \"%1\"\n\nThis reverts commit %2.").arg(summary, id);
  if (Settings::instance()->prompt(Prompt::Kind::Revert)) {
    // Prompt to edit message.
    CommitDialog *dialog = new CommitDialog(msg, Prompt::Kind::Revert, this);
    connect(dialog, &QDialog::rejected, this,
            [this, parent] { mergeAbort(parent); });
    connect(dialog, &QDialog::accepted, this,
            [this, dialog, parent, commit, committer] {
              // TODO: or doing it differently
              this->commit(commit.author(), committer, dialog->message(),
                           git::AnnotatedCommit(), parent);
            });

    dialog->open();
    return;
  }

  // Automatically commit with the default message.
  this->commit(commit.author(), committer, msg, git::AnnotatedCommit(), parent);
}

void RepoView::cherryPick(const git::Commit &commit) {
  if (!commit.isValid())
    return;

  QString link = commit.link();
  git::Branch head = mRepo.head();
  QString name = head.isValid() ? head.name() : tr("<i>detached HEAD</i>");
  QString text = tr("%1 on %2").arg(link, name);
  LogEntry *parent = addLogEntry(text, tr("Cherry-pick"));

  // FIXME: Report which files conflicted?
  if (!mRepo.cherryPick(commit)) {
    error(parent, tr("cherry-pick"), link);
    return;
  }

  // Check for conflicts.
  if (checkForConflicts(parent, tr("cherry-pick")))
    return;

  git::Signature committer = mRepo.defaultSignature(
      nullptr, mDetails->overrideUser(), mDetails->overrideEmail());

  QString msg = commit.message();
  if (Settings::instance()->prompt(Prompt::Kind::CherryPick)) {
    // Prompt to edit message.
    CommitDialog *dialog =
        new CommitDialog(msg, Prompt::Kind::CherryPick, this);
    connect(dialog, &QDialog::rejected, this,
            [this, parent] { mergeAbort(parent); });
    connect(dialog, &QDialog::accepted, this,
            [this, dialog, parent, commit, committer] {
              this->commit(commit.author(), committer, dialog->message(),
                           git::AnnotatedCommit(), parent);
            });

    dialog->open();
    return;
  }

  // Automatically commit with the default message.
  this->commit(commit.author(), committer, msg, git::AnnotatedCommit(), parent);
}

void RepoView::promptToForcePush(const git::Remote &remote,
                                 const git::Reference &src) {
  git::Remote targetRemote = remote.isValid() ? remote : mRepo.defaultRemote();
  git::Reference target = src.isValid() ? src : mRepo.head();
  if (!targetRemote.isValid() || !target.isValid()) {
    push(targetRemote, target, QString(), false, true);
    return;
  }

  QString title = tr("Force Push to %1?").arg(targetRemote.name());
  QString text = tr("Are you sure you want to force push?");
  QMessageBox *dialog = new QMessageBox(QMessageBox::Warning, title, text,
                                        QMessageBox::Cancel, this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);

  dialog->setInformativeText(
      tr("The remote will lose any commits that are reachable only from "
         "the overwritten reference. Dropped commits may be unexpectedly "
         "reintroduced by clones that already contain those commits locally."));

  QPushButton *accept =
      dialog->addButton(tr("Force Push"), QMessageBox::AcceptRole);
  connect(accept, &QPushButton::clicked, this, [this, targetRemote, target] {
    push(targetRemote, target, QString(), false, true);
  });

  dialog->open();
}

void RepoView::push(const git::Remote &rmt, const git::Reference &src,
                    const QString &dst, bool setUpstream, bool force, bool tags,
                    bool checkSubmodules) {
  if (mSubmodulePushCheckWatcher)
    return;

  if (mWatcher) {
    // Queue push.
    connect(mWatcher, &QFutureWatcher<git::Result>::finished, mWatcher,
            [this, rmt, src, dst, setUpstream, force, tags, checkSubmodules] {
              push(rmt, src, dst, setUpstream, force, tags, checkSubmodules);
            });

    return;
  }

  git::Reference ref = src.isValid() ? src : mRepo.head();
  QString refName = ref.isValid() ? ref.name() : tr("<i>no reference</i>");

  git::Remote remote = rmt.isValid() ? rmt : mRepo.defaultRemote();
  QString name = remote.isValid() ? remote.name() : tr("<i>no remote</i>");

  if (ref.isTag() && remote.isValid() &&
      remote.name() == QStringLiteral("origin")) {
    const QString key = originTagKey(ref);
    if (mValidatedOriginTagPushes.remove(key) == 0) {
      pushTagToOrigin(ref);
      return;
    }
  }

  git::Branch branch = ref;
  git::Branch upstream = branch ? branch.upstream() : git::Branch();
  git::Remote upstreamRemote = upstream ? upstream.remote() : git::Remote();
  if (upstreamRemote && upstreamRemote.name() != remote.name())
    upstream = git::Branch();

  if (branch.isValid() && remote.isValid() && !upstream.isValid() &&
      !setUpstream && !src.isValid()) {
    QString remoteBranchName = QString("%1/%2").arg(remote.name(), refName);
    git::Branch remoteBranch =
        mRepo.lookupBranch(remoteBranchName, GIT_BRANCH_REMOTE);
    QString title = remoteBranch.isValid() ? tr("Track Remote Branch?")
                                           : tr("Create Remote Branch?");
    QString text = remoteBranch.isValid()
                       ? tr("The local branch '%1' does not track '%2'.")
                             .arg(refName, remoteBranchName)
                       : tr("The branch '%1' does not exist on '%2'.")
                             .arg(refName, remote.name());
    QMessageBox *dialog = new QMessageBox(QMessageBox::Question, title, text,
                                          QMessageBox::Cancel, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setInformativeText(
        remoteBranch.isValid()
            ? tr("Track the existing remote branch and push to it?")
            : tr("Create '%1' and set it as the upstream branch?")
                  .arg(remoteBranchName));
    QPushButton *accept = dialog->addButton(
        remoteBranch.isValid() ? tr("Track and Push") : tr("Create Branch"),
        QMessageBox::AcceptRole);
    connect(accept, &QPushButton::clicked, this,
            [this, remote, ref, force, tags] {
              push(remote, ref, QString(), true, force, tags);
            });
    dialog->open();
    return;
  }

  QString title = !force ? tr("Push") : tr("Push (Force)");
  QString text = tr("%1 to %2").arg(refName, name);
  LogEntry *entry = addLogEntry(text, title);

  if (!ref.isValid()) {
    QString text = tr("You are not currently on a branch.");
    LogEntry *err = entry->addEntry(LogEntry::Error, text);
    if (mRepo.isHeadUnborn()) {
      QString hint = tr("Create a commit to add the default '%1' branch.");
      err->addEntry(LogEntry::Hint, hint.arg(mRepo.unbornHeadName()));
    } else {
      QString hint =
          tr("You can <a href='action:checkout'>checkout</a> a branch "
             "then <a href='action:push'>push</a> again, or "
             "<a href='action:push-to'>push to an explicit branch</a>.");
      err->addEntry(LogEntry::Hint, hint);
    }

    return;
  }

  if (!remote.isValid()) {
    QString text = tr("The current branch '%1' has no default remote.");
    LogEntry *err = entry->addEntry(LogEntry::Error, text.arg(refName));
    QString hint1 =
        tr("You may want to <a href='action:add-remote?name=origin'>add a "
           "remote "
           "named 'origin'</a>. Then <a href='action:push?set-upstream=true'>"
           "push and set the current branch's upstream</a> to begin tracking a "
           "remote branch called 'origin/%1'.")
            .arg(refName);
    QString hint2 =
        tr("You can also <a href='action:push-to'>push to an explicit URL</a> "
           "if you don't want to track a remote branch.");
    err->addEntry(LogEntry::Hint, hint1);
    err->addEntry(LogEntry::Hint, hint2);
    return;
  }

  QString unqualifiedName = !dst.isEmpty() ? dst : refName;
  QString remoteBranchName = QString("%1/%2").arg(name, unqualifiedName);
  if (!branch.isValid() && !setUpstream && !src.isValid()) {
    LogEntry *err = entry->addEntry(
        LogEntry::Error,
        tr("The current branch '%1' has no upstream branch.").arg(refName));
    QString hint1 = tr("To begin tracking a remote branch called '%1', "
                       "<a href='action:push?set-upstream=true'>push and "
                       "set the current branch's upstream</a>.")
                        .arg(remoteBranchName);
    QString hint2 = tr("To push without setting up tracking information, "
                       "<a href='action:push?ref=%1'>push '%2'</a> "
                       "explicitly.")
                        .arg(ref.qualifiedName(), refName);
    err->addEntry(LogEntry::Hint, hint1);
    err->addEntry(LogEntry::Hint, hint2);
    return;
  }

  if (!tags && upstream.isValid() && ref.target() == upstream.target() &&
      (dst.isEmpty() || dst == ref.qualifiedName())) {
    entry->addEntry(tr("Everything up-to-date."));
    return;
  }

  if (checkSubmodules) {
    git::Commit parent = ref.target();
    QList<git::Commit> parents{parent};
    if (tags) {
      foreach (const git::TagRef &tag, mRepo.tags()) {
        git::Commit commit = tag.target();
        if (commit.isValid() && commit != parent)
          parents.append(commit);
      }
    }
    QList<git::Submodule> submodules = mRepo.submodules();
    bool hasSubmoduleConfig = false;
    foreach (const git::Commit &commit, parents)
      hasSubmoduleConfig |= !commit.tree().id(".gitmodules").isNull();

    if (parent.isValid() && (!submodules.isEmpty() || hasSubmoduleConfig)) {
      LogEntry *checkEntry =
          entry->addEntry(tr("Checking submodule commit availability..."));
      checkEntry->setBusy(true);

      auto watcher =
          new QFutureWatcher<QList<git::SubmoduleAvailability::Issue>>(this);
      mSubmodulePushCheckWatcher = watcher;
      mCallbacks =
          new RemoteCallbacks(RemoteCallbacks::Receive, checkEntry,
                              remote.url(), remote.name(), watcher, mRepo);
      RemoteCallbacks *callbacks = mCallbacks;
      connect(
          watcher,
          &QFutureWatcher<QList<git::SubmoduleAvailability::Issue>>::finished,
          watcher,
          [this, watcher, callbacks, remote, src, ref, dst, setUpstream, force,
           tags, entry, remoteBranchName, checkEntry] {
            checkEntry->setBusy(false);
            QList<git::SubmoduleAvailability::Issue> issues = watcher->result();
            mSubmodulePushCheckWatcher = nullptr;
            mCallbacks = nullptr;
            watcher->deleteLater();

            if (callbacks->isCanceled()) {
              entry->addEntry(LogEntry::Error, tr("Push canceled."));
              return;
            }

            if (issues.isEmpty()) {
              callbacks->storeDeferredCredentials();
              pushRemote(remote, src, ref, dst, setUpstream, force, tags, entry,
                         remoteBranchName);
              return;
            }

            QStringList details;
            for (const git::SubmoduleAvailability::Issue &issue : issues) {
              QString detail = tr("%1 (%2)").arg(issue.name, issue.path);
              if (!issue.pinnedId.isNull())
                detail +=
                    tr("\nPinned commit: %1").arg(issue.pinnedId.toString());
              if (!issue.url.isEmpty())
                detail += tr("\nURL: %1").arg(issue.url);
              details.append(detail + "\n" + issue.message);
            }

            QMessageBox *dialog = new QMessageBox(
                QMessageBox::Warning, tr("Submodule Commits May Be Missing"),
                tr("One or more submodule commits cannot be proven available "
                   "from the URLs used by new clones."),
                QMessageBox::Cancel, this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setInformativeText(
                tr("New clones may be unable to check out the parent commit. "
                   "GitNortek will not push submodules automatically."));
            dialog->setDetailedText(details.join("\n\n"));
            QPushButton *accept = dialog->addButton(tr("Push Parent Anyway"),
                                                    QMessageBox::AcceptRole);
            dialog->setDefaultButton(QMessageBox::Cancel);
            dialog->setEscapeButton(QMessageBox::Cancel);
            connect(accept, &QPushButton::clicked, this,
                    [this, remote, src, ref, dst, setUpstream, force, tags,
                     entry, remoteBranchName] {
                      pushRemote(remote, src, ref, dst, setUpstream, force,
                                 tags, entry, remoteBranchName);
                    });
            connect(dialog, &QDialog::rejected, this, [this, entry] {
              entry->addEntry(LogEntry::Error, tr("Push canceled."));
            });
            dialog->open();
          });
      git::Repository repo = mRepo;
      watcher->setFuture(
          QtConcurrent::run([repo, parents, submodules, callbacks] {
            QList<git::SubmoduleAvailability::Issue> issues;
            QSet<QString> seen;
            foreach (const git::Commit &parent, parents) {
              foreach (const git::SubmoduleAvailability::Issue &issue,
                       git::SubmoduleAvailability::check(
                           repo, parent, submodules, callbacks)) {
                QString key = issue.path + issue.pinnedId.toString();
                if (!seen.contains(key)) {
                  seen.insert(key);
                  issues.append(issue);
                }
              }
            }
            return issues;
          }));
      return;
    }
  }

  pushRemote(remote, src, ref, dst, setUpstream, force, tags, entry,
             remoteBranchName);
}

QString RepoView::originTagKey(const git::Reference &tag) const {
  return tag.qualifiedName() + QLatin1Char('@') + tag.target().id().toString();
}

RepoView::OriginTagCheck *
RepoView::startOriginTagCheck(const git::Reference &tag) {
  const QString key = originTagKey(tag);
  OriginTagCheck &check = mOriginTagChecks[key];
  if (check.watcher) {
    return &check;
  }

  git::Remote origin = mRepo.lookupRemote(QStringLiteral("origin"));
  if (!origin.isValid())
    return &check;

  check.status = git::Remote::TagStatus::Unknown;
  ++check.generation;
  const quint64 generation = check.generation;
  auto *watcher = new QFutureWatcher<git::Remote::TagStatus>(this);
  check.watcher = watcher;
  connect(watcher, &QFutureWatcher<git::Remote::TagStatus>::finished, this,
          [this, key, generation, watcher] {
            auto it = mOriginTagChecks.find(key);
            if (it != mOriginTagChecks.end() && it->generation == generation &&
                it->watcher == watcher) {
              it->status = watcher->result();
              it->watcher = nullptr;
            }
            watcher->deleteLater();
          });
  watcher->setFuture(
      QtConcurrent::run([origin, tag] { return origin.tagStatus(tag); }));
  return &check;
}

void RepoView::pushTagToOrigin(const git::Reference &tag) {
  OriginTagCheck *check = startOriginTagCheck(tag);
  const QString key = originTagKey(tag);
  if (check->watcher) {
    connect(check->watcher, &QFutureWatcher<git::Remote::TagStatus>::finished,
            this, [this, tag] { pushTagToOrigin(tag); });
    return;
  }

  if (check->status == git::Remote::TagStatus::Pushable) {
    mValidatedOriginTagPushes.insert(key);
    push(mRepo.lookupRemote(QStringLiteral("origin")), tag);
    return;
  }

  LogEntry *entry = addLogEntry(tag.name(), tr("Push Tag to origin"));
  if (check->status == git::Remote::TagStatus::Present) {
    entry->addEntry(tr("The tag is already present on origin."));
  } else if (check->status == git::Remote::TagStatus::Conflict) {
    entry->addEntry(LogEntry::Error,
                    tr("Origin has a different tag with this name."));
  } else if (check->status == git::Remote::TagStatus::TargetLocalOnly) {
    entry->addEntry(LogEntry::Error,
                    tr("The tag's target commit is not reachable from origin."
                       " Push the commit to origin before pushing this tag."));
  } else {
    entry->addEntry(LogEntry::Error,
                    tr("Unable to validate the tag against origin."));
  }
}

void RepoView::pushRemote(const git::Remote &remote, const git::Reference &src,
                          const git::Reference &ref, const QString &dst,
                          bool setUpstream, bool force, bool tags,
                          LogEntry *entry, const QString &remoteBranchName) {
  mWatcher = new QFutureWatcher<git::Result>(this);
  connect(
      mWatcher, &QFutureWatcher<git::Result>::finished, mWatcher,
      [this, src, ref, setUpstream, remote, entry, remoteBranchName] {
        entry->setBusy(false);

        git::Result result = mWatcher->result();
        if (mCallbacks->isCanceled()) {
          entry->addEntry(LogEntry::Error, tr("Push canceled."));
        } else if (int err = result.error()) {
          QString name = remote.name();
          QString errString = result.errorString();
          LogEntry *errorEntry = error(entry, tr("push to"), name, errString);
          if (err == GIT_ENONFASTFORWARD) {
            if (ref.isTag()) {
              QString hint1 =
                  tr("The tag update may cause the remote to lose commits.");
              QString hint2 =
                  tr("If you want to risk the remote losing commits, you can "
                     "<a href='action:push?ref=%1&to=%2&force=true'>force "
                     "push</a>.");

              errorEntry->addEntry(LogEntry::Hint, hint1);
              errorEntry->addEntry(
                  LogEntry::Warning,
                  hint2.arg(ref.qualifiedName().toHtmlEscaped(),
                            name.toHtmlEscaped()));

            } else {
              QString hint1 =
                  tr("You may want to integrate remote commits first by "
                     "<a href='action:pull'>pulling</a>. Then "
                     "<a href='action:push?to=%1'>push</a> again.")
                      .arg(remote.name());
              QString hint2 =
                  tr("If you really want the remote to lose commits, you may "
                     "be able to <a href='action:push?to=%1&force=true'>force "
                     "push</a>.")
                      .arg(remote.name());
              errorEntry->addEntry(LogEntry::Hint, hint1);
              errorEntry->addEntry(LogEntry::Warning, hint2);
            }
          }
        } else {
          mCallbacks->storeDeferredCredentials();
          if (entry->entries().isEmpty())
            entry->addEntry(tr("Everything up-to-date."));

          if (setUpstream) {
            // Reset upstream unconditionally.
            git::Branch upstream =
                mRepo.lookupBranch(remoteBranchName, GIT_BRANCH_REMOTE);
            if (upstream.isValid()) {
              git::Branch head = src.isValid() ? src : mRepo.head();
              head.setUpstream(upstream);
            }
          }
          emit pushSucceeded(mRepo.workdir().canonicalPath());
          if (remote.name() == QStringLiteral("origin") && ref.isTag())
            mOriginTagChecks.remove(originTagKey(ref));
        }

        mWatcher->deleteLater();
        mWatcher = nullptr;
        mCallbacks = nullptr;
      });

  QString url = remote.url();
  mCallbacks = new RemoteCallbacks(RemoteCallbacks::Send, entry, url,
                                   remote.name(), mWatcher, mRepo);
  connect(mCallbacks, &RemoteCallbacks::referenceUpdated, this,
          &RepoView::notifyReferenceUpdated);

  entry->setBusy(true);
  git::Result (git::Remote::*push)(git::Remote::Callbacks *,
                                   const git::Reference &, const QString &,
                                   bool, bool) = &git::Remote::push;
  mWatcher->setFuture(
      QtConcurrent::run(push, remote, mCallbacks, ref, dst, force, tags));
}

bool RepoView::commit(const QString &message,
                      const git::AnnotatedCommit &upstream, LogEntry *parent,
                      bool force) {

  bool fakeSignature = false;
  git::Signature signature = mRepo.defaultSignature(
      &fakeSignature, mDetails->overrideUser(), mDetails->overrideEmail());
  return commit(signature, signature, message, upstream, parent, force,
                fakeSignature);
}

bool RepoView::commit(const git::Signature &author,
                      const git::Signature &commiter, const QString &message,
                      const git::AnnotatedCommit &upstream, LogEntry *parent,
                      bool force, bool fakeSignature) {
  // Check for detached head.
  git::Reference head = mRepo.head();
  if (!force && head.isValid() && !head.isLocalBranch()) {
    QString title = tr("Commit?");
    QString text = tr("Are you sure you want to commit on a detached HEAD?");
    QMessageBox *dialog = new QMessageBox(QMessageBox::Warning, title, text,
                                          QMessageBox::Cancel, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    dialog->setInformativeText(
        tr("<p>You are in a detached HEAD state. You can still commit, but the "
           "new commit will not be reachable from any branch. If you want to "
           "commit to an existing branch, checkout the branch first.</p>"));

    QPushButton *accept =
        dialog->addButton(tr("Commit"), QMessageBox::AcceptRole);
    connect(accept, &QPushButton::clicked, this,
            [this, message, upstream, parent] {
              this->commit(message, upstream, parent, true);
            });

    dialog->open();
    return false;
  }

  QString text = tr("<i>no commit</i>");
  LogEntry *entry = addLogEntry(text, tr("Commit"), parent);

  git::Commit commit = mRepo.commit(author, commiter, message, upstream);

  if (!commit.isValid()) {
    error(entry, tr("commit"));
    return false;
  }

  // Log commit message.
  entry->setText(msg(commit));
  if (fakeSignature) {
    QString text =
        tr("This commit was signed with a generated user name and email.");
    QString hint1 =
        tr("Consider setting the user name and email in "
           "<a href='action:config?global=true'>global settings</a>.");
    QString hint2 = tr(
        "If you want to limit the name and email settings to this repository, "
        "<a href='action:config'>edit repository settings</a> instead.");
    QString hint3 =
        tr("After settings have been updated, <a href='action:amend'> amend "
           "this commit</a> to record the new user name and email.");
    LogEntry *error = entry->addEntry(LogEntry::Error, text);
    error->addEntry(LogEntry::Hint, hint1);
    error->addEntry(LogEntry::Hint, hint2);
    error->addEntry(LogEntry::Hint, hint3);
  }

  if (!mPendingCheckoutRef.isEmpty()) {
    const QString pending = mPendingCheckoutRef;
    mPendingCheckoutRef.clear();
    mSelectingPendingCheckoutStatus = false;
    QTimer::singleShot(0, this, [this, pending] {
      git::Reference ref = mRepo.lookupRef(pending);
      if (ref.isValid())
        checkoutFromNavigator(ref);
    });
  }

  return true;
}

void RepoView::amendCommit() {
  // FIXME: Log errors.
  git::Branch head = mRepo.head();
  if (!head.isValid())
    return;

  git::Commit commit = head.target();
  if (!commit.isValid())
    return;

  promptToAmend(commit);
}

void RepoView::promptToCheckout() {
  git::Reference ref = reference();
  CheckoutDialog *dialog = new CheckoutDialog(mRepo, ref, this);
  connect(dialog, &QDialog::accepted, this,
          [this, dialog] { checkout(dialog->reference(), dialog->detach()); });

  dialog->open();
}

void RepoView::checkoutFromNavigator(const git::Reference &ref) {
  Q_ASSERT(ref.isValid());
  mPendingCheckoutRef.clear();
  mSelectingPendingCheckoutStatus = false;

  if (!ref.isLocalBranch() || ref.isHead() ||
      git::Branch(ref).isCheckedOut()) {
    checkout(ref);
    return;
  }

  CheckoutCallbacks callbacks(nullptr, GIT_CHECKOUT_NOTIFY_DIRTY);
  const int strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_DRY_RUN;
  if (mRepo.checkout(ref.target(), &callbacks, QStringList(), strategy) ||
      callbacks.conflicts().isEmpty()) {
    checkout(ref);
    return;
  }

  promptForCheckoutConflicts(ref, callbacks.conflicts());
}

void RepoView::promptForCheckoutConflicts(const git::Reference &ref,
                                          const QStringList &conflicts) {
  QMessageBox *dialog = new QMessageBox(QMessageBox::Warning,
                                        tr("Checkout Blocked"), QString(),
                                        QMessageBox::Cancel, this);
  dialog->setObjectName("CheckoutConflictsDialog");
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setText(
      tr("Checking out '%1' would overwrite uncommitted changes.")
          .arg(ref.name()));
  dialog->setInformativeText(
      tr("Stash or commit the conflicting changes before checking out this "
         "branch."));
  dialog->setDetailedText(conflicts.join('\n'));

  QPushButton *stashButton =
      dialog->addButton(tr("Stash and Checkout"), QMessageBox::AcceptRole);
  connect(stashButton, &QPushButton::clicked, this, [this, ref] {
    if (stash(QString(), true))
      checkoutFromNavigator(ref);
  });

  QPushButton *commitButton =
      dialog->addButton(tr("Commit Changes"), QMessageBox::ActionRole);
  connect(commitButton, &QPushButton::clicked, this, [this, ref] {
    mSelectingPendingCheckoutStatus = true;
    mPendingCheckoutRef = ref.qualifiedName();
    toolBar()->searchField()->clear();
    const bool selected = mCommits->selectRange("status", QString(), true);
    mSelectingPendingCheckoutStatus = false;
    if (selected)
      return;

    connect(
        mCommits, &CommitList::statusChanged, this,
        [this](bool) {
          if (!mPendingCheckoutRef.isEmpty()) {
            mSelectingPendingCheckoutStatus = true;
            mCommits->selectRange("status", QString(), true);
            mSelectingPendingCheckoutStatus = false;
          }
        },
        Qt::SingleShotConnection);
    refresh(false);
  });

  dialog->open();
}

void RepoView::checkout(const git::Commit &commit, const QStringList &paths) {
  QString count = QString::number(paths.size());
  QString name = (paths.size() == 1) ? tr("file") : tr("files");
  QString text = tr("%1 - %2 %3").arg(commit.link(), count, name);
  LogEntry *entry = addLogEntry(text, tr("Checkout"));

  CheckoutCallbacks callbacks(entry, GIT_CHECKOUT_NOTIFY_ALL);
  int strategy = GIT_CHECKOUT_SAFE | GIT_CHECKOUT_DONT_UPDATE_INDEX;
  mRepo.checkout(commit, &callbacks, paths, strategy);
  mRefs->select(mRepo.head());
}

void RepoView::checkout(const git::Reference &ref, bool detach) {
  Q_ASSERT(ref.isValid());
  mPendingCheckoutRef.clear();
  mSelectingPendingCheckoutStatus = false;

  if (!ref.isRemoteBranch()) {
    checkout(ref.target(), ref, detach);
    return;
  }

  // Prompt to create a new local branch instead
  // of checking out a remote tracking branch.
  QMessageBox *dialog = new QMessageBox(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setIcon(QMessageBox::Question);
  dialog->setStandardButtons(QMessageBox::Cancel);
  dialog->setWindowTitle(tr("Checkout Detached HEAD?"));

  QPushButton *checkoutButton = dialog->addButton(tr("Checkout Detached HEAD"),
                                                  QMessageBox::DestructiveRole);
  connect(checkoutButton, &QPushButton::clicked, this,
          [this, ref, detach] { checkout(ref.target(), ref, detach); });

  QString name = ref.name();
  QString local = name.section('/', 1);
  if (mRepo.lookupBranch(local, GIT_BRANCH_LOCAL)) {
    dialog->setText(
        tr("Checking out remote branch '%1' will result in a detached HEAD "
           "state. Do you want to reset the existing local branch '%2' to "
           "this commit instead?")
            .arg(name, local));

    QPushButton *resetButton =
        dialog->addButton(tr("Reset Local Branch"), QMessageBox::AcceptRole);
    connect(resetButton, &QPushButton::clicked, this, [this, ref, local] {
      createBranch(local, ref.target(), ref, true, true);
    });
  } else {
    dialog->setText(
        tr("Checking out remote branch '%1' will result in a detached HEAD "
           "state. Do you want to create a new local branch called '%2' to "
           "track it instead?")
            .arg(name, local));
    dialog->setInformativeText(
        tr("Create a local branch to start tracking remote changes and make "
           "new commits. Check out the detached HEAD to temporarily put your "
           "working directory into the state of the remote branch."));

    QPushButton *createButton =
        dialog->addButton(tr("Create Local Branch"), QMessageBox::AcceptRole);
    connect(createButton, &QPushButton::clicked, this, [this, ref, local] {
      createBranch(local, ref.target(), ref, true);
    });
  }

  dialog->open();
}

void RepoView::checkout(const git::Commit &commit, const git::Reference &ref,
                        bool detach) {
  Q_ASSERT(detach || ref.isValid());

  QString name = tr("<i>no commit</i>");
  if (!detach && ref.isValid()) {
    name = ref.name();
  } else if (commit.isValid()) {
    name = commit.link();
  }

  LogEntry *entry = addLogEntry(name, tr("Checkout"));
  if (!detach && ref.isLocalBranch() && !ref.isHead() &&
      git::Branch(ref).isCheckedOut()) {
    entry->addEntry(
        LogEntry::Error,
        tr("Branch '%1' is already checked out in another worktree.")
            .arg(ref.name()));
    return;
  }

  CheckoutCallbacks callbacks(entry, GIT_CHECKOUT_NOTIFY_DIRTY);
  if (!commit.isValid() || !mRepo.checkout(commit, &callbacks) ||
      (detach && !mRepo.setHeadDetached(commit)) ||
      (!detach && !mRepo.setHead(ref))) {
    LogEntry *err = error(entry, tr("checkout"), name);
    foreach (const QString &path, callbacks.conflicts())
      err->addEntry(LogEntry::File, path)->setStatus('!');

    if (ref.isValid()) {
      QUrlQuery query;
      query.addQueryItem("ref", ref.qualifiedName());
      if (detach)
        query.addQueryItem("detach", "true");

      // Add stash hint.
      QString text =
          tr("You may be able to reconcile your changes with the conflicting "
             "files by <a href='action:stash'>stashing</a> before you "
             "<a href='action:checkout?%1'>checkout '%2'</a>. Then "
             "<a href='action:unstash'>unstash</a> to restore your changes.");
      err->addEntry(LogEntry::Hint, text.arg(query.toString(), ref.name()));
    }

    return;
  }

  mRefs->select(mRepo.head());
}

void RepoView::promptToCreateBranch(const git::Commit &commit) {
  NewBranchDialog *dialog = new NewBranchDialog(mRepo, commit, this);
  connect(dialog, &QDialog::accepted, this, [this, dialog] {
    createBranch(dialog->name(), dialog->target(), dialog->upstream(),
                 dialog->checkout());
  });

  dialog->open();
}

git::Branch RepoView::createBranch(const QString &name,
                                   const git::Commit &target,
                                   const git::Branch &upstream, bool checkout,
                                   bool force) {
  LogEntry *entry = addLogEntry(name, tr("New Branch"));
  git::Branch branch = mRepo.createBranch(name, target, force);
  if (!branch.isValid()) {
    error(entry, tr("create new branch"), name);
    return git::Branch();
  }

  // Start tracking.
  branch.setUpstream(upstream);

  // Checkout.
  if (checkout)
    this->checkout(branch);

  return branch;
}

void RepoView::promptToDeleteBranch(const git::Reference &ref) {
  DeleteBranchDialog *dialog = new DeleteBranchDialog(ref, this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->open();
}

void RepoView::promptToRenameBranch(const git::Branch &branch) {
  Q_ASSERT(branch.isValid() && branch.isBranch());
  RenameBranchDialog *dialog = new RenameBranchDialog(mRepo, branch, this);
  // The dialog contains the code which performs the rename
  dialog->open();
}

void RepoView::populateRemoteContextMenu(QMenu *menu) {
  if (!menu)
    return;

  menu->addAction(tr("Pull"), this, [this] { pull(); });
  menu->addAction(tr("Push"), this, [this] { push(); });
  menu->addAction(tr("Force Push..."), this, [this] { promptToForcePush(); });
}

void RepoView::populateReferenceContextMenu(QMenu *menu,
                                            const git::Reference &ref) {
  if (!menu || !ref.isValid())
    return;

  if (ref.isLocalBranch() && ref.isHead()) {
    populateRemoteContextMenu(menu);
    menu->addSeparator();
  }

  QAction *checkout = menu->addAction(tr("Checkout"), this,
                                      [this, ref] { this->checkout(ref); });
  const bool checkedOutElsewhere =
      ref.isLocalBranch() && !ref.isHead() && git::Branch(ref).isCheckedOut();
  checkout->setEnabled(!ref.isHead() && !checkedOutElsewhere && !mRepo.isBare());
  menu->addSeparator();

  if (ref.isLocalBranch()) {
    QAction *rename =
        menu->addAction(tr("Rename %1").arg(ref.name()), this, [this, ref] {
          promptToRenameBranch(git::Branch(ref));
        });
    rename->setEnabled(ref.isHead() || !git::Branch(ref).isCheckedOut());
  }

  if (ref.isTag() || ref.isLocalBranch()) {
    QAction *remove = menu->addAction(tr("Delete"), this, [this, ref] {
      if (ref.isTag())
        promptToDeleteTag(ref);
      else
        promptToDeleteBranch(ref);
    });
    remove->setEnabled(ref.isTag() || !git::Branch(ref).isCheckedOut());
  }

  if (ref.isTag())
    addPushTagToOriginAction(menu, ref);

  if (ref.isRemoteBranch()) {
    menu->addAction(tr("Rename %1").arg(ref.name()), this, [this, ref] {
      promptToRenameBranch(git::Branch(ref));
    });
    menu->addAction(tr("Delete %1").arg(ref.name()), this,
                    [this, ref] { promptToDeleteBranch(ref); });
    menu->addAction(tr("New Local Branch"), this, [this, ref] {
      QString local = ref.name().section('/', 1);
      createBranch(local, ref.target(), git::Branch(ref), true);
    });
  }

  menu->addSeparator();
  const auto addMergeAction = [this, menu, ref](const QString &text,
                                                MergeFlags flags) {
    QAction *action = menu->addAction(text, this, [this, ref, flags] {
      MergeDialog *dialog = new MergeDialog(flags, mRepo, this);
      connect(dialog, &QDialog::accepted, this,
              [this, dialog] { merge(dialog->flags(), dialog->reference()); });
      dialog->setReference(ref);
      dialog->open();
    });
    action->setEnabled(!ref.isStash());
  };
  addMergeAction(tr("Merge..."), Merge);
  addMergeAction(tr("Rebase..."), Rebase);
  addMergeAction(tr("Squash..."), Squash);
}

void RepoView::addPushTagToOriginAction(QMenu *menu,
                                        const git::Reference &tag) {
  if (!menu || !tag.isValid() || !tag.isTag())
    return;

  git::Remote origin = tag.repo().lookupRemote(QStringLiteral("origin"));
  if (!origin.isValid())
    return;

  const QString key = originTagKey(tag);
  OriginTagCheck *check = startOriginTagCheck(tag);
  QAction *pushTag = menu->addAction(
      tr("Push Tag %1 to origin").arg(tag.name()), this,
      [this, tag] { pushTagToOrigin(tag); });
  auto updatePushTag = [this, pushTag, key] {
        const auto it = mOriginTagChecks.constFind(key);
        const git::Remote::TagStatus status =
            it == mOriginTagChecks.cend() ? git::Remote::TagStatus::Unknown
                                          : it->status;
        const bool pending = it != mOriginTagChecks.cend() && it->watcher;
        pushTag->setEnabled(status == git::Remote::TagStatus::Pushable);
        if (pending)
          pushTag->setToolTip(
              tr("Checking whether this tag is present on origin."));
        else if (status == git::Remote::TagStatus::Present)
          pushTag->setToolTip(tr("This tag is already present on origin."));
        else if (status == git::Remote::TagStatus::Conflict)
          pushTag->setToolTip(
              tr("Origin has a different tag with this name."));
        else if (status == git::Remote::TagStatus::TargetLocalOnly)
          pushTag->setToolTip(
              tr("The tag's target commit is not reachable from origin."));
        else
          pushTag->setToolTip(
              tr("Unable to validate this tag against origin."));
  };
  updatePushTag();
  if (check->watcher)
    connect(check->watcher, &QFutureWatcher<git::Remote::TagStatus>::finished,
            pushTag, updatePushTag);
}

void RepoView::promptToStash() {
  // Prompt to edit stash commit message.
  if (!Settings::instance()->prompt(Prompt::Kind::Stash)) {
    stash();
    return;
  }

  // Reproduce default commit message.
  git::Reference head = mRepo.head();
  git::Commit commit = head.target();
  QString id = commit.shortId();
  QString ref = head.isBranch() ? head.name() : tr("(no branch)");
  QString msg = tr("WIP on %1: %2 %3").arg(ref, id, commit.summary());
  CommitDialog *dialog = new CommitDialog(msg, Prompt::Kind::Stash, this);
  connect(dialog, &QDialog::accepted, this, [this, msg, dialog] {
    QString userMsg = dialog->message();
    stash(msg != userMsg ? userMsg : QString());
  });

  dialog->open();
}

bool RepoView::stash(const QString &message, bool includeUntracked) {
  QString text = tr("<i>working directory</i>");
  LogEntry *entry = addLogEntry(text, tr("Stash"));

  git::Commit commit = mRepo.stash(message, includeUntracked);
  if (!commit.isValid()) {
    error(entry, tr("stash"), text);
    return false;
  }

  entry->setText(msg(commit));
  refresh(false);
  return true;
}

void RepoView::applyStash(int index) {
  QList<git::Commit> stashes = mRepo.stashes();
  Q_ASSERT(index >= 0 && index < stashes.size());

  git::Commit commit = stashes.at(index);
  LogEntry *entry = addLogEntry(msg(commit), tr("Apply Stash"));
  if (!mRepo.applyStash(index)) {
    error(entry, tr("apply stash"), commit.link());
    return;
  }

  refresh(false);
}

void RepoView::dropStash(int index) {
  QList<git::Commit> stashes = mRepo.stashes();
  Q_ASSERT(index >= 0 && index < stashes.size());

  git::Commit commit = stashes.at(index);
  LogEntry *entry = addLogEntry(msg(commit), tr("Drop Stash"));
  if (!mRepo.dropStash(index))
    error(entry, tr("drop stash"), commit.link());

  if (mRepo.stashes().size() == 0) {
    // switch back to head when there are no stashes left
    mCommits->setReference(mRepo.head());
  } else {
    mCommits->setReference(mRepo.stashRef());
  }
}

void RepoView::popStash(int index) {
  QList<git::Commit> stashes = mRepo.stashes();
  Q_ASSERT(index >= 0 && index < stashes.size());

  git::Commit commit = stashes.at(index);
  LogEntry *entry = addLogEntry(msg(commit), tr("Pop Stash"));
  if (!mRepo.popStash(index)) {
    error(entry, tr("pop stash"), commit.link());
    return;
  }
  // switch back to head
  selectReference(mRepo.head());
  selectFirstCommit();
}

void RepoView::promptToAddTag(const git::Commit &commit) {
  TagDialog *dialog =
      new TagDialog(mRepo, commit.shortId(), mRepo.defaultRemote(), this);

  connect(dialog, &TagDialog::accepted, this, [this, commit, dialog] {
    bool force = dialog->force();
    QString name = dialog->name();
    QString msg = dialog->message();
    git::TagRef tag =
        mRepo.createTag(commit, name, msg, force, mDetails->overrideUser(),
                        mDetails->overrideEmail());

    git::Remote remote = dialog->remote();

    QString link = commit.link();
    QString text = tag.isValid() ? tr("%1 as %2").arg(link, tag.name()) : link;
    LogEntry *entry = addLogEntry(text, tr("Tag"));
    if (!tag.isValid())
      error(entry, tr("tag"), link);
    else if (remote.isValid())
      push(remote, tag);
  });

  dialog->open();
}

void RepoView::promptToDeleteTag(const git::Reference &ref) {
  DeleteTagDialog *dialog = new DeleteTagDialog(ref, this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->open();
}

void RepoView::promptToAmend(const git::Commit &commit) {
  auto *d = new AmendDialog(commit.author(), commit.committer(),
                            commit.message(), this);
  d->setAttribute(Qt::WA_DeleteOnClose);
  connect(d, &QDialog::accepted, [this, d, commit]() {
    auto info = d->getInfo();

    git::Signature author = getSignature(info.authorInfo);
    git::Signature committer = getSignature(info.committerInfo);

    amend(commit, author, committer, info.commitMessage);
  });

  d->show();
}

void RepoView::amend(const git::Commit &commit, const git::Signature &author,
                     const git::Signature &committer,
                     const QString &commitMessage) {
  git::Reference head = mRepo.head();
  Q_ASSERT(head.isValid());

  QString title = tr("Amend");

  if (!mRepo.amend(commit, author, committer, commitMessage)) {
    error(addLogEntry(tr("Amending commit %1").arg(commit.link()), title),
          tr("amend"), head.name());
  } else {
    head = mRepo.head();
    Q_ASSERT(head.isValid());

    QString text =
        tr("%1 to %2", "update ref").arg(head.name(), head.target().link());
    addLogEntry(text, title);
  }
}

void RepoView::promptToReset(const git::Commit &commit, git_reset_t type) {
  git::Branch head = mRepo.head();
  if (!head.isValid()) {
    QString title = tr("Reset");
    LogEntry *entry = addLogEntry(tr("<i>no branch</i>"), title);
    entry->addEntry(LogEntry::Error, tr("You are not currently on a branch."));
    return;
  }

  QString id = commit.shortId();
  QString title = tr("Reset");
  switch (type) {
    case GIT_RESET_SOFT:
      title += " Soft";
      break;
    case GIT_RESET_MIXED:
      title += " Mixed";
      break;
    case GIT_RESET_HARD:
      title += " Hard";
      break;
  }
  title += "?";

  QString text =
      tr("Are you sure you want to reset '%1' to '%2'?").arg(head.name(), id);
  QMessageBox *dialog = new QMessageBox(QMessageBox::Warning, title, text,
                                        QMessageBox::Cancel, this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);

  QString info;
  if (head.target() != commit)
    info = tr("<p>Some commits may become unreachable "
              "from the current branch.</p>");

  // Add warnings about destructive changes and resetting past upstream.
  git::Branch upstream = head.upstream();
  if (type == GIT_RESET_HARD && isWorkingDirectoryDirty()) {
    info += tr("<p>Resetting will cause you to lose uncommitted changes. "
               "Untracked and ignored files will not be affected.</p>");
  } else if (upstream.isValid() && !head.difference(upstream)) {
    info +=
        tr("<p>Your branch appears to be up-to-date with its upstream branch. "
           "Resetting may cause your branch history to diverge from the "
           "remote branch history.</p>");
  }

  dialog->setInformativeText(info);

  QString buttonText = tr("Reset");
  QPushButton *accept = dialog->addButton(buttonText, QMessageBox::AcceptRole);
  connect(accept, &QPushButton::clicked, this,
          [this, commit, type] { reset(commit, type); });

  dialog->open();
}

void RepoView::reset(const git::Commit &commit, git_reset_t type,
                     const git::Commit &commitToAmend) {
  git::Reference head = mRepo.head();
  Q_ASSERT(head.isValid());

  QString title = commitToAmend ? tr("Amend") : tr("Reset");
  QString text = tr("%1 to %2").arg(head.name(), commit.link());
  LogEntry *entry = addLogEntry(text, title);

  if (!commit.reset(type, QStringList(), false))
    error(entry, commitToAmend ? tr("amend") : tr("reset"), head.name());

  updateSubmodules(mRepo.submodules(), true, false,
                   (type == GIT_RESET_HARD) ? true : false, entry,
                   type == git_reset_t::GIT_RESET_HARD);
  if (mRepo.submodules().isEmpty())
    refresh(type == git_reset_t::GIT_RESET_HARD);
}

void RepoView::resetSubmodules(const QList<git::Submodule> &submodules,
                               bool recursive, git_reset_t type,
                               LogEntry *parent) {
  if (mWatcher) {
    // Queue update. synchrone
    connect(mWatcher, &QFutureWatcher<git::Result>::finished, mWatcher,
            [this, submodules, recursive, type, parent] {
              resetSubmodules(submodules, recursive, type, parent);
            });

    return;
  }

  QList<git::Submodule> modules =
      !submodules.isEmpty() ? submodules : mRepo.submodules();
  if (modules.isEmpty())
    return;

  if (type == GIT_RESET_HARD) {
    // Start updating asynchronously.
    QList<SubmoduleInfo> infos = submoduleResetInfoList(mRepo, modules, parent);
    resetSubmodulesAsync(infos, recursive, type);
  }
}

/*!
 * \brief RepoView::resetSubmodulesAsync
 *
 * \param submodules
 * \param recursive
 * \param type
 * \param parent
 */
void RepoView::resetSubmodulesAsync(const QList<SubmoduleInfo> &submodules,
                                    bool recursive, git_reset_t type) {
  if (submodules.isEmpty()) {
    refresh(true);
    return;
  }

  // Remove first submodule from the list.
  QList<SubmoduleInfo> tail = submodules;
  SubmoduleInfo info = tail.takeFirst();
  git::Submodule submodule = info.submodule;
  LogEntry *entry = info.entry->addEntry(submodule.name(), tr("Reset"));

  mWatcher = new QFutureWatcher<git::Result>(this);
  connect(mWatcher, &QFutureWatcher<git::Result>::finished, mWatcher,
          [this, recursive, tail, type, info, entry] {
            entry->setBusy(false);

            git::Result result = mWatcher->result();
            if (mCallbacks->isCanceled()) {
              entry->addEntry(LogEntry::Error, tr("Reset canceled."));
            } else if (!result) {
              QString name = info.submodule.name();
              error(entry, tr("update submodule"), name, result.errorString());
            } else {
              mCallbacks->storeDeferredCredentials();
            }

            mWatcher->deleteLater();
            mWatcher = nullptr;
            mCallbacks = nullptr;

            // Build list of submodules recursively.
            QList<SubmoduleInfo> prefix;
            if (recursive) {
              if (git::Repository repo = info.submodule.open()) {
                QList<git::Submodule> submodules = repo.submodules();
                if (!submodules.isEmpty())
                  prefix = submoduleResetInfoList(repo, submodules, entry);
              }
            }

            // Restart with smaller list.
            resetSubmodulesAsync(prefix + tail, recursive, type);
          });

  QString url = submodule.url();
  git::Repository repo = submodule.open();
  mCallbacks = new RemoteCallbacks(RemoteCallbacks::Receive, entry, url,
                                   QString(), mWatcher, repo);

  entry->setBusy(true);
  mWatcher->setFuture(QtConcurrent::run(&git::Submodule::update, submodule,
                                        mCallbacks, false, true));
}

/*!
 * \brief RepoView::submoduleResetInfoList
 * Return a list of Submodules which should be resetted
 * Additionally create the log message
 * \param repo
 * \param submodules
 * \param init
 * \param parent
 * \return
 */
QList<RepoView::SubmoduleInfo>
RepoView::submoduleResetInfoList(const git::Repository &repo,
                                 const QList<git::Submodule> &submodules,
                                 LogEntry *parent) {
  // Only reset modified submodules
  QList<git::Submodule> modules;
  foreach (const git::Submodule &submodule, submodules) {
    int status = repo.submoduleStatus(submodule.name());

    if (status & (GIT_SUBMODULE_STATUS_WD_MODIFIED |
                  GIT_SUBMODULE_STATUS_WD_WD_MODIFIED |
                  GIT_SUBMODULE_STATUS_WD_INDEX_MODIFIED))
      modules.append(submodule);
  }

  QString text =
      tr("%1 of %2 submodules").arg(modules.size()).arg(submodules.size());
  LogEntry *entry = addLogEntry(text, tr("Reset"), parent);

  if (modules.isEmpty())
    entry->addEntry(tr("Untouched"));

  QList<SubmoduleInfo> list;
  foreach (const git::Submodule &module, modules)
    list.append({module, repo, entry});
  return list;
}

void RepoView::updateSubmodules(const QList<git::Submodule> &submodules,
                                bool recursive, bool init, bool checkout_force,
                                LogEntry *parent, bool restoreSelection) {
  if (mWatcher) {
    // Queue update. synchrone
    connect(mWatcher, &QFutureWatcher<git::Result>::finished, mWatcher,
            [this, submodules, recursive, init, checkout_force, parent] {
              updateSubmodules(submodules, recursive, init, checkout_force,
                               parent);
            });

    return;
  }

  if (submodules.isEmpty())
    return;

  // Start updating asynchronously.
  QList<SubmoduleInfo> infos =
      submoduleUpdateInfoList(mRepo, submodules, init, checkout_force, parent);
  updateSubmodulesAsync(infos, recursive, init, checkout_force,
                        restoreSelection);
}

/*!
 * \brief RepoView::submoduleUpdateInfoList
 * Return a list of Submodules which should be updated
 * Additionally create the log message
 * \param repo
 * \param submodules
 * \param init
 * \param parent
 * \return
 */
QList<RepoView::SubmoduleInfo> RepoView::submoduleUpdateInfoList(
    const git::Repository &repo, const QList<git::Submodule> &submodules,
    bool init, bool checkout_force, LogEntry *parent) {
  // Gather list of submodules.
  QList<git::Submodule> modules;
  foreach (const git::Submodule &submodule, submodules) {
    const bool initialized = submodule.isInitialized();
    if (!init && !initialized)
      continue;

    if (initialized && !checkout_force &&
        submodule.workdirId() == submodule.headId()) // indexId == headId?
      continue;

    modules.append(submodule);
  }

  QString text =
      tr("%1 of %2 submodules").arg(modules.size()).arg(submodules.size());
  LogEntry *entry = addLogEntry(text, tr("Update"), parent);

  if (modules.isEmpty())
    entry->addEntry(tr("Already up-to-date."));

  QList<SubmoduleInfo> list;
  foreach (const git::Submodule &module, modules)
    list.append({module, repo, entry});
  return list;
}

void RepoView::updateSubmodulesAsync(const QList<SubmoduleInfo> &submodules,
                                     bool recursive, bool init,
                                     bool checkout_force,
                                     bool restoreSelection) {
  if (submodules.isEmpty()) {
    emit submodulesChanged();
    refresh(restoreSelection);
    return;
  }

  // Remove first submodule from the list.
  QList<SubmoduleInfo> tail = submodules;
  SubmoduleInfo info = tail.takeFirst();
  git::Submodule submodule = info.submodule;
  LogEntry *entry = info.entry->addEntry(submodule.name(), tr("Update"));

  mWatcher = new QFutureWatcher<git::Result>(this);
  connect(mWatcher, &QFutureWatcher<git::Result>::finished, mWatcher,
          [this, init, recursive, checkout_force, tail, info, entry,
           restoreSelection] {
            entry->setBusy(false);

            git::Result result = mWatcher->result();
            if (mCallbacks->isCanceled()) {
              entry->addEntry(LogEntry::Error, tr("Fetch canceled."));
            } else if (!result) {
              QString name = info.submodule.name();
              error(entry, tr("update submodule"), name, result.errorString());
            } else {
              mCallbacks->storeDeferredCredentials();
            }

            mWatcher->deleteLater();
            mWatcher = nullptr;
            mCallbacks = nullptr;

            // Build list of submodules recursively.
            QList<SubmoduleInfo> prefix;
            if (recursive) {
              if (git::Repository repo = info.submodule.open()) {
                QList<git::Submodule> submodules = repo.submodules();
                if (!submodules.isEmpty())
                  prefix = submoduleUpdateInfoList(repo, submodules, init,
                                                   checkout_force, entry);
              }
            }

            // Restart with smaller list.
            updateSubmodulesAsync(prefix + tail, recursive, init,
                                  checkout_force, restoreSelection);
          });

  QString url = submodule.url();
  git::Repository repo = submodule.open();
  mCallbacks = new RemoteCallbacks(RemoteCallbacks::Receive, entry, url,
                                   QString(), mWatcher, repo);

  entry->setBusy(true);
  mWatcher->setFuture(QtConcurrent::run(&git::Submodule::update, submodule,
                                        mCallbacks, init, checkout_force));
}

void RepoView::checkSubmoduleUpdates(bool automatic) {
  checkSubmoduleUpdates(QList<git::Submodule>(), automatic);
}

void RepoView::checkSubmoduleUpdates(
    const QList<git::Submodule> &requestedSubmodules, bool automatic) {
  if (mClosing)
    return;

  if (mSubmoduleUpdateWatcher) {
    if (automatic)
      mSubmoduleUpdateCheckPending = true;
    return;
  }

  if (mWatcher) {
    if (automatic) {
      mSubmoduleUpdateCheckPending = true;
      connect(
          mWatcher, &QFutureWatcher<git::Result>::finished, this,
          [this] {
            QTimer::singleShot(0, this, [this] {
              if (mSubmoduleUpdateCheckPending)
                checkSubmoduleUpdates(true);
            });
          },
          Qt::SingleShotConnection);
      return;
    }

    if (!automatic)
      addLogEntry(tr("Another remote operation is already running."),
                  tr("Submodule Updates"));
    return;
  }

  mSubmoduleUpdateCheckPending = false;
  const quint64 generation = mSubmoduleConfigurationGeneration;

  const bool fullCheck = requestedSubmodules.isEmpty();
  QList<git::Submodule> submodules =
      fullCheck ? mRepo.submodules() : requestedSubmodules;
  if (submodules.isEmpty()) {
    clearSubmoduleUpdateStatuses();
    if (!automatic)
      addLogEntry(tr("This repository has no submodules."),
                  tr("Submodule Updates"));
    return;
  }

  LogEntry *entry =
      addLogEntry(tr("Checking %1 submodules").arg(submodules.size()),
                  tr("Submodule Updates"));
  entry->setBusy(true);

  mSubmoduleUpdateWatcher =
      new QFutureWatcher<QList<git::Submodule::UpdateStatus>>(this);
  QFutureWatcher<QList<git::Submodule::UpdateStatus>> *watcher =
      mSubmoduleUpdateWatcher;
  mSubmoduleUpdateCallbacks =
      new RemoteCallbacks(RemoteCallbacks::Receive, entry,
                          submodules.first().url(), QString(), watcher, mRepo);
  RemoteCallbacks *callbacks = mSubmoduleUpdateCallbacks;

  connect(
      watcher, &QFutureWatcher<QList<git::Submodule::UpdateStatus>>::finished,
      watcher,
      [this, watcher, callbacks, entry, automatic, generation, fullCheck] {
        entry->setBusy(false);

        QList<git::Submodule::UpdateStatus> results = watcher->result();
        if (generation != mSubmoduleConfigurationGeneration) {
          entry->setText(
              tr("Submodule configuration changed; checking again."));
          callbacks->storeDeferredCredentials();
          mSubmoduleUpdateCheckPending = true;
          watcher->deleteLater();
          if (mSubmoduleUpdateWatcher == watcher)
            mSubmoduleUpdateWatcher = nullptr;
          if (mSubmoduleUpdateCallbacks == callbacks)
            mSubmoduleUpdateCallbacks = nullptr;
          QTimer::singleShot(0, this, [this] {
            if (mSubmoduleUpdateCheckPending)
              checkSubmoduleUpdates(true);
          });
          return;
        }

        if (fullCheck) {
          mSubmoduleUpdateStatuses = results;
        } else {
          for (const git::Submodule::UpdateStatus &result : results) {
            auto existing = std::find_if(
                mSubmoduleUpdateStatuses.begin(),
                mSubmoduleUpdateStatuses.end(),
                [&result](const git::Submodule::UpdateStatus &status) {
                  return status.name == result.name && status.path == result.path;
                });
            if (existing == mSubmoduleUpdateStatuses.end())
              mSubmoduleUpdateStatuses.append(result);
            else
              *existing = result;
          }
        }
        emit submoduleUpdateStatusesChanged(mSubmoduleUpdateStatuses);

        int updates = 0;
        int warnings = 0;
        int checked = 0;
        for (const git::Submodule::UpdateStatus &status : results) {
          if (status.state == git::Submodule::UpdateStatus::NotTracked)
            continue;

          ++checked;

          if (status.state == git::Submodule::UpdateStatus::UpdateAvailable)
            ++updates;
          else if (status.state == git::Submodule::UpdateStatus::Error ||
                   status.state ==
                       git::Submodule::UpdateStatus::DifferentHistory ||
                   status.state == git::Submodule::UpdateStatus::Unknown)
            ++warnings;
        }

        if (callbacks->isCanceled()) {
          entry->setText(tr("Submodule update check canceled."));
        } else if (!checked) {
          entry->setText(tr("No branch-tracked submodules to check."));
        } else if (updates) {
          entry->setText(tr("%1 submodules can be updated.").arg(updates));
        } else if (warnings) {
          entry->setText(
              tr("No updates found; %1 submodules need review.").arg(warnings));
        } else {
          entry->setText(tr("All submodules are up-to-date."));
        }

        for (const git::Submodule::UpdateStatus &status : results) {
          if (status.state == git::Submodule::UpdateStatus::NotTracked)
            continue;

          if (status.state == git::Submodule::UpdateStatus::UpToDate &&
              automatic)
            continue;

          QString text = tr("%1: %2").arg(
              status.name, submoduleUpdateStateText(status.state));
          if (!status.branch.isEmpty())
            text += tr(" on %1").arg(status.branch);

          if (status.targetId.isValid())
            text +=
                tr(" (%1 -> %2)")
                    .arg(shortId(status.pinnedId), shortId(status.targetId));

          if (!status.message.isEmpty())
            text += tr(" - %1").arg(status.message);

          LogEntry::Kind kind = LogEntry::Entry;
          if (status.state == git::Submodule::UpdateStatus::Error)
            kind = LogEntry::Error;
          else if (status.state ==
                       git::Submodule::UpdateStatus::DifferentHistory ||
                   status.state == git::Submodule::UpdateStatus::Unknown)
            kind = LogEntry::Warning;

          entry->addEntry(kind, text);
        }

        callbacks->storeDeferredCredentials();
        const bool restart = mSubmoduleUpdateCheckPending;
        watcher->deleteLater();
        if (mSubmoduleUpdateWatcher == watcher)
          mSubmoduleUpdateWatcher = nullptr;
        if (mSubmoduleUpdateCallbacks == callbacks)
          mSubmoduleUpdateCallbacks = nullptr;
        if (restart) {
          QTimer::singleShot(0, this, [this] {
            if (mSubmoduleUpdateCheckPending)
              checkSubmoduleUpdates(true);
          });
        }
      });

  watcher->setFuture(QtConcurrent::run([submodules, callbacks] {
    QList<git::Submodule::UpdateStatus> results;
    for (const git::Submodule &submodule : submodules) {
      if (callbacks->isCanceled())
        break;

      results.append(submodule.checkForUpdates(callbacks));
    }

    return results;
  }));
}

void RepoView::clearSubmoduleUpdateStatuses() {
  if (mSubmoduleUpdateStatuses.isEmpty())
    return;

  mSubmoduleUpdateStatuses.clear();
  emit submoduleUpdateStatusesChanged(mSubmoduleUpdateStatuses);
}

void RepoView::submoduleConfigurationChanged() {
  ++mSubmoduleConfigurationGeneration;
  mRepo.invalidateSubmoduleCache();
  clearSubmoduleUpdateStatuses();
  emit submodulesChanged();
  refresh(true);
  checkSubmoduleUpdates(true);
}

bool RepoView::checkoutSubmoduleOrigin(const QString &name,
                                       const QString &branch,
                                       const git::Id &target) {
  LogEntry *entry = addLogEntry(name, tr("Checkout Submodule"));
  if (mWatcher || mSubmoduleUpdateWatcher || mSubmodulePushCheckWatcher) {
    entry->addEntry(LogEntry::Error,
                    tr("Another remote operation is already running."));
    return false;
  }

  git::Submodule submodule = mRepo.lookupSubmodule(name);
  auto status = std::find_if(
      mSubmoduleUpdateStatuses.cbegin(), mSubmoduleUpdateStatuses.cend(),
      [submodule, branch, target](const git::Submodule::UpdateStatus &status) {
        return submodule.isValid() && status.name == submodule.name() &&
               status.path == submodule.path() &&
               (status.url == submodule.url() ||
                QUrl(submodule.url()).isRelative()) &&
               status.branch == branch && status.targetId == target;
      });
  if (!submodule.isInitialized() || !target.isValid() ||
      status == mSubmoduleUpdateStatuses.cend()) {
    entry->addEntry(
        LogEntry::Error,
        tr("The fetched submodule target is no longer current. Run the "
           "submodule update check again."));
    return false;
  }

  git::Repository child = submodule.open();
  if (!child.isValid()) {
    entry->addEntry(LogEntry::Error,
                    tr("The submodule repository is unavailable."));
    return false;
  }

  git::Commit commit = child.lookupCommit(target);
  if (!commit.isValid()) {
    entry->addEntry(LogEntry::Error,
                    tr("The fetched submodule commit is unavailable."));
    return false;
  }

  if (!child.checkout(commit)) {
    error(entry, tr("checkout submodule"), name);
    emit submodulesChanged();
    refresh(true);
    return false;
  }

  if (!child.setHeadDetached(commit)) {
    error(entry, tr("detach submodule HEAD"), name);
    emit submodulesChanged();
    refresh(true);
    return false;
  }

  entry->addEntry(
      tr("Checked out origin/%1 at %2.").arg(branch, target.shortId()));
  emit submodulesChanged();
  refresh(true);
  return true;
}

void RepoView::addSubmodule(const QString &url, const QString &path,
                            const QString &branch) {
  if (mWatcher || mSubmoduleUpdateWatcher) {
    addLogEntry(tr("Another remote operation is already running."),
                tr("Add Submodule"));
    return;
  }

  LogEntry *entry = addLogEntry(path, tr("Add Submodule"));

  mWatcher = new QFutureWatcher<git::Result>(this);
  connect(mWatcher, &QFutureWatcher<git::Result>::finished, mWatcher,
          [this, entry, path] {
            entry->setBusy(false);

            git::Result result = mWatcher->result();
            if (mCallbacks->isCanceled()) {
              entry->addEntry(LogEntry::Error, tr("Add submodule canceled."));
            } else if (!result) {
              error(entry, tr("add submodule"), path, result.errorString());
            } else {
              mCallbacks->storeDeferredCredentials();
              entry->addEntry(tr("Submodule added."));
              clearSubmoduleUpdateStatuses();
              emit submodulesChanged();
              refresh(true);
            }

            mWatcher->deleteLater();
            mWatcher = nullptr;
            mCallbacks = nullptr;
          });

  mCallbacks = new RemoteCallbacks(RemoteCallbacks::Receive, entry, url,
                                   QString(), mWatcher, mRepo);

  entry->setBusy(true);
  mWatcher->setFuture(QtConcurrent::run(&git::Submodule::add, mRepo, url, path,
                                        branch, mCallbacks));
}

bool RepoView::modifySubmodule(const QString &oldName, const QString &newName,
                               const QString &newPath, const QString &newUrl,
                               const QString &newBranch) {
  git::Result result = git::Submodule::modify(mRepo, oldName, newName, newPath,
                                              newUrl, newBranch);
  if (!result) {
    LogEntry *entry = addLogEntry(oldName, tr("Modify Submodule"));
    error(entry, tr("modify submodule"), oldName, result.errorString());
    return false;
  }

  addLogEntry(newName, tr("Submodule Modified"));
  submoduleConfigurationChanged();
  return true;
}

void RepoView::promptToModifySubmodule(const git::Submodule &submodule) {
  if (!submodule.isValid())
    return;

  ModifySubmoduleDialog *dialog = new ModifySubmoduleDialog(submodule, this);
  connect(dialog, &QDialog::accepted, this, [this, dialog, submodule] {
    modifySubmodule(submodule.name(), dialog->name(), dialog->path(),
                    dialog->url(), dialog->branch());
  });
  dialog->open();
}

void RepoView::promptToDeleteSubmodule(const git::Submodule &submodule) {
  if (!submodule.isValid())
    return;

  QString text =
      tr("Delete submodule '%1' at '%2'?\n\nThe submodule will be removed "
         "from this project. Its working files and cached local repository "
         "will be permanently deleted. Any unpublished commits will be lost.")
          .arg(submodule.name(), submodule.path());
  QMessageBox *message =
      new QMessageBox(QMessageBox::Warning, tr("Delete Submodule?"), text,
                      QMessageBox::Cancel, this);
  message->setAttribute(Qt::WA_DeleteOnClose);

  if (GIT_SUBMODULE_STATUS_IS_WD_DIRTY(
          mRepo.submoduleStatus(submodule.name()))) {
    message->setInformativeText(
        tr("The submodule working directory contains uncommitted changes "
           "that will be permanently lost."));
  }

  QPushButton *remove =
      message->addButton(tr("Delete Submodule"), QMessageBox::DestructiveRole);
  message->setDefaultButton(QMessageBox::Cancel);
  message->setEscapeButton(QMessageBox::Cancel);
  connect(remove, &QPushButton::clicked, this, [this, submodule] {
    const QString name = submodule.name();
    LogEntry *entry = addLogEntry(name, tr("Delete Submodule"));
    git::Result result = git::Submodule::remove(mRepo, submodule);
    if (!result)
      error(entry, tr("delete submodule"), name, result.errorString());
    else {
      entry->addEntry(tr("Submodule deleted."));
      clearSubmoduleUpdateStatuses();
    }
    emit submodulesChanged();
    refresh(true);
  });
  message->open();
}

bool RepoView::canCommitSubmoduleChanges(
    const git::Submodule &submodule) const {
  if (!mRepo.isValid() || !submodule.isValid())
    return false;

  git::Commit parent = mRepo.head().target();
  git::Repository child = submodule.open();
  if (!child.isValid())
    return false;

  git::Commit checkout = child.head().target();
  if (!mRepo.head().isLocalBranch() ||
      mRepo.state() != GIT_REPOSITORY_STATE_NONE || !parent.isValid() ||
      !checkout.isValid())
    return false;

  git::Id pinned = parent.tree().id(submodule.path());
  git::Id staged = submodule.indexId();
  return pinned.isValid() && staged.isValid() && pinned != checkout.id() &&
         (staged == pinned || staged == checkout.id());
}

void RepoView::commitSubmoduleChanges(const git::Submodule &submodule) {
  if (!mRepo.isValid() || !submodule.isValid())
    return;

  git::Commit parent = mRepo.head().target();
  git::Repository child = submodule.open();
  if (!child.isValid())
    return;

  git::Commit checkout = child.head().target();
  if (!mRepo.head().isLocalBranch() ||
      mRepo.state() != GIT_REPOSITORY_STATE_NONE || !parent.isValid() ||
      !checkout.isValid())
    return;

  git::Id pinnedId = parent.tree().id(submodule.path());
  git::Id checkoutId = checkout.id();
  if (!pinnedId.isValid() || pinnedId == checkoutId)
    return;

  QStringList changes;
  git::Commit pinned = child.lookupCommit(pinnedId);
  if (pinned.isValid()) {
    git::RevWalk walk = checkout.walker(GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
    walk.hide(pinned);
    for (git::Commit commit = walk.next(); commit.isValid();
         commit = walk.next()) {
      if (changes.size() == 100) {
        changes.append("- ...");
        break;
      }
      changes.append(QString("- %1").arg(commit.summary()));
    }
  }
  if (changes.isEmpty()) {
    changes.append(QString("- %1").arg(checkout.summary()));
  }

  QString name = QFileInfo(submodule.path()).fileName();
  QString message = tr("Update %1 from %2 to %3:\n%4")
                        .arg(name, pinnedId.toString().left(7),
                             checkoutId.toString().left(7), changes.join('\n'));
  LogEntry *entry = addLogEntry(tr("<i>no commit</i>"), tr("Commit Changes"));

  git::Commit commit = mRepo.commitSubmodule(submodule, checkoutId, message,
                                             nullptr, mDetails->overrideUser(),
                                             mDetails->overrideEmail());
  if (!commit.isValid()) {
    error(entry, tr("commit submodule changes"), name);
    return;
  }

  entry->setText(msg(commit));
  emit submodulesChanged();
}

bool RepoView::openSubmodule(const git::Submodule &submodule) {
  if (!submodule.isValid())
    return false;

  git::Repository repo = submodule.open();
  if (!repo.isValid()) {
    // Warn about trying to open a submodule that hasn't been inited.
    QString title = tr("Invalid Submodule Repository");
    QString text =
        tr("The submodule '%1' doesn't have a valid repository. You may need "
           "to init and/or update the submodule to check out a repository.");
    QMessageBox::warning(nullptr, title, text.arg(submodule.name()));
    return false;
  }

  RepoView *view = nullptr;
  if (Settings::instance()->value(Setting::Id::OpenSubmodulesInTabs).toBool()) {
    view = static_cast<MainWindow *>(window())->addTab(
        repo, mRepo.dir(false).dirName(), std::nullopt, true);
  } else if (MainWindow *submoduleWindow =
                 MainWindow::open(repo, std::nullopt, true)) {
    view = submoduleWindow->currentView();
  }

  if (!view)
    return false;

  connect(view, &RepoView::pushSucceeded, this,
          &RepoView::submodulePushSucceeded, Qt::UniqueConnection);
  return true;
}

void RepoView::submodulePushSucceeded(const QString &repositoryPath) {
  const QString pushedPath = QDir(repositoryPath).canonicalPath();
  for (const git::Submodule &submodule : mRepo.submodules()) {
    git::Repository repo = submodule.open();
    if (repo.isValid() && repo.workdir().canonicalPath() == pushedPath) {
      checkSubmoduleUpdates(QList<git::Submodule>{submodule}, true);
      return;
    }
  }
}

ConfigDialog *RepoView::configureSettings(ConfigDialog::Index index) {
  ConfigDialog *dialog = new ConfigDialog(this, index);
  dialog->open();
  return dialog;
}

void RepoView::openTerminal() {
  openTerminal(mRepo.workdir().absolutePath(), this);
}

void RepoView::openTerminal(const QString &workingDirectory, QWidget *parent) {
  QString terminalCmd =
      Settings::instance()->value(Setting::Id::TerminalCommand).toString();
#if defined(Q_OS_UNIX)
  bool launchDetectedPtyxis = false;
#endif

  if (terminalCmd.isEmpty()) {
#if defined(Q_OS_WIN)
    static QString detectedTerminal = nullptr;

    if (detectedTerminal.isNull()) {
      detectedTerminal = "";

      QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
      QString programFilesDir = env.value("PROGRAMFILES");
      QString programFiles32Dir = env.value("PROGRAMFILES(x86)");

      QStringList candidates;

      candidates.append("git-bash");
      if (!programFilesDir.isEmpty())
        candidates.append(programFilesDir + "/Git/git-bash.exe");
      if (!programFiles32Dir.isEmpty())
        candidates.append(programFiles32Dir + "/Git/git-bash.exe");
      if (!programFilesDir.isEmpty())
        candidates.append(programFilesDir + "/Git/bin/bash.exe");
      if (!programFiles32Dir.isEmpty())
        candidates.append(programFiles32Dir + "/Git/bin/bash.exe");
      candidates.append("cmd");

      for (QString candidate : candidates) {
        QString exePath;

        if (QDir::isAbsolutePath(candidate)) {
          if (QFile::exists(candidate))
            exePath = candidate;

        } else {
          exePath = QStandardPaths::findExecutable(candidate);
        }

        if (!exePath.isEmpty()) {
          detectedTerminal =
              '"' + QDir::toNativeSeparators(exePath.replace("\"", "\"\"")) +
              '"';
          break;
        }
      }
    }

    terminalCmd = detectedTerminal;

#elif defined(Q_OS_MACOS)
    static QString detectedTerminal = nullptr;
    static const char *candidates[] = {"com.googlecode.iterm2",
                                       "com.apple.Terminal", nullptr};

    if (detectedTerminal.isNull()) {
      detectedTerminal = "";

      for (const char **candidate = candidates; *candidate; ++candidate) {
        int res = QProcess::execute(
            "osascript", {"-e", QString("tell application \"Finder\" to get "
                                        "application file id \"%1\"")
                                    .arg(*candidate)});

        if (res == 0) {
          detectedTerminal = QString("open -b %1").arg(*candidate) + " .";
          break;
        }
      }
    }

    terminalCmd = detectedTerminal;

#elif defined(Q_OS_UNIX)
    static QString detectedTerminal = nullptr;
    static bool detectedPtyxis = false;
    static const QStringList candidates = {
        "x-terminal-emulator",
        "xdg-terminal",
        "i3-sensible-terminal",
        "gnome-terminal",
        "konsole",
        "ptyxis",
        "xterm",
    };

    if (detectedTerminal.isNull()) {
      detectedTerminal = "";

      for (auto candidate : candidates) {
#if defined(FLATPAK)
        // There is no graphical terminal in the flatpak environment. Use the
        // host terminal
        QProcess process;
        process.start("flatpak-spawn", {"--host", "which", candidate});
        process.waitForFinished(-1); // will wait forever until finished
        if (!process.readAllStandardOutput().isEmpty()) {
          detectedTerminal = candidate;
          detectedPtyxis = candidate == "ptyxis";
          break;
        }
#else
        QString exePath = QStandardPaths::findExecutable(candidate);
        if (!exePath.isEmpty()) {
          detectedTerminal =
              '"' + exePath.replace("\\", "\\\\").replace("\"", "\\\"") + '"';
          detectedPtyxis = candidate == "ptyxis";
          break;
        }
#endif
      }
    }

    terminalCmd = detectedTerminal;
    launchDetectedPtyxis = detectedPtyxis;
#if !defined(FLATPAK)
    if (launchDetectedPtyxis) {
      QString workdir = workingDirectory;
      workdir.replace("\\", "\\\\").replace("\"", "\\\"");
      terminalCmd +=
          QString(" --tab --working-directory \"%1\"").arg(workdir);
    }
#endif
#endif
  }

  if (terminalCmd.isEmpty()) {
    auto messagebox = new QMessageBox(parent);
    messagebox->setWindowTitle(tr("No terminal executable found"));
    messagebox->setText(tr("No terminal executable was found. Please configure "
                           "a terminal in the configuration."));
    messagebox->setStandardButtons(QMessageBox::Ok);
    messagebox->addButton(tr("Open Configuration"), QMessageBox::ApplyRole);
    messagebox->setAttribute(Qt::WA_DeleteOnClose);

    connect(
        messagebox, &QMessageBox::buttonClicked, messagebox,
        [=](QAbstractButton *button) {
          if (messagebox->buttonRole(button) == QMessageBox::ApplyRole) {
            SettingsDialog::openSharedInstance();
          }
        },
        Qt::QueuedConnection);
    messagebox->open();
    return;
  }

#if defined(Q_OS_WIN)
  // No direct method of QProcess can take a raw command line and a working
  // directory So we call CreateProcessW() directly

  std::unique_ptr<wchar_t[]> cmdBuffer(new wchar_t[terminalCmd.length() + 1]);
  int len = terminalCmd.toWCharArray(cmdBuffer.get());
  cmdBuffer[len] = L'\0';

  STARTUPINFOW startupInfo;
  PROCESS_INFORMATION processInfo;

  ZeroMemory(&startupInfo, sizeof(STARTUPINFOW));
  ZeroMemory(&processInfo, sizeof(PROCESS_INFORMATION));
  startupInfo.cb = sizeof(STARTUPINFOW);

  bool success = CreateProcessW(
      nullptr, cmdBuffer.get(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
      nullptr, (LPCWSTR)QDir::toNativeSeparators(workingDirectory).utf16(),
      &startupInfo, &processInfo);

  if (!success) {
    Debug("CreateProcessW failed: " << GetLastError());
    return;
  }

  CloseHandle(processInfo.hProcess);
  CloseHandle(processInfo.hThread);

#elif defined(Q_OS_UNIX)

  QProcess child;
#if defined(FLATPAK)
  child.setProgram("flatpak-spawn");
  QStringList arguments = {QStringLiteral("--host"), terminalCmd};
  if (launchDetectedPtyxis)
    arguments.append({QStringLiteral("--tab"),
                      QStringLiteral("--working-directory"), workingDirectory});
  child.setArguments(arguments);
#else
  child.setProgram("sh");
  child.setArguments(QStringList() << "-c" << terminalCmd);
#endif
  child.setWorkingDirectory(workingDirectory);
  Debug("Execute Terminal: Arguments: " << child.arguments());
  child.startDetached();
#endif
}

void RepoView::openFileManager() {
  openFileManager(mRepo.workdir().absolutePath());
}

void RepoView::openFileManager(const QString &path) {
  ShowTool::openFileManager(path);
}

void RepoView::ignore(const QString &name) {
  QFile file(mRepo.workdir().filePath(".gitignore"));
  if (!file.open(QFile::Append | QFile::Text))
    return;

  QTextStream(&file) << name << "\n";
  file.close();

  refresh(true);
}

EditorWindow *RepoView::newEditor() {
  EditorWindow *editor = new EditorWindow(repo(), window());
  editor->show();
  return editor;
}

bool RepoView::edit(const QString &path, int line) {
  return openSubmodule(mRepo.lookupSubmodule(path)) || openEditor(path, line);
}

EditorWindow *RepoView::openEditor(const QString &path, int line,
                                   const git::Blob &blob,
                                   const git::Commit &commit) {
  EditorWindow *window = EditorWindow::open(path, blob, commit, mRepo);
  if (!window)
    return nullptr;

  // Scroll line into view.
  BlameEditor *widget = window->widget();
  if (line >= 0) {
    TextEditor *editor = widget->editor();
    editor->ensureVisibleEnforcePolicy(line - 1);
    editor->gotoLine(line - 1);
  }

  connect(widget, &BlameEditor::linkActivated, this, &RepoView::visitLink);

  // Track this window.
  mTrackedWindows.append(window);
  connect(window, &QObject::destroyed, this,
          [this, window] { mTrackedWindows.removeAll(window); });

  return window;
}

void RepoView::refresh() { refresh(true); }

void RepoView::refreshAll() {
  emit manualRefreshRequested();
  refresh();
}

void RepoView::refresh(bool restoreSelection) {
  mRepo.invalidateSubmoduleCache();

  // Fake head update.
  auto dtw = findChild<DoubleTreeWidget *>();
  if (dtw) {
    dtw->setDiffCounter();
  }
  if (mRepo.head().isValid()) {
    DebugRefresh("Head name: " << mRepo.head().name());
  } else {
    DebugRefresh("Head invalid");
  }
  DebugRefresh("time: " << QDateTime::currentDateTime()
                        << " Set diff counter: " << counter);
  emit mRepo.notifier()->referenceUpdated(mRepo.head(), restoreSelection);
}

void RepoView::setPathspec(const QString &path) {
  mPathspec->setPathspec(path);
}

git::Commit RepoView::nextRevision(const QString &path) const {
  QList<git::Commit> commits = mCommits->selectedCommits();
  if (commits.isEmpty())
    return git::Commit();

  git::Reference ref = mRefs->currentReference();
  git::RevWalk walker =
      ref.walker(GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME | GIT_SORT_REVERSE);
  walker.hide(commits.first());
  return walker.next(path);
}

git::Commit RepoView::previousRevision(const QString &path) const {
  // Special case: Working tree to commit
  QList<git::Commit> commits = mCommits->selectedCommits();
  if (commits.isEmpty()) {
    git::Reference ref = mRefs->currentReference();
    if (!ref.isValid())
      return git::Commit();

    git::RevWalk walker = ref.walker(GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
    return walker.next(path);
  }

  git::Commit commit = commits.last();
  git::RevWalk walker = commit.walker(GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
  walker.next();
  return walker.next(path);
}

void RepoView::selectCommit(const git::Commit &commit, const QString &file) {
  mCommits->selectRange(commit.id().toString(), file, true);
}

RepoView *RepoView::parentView(const QWidget *widget) {
  QWidget *parent = widget->parentWidget();
  if (!parent)
    return nullptr;

  if (RepoView *view = qobject_cast<RepoView *>(parent))
    return view;

  return parentView(parent);
}

bool RepoView::detailsMaximized() { return mMaximized; }

void RepoView::showEvent(QShowEvent *event) {
  QSplitter::showEvent(event);

  if (mShown)
    return;

  // Start background tasks after showing for the first time.
  mShown = true;
  startFetchTimer();
  if (Settings::instance()
          ->value(Setting::Id::CheckSubmodulesForUpdatesAutomatically)
          .toBool())
    QTimer::singleShot(0, this, [this] { checkSubmoduleUpdates(true); });
}

void RepoView::closeEvent(QCloseEvent *event) {
  PerformanceTrace::event("close", "RepoView closeEvent entered",
                          mRepo.dir(false).path());
  if (mClosing) {
    event->accept();
    return;
  }

  // Try to close tracked windows.
  foreach (QWidget *window, mTrackedWindows) {
    if (!window->close()) {
      event->ignore();
      return;
    }
  }

  mClosing = true;
  mFetchTimer.stop();
  mSubmoduleUpdateCheckPending = false;
  ++mSubmoduleConfigurationGeneration;
  setAttribute(Qt::WA_DeleteOnClose, false);
  cancelBackgroundTasks();
  finishClosing();
  QSplitter::closeEvent(event);
  PerformanceTrace::event("close", "RepoView closeEvent accepted",
                          mRepo.dir(false).path());
}

void RepoView::finishClosing() {
  if (!mClosing)
    return;

  bool active = mIndexer.state() != QProcess::NotRunning || mWatcher ||
                mSubmoduleUpdateWatcher || mSubmodulePushCheckWatcher;
  QJsonObject fields;
  fields["indexer"] = mIndexer.state() != QProcess::NotRunning;
  fields["remote"] = mWatcher != nullptr;
  fields["submoduleUpdate"] = mSubmoduleUpdateWatcher != nullptr;
  fields["submodulePushCheck"] = mSubmodulePushCheckWatcher != nullptr;
  PerformanceTrace::event("close", active ? "finishClosing active"
                                           : "finishClosing deleteLater",
                          mRepo.dir(false).path(), fields);
  if (active) {
    mCloseCleanupTimer.start(50);
    return;
  }

  deleteLater();
}

void RepoView::startInitialLoadProgress() {
  if (!mOpenProgress)
    mOpenProgress = new RepositoryOpenProgress(this);
  mOpenProgress->start();
  if (mInitialLoadFinished)
    mOpenProgress->finish();
}

void RepoView::finishInitialLoad() {
  if (mInitialLoadFinished)
    return;

  mInitialLoadFinished = true;
  if (mOpenProgress)
    mOpenProgress->finish();
  emit initialLoadFinished();
  updateActivity();
}

bool RepoView::hasBackgroundActivity() const {
  return !mInitialLoadFinished || mWatcher || mSubmoduleUpdateWatcher ||
         mSubmodulePushCheckWatcher;
}

void RepoView::updateActivity() {
  const bool active = hasBackgroundActivity();
  if (mBackgroundActivity == active)
    return;

  mBackgroundActivity = active;
  emit activityChanged(active);
}

void RepoView::paintEvent(QPaintEvent *event) {
  QSplitter::paintEvent(event);
  if (!mFirstPaintTraced) {
    mFirstPaintTraced = true;
    PerformanceTrace::event("startup", "RepoView first paint",
                            mRepo.dir(false).path());
  }
}

ToolBar *RepoView::toolBar() const {
  return static_cast<MainWindow *>(window())->toolBar();
}

CommitList *RepoView::commitList() const { return mCommits; }

void RepoView::notifyReferenceUpdated(const QString &name) {
  emit mRepo.notifier()->referenceUpdated(mRepo.lookupRef(name), true);
}

bool RepoView::checkForConflicts(LogEntry *parent, const QString &action) {
  DebugRefresh("Has conflicts: " << mRepo.index().hasConflicts());
  // Check for conflicts.
  if (!mRepo.index().hasConflicts())
    return false;

  QString error = tr("There was a merge conflict.");
  LogEntry *entry = parent->addEntry(LogEntry::Error, error);

  QString help = tr("Resolve conflicts, then commit to conclude "
                    "the %1. See <a href='expand'>details</a>.");
  QString conflicts = tr("Resolve conflicts in each conflicted (!) file in "
                         "one of the following ways:");
  QString hint1 = tr("1. Click the 'Ours' or 'Theirs' button to choose the "
                     "correct change. Then click the 'Save' button to apply.");
  QString hint2 = tr("2. Edit the file in the editor to make a different "
                     "change. Remember to remove conflict markers.");
  QString hint3 = tr("3. Use an external merge tool. Right-click on the "
                     "files in the list and choose 'External Merge'.");
  QString mark = tr("After all conflicts in the file are resolved, "
                    "click the check box to mark it as resolved.");
  QString commit = tr("After all conflicted files are staged, "
                      "commit to conclude the %1.");
  LogEntry *details = entry->addEntry(LogEntry::Hint, help.arg(action));
  LogEntry *resolve = details->addEntry(LogEntry::Entry, conflicts);
  resolve->addEntry(LogEntry::Entry, hint1);
  resolve->addEntry(LogEntry::Entry, hint2);
  resolve->addEntry(LogEntry::Entry, hint3);
  details->addEntry(LogEntry::Entry, mark);
  details->addEntry(LogEntry::Entry, commit.arg(action));
  mLogView->setEntryExpanded(details, false);

  if (action != tr("squash")) {
    QString abort = tr("You can <a href='action:abort'>abort</a> the %1 "
                       "to return the repository to its previous state.");
    entry->addEntry(LogEntry::Hint, abort.arg(action));
  }

  refresh(false);
  return true;
}

git::Signature RepoView::getSignature(const ContributorInfo &info) {
  if (info.commitDateType != ContributorInfo::SelectedDateTimeType::Current)
    return mRepo.signature(info.name, info.email, info.commitDate);

  return mRepo.signature(info.name, info.email);
}

bool RepoView::match(QObject *search, QObject *parent) {
  QObjectList children = parent->children();
  for (auto child : children) {
    if (child == search)
      return true;

    if (match(search, child))
      return true;
  }
  return false;
}

RepoView::DetailSplitterWidgets
RepoView::detailSplitterMaximize(bool maximized,
                                 DetailSplitterWidgets maximizeWidget) {
  QWidget *widget = mDetailSplitter->focusWidget();

  DetailSplitterWidgets newMaximized = DetailSplitterWidgets::NotDefined;

  if (maximizeWidget != DetailSplitterWidgets::NotDefined)
    newMaximized = maximizeWidget;

  mMaximized = maximized;

  if (mMaximized) {
    bool found = false;
    for (int i = 0; i < mDetailSplitter->count(); i++) {
      QWidget *w = mDetailSplitter->widget(i);
      if (maximizeWidget == DetailSplitterWidgets::SideBar) {
        if (w == mPrimaryView) {
          mPrimaryView->setVisible(true);
          found = true;
          continue;
        }
      } else if (maximizeWidget == DetailSplitterWidgets::DetailView) {
        if (w == mDetails) {
          mDetails->setVisible(true);
          found = true;
          continue;
        }
      } else if (!widget)
        return DetailSplitterWidgets::NotDefined;
      else if (w == widget || match(widget, w)) {
        w->setVisible(true);
        found = true;
        if (w == mPrimaryView)
          newMaximized = DetailSplitterWidgets::SideBar;
        else if (w == mDetails)
          newMaximized = DetailSplitterWidgets::DetailView;
        continue;
      }
      w->setVisible(false);
    }

    assert(found);
    Q_UNUSED(found)
  } else {
    for (int i = 0; i < mDetailSplitter->count(); i++)
      mDetailSplitter->widget(i)->setVisible(true);
  }

  return newMaximized;
}
#include "RepoView.moc"

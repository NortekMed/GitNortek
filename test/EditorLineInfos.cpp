#include "Test.h"
#include "Debug.h"
#include "dialogs/ExternalToolsDialog.h"

#include "ui/DiffView/HunkWidget.h"
#include "ui/DiffView/FileWidget.h"
#include "ui/DiffView/CompleteFileDiffWidget.h"
#include "app/Application.h"
#include "conf/Settings.h"

#include "ui/MainWindow.h"
#include "ui/DiffView/DiffView.h"
#include "ui/RepoView.h"
#include <QString>

#include "git/Reference.h"
#include "git/Diff.h"
#include "git/Commit.h"
#include "git/Tree.h"
#include <QFile>
#include <QImage>
#include <QPalette>
#include <QPushButton>
#include <QScrollBar>
#include <QTest>
#include <QToolButton>
#include <QVBoxLayout>

#define INIT_REPO(repoPath, /* bool */ useTempDir)                             \
  QString path = Test::extractRepository(repoPath, useTempDir);                \
  QVERIFY2(!path.isEmpty(), qPrintable("Extracting repository failed"));       \
  mRepo = git::Repository::open(path);                                         \
  QVERIFY2(mRepo.isValid(), qPrintable("Unable to open repository"));          \
  Test::initRepo(mRepo);                                                       \
  MainWindow window(mRepo);                                                    \
  window.show();                                                               \
  QVERIFY(QTest::qWaitForWindowExposed(&window));                              \
                                                                               \
  git::Reference head = mRepo.head();                                          \
  git::Commit commit = head.target();                                          \
  git::Diff stagedDiff = mRepo.diffTreeToIndex(commit.tree()); /* correct */   \
                                                                               \
  RepoView *repoView = window.currentView();                                   \
  Test::refresh(repoView);                                                     \
  DiffView diffView = DiffView(mRepo, repoView);                               \
  auto diff = mRepo.status(mRepo.index(), nullptr, false);

#define BITSET(value, bit) ((value & (1 << bit)) == (1 << bit))

#define checkEditorMarkers(editor, unstagedAddition, stagedAddition,           \
                           unstagedDeletion, stagedDeletion)                   \
  for (int i = 0; i < editor->lineCount(); i++) {                              \
    QString line = editor->line(i);                                            \
    int markers = editor->markers(i);                                          \
                                                                               \
    QString state = "";                                                        \
    if (BITSET(markers, TextEditor::Marker::Addition))                         \
      state = "Addition";                                                      \
    else if (BITSET(markers, TextEditor::Marker::Deletion))                    \
      state = "Deletion";                                                      \
    else if (BITSET(markers, TextEditor::Marker::Context))                     \
      state = "Unchanged";                                                     \
    else                                                                       \
      state = QString::number(markers);                                        \
                                                                               \
    Debug("Index: " << i << ", State: " << state << ", Staged: "               \
                    << BITSET(markers, TextEditor::Marker::StagedMarker)       \
                    << line);                                                  \
    if (unstagedAddition.indexOf(i) != -1) {                                   \
      /* unstaged addition */                                                  \
      QVERIFY2(BITSET(markers, TextEditor::Marker::Addition),                  \
               qPrintable("Line index: " + QString::number(i)));               \
      QVERIFY2(!BITSET(markers, TextEditor::Marker::StagedMarker),             \
               qPrintable("Line index: " + QString::number(i)));               \
    } else if (stagedAddition.indexOf(i) != -1) {                              \
      /* staged addition */                                                    \
      QVERIFY2(BITSET(markers, TextEditor::Marker::Addition),                  \
               qPrintable("Line index: " + QString::number(i)));               \
      QVERIFY2(BITSET(markers, TextEditor::Marker::StagedMarker),              \
               qPrintable("Line index: " + QString::number(i)));               \
    } else if (unstagedDeletion.indexOf(i) != -1) {                            \
      /* unstaged deletion */                                                  \
      QVERIFY2(BITSET(markers, TextEditor::Marker::Deletion),                  \
               qPrintable("Line index: " + QString::number(i)));               \
      QVERIFY2(!BITSET(markers, TextEditor::Marker::StagedMarker),             \
               qPrintable("Line index: " + QString::number(i)));               \
    } else if (stagedDeletion.indexOf(i) != -1) {                              \
      /* staged deletion */                                                    \
      QVERIFY2(BITSET(markers, TextEditor::Marker::Deletion),                  \
               qPrintable("Line index: " + QString::number(i)));               \
      QVERIFY2(BITSET(markers, TextEditor::Marker::StagedMarker),              \
               qPrintable("Line index: " + QString::number(i)));               \
    } else {                                                                   \
      QVERIFY2(!BITSET(markers, TextEditor::Marker::Deletion),                 \
               qPrintable("Line index: " + QString::number(i)));               \
      QVERIFY2(!BITSET(markers, TextEditor::Marker::Addition),                 \
               qPrintable("Line index: " + QString::number(i)));               \
      QVERIFY2(!BITSET(markers, TextEditor::Marker::StagedMarker),             \
               qPrintable("Line index: " + QString::number(i)));               \
    }                                                                          \
  }

/*
 * For DEBUGGING PURPOSE:
 * git diff HEAD: to get the diff shown in the editor
 * git diff --cached: to get the staged diff
 */

#define EXECUTE_ONLY_LAST_TEST 0

using namespace QTest;

class TestEditorLineInfo : public QObject {
  Q_OBJECT

private slots:
  void initTestCase();
  void cleanupTestCase();

  // void editorLineMarkers1();
#if EXECUTE_ONLY_LAST_TEST == 0
  void editorLineSingleHunkAdditionStaged();
  void editorLineSingleHunkDeletionStaged();
  void editorLineSingleHunkChangeStaged();
  void editorLineSingleHunkChange_onlyAdditionStaged();
  void editorLineSingleHunkChange_onlyDeletionStaged();
  void singleHunk_multipleDeletions();
  void singleHunk_multipleAdditions();
  void multipleHunks_multipleDeletions();
  void multipleHunks_multipleAdditions();
  void multipleHunks_misc1();
  void singleHunk_additionsOnly_secondStagedPatch();
  void singleHunk_deletionsOnly_secondStagedPatch();
  void multipleHunks_StageSingleLines();
  void multipleHunks_StageSingleLines2();
#ifdef Q_OS_WIN
  void windowsCRLF();
#endif // Q_OS_WIN

  void windowsCRLFMultiHunk();
  void sameContentRemoveLine();
  void completeFilePresentationModes();
  void sameContentAddLine();

#endif // EXECUTE_ONLY_LAST_TEST == 0

  void discardCompleteDeletedContent();
  void discardCompleteAddedContent();

  //  void deleteCompleteContent();

private:
  int closeDelay = 0;
  git::Repository mRepo;
};

void TestEditorLineInfo::initTestCase() {}

#if EXECUTE_ONLY_LAST_TEST == 0
void TestEditorLineInfo::editorLineSingleHunkAdditionStaged() {
  INIT_REPO("01_singleHunkAdditionStaged.zip", true)
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);

  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
                       repoView);
  hw.load(stagedPatch, true);
  auto editor = hw.editor();

  for (int i = 0; i < editor->lineCount(); i++) {
    QString line = editor->line(i);
    int markers = editor->markers(i);

    if (i == 3) {
      QVERIFY(BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
      QCOMPARE(line, "3.5\n");
    } else {
      QVERIFY(!BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    }
  }
}

void TestEditorLineInfo::editorLineSingleHunkDeletionStaged() {
  INIT_REPO("02_singleHunkDeletionStaged.zip", true)
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);

  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
                       repoView);
  hw.load(stagedPatch, true);
  auto editor = hw.editor();

  for (int i = 0; i < editor->lineCount(); i++) {
    QString line = editor->line(i);
    int markers = editor->markers(i);

    if (i == 2) {
      QVERIFY(BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
      QCOMPARE(line, "3\n");
    } else {
      QVERIFY(!BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    }
  }
}

void TestEditorLineInfo::editorLineSingleHunkChangeStaged() {
  INIT_REPO("03_singleHunkChangeSingleLine.zip", true)
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);

  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
                       repoView);
  hw.load(stagedPatch, true);
  auto editor = hw.editor();

  for (int i = 0; i < editor->lineCount(); i++) {
    QString line = editor->line(i);
    int markers = editor->markers(i);

    switch (i) {
      case 2:
        QVERIFY(BITSET(markers, TextEditor::Marker::Deletion));
        QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
        QCOMPARE(line, "3\n");
        break;
      case 3:
        QVERIFY(BITSET(markers, TextEditor::Marker::Addition));
        QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
        QCOMPARE(line, "3.5\n");
        break;
      default:
        QVERIFY(!BITSET(markers, TextEditor::Marker::Deletion));
        QVERIFY(!BITSET(markers, TextEditor::Marker::Addition));
        QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
        break;
    }
  }
}

void TestEditorLineInfo::editorLineSingleHunkChange_onlyAdditionStaged() {
  INIT_REPO("04_singleHunkChange_onlyAdditionStaged.zip", true)
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);

  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
                       repoView);
  hw.load(stagedPatch, true);
  auto editor = hw.editor();

  for (int i = 0; i < editor->lineCount(); i++) {
    QString line = editor->line(i);
    int markers = editor->markers(i);

    switch (i) {
      case 2:
        QVERIFY(BITSET(markers, TextEditor::Marker::Deletion));
        QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
        QCOMPARE(line, "3\n");
        break;
      case 3:
        QVERIFY(BITSET(markers, TextEditor::Marker::Addition));
        QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
        QCOMPARE(line, "3.5\n");
        break;
      default:
        QVERIFY(!BITSET(markers, TextEditor::Marker::Deletion));
        QVERIFY(!BITSET(markers, TextEditor::Marker::Addition));
        QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
        break;
    }
  }
}

void TestEditorLineInfo::editorLineSingleHunkChange_onlyDeletionStaged() {
  INIT_REPO("05_singleHunkChange_onlyDeletionStaged.zip", true)
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);

  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
                       repoView);
  hw.load(stagedPatch, true);
  auto editor = hw.editor();

  for (int i = 0; i < editor->lineCount(); i++) {
    QString line = editor->line(i);
    int markers = editor->markers(i);

    switch (i) {
      case 2:
        QVERIFY(BITSET(markers, TextEditor::Marker::Deletion));
        QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
        QCOMPARE(line, "3\n");
        break;
      case 3:
        QVERIFY(BITSET(markers, TextEditor::Marker::Addition));
        QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
        QCOMPARE(line, "3.5\n");
        break;
      default:
        QVERIFY(!BITSET(markers, TextEditor::Marker::Deletion));
        QVERIFY(!BITSET(markers, TextEditor::Marker::Addition));
        QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
        break;
    }
  }
}

void TestEditorLineInfo::singleHunk_multipleDeletions() {
  INIT_REPO("06_singleHunk_multipleDeletions.zip", true)
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);

  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
                       repoView);
  hw.load(stagedPatch, true);
  auto editor = hw.editor();

  for (int i = 0; i < editor->lineCount(); i++) {
    QString line = editor->line(i);
    int markers = editor->markers(i);

    QVector<int> unstaged({3, 4, 7, 8, 10});
    QVector<int> staged({5, 6, 9});
    if (unstaged.indexOf(i) != -1) {
      QVERIFY(BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    } else if (staged.indexOf(i) != -1) {
      QVERIFY(BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
    } else {
      QVERIFY(!BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    }
  }
}

void TestEditorLineInfo::singleHunk_multipleAdditions() {
  INIT_REPO("07_singleHunk_multipleAdditions.zip", true)
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);

  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
                       repoView);
  hw.load(stagedPatch, true);
  auto editor = hw.editor();

  for (int i = 0; i < editor->lineCount(); i++) {
    QString line = editor->line(i);
    int markers = editor->markers(i);

    QVector<int> unstaged({3, 6, 7, 10});
    QVector<int> staged({4, 5, 8, 9, 11});
    if (unstaged.indexOf(i) != -1) {
      // unstaged
      QVERIFY(BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    } else if (staged.indexOf(i) != -1) {
      // staged
      QVERIFY(BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
    } else {
      QVERIFY(!BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    }
  }
}

void TestEditorLineInfo::multipleHunks_multipleDeletions() {
  INIT_REPO("08_multipleHunks_multipleDeletions.zip", true)
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);

  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
                       repoView);
  hw.load(stagedPatch, true);
  auto editor = hw.editor();

  for (int i = 0; i < editor->lineCount(); i++) {
    QString line = editor->line(i);
    int markers = editor->markers(i);

    QVector<int> unstaged({3, 6, 7});
    QVector<int> staged({4, 5});
    if (unstaged.indexOf(i) != -1) {
      // unstaged
      QVERIFY(BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    } else if (staged.indexOf(i) != -1) {
      // staged
      QVERIFY(BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
    } else {
      QVERIFY(!BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    }
  }

  // Second hunk
  auto hw2 = HunkWidget(&diffView, diff, patch, stagedPatch, 1, false, false,
                        repoView);
  hw2.load(stagedPatch, true);
  editor = hw2.editor();

  for (int i = 0; i < editor->lineCount(); i++) {
    QString line = editor->line(i);
    int markers = editor->markers(i);

    QVector<int> unstaged({3, 4, 7, 8});
    QVector<int> staged({5, 6});
    if (unstaged.indexOf(i) != -1) {
      // unstaged
      QVERIFY(BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    } else if (staged.indexOf(i) != -1) {
      // staged
      QVERIFY(BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
    } else {
      QVERIFY(!BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    }
  }
}

void TestEditorLineInfo::multipleHunks_multipleAdditions() {
  INIT_REPO("09_multipleHunks_multipleAdditions.zip", true)
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);

  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
                       repoView);
  hw.load(stagedPatch, true);
  auto editor = hw.editor();

  for (int i = 0; i < editor->lineCount(); i++) {
    QString line = editor->line(i);
    int markers = editor->markers(i);

    QVector<int> unstaged({3, 4, 7, 8});
    QVector<int> staged({5, 6, 9});
    if (unstaged.indexOf(i) != -1) {
      // unstaged
      QVERIFY(BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    } else if (staged.indexOf(i) != -1) {
      // staged
      QVERIFY(BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
    } else {
      QVERIFY(!BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    }
  }

  // Second hunk
  auto hw2 = HunkWidget(&diffView, diff, patch, stagedPatch, 1, false, false,
                        repoView);
  hw2.load(stagedPatch, true);
  editor = hw2.editor();

  for (int i = 0; i < editor->lineCount(); i++) {
    QString line = editor->line(i);
    int markers = editor->markers(i);

    QVector<int> unstaged({3, 6, 7, 8, 10});
    QVector<int> staged({4, 5, 9, 11});
    if (unstaged.indexOf(i) != -1) {
      // unstaged
      QVERIFY(BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    } else if (staged.indexOf(i) != -1) {
      // staged
      QVERIFY(BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
    } else {
      QVERIFY(!BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    }
  }
}

void TestEditorLineInfo::singleHunk_additionsOnly_secondStagedPatch() {
  // Testing the finding of the staged patch index

  INIT_REPO("11_singleHunk_additionsOnly_secondStagedPatch.zip", true)
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);

  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
                       repoView);
  hw.load(stagedPatch, true);
  auto editor = hw.editor();

  for (int i = 0; i < editor->lineCount(); i++) {
    QString line = editor->line(i);
    int markers = editor->markers(i);

    QVector<int> unstagedAddition({2});
    QVector<int> stagedAddition({6});
    if (unstagedAddition.indexOf(i) != -1) {
      // unstaged addition
      QVERIFY(BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    } else if (stagedAddition.indexOf(i) != -1) {
      // staged addition
      QVERIFY(BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
    } else {
      QVERIFY(!BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    }
  }
}

void TestEditorLineInfo::singleHunk_deletionsOnly_secondStagedPatch() {
  // Testing the finding of the staged patch index

  INIT_REPO("12_singleHunk_deletionsOnly_secondStagedPatch.zip", true)
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);

  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
                       repoView);
  hw.load(stagedPatch, true);
  auto editor = hw.editor();

  for (int i = 0; i < editor->lineCount(); i++) {
    QString line = editor->line(i);
    int markers = editor->markers(i);

    QVector<int> unstagedDeletion({1});
    QVector<int> stagedDeletion({4});
    if (unstagedDeletion.indexOf(i) != -1) {
      // unstaged addition
      QVERIFY(BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    } else if (stagedDeletion.indexOf(i) != -1) {
      // staged addition
      QVERIFY(BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
    } else {
      QVERIFY(!BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    }
  }
}

void TestEditorLineInfo::multipleHunks_misc1() {
  INIT_REPO("10_misc.zip", true)
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);

  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
                       repoView);
  hw.load(stagedPatch, true);
  auto editor = hw.editor();

  for (int i = 0; i < editor->lineCount(); i++) {
    QString line = editor->line(i);
    int markers = editor->markers(i);

    QVector<int> unstagedAddition({6, 9, 19, 23});
    QVector<int> stagedAddition({24, 25});
    QVector<int> unstagedDeletion({3, 4, 5, 16, 17, 18, 21, 22});
    QVector<int> stagedDeletion({});
    if (unstagedAddition.indexOf(i) != -1) {
      // unstaged addition
      QVERIFY(BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    } else if (stagedAddition.indexOf(i) != -1) {
      // staged addition
      QVERIFY(BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
    } else if (unstagedDeletion.indexOf(i) != -1) {
      // unstaged deletion
      QVERIFY(BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    } else if (stagedDeletion.indexOf(i) != -1) {
      // staged deletion
      QVERIFY(BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(BITSET(markers, TextEditor::Marker::StagedMarker));
    } else {
      QVERIFY(!BITSET(markers, TextEditor::Marker::Deletion));
      QVERIFY(!BITSET(markers, TextEditor::Marker::Addition));
      QVERIFY(!BITSET(markers, TextEditor::Marker::StagedMarker));
    }
  }
}

// void TestEditorLineInfo::editorLineMarkers1() {
//     //INIT_REPO("editorLineMarkers1.zip")
////     QString path =
/// Test::extractRepository("01_singleHunkAdditionStaged.zip"); /
/// QVERIFY(!path.isEmpty()); /    mRepo = git::Repository::open(path); /
/// QVERIFY(mRepo.isValid()); /    auto window = new MainWindow(mRepo);

////    git::Reference head = mRepo.head();
////    git::Commit commit = head.target();
////    git::Diff stagedDiff = mRepo.diffTreeToIndex(commit.tree()); // correct
////    QVERIFY(stagedDiff.count() > 0);

////    RepoView *view = window->currentView();
////    //Test::refresh(view);
////    auto diff = mRepo.status(mRepo.index(), nullptr, false);
/////commit.diff(git::Commit(), -1, false); /    QVERIFY(diff.count() > 0); /
/// git::Patch patch = diff.patch(0); /    auto stagedPatch =
/// stagedDiff.patch(0); /    DiffView diffView = DiffView(mRepo, view);

////    auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false,
/// false, view); /    hw.load(stagedPatch, true);
//}

void TestEditorLineInfo::multipleHunks_StageSingleLines() {
  INIT_REPO("13_singleHunkNoStaged.zip", true)
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  // no staged lines yet, so no staged patch
  QCOMPARE(mRepo.diffTreeToIndex(commit.tree()).count(), 0);
  git::Patch stagedPatch = git::Patch();

  QString name = patch.name();
  QString path_ = mRepo.workdir().filePath(name);
  bool submodule = mRepo.lookupSubmodule(name).isValid();
  {
    FileWidget fw(&diffView, diff, patch, stagedPatch, QModelIndex(), name,
                  path_, submodule);
    fw.setStageState(git::Index::StagedState::Unstaged);

    auto hunks = fw.hunks();
    QVERIFY(hunks.count() == 2);
    for (auto *hunk : hunks)
      hunk->load();

    checkEditorMarkers(hunks.at(0)->editor(),
                       QVector<int>({0, 1, 2, 3, 4, 5, 11}), QVector<int>(),
                       QVector<int>(), QVector<int>());
    checkEditorMarkers(
        hunks.at(1)->editor(),
        QVector<int>({3, 4, 5, 6, 7, 8, 9, 10, 15, 16, 20, 21, 22, 23, 30}),
        QVector<int>({}), QVector<int>({14, 19, 29}), QVector<int>());

    // Stage single lines
    hunks.at(0)->stageSelected(5, 6); // stage line 5
  }

  Test::refresh(repoView);
  stagedDiff = mRepo.diffTreeToIndex(commit.tree()); /* correct */
  diff = mRepo.status(mRepo.index(), nullptr, false);
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  stagedPatch = stagedDiff.patch(0);

  {
    FileWidget fw(&diffView, diff, patch, stagedPatch, QModelIndex(), name,
                  path, submodule);

    // It is important that all hunks are loaded!!!!
    auto hunks = fw.hunks();
    QVERIFY(hunks.count() == 2);
    for (auto *hunk : hunks)
      hunk->load();

    checkEditorMarkers(hunks.at(0)->editor(), QVector<int>({0, 1, 2, 3, 4, 11}),
                       QVector<int>({5}), QVector<int>(), QVector<int>());
    checkEditorMarkers(
        hunks.at(1)->editor(),
        QVector<int>({3, 4, 5, 6, 7, 8, 9, 10, 15, 16, 20, 21, 22, 23, 30}),
        QVector<int>({}), QVector<int>({14, 19, 29}), QVector<int>());

    hunks.at(0)->stageSelected(4, 5); // stage line 4
  }

  Test::refresh(repoView);
  stagedDiff = mRepo.diffTreeToIndex(commit.tree()); /* correct */
  diff = mRepo.status(mRepo.index(), nullptr, false);
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  stagedPatch = stagedDiff.patch(0);

  {
    FileWidget fw(&diffView, diff, patch, stagedPatch, QModelIndex(), name,
                  path, submodule);

    auto hunks = fw.hunks();
    QCOMPARE(hunks.count(), 2);
    for (auto *hunk : hunks)
      hunk->load();

    checkEditorMarkers(hunks.at(0)->editor(), QVector<int>({0, 1, 2, 3, 11}),
                       QVector<int>({4, 5}), QVector<int>(), QVector<int>());
    checkEditorMarkers(
        hunks.at(1)->editor(),
        QVector<int>({3, 4, 5, 6, 7, 8, 9, 10, 15, 16, 20, 21, 22, 23, 30}),
        QVector<int>({}), QVector<int>({14, 19, 29}), QVector<int>());
  }
}

void TestEditorLineInfo::multipleHunks_StageSingleLines2() {
  /*
   * Staging the last line, first only the deleted one
   * and then the added line
   */

  INIT_REPO("13_singleHunkNoStaged.zip", true)
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  // no staged lines yet, so no staged patch
  QCOMPARE(mRepo.diffTreeToIndex(commit.tree()).count(), 0);
  git::Patch stagedPatch = git::Patch();

  QString name = patch.name();
  QString path_ = mRepo.workdir().filePath(name);
  bool submodule = mRepo.lookupSubmodule(name).isValid();

  {
    FileWidget fw(&diffView, diff, patch, stagedPatch, QModelIndex(), name,
                  path_, submodule);
    fw.setStageState(git::Index::StagedState::Unstaged);

    auto hunks = fw.hunks();
    QVERIFY(hunks.count() == 2);
    for (auto *hunk : hunks)
      hunk->load();

    checkEditorMarkers(hunks.at(0)->editor(),
                       QVector<int>({0, 1, 2, 3, 4, 5, 11}), QVector<int>(),
                       QVector<int>(), QVector<int>());
    checkEditorMarkers(
        hunks.at(1)->editor(),
        QVector<int>({3, 4, 5, 6, 7, 8, 9, 10, 15, 16, 20, 21, 22, 23, 30}),
        QVector<int>({}), QVector<int>({14, 19, 29}), QVector<int>());

    // Stage single lines
    hunks.at(1)->stageSelected(29, 30);
  }

  Test::refresh(repoView);
  stagedDiff = mRepo.diffTreeToIndex(commit.tree()); /* correct */
  diff = mRepo.status(mRepo.index(), nullptr, false);
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  stagedPatch = stagedDiff.patch(0);

  {
    FileWidget fw(&diffView, diff, patch, stagedPatch, QModelIndex(), name,
                  path, submodule);

    // It is important that all hunks are loaded!!!!
    auto hunks = fw.hunks();
    QVERIFY(hunks.count() == 2);
    for (auto *hunk : hunks)
      hunk->load();

    checkEditorMarkers(hunks.at(0)->editor(),
                       QVector<int>({0, 1, 2, 3, 4, 5, 11}), QVector<int>(),
                       QVector<int>(), QVector<int>());
    checkEditorMarkers(
        hunks.at(1)->editor(),
        QVector<int>({3, 4, 5, 6, 7, 8, 9, 10, 15, 16, 20, 21, 22, 23, 30}),
        QVector<int>({}), QVector<int>({14, 19}), QVector<int>({29}));

    hunks.at(1)->stageSelected(30, 31);
  }

  Test::refresh(repoView);
  stagedDiff = mRepo.diffTreeToIndex(commit.tree()); /* correct */
  diff = mRepo.status(mRepo.index(), nullptr, false);
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  stagedPatch = stagedDiff.patch(0);

  {
    FileWidget fw(&diffView, diff, patch, stagedPatch, QModelIndex(), name,
                  path, submodule);

    auto hunks = fw.hunks();
    QCOMPARE(hunks.count(), 2);
    for (auto *hunk : hunks)
      hunk->load();

    checkEditorMarkers(hunks.at(0)->editor(),
                       QVector<int>({0, 1, 2, 3, 4, 5, 11}), QVector<int>(),
                       QVector<int>(), QVector<int>());
    checkEditorMarkers(
        hunks.at(1)->editor(),
        QVector<int>({3, 4, 5, 6, 7, 8, 9, 10, 15, 16, 20, 21, 22, 23}),
        QVector<int>({30}), QVector<int>({14, 19}), QVector<int>({29}));
  }
}

#ifdef Q_OS_WIN
// This test is only relevant on windows, because on linux this scenario does
// not happen. The problem with this repo on linux is that the repo was created
// on windows but git uses internally \n instead of \r\n so when opening this
// repo on linux, the diff shows more than it should, because all unchanged
// lines have \n and all changed lines have \r\n so the diff looks different.
// This cannot happen normaly, because when cloning, git converts automatically
// for the used OS
void TestEditorLineInfo::windowsCRLF() {
  /*
   * Staging single lines in a file with CRLF instead of single LF
   * The repository was created on windows
   */

  INIT_REPO("14_windowsCRLF.zip", true)
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  // no staged lines yet, so no staged patch
  QCOMPARE(mRepo.diffTreeToIndex(commit.tree()).count(), 0);
  git::Patch stagedPatch = git::Patch();

  QString name = patch.name();
  QString path_ = mRepo.workdir().filePath(name);
  bool submodule = mRepo.lookupSubmodule(name).isValid();

  {
    FileWidget fw(&diffView, diff, patch, stagedPatch, QModelIndex(), name,
                  path_, submodule);
    fw.setStageState(git::Index::StagedState::Unstaged);

    auto hunks = fw.hunks();
    QVERIFY(hunks.count() == 1);
    hunks[0]->load();

    checkEditorMarkers(hunks.at(0)->editor(), QVector<int>(), QVector<int>(),
                       QVector<int>({3, 4, 5}), QVector<int>());

    // Stage single lines
    hunks[0]->stageSelected(3, 5);
  }

  Test::refresh(repoView);
  stagedDiff = mRepo.diffTreeToIndex(commit.tree()); /* correct */
  diff = mRepo.status(mRepo.index(), nullptr, false);
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  stagedPatch = stagedDiff.patch(0);

  {
    FileWidget fw(&diffView, diff, patch, stagedPatch, QModelIndex(), name,
                  path, submodule);

    auto hunks = fw.hunks();
    QVERIFY(hunks.count() == 1);
    hunks[0]->load();

    checkEditorMarkers(hunks.at(0)->editor(), QVector<int>(), QVector<int>(),
                       QVector<int>({5}), QVector<int>({3, 4}));
  }
}
#endif // Q_OS_WIN

void TestEditorLineInfo::windowsCRLFMultiHunk() {
  /*
   * Staging single lines in a file with CRLF instead of single LF for multiple
   * hunks. The CRLF file was created directly on linux and not on windows
   */

  INIT_REPO("15_windowsCRLF_multipleHunks.zip", true)
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  // no staged lines yet, so no staged patch
  QCOMPARE(mRepo.diffTreeToIndex(commit.tree()).count(), 0);
  git::Patch stagedPatch = git::Patch();

  QString name = patch.name();
  QString path_ = mRepo.workdir().filePath(name);
  bool submodule = mRepo.lookupSubmodule(name).isValid();

  {
    FileWidget fw(&diffView, diff, patch, stagedPatch, QModelIndex(), name,
                  path_, submodule);
    fw.setStageState(git::Index::StagedState::Unstaged);

    auto hunks = fw.hunks();
    QVERIFY(hunks.count() == 2);
    hunks[0]->load();
    hunks[1]->load();

    checkEditorMarkers(hunks.at(0)->editor(), QVector<int>(), QVector<int>(),
                       QVector<int>({3, 4}), QVector<int>());

    checkEditorMarkers(hunks.at(1)->editor(), QVector<int>(), QVector<int>(),
                       QVector<int>({3}), QVector<int>());

    // Stage single lines
    hunks[0]->stageSelected(3, 4); // stage first deletion
  }

  Test::refresh(repoView);
  stagedDiff = mRepo.diffTreeToIndex(commit.tree()); /* correct */
  diff = mRepo.status(mRepo.index(), nullptr, false);
  QVERIFY(stagedDiff.count() == 1);
  QVERIFY(diff.count() == 1); // one file changed
  stagedPatch = stagedDiff.patch(0);

  {
    FileWidget fw(&diffView, diff, patch, stagedPatch, QModelIndex(), name,
                  path, submodule);

    auto hunks = fw.hunks();
    QVERIFY(hunks.count() == 2);
    hunks[0]->load();
    hunks[1]->load();

    checkEditorMarkers(hunks.at(0)->editor(), QVector<int>(), QVector<int>(),
                       QVector<int>({4}), QVector<int>({3}));

    checkEditorMarkers(hunks.at(1)->editor(), QVector<int>(), QVector<int>(),
                       QVector<int>({3}), QVector<int>({}));
  }
}

void TestEditorLineInfo::sameContentRemoveLine() {
  INIT_REPO("16_LinestagingLineContent.zip", true)
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);

  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
                       repoView);
  hw.load(stagedPatch, true);
  checkEditorMarkers(hw.editor(), QVector<int>({3, 4, 5, 12, 13, 21, 22}),
                     QVector<int>(), QVector<int>({11, 19, 20}),
                     QVector<int>({10}));
}

void TestEditorLineInfo::completeFilePresentationModes() {
  INIT_REPO("16_LinestagingLineContent.zip", true)
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);
  const QString name = patch.name();
  const QString path_ = mRepo.workdir().filePath(name);

  Settings::instance()->setDiffMode(Settings::DiffMode::Inline);
  FileWidget inlineFile(&diffView, diff, patch, stagedPatch, QModelIndex(), name,
                        path_, false, repoView);
  auto *inlineView =
      inlineFile.findChild<CompleteFileDiffWidget *>("InlineFileDiff");
  QVERIFY(inlineView);
  QCOMPARE(inlineFile.editors().size(), 1);
  QVERIFY(inlineFile.findChild<QPushButton *>("StageFileButton"));
  QVERIFY(inlineFile.findChild<QPushButton *>("StageHunkButton"));
  TextEditor *inlineEditor = inlineFile.editors().first();
  QFile completeFile(path_);
  QVERIFY(completeFile.open(QFile::ReadOnly));
  QString completeText = mRepo.decode(completeFile.readAll());
  completeText.replace("\r\n", "\n");
  if (completeText.endsWith('\n'))
    completeText.chop(1);
  const int completeLineCount =
      completeText.isEmpty() ? 1 : completeText.count('\n') + 1;
  QVERIFY(inlineEditor->lineCount() >= completeLineCount);

  auto modifiedBlockStarts = [](TextEditor *editor) {
    QList<int> result;
    bool previousModified = false;
    for (int line = 0; line < editor->lineCount(); ++line) {
      const int markers = editor->markers(line);
      const bool modified = BITSET(markers, TextEditor::Deletion) ||
                            BITSET(markers, TextEditor::Addition);
      if (modified && !previousModified)
        result.append(line);
      previousModified = modified;
    }
    return result;
  };
  auto checkLineNumberHighlight = [](TextEditor *editor, int highlightedLine) {
    for (int line = 0; line < editor->lineCount(); ++line) {
      const int expected = line == highlightedLine
                               ? TextEditor::ModifiedBlockLineNumber
                               : STYLE_LINENUMBER;
      QCOMPARE(editor->marginStyle(line), expected);
    }
  };

  QToolButton *inlinePrevious =
      inlineView->findChild<QToolButton *>("PreviousModifiedBlock");
  QToolButton *inlineNext =
      inlineView->findChild<QToolButton *>("NextModifiedBlock");
  QVERIFY(!inlinePrevious);
  QVERIFY(!inlineNext);
  QVERIFY(!inlineView->findChild<QWidget *>("ModifiedBlockNavigation"));
  QCOMPARE(inlineEditor->marginTypeN(TextEditor::LineNumber), SC_MARGIN_RTEXT);
  const QList<int> inlineBlocks = modifiedBlockStarts(inlineEditor);
  QVERIFY(inlineBlocks.size() > 1);
  checkLineNumberHighlight(inlineEditor, -1);

  QWidget *inlineOverview = inlineView->findChild<QWidget *>("DiffOverviewBar");
  QVERIFY(inlineOverview);
  QCOMPARE(inlineEditor->marginTypeN(TextEditor::LineNumber), SC_MARGIN_RTEXT);

  auto containsColor = [](const QImage &image, int firstColumn,
                          int lastColumn, const QColor &color) {
    for (int y = 0; y < image.height(); ++y) {
      for (int x = firstColumn; x < lastColumn; ++x) {
        if (image.pixelColor(x, y) == color)
          return true;
      }
    }
    return false;
  };
  const QColor addition =
      Application::theme()->diff(Theme::Diff::Addition);
  const QColor deletion =
      Application::theme()->diff(Theme::Diff::Deletion);
  QCoreApplication::processEvents();
  inlineOverview->resize(32, 320);
  inlineOverview->show();
  inlineOverview->repaint();
  const QImage inlineOverviewImage = inlineOverview->grab().toImage();
  const int inlineColumn = inlineOverviewImage.width() / 2;
  QVERIFY(containsColor(inlineOverviewImage, 0, inlineColumn, deletion));
  QVERIFY(containsColor(inlineOverviewImage, inlineColumn,
                        inlineOverviewImage.width(), addition));

  auto hasWordHighlight = [](TextEditor *editor) {
    for (int position = 0; position < editor->length(); ++position) {
      if (editor->indicatorValueAt(TextEditor::WordDeletion, position) ||
          editor->indicatorValueAt(TextEditor::WordAddition, position))
        return true;
    }
    return false;
  };
  bool sourceHasWordHighlight = false;
  for (HunkWidget *hunk : inlineFile.hunks())
    sourceHasWordHighlight =
        sourceHasWordHighlight || hasWordHighlight(hunk->editor());
  if (sourceHasWordHighlight)
    QVERIFY(hasWordHighlight(inlineEditor));

  disconnect(inlineView, nullptr, &inlineFile, nullptr);
  QList<CompleteFileDiffWidget::Target> selectedTargets;
  connect(inlineView, &CompleteFileDiffWidget::stageLinesRequested, inlineView,
          [&selectedTargets](const QList<CompleteFileDiffWidget::Target> &targets,
                             bool) { selectedTargets = targets; });
  int changedLine = -1;
  for (int i = 0; i < inlineEditor->lineCount(); ++i) {
    const int markers = inlineEditor->markers(i);
    if (BITSET(markers, TextEditor::Deletion) ||
        BITSET(markers, TextEditor::Addition)) {
      changedLine = i;
      break;
    }
  }
  QVERIFY(changedLine >= 0);
  inlineEditor->stageSelectedSignal(changedLine, changedLine + 1);
  QCOMPARE(selectedTargets.size(), 1);
  QVERIFY(selectedTargets.first().first >= 0);
  QVERIFY(selectedTargets.first().second >= 0);

  Settings::instance()->setDiffMode(Settings::DiffMode::Split);
  FileWidget splitFile(&diffView, diff, patch, stagedPatch, QModelIndex(), name,
                       path_, false, repoView);
  auto *splitView =
      splitFile.findChild<CompleteFileDiffWidget *>("SplitFileDiff");
  QVERIFY(splitView);
  QCOMPARE(splitFile.editors().size(), 2);
  QCOMPARE(splitFile.editors().at(0)->lineCount(),
           splitFile.editors().at(1)->lineCount());

  auto *splitPrevious =
      splitView->findChild<QToolButton *>("PreviousModifiedBlock");
  auto *splitNext = splitView->findChild<QToolButton *>("NextModifiedBlock");
  QVERIFY(splitPrevious);
  QVERIFY(splitNext);
  QCOMPARE(splitPrevious->size(), QSize(20, 20));
  QCOMPARE(splitNext->size(), QSize(20, 20));
  QCOMPARE(splitPrevious->iconSize(), QSize(16, 16));
  QCOMPARE(splitNext->iconSize(), QSize(16, 16));
  auto *splitNavigation =
      splitView->findChild<QWidget *>("ModifiedBlockNavigation");
  QVERIFY(splitNavigation);
  QCOMPARE(splitNavigation->size(), QSize(40, 50));
  const QList<int> splitBlocks = [&splitFile] {
    TextEditor *oldEditor = splitFile.editors().first();
    TextEditor *newEditor = splitFile.editors().last();
    QList<int> result;
    bool previousModified = false;
    for (int line = 0; line < oldEditor->lineCount(); ++line) {
      const int markers = oldEditor->markers(line) | newEditor->markers(line);
      const bool modified = BITSET(markers, TextEditor::Deletion) ||
                            BITSET(markers, TextEditor::Addition);
      if (modified && !previousModified)
        result.append(line);
      previousModified = modified;
    }
    return result;
  }();
  QCOMPARE(splitBlocks.size(), inlineBlocks.size());
  QVERIFY(!splitPrevious->isEnabled());
  QVERIFY(splitNext->isEnabled());
  for (TextEditor *editor : splitFile.editors())
    checkLineNumberHighlight(editor, -1);
  for (int i = 0; i < splitBlocks.size(); ++i) {
    splitNext->click();
    for (TextEditor *editor : splitFile.editors()) {
      QCOMPARE(editor->lineFromPosition(editor->currentPos()),
               splitBlocks.at(i));
      checkLineNumberHighlight(editor, splitBlocks.at(i));
    }
  }
  QVERIFY(splitPrevious->isEnabled());
  QVERIFY(!splitNext->isEnabled());

  QWidget *splitOverview = splitView->findChild<QWidget *>("DiffOverviewBar");
  QVERIFY(splitOverview);
  splitOverview->resize(32, 320);
  splitOverview->show();
  splitOverview->repaint();
  const QImage splitOverviewImage = splitOverview->grab().toImage();
  const int splitColumn = splitOverviewImage.width() / 2;
  QVERIFY(containsColor(splitOverviewImage, 0, splitColumn, deletion));
  QVERIFY(containsColor(splitOverviewImage, splitColumn,
                        splitOverviewImage.width(), addition));
  int unchangedLine = -1;
  for (int line = 0; line < splitFile.editors().first()->lineCount(); ++line) {
    const int oldMarkers = splitFile.editors().first()->markers(line);
    const int newMarkers = splitFile.editors().last()->markers(line);
    if (!BITSET(oldMarkers, TextEditor::Deletion) &&
        !BITSET(oldMarkers, TextEditor::Addition) &&
        !BITSET(newMarkers, TextEditor::Deletion) &&
        !BITSET(newMarkers, TextEditor::Addition)) {
      unchangedLine = line;
      break;
    }
  }
  QVERIFY(unchangedLine >= 0);
  const int unchangedY =
      unchangedLine * splitOverviewImage.height() /
      splitFile.editors().first()->lineCount();
  const QColor background = splitOverview->palette().color(QPalette::Base);
  QCOMPARE(splitOverviewImage.pixelColor(2, unchangedY), background);
  QCOMPARE(splitOverviewImage.pixelColor(splitOverviewImage.width() - 2,
                                         unchangedY),
           background);

  DiffView *realDiffView = &diffView;
  QWidget *realContent = new QWidget(realDiffView);
  realDiffView->setWidget(realContent);
  realDiffView->setFixedSize(640, 160);
  realDiffView->show();
  QVBoxLayout *realLayout = new QVBoxLayout(realContent);
  realLayout->setContentsMargins(0, 0, 0, 0);
  auto verifyRealFile = [&](Settings::DiffMode mode) {
    Settings::instance()->setDiffMode(mode);
    FileWidget realFile(realDiffView, diff, patch, stagedPatch, QModelIndex(),
                        name, path_, false, realContent);
    realLayout->addWidget(&realFile);
    realFile.show();
    realContent->show();
    QCoreApplication::processEvents();

    const char *objectName = mode == Settings::DiffMode::Split
                                 ? "SplitFileDiff"
                                 : "InlineFileDiff";
    auto *realView = realFile.findChild<CompleteFileDiffWidget *>(objectName);
    QVERIFY(realView);
    QScrollBar *scrollBar = realDiffView->verticalScrollBar();
    auto *realPrevious =
        realView->findChild<QToolButton *>("PreviousModifiedBlock");
    auto *realNext = realView->findChild<QToolButton *>("NextModifiedBlock");
    if (mode == Settings::DiffMode::Split) {
      QVERIFY(realPrevious);
      QVERIFY(realNext);
      QTRY_VERIFY(realPrevious->isVisible());
      QTRY_VERIFY(realNext->isVisible());
    } else {
      QVERIFY(!realPrevious);
      QVERIFY(!realNext);
    }

    QWidget *realOverview = realView->findChild<QWidget *>("DiffOverviewBar");
    QVERIFY(realOverview);
    QTRY_VERIFY(realDiffView->verticalScrollBar()->maximum() > 0);
    QTRY_COMPARE(realDiffView->verticalScrollBarPolicy(),
                 Qt::ScrollBarAlwaysOff);
    QTRY_VERIFY(realOverview->isVisible());
    QCOMPARE(realOverview->parentWidget(),
             realView->findChild<QWidget *>("DiffOverviewSlot"));
    QCOMPARE(realOverview->width(), 32);
    const QRect overviewRect(
        realOverview->mapTo(realContent, QPoint()),
        realOverview->size());
    QVERIFY(!overviewRect.intersected(realContent->rect()).isEmpty());

    scrollBar->setValue(0);
    QCoreApplication::processEvents();
    const QImage viewportImage = realContent->grab().toImage();
    const int railLeft = overviewRect.x();
    QVERIFY(containsColor(viewportImage, railLeft,
                          railLeft + realOverview->width() / 2, deletion));
    QVERIFY(containsColor(viewportImage,
                          railLeft + realOverview->width() / 2,
                          viewportImage.width(), addition));

    if (mode == Settings::DiffMode::Split) {
      TextEditor *realEditor = realView->editors().last();
      const int viewportHeight = realDiffView->viewport()->height();
      const int topAnchor = qRound(viewportHeight * 0.20);
      const int bottomAnchor = qRound(viewportHeight * 0.80);
      auto documentLineY = [&](int line) {
        return realEditor->mapTo(realDiffView->widget(), QPoint()).y() +
               realEditor
                   ->pointFromPosition(realEditor->positionFromLine(line))
                   .y();
      };

      const int firstDocumentY = documentLineY(splitBlocks.first());
      realNext->click();
      QCoreApplication::processEvents();
      QCOMPARE(scrollBar->value(),
               qBound(scrollBar->minimum(), firstDocumentY - topAnchor,
                      scrollBar->maximum()));
      QCOMPARE(documentLineY(splitBlocks.first()) - scrollBar->value(),
               topAnchor);

      for (int i = 1; i < splitBlocks.size(); ++i) {
        const int before = scrollBar->value();
        const int targetDocumentY = documentLineY(splitBlocks.at(i));
        const int targetViewportY = targetDocumentY - before;
        realNext->click();
        QCoreApplication::processEvents();
        const int expected = targetViewportY > bottomAnchor
                                 ? qBound(scrollBar->minimum(),
                                          targetDocumentY - bottomAnchor,
                                          scrollBar->maximum())
                                 : before;
        QCOMPARE(scrollBar->value(), expected);
        QCOMPARE(documentLineY(splitBlocks.at(i)) - scrollBar->value(),
                 targetViewportY > bottomAnchor ? bottomAnchor
                                                 : targetViewportY);
      }

      for (int i = splitBlocks.size() - 2; i >= 0; --i) {
        const int before = scrollBar->value();
        const int targetDocumentY = documentLineY(splitBlocks.at(i));
        const int targetViewportY = targetDocumentY - before;
        realPrevious->click();
        QCoreApplication::processEvents();
        const int expected = targetViewportY < topAnchor
                                 ? qBound(scrollBar->minimum(),
                                          targetDocumentY - topAnchor,
                                          scrollBar->maximum())
                                 : before;
        QCOMPARE(scrollBar->value(), expected);
        QCOMPARE(documentLineY(splitBlocks.at(i)) - scrollBar->value(),
                 targetViewportY < topAnchor ? topAnchor : targetViewportY);
      }
    }

    scrollBar->setValue(scrollBar->maximum());
    QTRY_COMPARE(scrollBar->value(), scrollBar->maximum());
    QTRY_VERIFY(realOverview->isVisible());
  };
  verifyRealFile(Settings::DiffMode::Split);
  verifyRealFile(Settings::DiffMode::Inline);

  Settings::instance()->setDiffMode(Settings::DiffMode::Hunk);
  inlineFile.rebuildPresentation();
  splitFile.rebuildPresentation();
  QTRY_COMPARE(diffView.verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);

  Settings::instance()->setTextEditorWrapLines(true);
  QCOMPARE(splitFile.editors().first()->wrapMode(), SC_WRAP_WORD);
  Settings::instance()->setTextEditorWrapLines(false);
  QCOMPARE(splitFile.editors().first()->wrapMode(), SC_WRAP_NONE);

  Settings::instance()->setDiffMode(Settings::DiffMode::Inline);
}

void TestEditorLineInfo::sameContentAddLine() {
  INIT_REPO("17_LinestagingLineContentStageAddedLine.zip", true)
  QVERIFY(stagedDiff.count() > 0);
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  git::Patch stagedPatch = stagedDiff.patch(0);

  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
                       repoView);
  hw.load(stagedPatch, true);
  checkEditorMarkers(hw.editor(), QVector<int>({3, 4}), QVector<int>({9}),
                     QVector<int>({}), QVector<int>({}));
}

#endif

// void TestEditorLineInfo::deleteCompleteContent() {
//   INIT_REPO("18_deleteLinesStagedLast.zip", true)
//   QVERIFY(stagedDiff.count() > 0);
//   QVERIFY(diff.count() > 0);
//   git::Patch patch = diff.patch(0);
//   git::Patch stagedPatch = stagedDiff.patch(0);

//  auto hw = HunkWidget(&diffView, diff, patch, stagedPatch, 0, false, false,
//                       repoView);
//  hw.load(stagedPatch, true);
//  checkEditorMarkers(
//      hw.editor(), QVector<int>({24}), QVector<int>({}),
//      QVector<int>({0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,
//                    12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22}),
//      QVector<int>({23}));
//}

void TestEditorLineInfo::discardCompleteDeletedContent() {
  INIT_REPO("19_discardCompleteDeletedContent.zip", true)
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  // no staged lines yet, so no staged patch
  QCOMPARE(mRepo.diffTreeToIndex(commit.tree()).count(), 0);
  git::Patch stagedPatch = git::Patch();

  QString name = patch.name();
  QString path_ = mRepo.workdir().filePath(name);
  bool submodule = mRepo.lookupSubmodule(name).isValid();
  {
    FileWidget fw(&diffView, diff, patch, stagedPatch, QModelIndex(), name,
                  path_, submodule, repoView);
    fw.setStageState(git::Index::StagedState::Unstaged);

    auto hunks = fw.hunks();
    QVERIFY(hunks.count() == 1);
    for (auto *hunk : hunks)
      hunk->load();

    QCOMPARE(hunks.at(0)->hunk(), QByteArray());

    hunks.at(0)->discardSelected(0, 1);
  }

  // Check that discard was successfull and the application does not crash
  QFile f(path + "/File.txt");
  QVERIFY(f.open(QIODevice::ReadOnly));
  QCOMPARE(f.readAll(), "Content\n");
}

void TestEditorLineInfo::discardCompleteAddedContent() {
  INIT_REPO("20_discardCompleteAddedContent.zip", true)
  QVERIFY(diff.count() > 0);
  git::Patch patch = diff.patch(0);
  // no staged lines yet, so no staged patch
  QCOMPARE(mRepo.diffTreeToIndex(commit.tree()).count(), 0);
  git::Patch stagedPatch = git::Patch();

  QString name = patch.name();
  QString path_ = mRepo.workdir().filePath(name);
  bool submodule = mRepo.lookupSubmodule(name).isValid();
  {
    FileWidget fw(&diffView, diff, patch, stagedPatch, QModelIndex(), name,
                  path_, submodule, repoView);
    fw.setStageState(git::Index::StagedState::Unstaged);

    auto hunks = fw.hunks();
    QVERIFY(hunks.count() == 1);
    for (auto *hunk : hunks)
      hunk->load();

    hunks.at(0)->discardSelected(0, 1);
  }

  // Check that discard was successfull and the application does not crash
  QFile f(path + "/Test.txt");
  QVERIFY(f.open(QIODevice::ReadOnly));
  QCOMPARE(f.readAll(), "");
}

void TestEditorLineInfo::cleanupTestCase() { qWait(closeDelay); }

TEST_MAIN(TestEditorLineInfo)
#include "EditorLineInfos.moc"

#ifndef CONFLICTRESOLVERWIDGET_H
#define CONFLICTRESOLVERWIDGET_H

#include "git/Patch.h"
#include <QWidget>

class QCheckBox;
class QPlainTextEdit;

class ConflictSourcePanel;
class SourceLineRow;

class ConflictResolverWidget : public QWidget {
  Q_OBJECT

public:
  explicit ConflictResolverWidget(const git::Patch &patch,
                                  QWidget *parent = nullptr);

  bool isComplete() const;
  QByteArray result() const;

signals:
  void completenessChanged(bool complete);

private:
  enum Side { Current, Incoming };

  struct Selection {
    Side side;
    int line;
  };

  struct BlockState {
    git::Patch::ConflictBlock block;
    QList<Selection> selections;
    QList<SourceLineRow *> currentRows;
    QList<SourceLineRow *> incomingRows;
    QCheckBox *currentCheck = nullptr;
    QCheckBox *incomingCheck = nullptr;
    int resultStart = 0;
    int resultEnd = 0;
  };

  void toggleBlock(int block, Side side);
  void toggleLine(int block, Side side, int line);
  void replaceResultBlock(int block);
  void updateBlockUi(int block);
  bool contains(const BlockState &state, Side side, int line) const;
  bool allSelected(const BlockState &state, Side side) const;
  QString lineText(const QByteArray &line) const;
  QString incomingCommit() const;

  git::Patch mPatch;
  QList<BlockState> mBlocks;
  ConflictSourcePanel *mCurrentPanel = nullptr;
  ConflictSourcePanel *mIncomingPanel = nullptr;
  QPlainTextEdit *mResult = nullptr;
  bool mUpdatingResult = false;
  bool mResultRangesValid = true;
  bool mCrLf = false;
};

#endif // CONFLICTRESOLVERWIDGET_H

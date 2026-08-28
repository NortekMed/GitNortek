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
  int untouchedBlockCount() const;
  QByteArray result() const;
  QByteArray resultWithConflictChunks() const;
  bool canCreateConflictChunks() const { return mResultRangesValid; }
  bool hasUnsavedOutput() const;
  void acceptAll();

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
  void toggleAll(Side side);
  void setAllSelected(Side side, bool selected);
  void replaceResultBlock(int block);
  void updateBlockUi(int block);
  void updateMasterUi();
  bool contains(const BlockState &state, Side side, int line) const;
  bool allSelected(const BlockState &state, Side side) const;
  bool anySelected(const BlockState &state, Side side) const;
  QByteArray encodeResult(QString text) const;
  QString conflictChunk(const BlockState &state) const;
  QString resultLine(const QByteArray &line) const;
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
  QByteArray mInitialOutput;
};

#endif // CONFLICTRESOLVERWIDGET_H

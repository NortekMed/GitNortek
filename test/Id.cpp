//
//          Copyright (c) 2026, GitNortek Contributors
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "Test.h"
#include "git/Id.h"

class TestId : public QObject {
  Q_OBJECT

private slots:
  void shortId();
};

void TestId::shortId() {
  const git::Id id(
      QByteArray::fromHex("b52a705335ca4ad5035eb16f9501a846f96f2f35"),
      GIT_OID_SHA1);

  QCOMPARE(id.shortId(), QString("b52a7053"));
  QVERIFY(git::Id().shortId().isEmpty());
}

TEST_MAIN(TestId)

#include "Id.moc"

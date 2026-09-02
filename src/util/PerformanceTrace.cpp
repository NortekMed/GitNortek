//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#include "PerformanceTrace.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QMutex>
#include <QThread>

namespace {

QFile traceFile;
QElapsedTimer traceTimer;
QMutex traceMutex;
quint64 traceSequence = 0;

void writeEvent(const QString &category, const QString &phase,
                qint64 durationUs, const QString &repository,
                const QJsonObject &fields) {
  if (!PerformanceTrace::isEnabled())
    return;

  QMutexLocker locker(&traceMutex);
  QJsonObject object;
  object["sequence"] = QString::number(++traceSequence);
  object["elapsed_us"] = QString::number(traceTimer.nsecsElapsed() / 1000);
  if (durationUs >= 0)
    object["duration_us"] = QString::number(durationUs);
  object["thread"] = QString::number(quintptr(QThread::currentThreadId()), 16);
  object["category"] = category;
  object["phase"] = phase;
  if (!repository.isEmpty())
    object["repository"] = repository;
  if (!fields.isEmpty())
    object["fields"] = fields;

  traceFile.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
  traceFile.write("\n");
  traceFile.flush();
}

} // namespace

std::atomic_bool PerformanceTrace::sEnabled = false;

PerformanceTrace::Span::Span(const QString &category, const QString &phase,
                             const QString &repository,
                             const QJsonObject &fields)
    : mCategory(category), mPhase(phase), mRepository(repository),
      mFields(fields), mEnabled(PerformanceTrace::isEnabled()) {
  if (mEnabled)
    mTimer.start();
}

PerformanceTrace::Span::~Span() {
  if (mEnabled)
    PerformanceTrace::span(mCategory, mPhase, mTimer.nsecsElapsed() / 1000,
                           mRepository, mFields);
}

void PerformanceTrace::init(const QString &path) {
  if (path.isEmpty())
    return;

  QMutexLocker locker(&traceMutex);
  traceFile.setFileName(path);
  if (!traceFile.open(QIODevice::WriteOnly | QIODevice::Truncate |
                      QIODevice::Text))
    return;

  traceTimer.start();
  sEnabled.store(true);

  QJsonObject fields;
  fields["application"] = QCoreApplication::applicationName();
  fields["version"] = QCoreApplication::applicationVersion();
  fields["qt"] = QString(qVersion());
  fields["timestamp"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
  fields["pid"] = qint64(QCoreApplication::applicationPid());
  locker.unlock();
  event("trace", "started", QString(), fields);
}

bool PerformanceTrace::isEnabled() { return sEnabled.load(); }

void PerformanceTrace::event(const QString &category, const QString &phase,
                             const QString &repository,
                             const QJsonObject &fields) {
  writeEvent(category, phase, -1, repository, fields);
}

void PerformanceTrace::span(const QString &category, const QString &phase,
                            qint64 durationUs, const QString &repository,
                            const QJsonObject &fields) {
  writeEvent(category, phase, durationUs, repository, fields);
}

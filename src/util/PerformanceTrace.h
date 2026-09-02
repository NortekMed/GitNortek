//
//          Copyright (c) 2026, NortekMed
//
// This software is licensed under the MIT License. The LICENSE.md file
// describes the conditions under which this software may be distributed.
//

#ifndef PERFORMANCETRACE_H
#define PERFORMANCETRACE_H

#include <QElapsedTimer>
#include <QJsonObject>
#include <QString>
#include <atomic>

class PerformanceTrace {
public:
  class Span {
  public:
    Span(const QString &category, const QString &phase,
         const QString &repository = QString(),
         const QJsonObject &fields = QJsonObject());
    ~Span();

    Span(const Span &) = delete;
    Span &operator=(const Span &) = delete;

  private:
    QString mCategory;
    QString mPhase;
    QString mRepository;
    QJsonObject mFields;
    QElapsedTimer mTimer;
    bool mEnabled = false;
  };

  static void init(const QString &path);
  static bool isEnabled();
  static void event(const QString &category, const QString &phase,
                    const QString &repository = QString(),
                    const QJsonObject &fields = QJsonObject());
  static void span(const QString &category, const QString &phase,
                   qint64 durationUs, const QString &repository = QString(),
                   const QJsonObject &fields = QJsonObject());

private:
  static std::atomic_bool sEnabled;
};

#endif // PERFORMANCETRACE_H

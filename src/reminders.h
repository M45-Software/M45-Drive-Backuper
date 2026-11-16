#pragma once
#include <QDateTime>
#include <QSettings>

namespace Reminders {
struct Result { QDateTime due; bool notify = false; };
// State lives in the desktop user's settings, separate from privileged jobs.
inline Result evaluate(QSettings &settings, const QString &key, int days,
                       QDateTime success, bool paused, QDateTime now) {
    if (days <= 0) return {};
    auto enabled = settings.value(key + "/enabledAt").toDateTime();
    if (!enabled.isValid()) {
        enabled = now;
        settings.setValue(key + "/enabledAt", enabled);
    }
    if (success.isValid() && success <= now) {
        const auto previous = settings.value(key + "/success").toDateTime();
        if (!previous.isValid() || success > previous) settings.setValue(key + "/success", success);
    }
    const auto savedSuccess = settings.value(key + "/success").toDateTime();
    auto anchor = savedSuccess.isValid() ? savedSuccess : enabled;
    // A clock correction must not postpone reminders indefinitely.
    if (anchor > now) {
        anchor = now;
        settings.setValue(key + (savedSuccess.isValid() ? "/success" : "/enabledAt"), now);
    }
    Result result{anchor.addDays(days), false};
    const auto snoozed = settings.value(key + "/snoozedUntil").toDateTime();
    if (snoozed.isValid() && snoozed > result.due) result.due = snoozed;
    const auto notified = settings.value(key + "/notifiedAt").toDateTime();
    if (!paused && result.due <= now && (!notified.isValid() || notified > now || notified.addDays(1) <= now)) {
        result.notify = true;
        settings.setValue(key + "/notifiedAt", now);
    }
    return result;
}
}

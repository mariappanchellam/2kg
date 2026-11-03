#pragma once

#include <QtCore/QString>

/// A log file for analysing field and flight problems: one file per app run, written line by line straight to
/// the file so that every line before a crash is kept. It holds the app's warnings, the camera video events and
/// statistics, and, on Android and Linux, a crash report (signal, crashing library and call stack). On start-up
/// it notes whether the previous run ended cleanly, crashed, or was closed without warning (swiped away, killed
/// by Android, battery pulled).
namespace DhakshaFlightLog {

/// Opens a new file in `directory` (falls back to Documents, then the app's own data folder, if it cannot be
/// written); the newest 20 files are kept.
void start(const QString& directory, const QString& appVersion);

/// Appends one time-stamped line; safe from any thread, a no-op before start().
void write(const QString& line);

/// Marks the run as ended cleanly.
void finish();

/// The file being written, empty before start().
QString filePath();

}  // namespace DhakshaFlightLog

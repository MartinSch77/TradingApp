// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_TESTS_CROWDFIXTURES_H
#define TRADINGAPP_TESTS_CROWDFIXTURES_H

#include "domain/CrowdObservation.h"

#include <QByteArray>
#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

class QTemporaryDir;

// Shared fixtures for the crowd ML tests (tst_crowdml, tst_crowdmodel): both drive the SAME
// offline pipeline (tools/ml/) over stores the C++ CrowdStore writes, so the invocation and
// fixture construction live once here rather than as clones. Compiled into each test target;
// the including target must define TRADINGAPP_SOURCE_DIR.
namespace crowdtest {

struct ToolRun {
    bool started = false;
    int exitCode = -1;
    QString stdOut;
    QString stdErr;
};

[[nodiscard]] QString repoRoot();
[[nodiscard]] QString datasetTool();
[[nodiscard]] QString trainerTool();

// Run an interpreter over a tool; started=false means "no such interpreter" (callers QSKIP).
[[nodiscard]] ToolRun runPython(const QString &python, const QStringList &arguments,
                                int timeoutMs);

// The shared spellings of the two tool invocations — tests vary only paths and the knob under
// test. buildToolArgs pins --horizon-days 5, the horizon every fixture is constructed around.
[[nodiscard]] QStringList buildToolArgs(const QString &store, const QString &prices,
                                        const QString &dataset, const QString &manifest,
                                        const QStringList &extra = {});
[[nodiscard]] QStringList trainerArgs(const QString &dataset, const QString &manifest,
                                      const QString &outDir, const QStringList &extra = {});

// One valid observation for the store fixtures; provider name and unit are constants because
// no assertion reads them.
[[nodiscard]] trading::crowd::Observation makeObservation(trading::crowd::Source source,
                                                          const QString &series,
                                                          const QDateTime &event,
                                                          const QDateTime &received,
                                                          double value);

// date,close CSV of daily closes from `start`. False when the file cannot be written.
[[nodiscard]] bool writePricesCsv(const QString &path, const QDate &start,
                                  const QList<double> &closes);

// The LEARNABLE fixture both end-to-end tests train on: `days` daily VIX observations whose
// regime alternates every 10 days (low 10.x / high 30.x), and a price series that rises 1%
// per day in the low regime and falls 1% in the high one — so direction is predictable from
// the volatility level. False when the store cannot be written.
[[nodiscard]] bool writeRegimeFixture(const QString &storePath, const QString &pricesPath,
                                      const QDate &start, int days);

// The store/prices pair a fixture writer (e.g. writeRegimeFixture) produces together, bundled
// so runDatasetBuildAndTrain stays under the 5-parameter metrics threshold.
struct FixtureFiles {
    QString storePath;
    QString pricesPath;
};

// Runs the dataset-build step over an already-written fixture, then the trainer over its
// output — the two-stage invocation tst_crowdml and tst_crowdmodel's end-to-end tests both
// drive. Returns whichever run actually failed (dataset build if it could not start or exited
// non-zero, the trainer otherwise), so a caller's single
// `QVERIFY2(run.exitCode == 0, qPrintable(run.stdErr))` reports the real failure either way.
[[nodiscard]] ToolRun runDatasetBuildAndTrain(const FixtureFiles &fixture, const QString &dataset,
                                              const QString &manifest, const QString &outDir,
                                              const QString &python);

// Writes the LEARNABLE regime fixture into `dir` and runs the full dataset-build + train
// pipeline over it — the common core of tst_crowdml's and tst_crowdmodel's end-to-end tests,
// which differ only in what they do with a successful run afterward (`dir` outlives this
// call, so the temp directory is the caller's, not owned here). `outDir`/`dataset`/`manifest`
// are set to the pipeline's own paths even on failure, so a caller that inspects them after a
// QSKIP check still has valid paths. Returns a not-started run only if the fixture itself
// could not be written (a hard test bug, not a pipeline failure).
[[nodiscard]] ToolRun runRegimeTrainingPipeline(const QTemporaryDir &dir, const QString &python,
                                                QString &outDir, QString &dataset,
                                                QString &manifest);

// The dataset CSV has no quoting (numbers, ISO stamps, label words): rows keyed by column NAME.
[[nodiscard]] QList<QHash<QString, QString>> readCsv(const QString &path);
[[nodiscard]] const QHash<QString, QString> *rowForDay(const QList<QHash<QString, QString>> &rows,
                                                       const QString &day);
[[nodiscard]] QByteArray fileBytes(const QString &path);

// A python able to run the FITTING half: the setup-provisioned venv first, the system
// interpreter second. Empty when neither imports the ML stack — callers SKIP, the same
// discipline as the licence-bound stages.
[[nodiscard]] QString mlPython();

} // namespace crowdtest

#endif // TRADINGAPP_TESTS_CROWDFIXTURES_H

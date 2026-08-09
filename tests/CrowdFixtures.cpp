// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CrowdFixtures.h"

#include "services/CrowdStore.h"

#include <QDir>
#include <QFile>
#include <QProcess>

namespace crowdtest {

using trading::crowd::CrowdStore;
using trading::crowd::Observation;
using trading::crowd::Source;

QString repoRoot()
{
    return QStringLiteral(TRADINGAPP_SOURCE_DIR);
}

QString datasetTool()
{
    return repoRoot() + QStringLiteral("/tools/ml/crowd_dataset.py");
}

QString trainerTool()
{
    return repoRoot() + QStringLiteral("/tools/ml/train_crowd_model.py");
}

ToolRun runPython(const QString &python, const QStringList &arguments, int timeoutMs)
{
    QProcess process;
    process.start(python, arguments);
    ToolRun run;
    run.started = process.waitForStarted(5000);
    if (!run.started) {
        return run;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        return run;
    }
    run.exitCode = process.exitCode();
    run.stdOut = QString::fromUtf8(process.readAllStandardOutput());
    run.stdErr = QString::fromUtf8(process.readAllStandardError());
    return run;
}

QStringList buildToolArgs(const QString &store, const QString &prices, const QString &dataset,
                          const QString &manifest, const QStringList &extra)
{
    return QStringList{datasetTool(), QStringLiteral("build"),
                       QStringLiteral("--store"), store,
                       QStringLiteral("--instrument"), QStringLiteral("SPX500"),
                       QStringLiteral("--prices"), prices,
                       QStringLiteral("--horizon-days"), QStringLiteral("5"),
                       QStringLiteral("--out"), dataset,
                       QStringLiteral("--manifest"), manifest}
           + extra;
}

QStringList trainerArgs(const QString &dataset, const QString &manifest, const QString &outDir,
                        const QStringList &extra)
{
    return QStringList{trainerTool(), QStringLiteral("--dataset"), dataset,
                       QStringLiteral("--manifest"), manifest,
                       QStringLiteral("--out-dir"), outDir}
           + extra;
}

Observation makeObservation(Source source, const QString &series, const QDateTime &event,
                            const QDateTime &received, double value)
{
    Observation obs;
    obs.instrument = QStringLiteral("SPX500");
    obs.source = source;
    obs.sourceName = QStringLiteral("fixture");
    obs.seriesId = series;
    obs.eventTime = event;
    obs.receivedTime = received;
    obs.value = value;
    obs.unit = QStringLiteral("unit");
    obs.valid = true;
    return obs;
}

bool writePricesCsv(const QString &path, const QDate &start, const QList<double> &closes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write("date,close\n");
    for (qsizetype i = 0; i < closes.size(); ++i) {
        file.write(QStringLiteral("%1,%2\n")
                       .arg(start.addDays(i).toString(QStringLiteral("yyyy-MM-dd")))
                       .arg(closes.at(i), 0, 'g', 10)
                       .toUtf8());
    }
    return true;
}

bool writeRegimeFixture(const QString &storePath, const QString &pricesPath, const QDate &start,
                        int days)
{
    QList<double> closes{100.0};
    {
        CrowdStore store(storePath);
        if (!store.isOpen()) {
            return false;
        }
        QList<Observation> observations;
        for (int i = 0; i < days; ++i) {
            const bool lowVol = (i / 10) % 2 == 0;
            const QDateTime stamp(start.addDays(i), QTime(20, 5), QTimeZone::UTC);
            observations.append(makeObservation(Source::Volatility, QStringLiteral("VIX"),
                                                stamp, stamp,
                                                (lowVol ? 10.0 : 30.0) + 0.1 * (i % 4)));
            if (i < days - 1) {
                closes.append(closes.last() * (lowVol ? 1.01 : 0.99));
            }
        }
        if (store.upsert(observations) != days) {
            return false;
        }
    } // the store's connection closes before any tool reads the file
    return writePricesCsv(pricesPath, start, closes);
}

QList<QHash<QString, QString>> readCsv(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'),
                                                                      Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        return {};
    }
    const QStringList header = lines.first().split(QLatin1Char(','));
    QList<QHash<QString, QString>> rows;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QStringList cells = lines.at(i).split(QLatin1Char(','));
        QHash<QString, QString> row;
        for (qsizetype c = 0; c < header.size() && c < cells.size(); ++c) {
            row.insert(header.at(c), cells.at(c));
        }
        rows.append(row);
    }
    return rows;
}

const QHash<QString, QString> *rowForDay(const QList<QHash<QString, QString>> &rows,
                                         const QString &day)
{
    for (const auto &row : rows) {
        if (row.value(QStringLiteral("decision_time")).startsWith(day)) {
            return &row;
        }
    }
    return nullptr;
}

QByteArray fileBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

QString mlPython()
{
    const QString venv = qEnvironmentVariable(
        "ML_VENV_DIR", QDir::homePath() + QStringLiteral("/.local/tradingapp-ml"));
    const QStringList candidates{venv + QStringLiteral("/bin/python3"),
                                 venv + QStringLiteral("/Scripts/python.exe"),
                                 QStringLiteral("python3")};
    const QStringList probe{QStringLiteral("-c"),
                            QStringLiteral("import numpy, sklearn, xgboost, skl2onnx, "
                                           "onnxmltools, onnxruntime")};
    for (const QString &candidate : candidates) {
        const ToolRun run = runPython(candidate, probe, 60000);
        if (run.started && run.exitCode == 0) {
            return candidate;
        }
    }
    return {};
}

} // namespace crowdtest

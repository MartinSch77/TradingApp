// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_UI_SCREENERDIALOG_H
#define TRADINGAPP_UI_SCREENERDIALOG_H

#include "domain/Models.h"

#include <QDialog>
#include <QList>

QT_FORWARD_DECLARE_CLASS(QLabel)
QT_FORWARD_DECLARE_CLASS(QPushButton)
QT_FORWARD_DECLARE_CLASS(QTableWidget)

// The leverage screener window: every tradable instrument ranked BUY-first then
// by confidence, each with the same ensemble call as the live signals panel.
// Owns only its widgets and ranking — the scan data arrives through updateRows()
// and user intent leaves through the two signals, so the dialog stays decoupled
// from the client and the main window.
class ScreenerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ScreenerDialog(QWidget *parent = nullptr);

    // Rebuild the ranked table from the latest scan rows. The VIX context feeds
    // the ensemble's directional vote and the confidence haircut, exactly as in
    // the live panel, so a symbol ranks identically in both places.
    void updateRows(const QList<ScreenerRow> &rows, bool vixValid, double vixLevel,
                    double vixChangePct);

    void scanStarted();                       // clear the table, show "Scanning..."
    void scanProgress(qint32 done, qint32 total);
    void scanFinished(qsizetype instrumentCount);

signals:
    void instrumentChosen(const QString &symbol);  // double-clicked row
    void rescanRequested();

private:
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_rescan = nullptr;
};

#endif // TRADINGAPP_UI_SCREENERDIALOG_H

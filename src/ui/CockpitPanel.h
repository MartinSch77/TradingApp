// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_UI_COCKPITPANEL_H
#define TRADINGAPP_UI_COCKPITPANEL_H

#include "ui/CockpitModel.h"

#include <QDialog>

class QQuickWidget;

// The window that hosts the declarative cockpit (REQ-F-038, DES-UI-COCKPIT).
//
// A QQuickWidget inside an ordinary QDialog, so the QML surface is ONE page of the existing
// Widgets application rather than a second executable. That is the point of the exercise: the
// same trading_domain / trading_services libraries, two presentations, and the layering claim
// demonstrated instead of asserted.
//
// This class is host plumbing only — it owns the model, sets it as a context property and
// loads the QML. Every rule about what may be shown lives in CockpitModel, where a test can
// reach it without a window.
namespace trading::ui {

class CockpitPanel : public QDialog
{
    Q_OBJECT;

public:
    explicit CockpitPanel(QWidget *parent = nullptr);

    // The one way in. Callers push; nothing here polls a feed, because a read-only view must
    // not be able to start network traffic of its own.
    [[nodiscard]] CockpitModel *model() const { return m_model; }

    // True when the QML actually loaded. A QQuickWidget with a failed component shows an
    // empty rectangle, which would look like "no data" rather than "broken" — so the caller
    // is told and can say so out loud.
    // Asked of the view, never cached. A bool captured at construction is a second copy of
    // state the QQuickWidget already owns, and it goes stale the moment the source is
    // reloaded — so the panel could report "ready" about a component that had since failed.
    // (clang-tidy's cppcoreguidelines-prefer-member-initializer pointed at the cached flag;
    // the fix is to remove the duplicate rather than to move its assignment.)
    [[nodiscard]] bool ready() const;

private:
    CockpitModel *m_model = nullptr;
    QQuickWidget *m_view = nullptr;
};

} // namespace trading::ui

#endif // TRADINGAPP_UI_COCKPITPANEL_H

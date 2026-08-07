// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/CockpitPanel.h"

#include <QQuickWidget>
#include <QVBoxLayout>

namespace trading::ui {

CockpitPanel::CockpitPanel(QWidget *parent)
    : QDialog(parent), m_model(new CockpitModel(this)), m_view(new QQuickWidget(this))
{
    setObjectName(QStringLiteral("cockpitPanel"));   // REQ-N-007: addressable by name
    setWindowTitle(tr("Market cockpit (Qt Quick)"));
    resize(1180, 700);

    m_view->setObjectName(QStringLiteral("cockpitView"));
    m_view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    // setInitialProperties, NOT a rootContext property. Measured: as a context property the
    // model resolved for bindings on Main.qml's own objects but read as NULL inside the
    // bindings created for child components ("Cannot read property 'ticks' of null"). An
    // initial property is applied as part of construction, so Main.qml can declare it
    // `required` and the timing stops being a question — and a missing injection then fails
    // loudly instead of rendering a half-empty view.
    m_view->setInitialProperties({{QStringLiteral("cockpit"), QVariant::fromValue(m_model)}});
    m_view->setSource(QUrl(QStringLiteral("qrc:/qt/qml/TradingApp/Cockpit/Main.qml")));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
}

// Report a load failure rather than presenting an empty rectangle that reads as "no data".
// Out-of-line so the comdat coverage records stay unambiguous, per this project's rule for
// header-inline functions that carry logic.
bool CockpitPanel::ready() const
{
    return (m_view != nullptr) && (m_view->status() == QQuickWidget::Ready);
}

} // namespace trading::ui

// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/CockpitPanel.h"

#include <QQuickWidget>
#include <QVBoxLayout>

namespace trading::ui {

CockpitPanel::CockpitPanel(QWidget *parent) : QDialog(parent)
{
    setObjectName(QStringLiteral("cockpitPanel"));   // REQ-N-007: addressable by name
    setWindowTitle(tr("Market cockpit (Qt Quick)"));
    resize(1180, 700);

    m_model = new CockpitModel(this);

    m_view = new QQuickWidget(this);
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

    // Report a load failure rather than presenting an empty rectangle that reads as "no
    // data". The status is checked once here, after setSource, because QQuickWidget loads
    // synchronously for a qrc URL.
    m_ready = (m_view->status() == QQuickWidget::Ready);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_view);
}

} // namespace trading::ui

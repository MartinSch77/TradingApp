// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/AiAdvisor.h"
#include "services/Config.h"
#include "services/EconomicCalendar.h"
#include "services/EtoroClient.h"
#include "services/MarketFeeds.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QByteArray>
#include <QTimer>

namespace {

// The two nudges WSL2 needs BEFORE the GUI stack starts, both defensive (an explicit user
// setting always wins). Extracted from main() so main() stays within the metrics complexity
// budget.
//
//  * PLATFORM: WSLg's strict Wayland compositor trips Qt's client-side decorations — a maximized
//    top-level gets a buffer a few pixels taller than the configured maximized state, which the
//    compositor rejects with a fatal "xdg_surface buffer does not match the configured maximized
//    state". XWayland (xcb) does not hit this, so prefer it and drop decorations on the wayland
//    fallback.
//  * GRAPHICS: no usable GPU passthrough, so Mesa's default Zink (OpenGL-on-Vulkan) path probes a
//    Vulkan device that does not exist, prints the libEGL / "ZINK: failed to choose pdev"
//    warnings, and falls back to software anyway. Force the software rasteriser up front so that
//    fallback is clean and silent; a real-GPU machine is untouched.
void configureWslEnvironment()
{
    const bool onWsl = qEnvironmentVariableIsSet("WSL_DISTRO_NAME")
                       || qEnvironmentVariableIsSet("WSL_INTEROP");
    if (!onWsl) {
        return;
    }
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        static_cast<void>(qputenv("QT_QPA_PLATFORM", "xcb;wayland"));
        if (!qEnvironmentVariableIsSet("QT_WAYLAND_DISABLE_WINDOWDECORATION")) {
            static_cast<void>(qputenv("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1"));
        }
    }
    if (!qEnvironmentVariableIsSet("LIBGL_ALWAYS_SOFTWARE")) {
        static_cast<void>(qputenv("LIBGL_ALWAYS_SOFTWARE", "1"));
        static_cast<void>(qputenv("GALLIUM_DRIVER", "llvmpipe"));
    }
}

} // namespace

int main(int argc, char *argv[])
{
    configureWslEnvironment();

    // const: the instance is never mutated through this name — the app is
    // driven through QApplication's static API (setApplicationName, exec).
    const QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("eToro Trader"));
    QApplication::setOrganizationName(QStringLiteral("TradingApp"));

    const Config config = Config::load();

    // Composition root: each service has one job — the broker client (real or
    // simulated), the public web feeds and the AI advisor — and the window only
    // consumes their signals. Declared before the window so they outlive it
    // during teardown.
    EtoroClient client(config);
    MarketFeeds feeds;
    feeds.setCurrentSymbol(config.symbol);
    AiAdvisor aiAdvisor(config.anthropicApiKey);
    EconomicCalendar calendar;
    MainWindow window(&client, &feeds, &aiAdvisor, &calendar);
    window.show();

    // Kick off instrument resolution / history / polling after the UI exists
    // so the first signals land on connected slots.
    client.start();
    // VIX + web rating move slowly; refresh them on the same ~20-poll cadence
    // the client used before the feeds were split out (100s at the default poll).
    feeds.start(config.pollIntervalMs * 20);
    calendar.setInstrument(config.symbol);
    calendar.start();

    // QA aid: TRADINGAPP_SHOT=<path> grabs every visible window to PNGs and quits.
    // TRADINGAPP_SHOT_OPEN=1 also opens the decision, closed-trades and bot-simulation
    // windows first, and TRADINGAPP_SHOT_DELAY_MS overrides the capture delay (default
    // 3000) so slow scans can finish before the grab. TRADINGAPP_BOT_ARM=1 (see
    // MainWindow::setupRunners) arms the paper-trading bot, which is what makes the
    // bot window worth capturing at all, and TRADINGAPP_BOT_AI=off|confirm|lead
    // selects whether the local model's proposal only gets logged, vetoes, or leads.
    const QString path = qEnvironmentVariable("TRADINGAPP_SHOT");
    if (!path.isEmpty()) {
        if (qEnvironmentVariableIsSet("TRADINGAPP_SHOT_OPEN")) {
            QTimer::singleShot(1500, &window, [&window]() {
                static_cast<void>(QMetaObject::invokeMethod(&window, "openDecision"));
                static_cast<void>(QMetaObject::invokeMethod(&window, "openClosedTrades"));
                static_cast<void>(QMetaObject::invokeMethod(&window, "openBotSim"));
                // The heavyweight window too: it is the one view whose emptiness was a
                // real defect nobody could see from a screenshot set that omitted it
                // (the constituent reads went dark when the feed stopped serving equity
                // bars). A QA capture that cannot show the regression is not QA.
                static_cast<void>(QMetaObject::invokeMethod(&window, "openHeavyPanel"));
                // The Qt Quick cockpit too (REQ-F-038): a QQuickWidget whose component
                // failed renders an empty rectangle, so a capture set that omitted it could
                // not show the difference between "loaded and empty" and "did not load".
                static_cast<void>(QMetaObject::invokeMethod(&window, "openCockpit"));
            });
        }
        const qint32 delayMs = qEnvironmentVariableIsSet("TRADINGAPP_SHOT_DELAY_MS")
                                   ? qEnvironmentVariableIntValue("TRADINGAPP_SHOT_DELAY_MS")
                                   : 3000;
        QTimer::singleShot(delayMs, qApp, [path]() {
            qint32 idx = 0;
            const QWidgetList widgets = QApplication::allWidgets();
            for (QWidget *w : widgets) {
                if (!w->isWindow()) {
                    continue;
                }
                if (!w->isVisible()) {
                    continue;
                }
                if (w->size().isEmpty()) {
                    continue;
                }
                QString p = path;
                if (idx > 0) {
                    const qsizetype dot = path.lastIndexOf(QLatin1Char('.'));
                    const QString suffix = QStringLiteral("-%1").arg(idx);
                    if (dot >= 0) {
                        const QString stem = path.left(dot);
                        const QString ext = path.mid(dot);
                        p = stem + suffix + ext;
                    } else {
                        p = path + suffix;
                    }
                }
                static_cast<void>(w->grab().save(p));
                ++idx;
            }
            QApplication::quit();
        });
    }

    return QApplication::exec();
}

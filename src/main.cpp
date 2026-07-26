#include "services/AiAdvisor.h"
#include "services/Config.h"
#include "services/EtoroClient.h"
#include "services/MarketFeeds.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QByteArray>
#include <QTimer>


int main(int argc, char *argv[])
{

    // WSL2's built-in GUI (WSLg) runs a strict Wayland compositor that Qt's client-side
    // window decorations trip over: a maximized top-level ends up with a buffer a few
    // pixels taller than the configured maximized state, which the compositor rejects
    // with a fatal "xdg_surface buffer does not match the configured maximized state"
    // protocol error. XWayland (the xcb platform) doesn't hit this, so on WSL prefer it,
    // falling back to wayland if xcb is unavailable. An explicit QT_QPA_PLATFORM always
    // wins, so the choice stays the user's.
    const bool hasWslDistro = qEnvironmentVariableIsSet("WSL_DISTRO_NAME");
    const bool hasWslInterop = qEnvironmentVariableIsSet("WSL_INTEROP");
    const bool platformForced = qEnvironmentVariableIsSet("QT_QPA_PLATFORM");
    if ((hasWslDistro || hasWslInterop) && !platformForced) {
        static_cast<void>(qputenv("QT_QPA_PLATFORM", "xcb;wayland"));
        // If it does fall back to wayland, drop Qt's decorations too — the same buffer
        // mismatch otherwise resurfaces there.
        if (!qEnvironmentVariableIsSet("QT_WAYLAND_DISABLE_WINDOWDECORATION")) {
            static_cast<void>(qputenv("QT_WAYLAND_DISABLE_WINDOWDECORATION", "1"));
        }
    }

    QApplication app(argc, argv);
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
    MainWindow window(&client, &feeds, &aiAdvisor);
    window.show();

    // Kick off instrument resolution / history / polling after the UI exists
    // so the first signals land on connected slots.
    client.start();
    // VIX + web rating move slowly; refresh them on the same ~20-poll cadence
    // the client used before the feeds were split out (100s at the default poll).
    feeds.start(config.pollIntervalMs * 20);

    // QA aid: TRADINGAPP_SHOT=<path> grabs every visible window to PNGs and quits.
    // TRADINGAPP_SHOT_OPEN=1 also opens the decision and closed-trades windows
    // first, and TRADINGAPP_SHOT_DELAY_MS overrides the capture delay (default
    // 3000) so slow scans can finish before the grab.
    const QString path = qEnvironmentVariable("TRADINGAPP_SHOT");
    if (!path.isEmpty()) {
        if (qEnvironmentVariableIsSet("TRADINGAPP_SHOT_OPEN")) {
            QTimer::singleShot(1500, &window, [&window]() {
                static_cast<void>(QMetaObject::invokeMethod(&window, "openDecision"));
                static_cast<void>(QMetaObject::invokeMethod(&window, "openClosedTrades"));
            });
        }
        const qint32 delayMs = qEnvironmentVariableIsSet("TRADINGAPP_SHOT_DELAY_MS")
                                   ? qEnvironmentVariableIntValue("TRADINGAPP_SHOT_DELAY_MS")
                                   : 3000;
        QTimer::singleShot(delayMs, qApp, [path]() {
            qint32 idx = 0;
            for (QWidget *w : QApplication::allWidgets()) {
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

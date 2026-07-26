#ifndef TRADINGAPP_SERVICES_AIADVISOR_H
#define TRADINGAPP_SERVICES_AIADVISOR_H

#include "domain/Models.h"

#include <QObject>
#include <QString>

class JsonHttp;
class QNetworkAccessManager;

// Asks Claude (Anthropic Messages API) to synthesise a final buy/sell/hold call
// from a caller-built evidence prompt. A thin, replaceable adapter: the rest of
// the app only sees requestDecision() in and AiDecision out, so a different
// model/provider (or a stub for tests) can be swapped in behind this interface.
class AiAdvisor : public QObject
{
    Q_OBJECT
public:
    explicit AiAdvisor(QString apiKey, QObject *parent = nullptr);

    // False when no API key is configured; requestDecision then reports
    // decisionReady(ok=false) instead of calling out.
    [[nodiscard]] bool isConfigured() const { return !m_apiKey.isEmpty(); }


    void requestDecision(const QString &evidencePrompt);

signals:
    // Claude's synthesised final decision (or ok=false with an error / when unconfigured).
    void decisionReady(const AiDecision &decision);

private:
    QString m_apiKey;
    QNetworkAccessManager *m_nam = nullptr;
    JsonHttp *m_http = nullptr;
};

#endif // TRADINGAPP_SERVICES_AIADVISOR_H

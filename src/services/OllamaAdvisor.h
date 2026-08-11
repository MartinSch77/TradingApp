// SPDX-FileCopyrightText: 2026 Martin Schuler
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TRADINGAPP_SERVICES_OLLAMAADVISOR_H
#define TRADINGAPP_SERVICES_OLLAMAADVISOR_H

#include "domain/Models.h"

#include <QObject>
#include <QString>
#include <QStringList>

class JsonHttp;
class QNetworkAccessManager;

// Asks a LOCAL model served by Ollama for a trading proposal (REQ-F-030), from
// the same evidence prompt the cloud advisor gets — so the bot simulation can be
// run on a model that costs nothing, needs no key and never leaves the machine.
//
// Deliberately the same shape as AiAdvisor (requestDecision in, AiDecision out):
// the consumers do not care which brain answered, and either can be swapped for
// the other or for a test stub. Two things differ from the cloud path and drive
// the design:
//
//  * A local model is SLOW (tens of seconds on CPU) and may not be running at
//    all. Hence the availability probe, the long per-request timeout, the
//    one-request-at-a-time guard, and the rule that nothing here may ever block
//    the GUI thread or another feature.
//  * A small local model is a WEAK instruction follower. `format: json` is
//    requested, but the reply is still parsed defensively — JSON wrapped in
//    prose or fences, a word ("high") where a number belongs, "SELL (short)"
//    where an enum belongs. Anything that cannot be read confidently becomes a
//    reported failure, never a trade on a guess.
class OllamaAdvisor : public QObject
{
    Q_OBJECT
public:
    // host: e.g. "http://localhost:11434" (a scheme-less "127.0.0.1:11434", as
    // Ollama's own OLLAMA_HOST is usually written, is accepted too).
    OllamaAdvisor(QString host, QString model, QObject *parent = nullptr);

    // False when no model name is configured: requestDecision then reports
    // decisionReady(ok=false) instead of calling out, exactly like AiAdvisor.
    [[nodiscard]] bool isConfigured() const { return !m_model.isEmpty(); }
    [[nodiscard]] QString model() const { return m_model; }
    [[nodiscard]] QString host() const { return m_host; }

    // GET /api/tags: is the daemon there, and does it serve the configured model?
    // Answers through availability() — the window states the result rather than
    // letting a silent "no proposal" look like the model's opinion.
    void checkAvailability();

    // POST /api/generate (stream=false, format=json). One request at a time: a
    // second call while one is in flight is refused through proposalsReady(error)
    // so a slow model cannot pile up work.
    //
    // The model is asked for EVERY instrument it considers worth trading now, best
    // first (at most kMaxPicks) — not for a single pick: how many trades the bot
    // takes is governed by its risk budget, and the protocol must not be the thing
    // that limits it (REQ-F-030).
    void requestDecision(const QString &evidencePrompt);

    // Ask for a plain-words EXPLANATION of the given evidence (REQ-F-045): displayed, never
    // parsed into anything — the words-only property is the caller's wiring, this method only
    // instructs the model and shapes the answer. Same one-in-flight rule as the decisions.
    void requestExplanation(const QString &evidence);
    [[nodiscard]] bool busy() const { return m_inFlight; }

    // Point the endpoints at `base`, e.g. the tests' in-process MockHttpServer
    // (the same test seam AiAdvisor has). Empty = the configured host.
    void setEndpointBaseForTesting(const QString &base);

signals:
    // The model's picks, best first (empty with `error` set when the request or the
    // parse failed: unconfigured, unreachable, timed out, no JSON). One signal for
    // both outcomes, so a consumer cannot accidentally treat a failure as a HOLD.
    void proposalsReady(const QList<AiDecision> &picks, const QString &error);
    // The explanation's words, or a NAMED error (unconfigured, busy, transport, nothing
    // readable) — exactly one of the two is non-empty, so a blank can never read as
    // "nothing to say".
    void explanationReady(const QString &explanation, const QString &error);
    // ok = the daemon answered AND the configured model is installed.
    // `detail` is a one-line, user-facing diagnosis either way; `models` lists
    // what the daemon actually serves (empty when it could not be asked).
    void availability(bool ok, const QString &detail, const QStringList &models);

private:
    [[nodiscard]] QString endpointBase() const;

    QString m_host;
    QString m_model;
    QString m_endpointBaseForTesting;
    bool m_inFlight = false;
    QNetworkAccessManager *m_nam = nullptr;
    JsonHttp *m_http = nullptr;
};

#endif // TRADINGAPP_SERVICES_OLLAMAADVISOR_H

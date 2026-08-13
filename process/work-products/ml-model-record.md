# Work Product: ML Model Record

**Produced by:** MLE.1–MLE.4. **Owning role:** Software Designer/Developer
(ML), Software Architect (approval to gate a real decision).
**Location:** a section per model in this file (a real, running instance —
see the entries below for TradingApp's actual models) plus the model
artefact's own version metadata (`BotNet`'s persisted file header, ONNX
export metadata).

## Content rules

Per `processes/MLE.1-4-machine-learning-engineering.md`: model category
(A/B/C), dataset provenance (A only), split/embargo method (A only),
baseline comparison and approval threshold result (A only), current version
identifier, deployment status, withdrawal history if any.

## Quality criteria

A Category A model's record shows a TIME-ORDERED split, never a random one;
an "approved" model shows it beat a REAL baseline this project has data for,
not an aspirational one.

## Review requirement

`templates/review-checklist-design.md`'s ML section (extends the unit design
review for a model-producing unit).

## KPIs

Retrain cadence adherence (`BotNet`'s `kRetrainEvery`); count of models
gating a real decision without the never-solely-decides safeguard verified
(target 0).

## Traceability

Model ↔ dataset version (MLE.1/2) ↔ training run/evaluation (MLE.3) ↔
deployed version (MLE.4) ↔ the requirement it serves (REQ-F-033, REQ-F-037,
REQ-F-030 for the advisory Category C case).

## Current model instances (real, not illustrative)

| Model | Category | Dataset | Baseline compared | Status |
|---|---|---|---|---|
| `BotNet` | A | `botsim-experience.jsonl` (append-only, live paper-book closes) | The bot's own prior/naive heuristics (`tools/train_bot_net.py`'s held-out AUC) | Live, retrains every 25 closed trades |
| Crowd-sentiment model | A | `tools/ml/` CrowdStore SQLite export | Always-long, prior-5-min-move, VWAP-side baselines (`CLAUDE.md`) | Offline-trained, optional in-process inference |
| Bot-strategy model | A | `prediction-ledger.jsonl` (every evaluation, including stay-outs) | The bot's own composite call (`dir`, strength-gated) | Offline-trained (`tools/ml/train_bot_model.py`) |
| Local sentiment model (FinBERT-shaped) | B | N/A (externally supplied) | N/A | Test fixture only in this environment; real ~400 MB model is an external download |
| `OllamaAdvisor` (qwen2.5:1.5b) | C | N/A | N/A | Advisory-only; never solely decides a trade (REQ-F-030) |

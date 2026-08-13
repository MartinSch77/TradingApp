# MLE.1–MLE.4 — Machine Learning Engineering

## Purpose

Cover the parts of the ML lifecycle SWE.1–SWE.6 do not ask about — dataset
provenance, temporal-leakage-safe splitting, evaluation/approval thresholds,
and model versioning/withdrawal — for models this project actually TRAINS.
Not (yet) a numbered ASPICE 4.0 process group in the mainline PAM; modelled
here on the emerging ASPICE-for-AI / ISO/IEC 5338-style ML lifecycle
activities. Kept as ONE file, deliberately, rather than four: the four
activities below are one continuous pipeline for this project's models and
splitting them would duplicate the shared "which model category" table.

## Three model categories this project must never conflate

| Category | Examples in this repo | What MLE.1–4 apply to |
|---|---|---|
| **A — Trained by TradingApp** | `BotNet` (`domain/BotNet.h/.cpp` + `tools/train_bot_net.py`), the crowd-sentiment model (`tools/ml/train_crowd_model.py`), the bot-strategy model (`tools/ml/train_bot_model.py`) | Full MLE.1–4 below |
| **B — Externally supplied pretrained** | The local FinBERT-shaped sentiment model (`domain/` finbert runner + `tools/make_finbert_fixture.py`'s tiny fixture stands in for the real ~400 MB model in tests) | MLE.4 only (versioning/withdrawal of the SUPPLIED artifact) — MLE.1–3 do not apply, since this project trains nothing here; provenance is "which upstream release, which checksum" |
| **C — Advisory-only local LLM** | `OllamaAdvisor` (qwen2.5:1.5b via Ollama) | **Never trades on its own** (`CLAUDE.md`'s REQ-F-030 non-negotiable: direction only, never exceeds risk limits) — MLE.1–4 do not apply as a training lifecycle; it is governed instead as a SUPPLIED component like Category B, plus the defensive-parsing discipline already documented (`OllamaResponseParser`) |

The category table itself is the first MLE deliverable: every new model this
project adds must be classified into A/B/C before any of the tasks below are
meaningful, and that classification is recorded in
`work-products/ml-model-record.md`.

## MLE.1 — Data Requirements and Provenance

**Purpose.** Establish where training data comes from, what it may legally
and technically be used for, and — the failure mode this project has already
documented once (`prediction-ledger.jsonl`'s stay-out rows) — that it
includes the cases a naive dataset would silently exclude.

**Tasks.** Record provenance per dataset (e.g. `botsim-experience.jsonl`
comes from the LIVE paper-trading book, not synthetic data;
`prediction-ledger.jsonl` includes EVERY evaluation, including refusals, per
`CLAUDE.md`'s explicit anti-selection-bias design). State any legal/licence
constraint (public market data feeds, no PII). Version the dataset identity
(a content hash or a git-tracked fixture) alongside the model it trained.

**Work Products.** `work-products/ml-model-record.md`'s "Dataset provenance"
section.

## MLE.2 — Data Engineering (temporal leakage, splitting, versioning)

**Purpose.** Prevent the single most common ML defect class: a model that
looks good because it was evaluated on data time-adjacent to what it trained
on.

**Tasks.** Every split in this project is **time-ordered**, never random
(`CLAUDE.md`: "the validation split is by TIME... a random split leaks the
future"), with an EMBARGO between train and holdout where the pipeline
defines one (`crowd_dataset.walk_forward_splits`, reused verbatim by
`bot_dataset.py` rather than reimplemented — one function deciding both
models' folds, so they cannot silently diverge). Missing values are imputed
using ONLY train-set statistics (median), never the whole dataset's.

**Work Products.** The split/embargo parameters recorded in
`work-products/ml-model-record.md`.

## MLE.3 — Model Training, Evaluation, and Approval Thresholds

**Purpose.** Train reproducibly and refuse to ship a model that has not
beaten a real baseline by a real margin.

**Tasks.** Every model is compared against a baseline this project actually
has data for (e.g. the bot's own composite call, NOT the crowd model's
baseline — `CLAUDE.md`'s explicit note that "beats the baseline" must mean a
DIFFERENT real baseline per dataset). An "untrained/under-sampled" model
(< `minSamples` or AUC < `minAuc`) NEVER gates a trade — it only annotates
(`CLAUDE.md`'s `BotNet` trust rule) — this is MLE.3's approval threshold
expressed as running code, not merely a document. ONNX exports are verified
against `onnxruntime` before the file reaches disk.

**Work Products.** `work-products/ml-model-record.md`'s "Evaluation and
approval" section, citing the actual AUC/Brier/baseline-comparison numbers
from the training run.

## MLE.4 — Model Versioning, Deployment, and Withdrawal

**Purpose.** Know which model version is live, be able to roll back, and
retire a model deliberately rather than let it silently keep serving stale
weights.

**Tasks.** `BotNet`'s model file and `tools/ml/`'s ONNX exports are versioned
artefacts under `SUP.8`, tagged with the dataset version (MLE.1) and the
training run's evaluation numbers (MLE.3). Retraining cadence is explicit
(`kRetrainEvery` closed trades for `BotNet`) rather than "whenever someone
remembers." Withdrawal: an untrusted/failing model is disabled by config
(`BotNetMode::Off`), never deleted silently — a withdrawal is itself a
`SUP.10` change with a recorded reason. Category B/C artefacts (Section
"model categories") are versioned by their UPSTREAM release identifier
(Ollama model tag, FinBERT checkpoint name) since this project does not train
them.

## Roles

Software Designer/Developer with ML training responsibility (Responsible),
Software Architect (Accountable for whether a model may gate a real decision
at all, per the never-solely-decides rule above) — see `roles.md`.

## Verification / QA Hooks

QA confirms every model in `work-products/ml-model-record.md` is correctly
classified A/B/C, that a Category A model's record shows a time-ordered split
and a stated baseline comparison, and that no model — of any category — is
wired to bypass the risk/leverage/exposure limits `CLAUDE.md` states as
non-negotiable for any decision source.

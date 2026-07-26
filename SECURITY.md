# Security Policy

This application can place real-money trades through the eToro API. Treat
every finding that could cause an unintended order, leak a credential, or
tamper with the advisory logic as security-relevant.

## Reporting a vulnerability

Please use GitHub's **private vulnerability reporting** (Security → Report a
vulnerability) instead of a public issue. Include reproduction steps and the
affected component (domain / services / ui / tooling). You should receive a
first response within a week.

## Scope notes

- **Secrets**: API keys live exclusively in the git-ignored
  `apiKeyEtoro.json` (see `apiKeyEtoro.example.json`). If you find a way a
  key can end up in the repository, logs, or the dashboard import — report it.
- **Order safety**: money-moving actions require a deliberate double-press
  confirmation and are never triggered by advisory features (plans, close
  watchdog, AI synthesis). Bypasses are vulnerabilities (REQ-N-005).
- **Dependencies**: report vulnerable pinned versions (Qt, PlantUML, pipx
  tools) via the same channel; `./setup.sh update` is the standard remedy.

---
name: add-requirement
description: Add or change a TradingApp requirement the everything-as-code way — edit the StrictDoc source of truth, tag the verifying test, regenerate the derived artefacts and prove the traceability chain is intact. Use whenever new behaviour is requested, a requirement changes, or the user says "add a requirement".
---

# Requirements-as-code workflow (TradingApp)

1. **Edit the source of truth**: `requirements/requirements.sdoc` (StrictDoc).
   New ids continue the scheme `REQ-F-###` (functional) / `REQ-N-###`
   (non-functional); fields: UID, VERIFICATION (T/A/I combinations), TITLE,
   STATEMENT. Never edit `docs/requirements.md` — it is generated.
2. **Design leg**: extend the matching DES row in `docs/design.md`
   (`satisfies` column gains the new REQ id).
3. **Test leg**: add the Qt Test function with the tag block directly above it:
   ```cpp
   //! @tstid TS-XXX-00N @design DES-…
   // @relation(REQ-F-0NN, scope=function)
   void TS_XXX_00N_meaningfulName() { … }
   ```
   (plain `//` for the @relation line — StrictDoc's parser does not read
   Doxygen `//!` comments; test classes use `Q_OBJECT;` with a semicolon so
   tree-sitter parses the first slot.) Add the spec row in
   `docs/test_spec.md`. Verification method I (inspection) needs no test but
   will be listed as an open coverage gap — that is intended honesty.
4. **Regenerate + prove**:
   ```bash
   tools/make_requirements.sh          # sdoc → docs/strictdoc/ + requirements.md
   cmake --build build && tools/run_tests.sh build
   python3 tools/trace_report.py       # must report 0 hard gaps
   ```
5. Report the new trace line (N requirements, N tests, hard gaps) to the user.

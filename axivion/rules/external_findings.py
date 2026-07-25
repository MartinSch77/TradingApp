#  TradingApp — external-findings import rule for the Axivion Suite.
#
#  Bridges the third-party analyzers (clang-tidy, cppcheck, clazy) onto the
#  Axivion dashboard: tools/static_analysis.sh writes their merged findings to
#  analysis-results/external_findings.csv (tool;file;line;rule;severity;message)
#  and this custom rule re-emits each row as a style violation during the next
#  axivion_ci run, so ALL findings live in one dashboard with one workflow
#  (filtering, suppression, delta views).
#
#  One-time registration (Axivion 7.12.x):
#    axivion_config axivion/axivion_config.json
#      -> right-click "Analysis" -> "Additional rules..." -> select axivion/rules
#      -> enable "ExternalFindings-Import" under Stylechecks
#  The rule is a no-op when the CSV is absent, so normal analysis runs are
#  unaffected when the external tools haven't been run.

import csv
import pathlib
import typing

from bauhaus import analysis, ir


@analysis.rule('ExternalFindings-Import', rulegroup='Stylechecks')
class ExternalFindingsImport(analysis.AnalysisRule):
    title = '''Findings imported from clang-tidy / cppcheck / clazy.'''

    _message_descriptions = {
        'external_finding': '{tool} [{rule}]: {message}',
    }

    csv_path: str = 'analysis-results/external_findings.csv'
    """Path (relative to the project directory) of the merged findings CSV
    written by tools/static_analysis.sh."""

    def get_rulehtml_description(
        self, config: typing.Optional[analysis.RuleConfiguration] = None
    ) -> str:
        return """Re-emits the findings of the project's external analyzers
        (clang-tidy, cppcheck, clazy — see tools/static_analysis.sh) as style
        violations, so the Axivion dashboard is the single place to review,
        filter and suppress every static-analysis result. The message carries
        the originating tool and its rule id."""

    def execute(self, ir_graph: ir.Graph):
        path = pathlib.Path(self.csv_path)
        if not path.is_absolute():
            # Resolve against the analysed project's root directory.
            path = pathlib.Path(ir_graph.project_directory) / path \
                if hasattr(ir_graph, 'project_directory') else path
        if not path.exists():
            return  # external tools not run — nothing to import

        with path.open(newline='') as f:
            for row in csv.DictReader(f, delimiter=';'):
                try:
                    sloc = self.source_location(row['file'], int(row['line']))
                except (KeyError, ValueError):
                    continue
                self.add_message(
                    sloc,
                    'external_finding',
                    tool=row.get('tool', 'external'),
                    rule=row.get('rule', ''),
                    message=row.get('message', ''),
                )

    def source_location(self, filename: str, line: int):
        """Build a reportable source location; the exact factory differs
        between Suite versions, so try the known spellings."""
        for attr in ('create_source_location', 'make_source_location'):
            factory = getattr(self, attr, None)
            if factory is not None:
                return factory(filename, line)
        loc = getattr(analysis, 'SourceLocation', None)
        if loc is not None:
            return loc(filename, line)
        raise ValueError('no source-location factory available in this Suite')

# StrictDoc project configuration — requirements-as-code for TradingApp.
#
# Source of truth: requirements/requirements.sdoc. Export via
# tools/make_requirements.sh (HTML with requirement <-> source traceability
# into docs/strictdoc/, plus the regenerated docs/requirements.md).
#
# Python config format (the TOML format is deprecated since StrictDoc 0.27).

from strictdoc.core.project_config import ProjectConfig


def create_config() -> ProjectConfig:
    return ProjectConfig(
        project_title="TradingApp",
        project_features=[
            "TABLE_SCREEN",
            "TRACEABILITY_SCREEN",
            "DEEP_TRACEABILITY_SCREEN",
            "TRACEABILITY_MATRIX_SCREEN",
            "PROJECT_STATISTICS_SCREEN",
            "REQUIREMENT_TO_SOURCE_TRACEABILITY",
            "SOURCE_FILE_LANGUAGE_PARSERS",
            "SEARCH",
        ],
        # Only the sdoc tree is a document source — without this, StrictDoc
        # would ingest every *.md in the repository (Doxygen pages, README,
        # .claude skills) and fail on their formatting.
        include_doc_paths=["requirements/**"],
        # Files scanned for @relation(REQ-…, scope=…) markers: the test suite
        # carries the requirement links (one marker per Qt Test function).
        include_source_paths=["src/**", "tests/**"],
        exclude_source_paths=["build*/**"],
    )

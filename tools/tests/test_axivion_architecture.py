# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

"""Unit tests for axivion/architecture.py.

The real module is Axivion-Suite configuration: it does
`from bauhaus.architecture.scripted_architecture import *`, and that package
only exists inside an installed Axivion Suite, not in this repo's Python
environment. These tests stub `bauhaus.architecture.scripted_architecture`
with minimal recording classes — enough to exercise the module's own control
flow (which components depend on which, which directories map to which
component, which views get created) without needing the Suite installed.

The file itself is straight-line (no if/else), so "branch coverage" here
means every statement executes and every dependency/mapping/view call is
verified against the layering docs (CLAUDE.md: domain <- services <- ui,
main composes both)."""

import sys
import types

import pytest


class _Component:
    def __init__(self, label):
        self.label = label
        self.deps = []

    def depends_on(self, other):
        self.deps.append(other)


class _Architecture:
    def __init__(self, name, *components):
        self.name = name
        self.components = components
        self.views = []
        for component in components:
            setattr(self, component.label, component)

    def create_view(self, rfg, name):
        self.views.append((rfg, name))


class _Mapping:
    def __init__(self, rfg, kind):
        self.rfg = rfg
        self.kind = kind
        self.mappings = []
        self.mapping_views = []

    def add_mapping(self, path, component):
        self.mappings.append((path, component))

    def create_mapping_view(self, name):
        self.mapping_views.append(name)


_INPUT_RFG = object()


@pytest.fixture
def stub_bauhaus(monkeypatch):
    mod_bauhaus = types.ModuleType("bauhaus")
    mod_arch_pkg = types.ModuleType("bauhaus.architecture")
    mod_scripted = types.ModuleType("bauhaus.architecture.scripted_architecture")
    mod_scripted.Architecture = _Architecture
    mod_scripted.Component = _Component
    mod_scripted.Mapping = _Mapping
    mod_scripted.INPUT_RFG = _INPUT_RFG
    mod_bauhaus.architecture = mod_arch_pkg
    mod_arch_pkg.scripted_architecture = mod_scripted

    monkeypatch.setitem(sys.modules, "bauhaus", mod_bauhaus)
    monkeypatch.setitem(sys.modules, "bauhaus.architecture", mod_arch_pkg)
    monkeypatch.setitem(sys.modules, "bauhaus.architecture.scripted_architecture", mod_scripted)
    sys.modules.pop("architecture", None)
    yield
    sys.modules.pop("architecture", None)


def test_layering_is_strictly_downward(stub_bauhaus):
    import architecture

    arch = architecture.ARCH
    assert arch.Domain.deps == []
    assert arch.Services.deps == [arch.Domain]
    assert arch.UI.deps == [arch.Services, arch.Domain]
    assert arch.Main.deps == [arch.UI, arch.Services]


def test_directory_mapping_matches_layers(stub_bauhaus):
    import architecture

    arch = architecture.ARCH
    mapping = architecture.MAPPING
    assert mapping.mappings == [
        ("src/domain", arch.Domain),
        ("src/services", arch.Services),
        ("src/ui", arch.UI),
        ("src/main.cpp", arch.Main),
    ]


def test_views_are_materialized_with_the_rule_defaults(stub_bauhaus):
    import architecture

    arch = architecture.ARCH
    mapping = architecture.MAPPING
    assert arch.views == [(_INPUT_RFG, "Architecture")]
    assert mapping.mapping_views == ["Mapping"]
    assert mapping.rfg is _INPUT_RFG
    assert mapping.kind == "File"


def test_no_tests_component_is_declared(stub_bauhaus):
    """Regression guard for the documented "3 Absence AVs" mistake: a Tests
    component must never be declared since the analysis IR has no test
    binary in it."""
    import architecture

    labels = {c.label for c in architecture.ARCH.components}
    assert labels == {"Domain", "Services", "UI", "Main"}

# The reference architecture of TradingApp, as code — the same layering the
# linker already enforces (CMakeLists: domain links Qt Core only; services may
# not link ui; ui sits on top), now stated once as the model the Axivion
# architecture check verifies on every run. Divergences (a source dependency
# the model does not allow) and absences (a declared dependency the code no
# longer has) both surface as AV findings on the dashboard, and the model is
# browsable in Gravis (views "Architecture" / "Mapping" / "Architecture Check").
#
# Executed by the Architecture-ScriptedArchitecture rule (rule_config.json),
# which provides INPUT_RFG. Edges are declared ONLY where the dependency both
# is intended AND exists — a declared-but-unused edge is reported as an
# Absence, so "main may use domain directly" is deliberately not modelled
# until main.cpp actually does (today it composes services + ui only).
# Qt and other unmapped externals do not participate in the check.
from bauhaus.architecture.scripted_architecture import *

# No Tests component: the analysis IR is the TradingApp executable
# (ci_config.json "ir"), so the test binaries never enter the RFG — a declared
# Tests edge would map to nothing and be reported as an Absence (measured:
# exactly that happened with a Tests component, 3 Absence AVs).
ARCH = Architecture(
    "TradingApp Layered Architecture",
    Component("Domain"),    # pure trading logic — Qt Core only, no I/O, no UI
    Component("Services"),  # broker client, web feeds, AI advisor, calendar (Qt Network)
    Component("UI"),        # windows, panels, models/views (Qt Widgets/Charts/Concurrent)
    Component("Main"),      # the composition root (src/main.cpp)
)

MAPPING = Mapping(INPUT_RFG, 'File')

# --- allowed (and present) dependencies, strictly downward ------------------
ARCH.Services.depends_on(ARCH.Domain)

ARCH.UI.depends_on(ARCH.Services)
ARCH.UI.depends_on(ARCH.Domain)

ARCH.Main.depends_on(ARCH.UI)
ARCH.Main.depends_on(ARCH.Services)

# --- mapping: one directory per layer, main.cpp on its own ------------------
# Directory mapping is transitive: new files under a mapped directory are
# mapped implicitly, so adding a class never requires touching this model.
MAPPING.add_mapping('src/domain', ARCH.Domain)
MAPPING.add_mapping('src/services', ARCH.Services)
MAPPING.add_mapping('src/ui', ARCH.UI)
MAPPING.add_mapping('src/main.cpp', ARCH.Main)

# Materialize the RFG views the ArchitectureCheck rule consumes (their names
# are that rule's defaults: architecture_view_name / mapping_view_name).
ARCH.create_view(INPUT_RFG, "Architecture")
MAPPING.create_mapping_view("Mapping")

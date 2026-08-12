# SPDX-FileCopyrightText: 2026 Martin Schuler
# SPDX-License-Identifier: GPL-3.0-or-later

# One target per fuzz harness. Each links trading_domain (Qt Core only — see the root
# CMakeLists' layering comment) plus libFuzzer/ASan/UBSan themselves, via
# -fsanitize=fuzzer,address,undefined on both compile and link lines. No
# find_package/FetchContent for libFuzzer: it ships inside clang's own compiler-rt.
function(tradingapp_add_fuzzer name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE trading_domain Qt6::Core)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -g -O1)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
endfunction()

# TradeScript's parser (src/domain/TradeScript.cpp, REQ-F-028): untrusted text read
# from a plain file the user hand-edits, split into ';'-separated fields and coerced
# through QDateTime::fromString/QString::toDouble/toInt — exactly the kind of
# hand-rolled text parser libFuzzer is best at (tooling backlog item 3).
tradingapp_add_fuzzer(fuzz_tradescript tradescript_fuzzer.cpp)

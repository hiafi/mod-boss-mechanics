# Pulls the `.mech test` parser out of src/MechanicCommand.cpp for tests/drill_parse_test.cpp (tests/run.sh): the
# namespace-level blocks named below, each from its first line to its closing brace at the same 4-space indent. A
# renamed or missing block fails the test build (the #error) instead of testing a stale copy.
BEGIN {
    starts = "std::optional<Color> ParseColor\\(|std::optional<uint8> ParseSchool\\(|class Options$|"
    starts = starts "std::vector<std::string_view> Fields\\(|std::optional<Role> ParseRole\\(|bool ParseAuraBand\\(|"
    starts = starts "bool ParseSelfTrigger\\(|bool ParseOrdering\\(|bool ParseShift\\(|struct TestSpec$|"
    starts = starts "bool IsShapeName\\(|struct ParseCtx$|std::string ParseTest\\("
    want = 13
    pattern = "^    (" starts ")"
}
!on && $0 ~ pattern {
    on = 1
    n++
}
on { print }
on && /^    \};?$/ {
    on = 0
    print ""
}
END {
    if (n != want)
        print "#error tests/extract_parse.awk: extracted " n " of " want " blocks from MechanicCommand.cpp"
}

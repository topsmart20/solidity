contract C {
    function f() public returns (uint256) {
        return ([1, 2, 3, 4][2]);
    }
}

// ====
// compileViaYul: also
// compileToEwasm: also
// ----
// f() -> 3

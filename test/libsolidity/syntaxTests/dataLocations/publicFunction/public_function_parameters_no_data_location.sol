contract C {
    function h(uint[]) public pure {}
}
// ----
// TypeError: (28-34): Data location must be "memory" or "calldata" for parameter in function, but none was given.

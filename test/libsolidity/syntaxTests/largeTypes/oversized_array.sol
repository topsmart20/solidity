// SPDX-License-Identifier: GPL-3.0
pragma solidity >= 0.0;
contract C {
    uint[2**255][2] a;
}
// ----
// TypeError 1534: (77-94): Type too large for storage.
// TypeError 7676: (60-97): Contract too large for storage.

contract Small {
    uint public a;
    uint[] public b;
    function f1(uint x) public returns (uint) { a = x; b[uint8(msg.data[0])] = x; }
    fallback () external payable {}
}
// ----
// creation:
//   codeDepositCost: 120600
//   executionCost: 165
//   totalCost: 120765
// external:
//   fallback: 129
//   a(): 1083
//   b(uint256): infinite
//   f1(uint256): infinite

export function hook(_reserved) {
  void _reserved;

  const txType = otxn.type();
  if (!txType.ok) lifecycle.rollback("otxn.type failed", txType.code);

  const account = lifecycle.account();
  if (!account.ok) lifecycle.rollback("hook_account failed", account.code);

  const write = state.set("HELLO", new Uint8Array([1, 2, 3, 4]));
  if (!write.ok) lifecycle.rollback("state_set failed", write.code);

  if (ledger.sequence <= 0 || ledger.lastTime < 0 || ledger.lastHash.isZero()) {
    lifecycle.rollback("invalid ledger context", -1);
  }

  lifecycle.accept(
    `hello:${txType.value}:${account.value.toHex().slice(0, 8)}`,
    17,
  );
}

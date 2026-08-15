export function main() {

  const txType = otxn.type();
  if (!txType.ok) rollback("otxn.type failed", txType.error.code);

  const account = hook.account();

  const write = state.set("HELLO", new Uint8Array([1, 2, 3, 4]));
  if (!write.ok) rollback("state_set failed", write.error.code);

  if (ledger.sequence <= 0 || ledger.lastTime < 0 || ledger.lastHash.isZero()) {
    rollback("invalid ledger context", -1);
  }

  accept(
    `hello:${txType.value}:${account.toHex().slice(0, 8)}`,
    17,
  );
}

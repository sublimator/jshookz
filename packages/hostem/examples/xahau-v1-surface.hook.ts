/** Compile-only coverage of every member in xahau-quickjs-v1.d.ts. */
function typecheckV1Surface(blob: STBlob, hash: Hash256, account: AccountID): void {
  STBlob.from(blob.toBytes());
  blob.byteLength;
  blob.byteAt(0);
  blob.toBytes();
  blob.toHex();
  blob.equals(blob);

  Hash256.from(hash.toBytes());
  hash.toHex();
  hash.toBytes();
  hash.isZero();
  hash.equals(hash);

  AccountID.from(account.toBytes());
  account.toHex();
  account.toBytes();

  const xfl = XFL.fromRaw(0n);
  xfl.raw;
  xfl.mantissa();
  xfl.exponent();
  xfl.isNegative();
  xfl.isZero();

  lifecycle.account();
  ledger.sequence;
  ledger.lastTime;
  ledger.lastHash;
  const transactionType = otxn.type();
  if (transactionType.ok) {
    void (transactionType.value === TransactionType.Payment);
  }
  state.get("key");
  state.set("key", blob);
  emit.reserve(1);
  emit.prepare(blob);
  emit.tx(blob);
  trace("surface", 1);
}

export function hook(_reserved: number): never {
  void _reserved;
  void typecheckV1Surface;
  return accept("surface declarations compile", 0);
}

export function cbak(_reserved: number): never {
  void _reserved;
  return accept("callback declarations compile", 0);
}

/** Compile-only coverage of every member in xahau-quickjs-v1.d.ts. */
function typecheckV1Surface(blob: STBlob, hash: Hash256, account: AccountID): void {
  STBlob.from(blob.toBytes());
  STBlob.fromHex("");
  blob.byteLength;
  blob.byteAt(0);
  blob.toBytes();
  blob.toHex();
  blob.equals(blob);

  Hash256.from(hash.toBytes());
  Hash256.fromHex("00".repeat(32));
  hash.toHex();
  hash.toBytes();
  hash.isZero();
  hash.equals(hash);

  AccountID.from(account.toBytes());
  AccountID.fromHex("00".repeat(20));
  AccountID.zero.toHex();
  AccountID.one.toHex();
  account.toHex();
  account.toBytes();

  const xfl = XFLDecimal.fromRaw(0n);
  xfl.raw;
  xfl.mantissa();
  xfl.exponent();
  xfl.isNegative();
  xfl.isZero();

  const uint = rollback.onFail(UInt64.from(1n), "UInt64 construction failed", 1);
  uint.bits;
  uint.byteLength;
  uint.toBigInt();
  uint.toString();
  rollback.onFail(uint.toNumber(), "UInt64 conversion failed", 1);
  uint.isZero();
  uint.equals(UInt64.max);
  uint.compare(uint);
  rollback.onFail(uint.add(1n), "UInt64 addition failed", 1);
  rollback.onFail(uint.subtract(1n), "UInt64 subtraction failed", 1);
  rollback.onFail(UInt64.mulDiv(uint, 3n, 2n), "UInt64 mulDiv failed", 1);
  uint.saturatingAdd(uint);
  uint.saturatingSubtract(uint);
  UInt8.zero;
  UInt16.max;
  UInt32.zero;
  UInt64.max;

  hook.account().toHex();
  ledger.sequence;
  ledger.lastTime;
  ledger.lastHash;
  const transactionType = otxn.type();
  if (transactionType.ok) {
    void (transactionType.value === TransactionType.Payment);
  }
  state.get("key").okOr(undefined);
  state.set("key", blob).moot();
  rollback.onFail(state.get("key"), "state read failed");
  rollback.require(state.get("key"), "state is required", 2);
  emit.reserve(1).moot();
  emit.prepare(blob).okOr(blob);
  emit.tx(blob).okOr(undefined);
  trace("surface", 1);
}

export function main(): never {
  void typecheckV1Surface;
  return accept("surface declarations compile", 0);
}

export function callback(info: CallbackInfo): never {
  if (info.failed !== ((info.rawFlags & 1) !== 0)) {
    return rollback("callback info invariant failed", 1);
  }
  return accept("callback declarations compile", 0);
}

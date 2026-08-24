export function main(): never {
  rollback.onAnyFail(
    [
      state.set("FIRST", new Uint8Array([1])),
      state.set("SECOND", new Uint8Array([2])),
    ],
    "batch write failed",
  );

  const first = rollback.requirePresent(
    state.get("FIRST"),
    "first value disappeared",
    1,
  );
  accept(`first=${first.byteAt(0)}`, 0);
}

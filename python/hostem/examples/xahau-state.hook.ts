export function main(): never {
  const previous = rollback.onFail(state.get("GREETING"), "state read failed");

  if (previous === undefined) {
    rollback.onFail(
      state.set("GREETING", new Uint8Array([72, 105])),
      "state write failed",
    );
  }

  const greeting = rollback.requirePresent(
    state.get("GREETING"),
    "state disappeared",
    1,
  );
  accept(greeting.toBytes(), 0);
}

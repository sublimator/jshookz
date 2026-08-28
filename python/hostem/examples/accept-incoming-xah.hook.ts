const MAX_INCOMING_DROPS = 100_000_000n;

enum ResultCode {
  WrongDestination = 1,
  SelfPayment = 2,
  NonNativeAmount = 3,
  AmountOutOfRange = 4,
}

/**
 * Accept bounded native-XAH Payments into this Hook account.
 *
 * Non-Payment transactions do not participate in this policy. Payment fields
 * are read from one certified local originating-transaction snapshot; field
 * access performs no additional host calls.
 */
export function main(): never {
  const transactionType = rollback.onFail(
    otxn.type(),
    "cannot classify originating transaction",
  );
  accept.when(
    transactionType !== TransactionType.Payment,
    "not an incoming Payment",
  );

  const transaction = otxn.object();
  accept.require(
    transaction instanceof Payment,
    "originating Payment is not structurally complete",
  );

  const source = transaction.Account;
  const destination = transaction.Destination;
  const amount = transaction.Amount;

  const hookAccount = hook.account();
  rollback.when(
    !destination.equals(hookAccount),
    "Payment is not addressed to this Hook account",
    ResultCode.WrongDestination,
  );
  rollback.when(
    source.equals(hookAccount),
    "self-Payments are outside incoming-payment policy",
    ResultCode.SelfPayment,
  );

  const nativeAmount = rollback.requirePresent(
    amount.asNative(),
    "Payment Amount must be native XAH",
    ResultCode.NonNativeAmount,
  );
  rollback.when(
    nativeAmount.drops <= 0n || nativeAmount.drops > MAX_INCOMING_DROPS,
    "Payment Amount is outside the accepted range",
    ResultCode.AmountOutOfRange,
  );

  accept("incoming native XAH accepted", 0);
}

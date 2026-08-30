const ENTRY_DROPS = 1_000_000n;
const PRIZE_DROPS = 5_000_000n;
const ENTROPY_POLICY = record("LuckySixEntropyPolicy", 3, [
  ["requireFullCommitReveal", record.u8()],
  ["minimumContributors", record.u16le()],
]);

enum ResultCode {
  WrongDestination = 1,
  SelfPayment = 2,
  InvalidStake = 3,
  InvalidConfiguration = 4,
  EntropyQuality = 5,
  EntropyUnavailable = 6,
  PrizeReserve = 7,
  PrizeBuild = 8,
  PrizeEmit = 9,
}

function requiredEntropyTier(): EntropyTier {
  const policy = rollback.onFail(
    hook.param("EntropyPolicy", ENTROPY_POLICY),
    "Cannot read Lucky Six entropy policy",
    ResultCode.InvalidConfiguration,
  );
  if (policy === undefined) return EntropyTier.validatorQuorum;
  rollback.when(
    policy.requireFullCommitReveal > 1,
    "EntropyPolicy full-commit/reveal flag must be zero or one",
    ResultCode.InvalidConfiguration,
  );

  const requireFull = policy.requireFullCommitReveal === 1;
  if (!requireFull && policy.minimumContributors === 0) {
    return EntropyTier.validatorQuorum;
  }

  const quality = rollback.onFail(
    entropy.cr.status(),
    "Cannot inspect commit/reveal quality",
    ResultCode.EntropyQuality,
  );
  if (requireFull) {
    rollback.when(
      quality.tier !== EntropyTier.validatorFull,
      "Full commit/reveal tier is required",
      ResultCode.EntropyQuality,
    );
    rollback.when(
      quality.count !== quality.denominator,
      "Every entropy contributor must reveal",
      ResultCode.EntropyQuality,
    );
  }
  if (policy.minimumContributors > 0) {
    rollback.when(
      quality.count < policy.minimumContributors,
      "Minimum entropy contributor count is not met",
      ResultCode.EntropyQuality,
    );
  }
  if (requireFull) return EntropyTier.validatorFull;
  return EntropyTier.validatorQuorum;
}

function sendPrize(destination: AccountID): void {
  rollback.onFail(
    emit.reserve(1),
    "Cannot reserve the Lucky Six prize",
    ResultCode.PrizeReserve,
  );
  const prize = rollback.onFail(
    emit.build.payment({
      Destination: destination,
      Amount: Amount.drops(PRIZE_DROPS),
    }),
    "Cannot build the Lucky Six prize",
    ResultCode.PrizeBuild,
  );
  rollback.onFail(
    emit.tx(prize),
    "Cannot emit the Lucky Six prize",
    ResultCode.PrizeEmit,
  );
}

/**
 * Lucky Six Tip Jar
 *
 * Send exactly one XAH. Rolls one quorum-grade D6; six wins five XAH.
 * Open-ledger entropy is a preview; closed-ledger execution is authoritative.
 */
export function main(): never {
  const transaction = accept.requireTransaction(TransactionType.Payment);

  const hookAccount = hook.account();
  rollback.when(
    !transaction.Destination.equals(hookAccount),
    "Payment is not addressed to Lucky Six",
    ResultCode.WrongDestination,
  );
  rollback.when(
    transaction.Account.equals(hookAccount),
    "Lucky Six cannot play against itself",
    ResultCode.SelfPayment,
  );

  const stake = rollback.requirePresent(
    transaction.Amount.asNative(),
    "Lucky Six accepts native XAH only",
    ResultCode.InvalidStake,
  );
  rollback.when(
    stake.drops !== ENTRY_DROPS,
    "Lucky Six costs exactly one XAH",
    ResultCode.InvalidStake,
  );

  const face = rollback.onFail(
    entropy.cr.dice(6, requiredEntropyTier()),
    "Required entropy is unavailable",
    ResultCode.EntropyUnavailable,
  );
  accept.when(face !== 5, `Thanks! You rolled ${face + 1}.`);

  sendPrize(transaction.Account);

  accept("Lucky six! Your five-XAH prize is on its way.");
}

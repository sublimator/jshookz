/**
 * Exact JavaScript surface implemented by the sealed xahau-quickjs-v1
 * provider. The accompanying API artifact manifest closes this declaration,
 * its broader specification, and the provider surface manifest by hash.
 */

type BytesLike = Uint8Array | ArrayBuffer | readonly number[];

type HexString = string;

type UIntWidth = 8 | 16 | 32 | 64;

type UIntInput<Bits extends UIntWidth = UIntWidth> = UInt<Bits> | bigint | number;

type LedgerSequence = number;

type RippleTime = number;

type Falsy = false | 0 | 0n | "" | null | undefined;

type Truthy<T> = Exclude<T, Falsy>;

/**
 * Type-only surface shared by every nominal provider-produced Result.
 *
 * There are six ways to leave a Result; anything else is a bug: `okOr`,
 * `okOrHandle`, `okMapOr`, `moot`, exhaustive `.ok` narrowing, or a
 * `rollback.*` consumer.
 */
interface ResultInstance<T, Error> {

  /**
   * Return `.value` whenever `.ok` is true, including a successful
   * `undefined`; return `fallback` only when `.ok` is false. The fallback
   * expression is evaluated before this method is called; use `okOrHandle`
   * when producing it has work or side effects.
  */
  okOr<Fallback>(fallback: Fallback): T | Fallback;

  /**
   * Return `.value` whenever `.ok` is true; otherwise invoke `handler` once
   * with `.error` and return the value it produces.
   *
   * The compiler rejects handlers whose inferred return type is `never`.
   * Termination is explicit control flow: use `rollback.onFail` for failure
   * rollback, or exhaustive `.ok` narrowing for another terminal.
   */
  okOrHandle<Fallback>(handler: (error: Error) => Fallback): T | Fallback;

  /**
   * Transform a successful value and return it directly; return `fallback`
   * when this Result is a failure. The fallback expression is evaluated
   * before this method is called.
   *
   * Use this only when discarding the failure is deliberate. In particular,
   * malformed persisted state should normally remain loud rather than be
   * collapsed into the same value used for absent state.
   */
  okMapOr<Value, Fallback>(
    handler: (value: T) => Value,
    fallback: Fallback,
  ): Value | Fallback;

}

/** Capability exposed only by Results whose success type is exactly void. */
interface MootableResultInstance<Error> {
  /**
   * Declare the failure of this void Result moot: neither outcome bears on
   * contract correctness. JavaScript exceptions and provider faults are not
   * Result failures and are not suppressed.
   */
  moot(this: Result<void, Error>): void;
}

/** Shared discriminated carrier; each domain owns its error payload. */
type ResultSuccess<T, Error> = ResultInstance<T, Error> & {
  readonly ok: true;
  readonly value: T;
};

type ResultFailure<T, Error> = ResultInstance<T, Error> & {
  readonly ok: false;
  readonly error: Error;
};

type Result<T, Error> =
  ([T] extends [void]
    ? [void] extends [T]
      ? MootableResultInstance<Error>
      : unknown
    : unknown) &
  (ResultSuccess<T, Error> | ResultFailure<T, Error>);

interface HostError {
  readonly domain: "host";
  readonly code: HookReturnCode;
}

/**
 * The result of an operation governed by Hooks host-status semantics.
 *
 * A negative Hooks status is ordinary contract-visible data, not an exception.
 * Successful optional reads may still contain `undefined` when the operation
 * explicitly folds `DOESNT_EXIST` into absence; every other host failure keeps
 * its exact `HookReturnCode`. Hook termination and JavaScript/runtime faults
 * remain exceptions.
 *
 * This carrier does not prove that a Wasm host bridge was physically crossed.
 * Provider-side rich facades may return `HostResult` when they enforce the
 * same host rules and preserve the corresponding Hook status exactly.
 */
type HostResult<T> = Result<T, HostError>;

/** Failure from constructing or operating on a bounded unsigned integer. */
interface UIntError {
  readonly domain: "uint";
  readonly issue:
    | "out-of-range"
    | "overflow"
    | "underflow"
    | "division-by-zero";
  readonly bits: UIntWidth;
}

type UIntResult<T> = Result<T, UIntError>;

/**
 * Immutable fixed-width unsigned integer. Width classes extend this
 * (`instanceof UInt` and `instanceof UInt8`).
 *
 * JavaScript operators intentionally remain available through the default
 * primitive record projections (`u32be`, `u64be`). Choose this value type
 * when the contract wants its width and overflow policy carried with the
 * value rather than re-established around every arithmetic expression.
 */
declare abstract class UInt<Bits extends UIntWidth = UIntWidth> {
  readonly bits: Bits;
  readonly byteLength: Bits extends 8 ? 1 : Bits extends 16 ? 2 : Bits extends 32 ? 4 : 8;
  protected constructor();

  toBigInt(): bigint;
  toString(): string;
  /** Conversion is total through 32 bits; UInt64 may exceed JS safe integer. */
  toNumber(): Bits extends 64 ? UIntResult<number> : number;
  isZero(): boolean;
  equals(other: unknown): boolean;
  compare(other: UInt<Bits>): -1 | 0 | 1;
  add(other: UIntInput<Bits>): UIntResult<UInt<Bits>>;
  subtract(other: UIntInput<Bits>): UIntResult<UInt<Bits>>;
  saturatingAdd(other: UInt<Bits>): UInt<Bits>;
  saturatingSubtract(other: UInt<Bits>): UInt<Bits>;
}

declare class UInt8 extends UInt<8> {
  private constructor();
  static readonly zero: UInt8;
  static readonly max: UInt8;
  static from(value: bigint | number): UIntResult<UInt8>;
  static mulDiv(
    multiplicand: UIntInput<8>,
    multiplier: UIntInput<8>,
    divisor: UIntInput<8>,
  ): UIntResult<UInt8>;
}

declare class UInt16 extends UInt<16> {
  private constructor();
  static readonly zero: UInt16;
  static readonly max: UInt16;
  static from(value: bigint | number): UIntResult<UInt16>;
  static mulDiv(
    multiplicand: UIntInput<16>,
    multiplier: UIntInput<16>,
    divisor: UIntInput<16>,
  ): UIntResult<UInt16>;
}

declare class UInt32 extends UInt<32> {
  private constructor();
  static readonly zero: UInt32;
  static readonly max: UInt32;
  static from(value: bigint | number): UIntResult<UInt32>;
  static mulDiv(
    multiplicand: UIntInput<32>,
    multiplier: UIntInput<32>,
    divisor: UIntInput<32>,
  ): UIntResult<UInt32>;
}

declare class UInt64 extends UInt<64> {
  private constructor();
  static readonly zero: UInt64;
  static readonly max: UInt64;
  static from(value: bigint | number): UIntResult<UInt64>;
  static mulDiv(
    multiplicand: UIntInput<64>,
    multiplier: UIntInput<64>,
    divisor: UIntInput<64>,
  ): UIntResult<UInt64>;
}

/** @serial Blob */
declare class STBlob {
  private constructor();
  readonly byteLength: number;
  byteAt(index: number): number;
  toBytes(): Uint8Array;
  toHex(): HexString;
  equals(other: BytesLike | STBlob): boolean;
  static from(value: BytesLike): STBlob;
  /** Decode an even-length hexadecimal literal. */
  static fromHex(value: HexString): STBlob;
}

/** @serial Hash256 */
declare class Hash256 {
  static readonly zero: Hash256;
  static from(value: BytesLike | Hash256): Hash256;
  /** Decode exactly 32 bytes from an even-length hexadecimal literal. */
  static fromHex(value: HexString): Hash256;
  private constructor();
  toHex(): HexString;
  toBytes(): Uint8Array;
  isZero(): boolean;
  equals(other: BytesLike | Hash256): boolean;
}

/** @serial AccountID */
declare class AccountID {
  /** XRP's native-issue account: 20 zero bytes. */
  static readonly zero: AccountID;
  /** Ripple's no-account sentinel: integer one as a 20-byte AccountID. */
  static readonly one: AccountID;
  static from(value: BytesLike): AccountID;
  /** Decode exactly 20 bytes from an even-length hexadecimal literal. */
  static fromHex(value: HexString): AccountID;
  private constructor();
  toHex(): HexString;
  toBytes(): Uint8Array;
  isZero(): boolean;
  equals(other: BytesLike | AccountID): boolean;
}

declare const enum TransactionType {
  Payment = 0,
  Invoke = 99,
  GenesisMint = 96,
  Import = 97,
  ClaimReward = 98,
  SetHook = 22,
  TrustSet = 20,
  Remit = 95,
  NFTokenBurn = 26,
  URITokenMint = 45,
  UNLReport = 104,
  EmitFailure = 103,
  UNLModify = 102,
  SetFee = 101,
  EnableAmendment = 100,
  SetRemarks = 94,
  CronSet = 93,
  Cron = 92,
  PermissionedDomainDelete = 72,
  PermissionedDomainSet = 71,
  NFTokenModify = 70,
  CredentialDelete = 69,
  CredentialAccept = 68,
  CredentialCreate = 67,
  MPTokenAuthorize = 66,
  MPTokenIssuanceSet = 65,
  MPTokenIssuanceDestroy = 64,
  MPTokenIssuanceCreate = 63,
  LedgerStateFix = 62,
  OracleDelete = 61,
  OracleSet = 60,
  DIDDelete = 59,
  DIDSet = 58,
  XChainCreateBridge = 57,
  XChainModifyBridge = 56,
  XChainAddAccountCreateAttestation = 55,
  XChainAddClaimAttestation = 54,
  XChainAccountCreateCommit = 53,
  XChainClaim = 52,
  XChainCommit = 51,
  XChainCreateClaimID = 50,
  URITokenCancelSellOffer = 49,
  URITokenCreateSellOffer = 48,
  URITokenBuy = 47,
  URITokenBurn = 46,
  AMMDelete = 40,
  AMMBid = 39,
  AMMVote = 38,
  AMMWithdraw = 37,
  AMMDeposit = 36,
  AMMCreate = 35,
  AMMClawback = 31,
  Clawback = 30,
  NFTokenAcceptOffer = 29,
  NFTokenCancelOffer = 28,
  NFTokenCreateOffer = 27,
  NFTokenMint = 25,
  AccountDelete = 21,
  DepositPreauth = 19,
  CheckCancel = 18,
  CheckCash = 17,
  CheckCreate = 16,
  PaymentChannelClaim = 15,
  PaymentChannelFund = 14,
  PaymentChannelCreate = 13,
  SignerListSet = 12,
  SpinalTap = 11,
  TicketCreate = 10,
  Contract = 9,
  OfferCancel = 8,
  OfferCreate = 7,
  NicknameSet = 6,
  SetRegularKey = 5,
  EscrowCancel = 4,
  AccountSet = 3,
  EscrowFinish = 2,
  EscrowCreate = 1,
}

/** Information supplied to an emitted-transaction callback entry point. */
interface CallbackInfo {
  /** Whether the emitted transaction failed. */
  readonly failed: boolean;

  /**
   * Exact uint32 callback word supplied by Xahau. Prefer named properties;
   * this is retained for diagnostics and forward-compatible expert use.
   */
  readonly rawFlags: number;
}

declare namespace otxn {
  function type(): HostResult<TransactionType>;
}

declare namespace state {
  function get(key: string | BytesLike | STBlob | Hash256 | AccountID): HostResult<STBlob | undefined>;
  function set(
    key: string | BytesLike | STBlob | Hash256 | AccountID,
    value: string | BytesLike | STBlob | Hash256 | AccountID,
  ): HostResult<void>;
}

declare namespace emit {
  function reserve(count: number): HostResult<void>;
  function tx(transaction: BytesLike | STBlob): HostResult<Hash256>;
  function prepare(partial: BytesLike | STBlob): HostResult<STBlob>;
}

declare namespace ledger {
  const sequence: LedgerSequence;
  const lastTime: RippleTime;
  const lastHash: Hash256;
}

/** Metadata and configuration for the currently executing Hook. */
declare namespace hook {
  /** Hook account for this invocation; provider construction is total. */
  function account(): AccountID;
}

/**
 * Accept and terminate this hook execution. The host records the terminal
 * result and unwinds the current Wasm invocation; no JavaScript statement
 * after this call can run, hence the `never` return type.
 *
 * C Hooks have the same observable behavior even though their import ABI is
 * declared as returning `int64_t`: Xahaud turns `RC_ACCEPT` into engine
 * termination before the import returns to guest code. C source therefore
 * uses both `accept(...)` and the redundant `return accept(...)` spelling.
 * TypeScript should normally use the direct call.
 *
 * A supplied `code` becomes the integer HookReturnCode recorded on-ledger.
 * Omit it to let the compiler/provider record source location. Do not copy
 * C `__LINE__`. Explicitly meaningful codes remain caller-owned.
 */
declare function accept(message?: string | BytesLike | STBlob, code?: number): never;

declare namespace accept {
  /**
   * Continue only if this is present/successful. Otherwise `accept` —
   * this invocation succeeded and did nothing.
   * There is no `accept.require`; that name is a compile error on purpose.
   */
  function unless<T, Error>(
    result: Result<T, Error>,
    message?: string | BytesLike | STBlob,
    code?: number,
  ): Exclude<T, null | undefined>;
  function unless<T>(
    value: T,
    message?: string | BytesLike | STBlob,
    code?: number,
  ): Exclude<T, Falsy>;
  /**
   * If `condition` is true, `accept`. If false, return and continue.
   */
  function when(
    condition: unknown,
    message?: string | BytesLike | STBlob,
    code?: number,
  ): void;
}

/**
 * Reject, atomically roll back, and terminate this hook execution. Like
 * `accept`, the host unwinds the Wasm invocation and this call never returns.
 * Its `never` return is intentionally usable in expression positions such as
 * `value ?? rollback("value is required")`.
 */
declare function rollback(message?: string | BytesLike | STBlob, code?: number): never;

declare namespace rollback {
  /**
   * Return a successful host value, including a legitimate `undefined`, or
   * atomically roll back with the exact failed host status as the terminal
   * code. Supply `message` when the surrounding contract gives that failure
   * more useful context than the default host-status diagnostic.
   */
  function onFail<T>(
    result: HostResult<T>,
    message?: string | BytesLike | STBlob,
  ): T;
  /**
   * Apply a contract-owned terminal policy to a result whose failure does not
   * already carry a Hook status.
   */
  function onFail<T, Error>(
    result: Result<T, Error>,
    message: string | BytesLike | STBlob,
    code?: number,
  ): T;
  /**
   * Require a successful, present value. A failed result and a successful
   * `undefined` or `null` are translated into the supplied contract-owned
   * rollback policy rather than preserving an incidental failure status.
   * Other falsy successes (`0`, `0n`, `false`, and `""`) remain valid values.
   */
  function require<T, Error>(
    result: Result<T, Error>,
    message: [T] extends [void]
      ? [void] extends [T]
        ? never
        : string | BytesLike | STBlob
      : string | BytesLike | STBlob,
    code?: number,
  ): Exclude<T, null | undefined>;
  /**
   * Require a truthy direct value. `false`, `0`, `0n`, `""`, `null`,
   * `undefined`, and `NaN` therefore apply the rollback policy.
   */
  function require<T>(
    value: T,
    message: string | BytesLike | STBlob,
    code?: number,
  ): Exclude<T, Falsy>;
  /**
   * If `condition` is true, `rollback`. If false, return and continue.
   */
  function when(
    condition: unknown,
    message?: string | BytesLike | STBlob,
    code?: number,
  ): void;
  /**
   * Return every value when every host operation succeeded. If any failed,
   * roll back with the first failure's host code, in input order. JavaScript
   * evaluates every array element before this helper runs; use sequential
   * checks when later host calls must be conditional on earlier success.
   */
  function onAnyFail<T>(
    results: readonly HostResult<T>[],
    message?: string | BytesLike | STBlob,
  ): readonly T[];
  /** Apply a contract-owned rollback policy when any domain result failed. */
  function onAnyFail<T, Error>(
    results: readonly Result<T, Error>[],
    message: string | BytesLike | STBlob,
    code: number,
  ): readonly T[];
  /**
   * Return the successful values in input order when at least one operation
   * succeeded. If all operations failed, apply the supplied contract-owned
   * rollback policy. This accepts `ParseResult` and other result domains as
   * well as host results. An empty input therefore rolls back. All array
   * elements are evaluated before this helper inspects their Results.
   */
  function onAllFail<T, Error>(
    results: readonly Result<T, Error>[],
    message: string | BytesLike | STBlob,
    code: number,
  ): readonly T[];
}

/**
 * Trace a diagnostic value. The provider renders scalar values and emits
 * byte-bearing values as hexadecimal without requiring encoding-specific
 * contract calls.
 */
declare function trace(label: string, value?: unknown): void;

/** Negative values returned by host functions on failure. */
declare const enum HookReturnCode {
  SUCCESS = 0,
  OUT_OF_BOUNDS = -1,
  INTERNAL_ERROR = -2,
  TOO_BIG = -3,
  TOO_SMALL = -4,
  DOESNT_EXIST = -5,
  NO_FREE_SLOTS = -6,
  INVALID_ARGUMENT = -7,
  ALREADY_SET = -8,
  PREREQUISITE_NOT_MET = -9,
  FEE_TOO_LARGE = -10,
  EMISSION_FAILURE = -11,
  TOO_MANY_NONCES = -12,
  TOO_MANY_EMITTED_TXN = -13,
  NOT_IMPLEMENTED = -14,
  INVALID_ACCOUNT = -15,
  GUARD_VIOLATION = -16,
  INVALID_FIELD = -17,
  PARSE_ERROR = -18,
  RC_ROLLBACK = -19,
  RC_ACCEPT = -20,
  NO_SUCH_KEYLET = -21,
  NOT_AN_ARRAY = -22,
  NOT_AN_OBJECT = -23,
  DIVISION_BY_ZERO = -25,
  MANTISSA_OVERSIZED = -26,
  MANTISSA_UNDERSIZED = -27,
  EXPONENT_OVERSIZED = -28,
  EXPONENT_UNDERSIZED = -29,
  XFL_OVERFLOW = -30,
  NOT_IOU_AMOUNT = -31,
  NOT_AN_AMOUNT = -32,
  CANT_RETURN_NEGATIVE = -33,
  NOT_AUTHORIZED = -34,
  PREVIOUS_FAILURE_PREVENTS_RETRY = -35,
  TOO_MANY_PARAMS = -36,
  INVALID_TXN = -37,
  RESERVE_INSUFFICIENT = -38,
  COMPLEX_NOT_SUPPORTED = -39,
  DOES_NOT_MATCH = -40,
  INVALID_KEY = -41,
  NOT_A_STRING = -42,
  MEM_OVERLAP = -43,
  TOO_MANY_STATE_MODIFICATIONS = -44,
  TOO_MANY_NAMESPACES = -45,
  INVALID_FLOAT = -10024,
}

/**
 * Generated from hooks-api.d.ts by scripts/project-v1-declarations.py.
 *
 * Exact JavaScript surface implemented by the sealed xahau-quickjs-v1
 * provider. This deliberately narrow declaration is the compiler default.
 * Edit the canonical declaration or PROFILE in the generator, not this file.
 */

type HookTypedArray =
  | Int8Array
  | Uint8Array
  | Uint8ClampedArray
  | Int16Array
  | Uint16Array
  | Int32Array
  | Uint32Array
  | Float32Array
  | Float64Array
  | BigInt64Array
  | BigUint64Array;

type BytesLike =
  | HookTypedArray
  | ArrayBuffer
  | string;

type HookReturnCode = number;

/** Shared discriminated carrier; each domain owns its failure payload. */
type ResultSuccess<T> = { readonly ok: true; readonly value: T };

type ResultFailure = { readonly ok: false };

type Result<T, Failure extends ResultFailure> = ResultSuccess<T> | Failure;

type HostFailure = ResultFailure & { readonly code: HookReturnCode };

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
type HostResult<T> = Result<T, HostFailure>;

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

declare class STBlob {
  readonly byteLength: number;
  static from(value: BytesLike): STBlob;
  byteAt(index: number): number;
  toBytes(): Uint8Array;
  toHex(): string;
  equals(other: BytesLike | STBlob): boolean;
}

declare class Hash256 {
  static from(value: BytesLike): Hash256;
  toHex(): string;
  toBytes(): Uint8Array;
  isZero(): boolean;
  equals(other: Hash256): boolean;
}

declare class AccountID {
  static from(value: BytesLike): AccountID;
  toHex(): string;
  toBytes(): Uint8Array;
}

declare class XFL {
  readonly raw: bigint;
  static fromRaw(raw: bigint | number): XFL;
  mantissa(): number;
  exponent(): number;
  isNegative(): boolean;
  isZero(): boolean;
}

declare namespace hook {
  function account(): HostResult<AccountID>;
}

declare namespace ledger {
  const sequence: number;
  const lastTime: number;
  const lastHash: Hash256;
}

declare namespace otxn {
  function type(): HostResult<TransactionType>;
}

declare namespace state {
  function get(key: BytesLike | STBlob | Hash256 | AccountID): HostResult<STBlob | undefined>;
  function set(
    key: BytesLike | STBlob | Hash256 | AccountID,
    value: BytesLike | STBlob | Hash256 | AccountID,
  ): HostResult<void>;
}

declare namespace emit {
  function reserve(count: number): HostResult<void>;
  function prepare(transaction: BytesLike | STBlob): HostResult<STBlob>;
  function tx(transaction: BytesLike | STBlob): HostResult<Hash256>;
}

declare namespace rollback {
  function onFail<T>(
    result: HostResult<T>,
    message?: string | BytesLike | STBlob,
  ): T;
  function onFail<T, Failure extends ResultFailure>(
    result: Result<T, Failure>,
    message: string | BytesLike | STBlob,
    code: number,
  ): T;
  function require<T, Failure extends ResultFailure>(
    result: Result<T | undefined, Failure>,
    message: string | BytesLike | STBlob,
    code: number,
  ): T;
  function onAnyFail<T>(
    results: readonly HostResult<T>[],
    message?: string | BytesLike | STBlob,
  ): readonly T[];
  function onAnyFail<T, Failure extends ResultFailure>(
    results: readonly Result<T, Failure>[],
    message: string | BytesLike | STBlob,
    code: number,
  ): readonly T[];
  function onAllFail<T, Failure extends ResultFailure>(
    results: readonly Result<T, Failure>[],
    message: string | BytesLike | STBlob,
    code: number,
  ): readonly T[];
}

/**
 * `accept` and `rollback` are non-returning Hook terminals: Xahaud records
 * the outcome and unwinds the current Wasm invocation. C Hooks have the same
 * behavior despite an `int64_t` import ABI; engine termination happens before
 * the import can return to guest code.
 */

declare function accept(
  message?: string | Uint8Array | ArrayBuffer,
  code?: number,
): never;

declare function rollback(
  message?: string | Uint8Array | ArrayBuffer,
  code?: number,
): never;

declare function trace(label: string, value?: unknown): void;

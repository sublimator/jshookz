/**
 * Exact JavaScript surface implemented by the sealed xahau-quickjs-v1
 * provider. This deliberately narrow declaration is the compiler default.
 * See hooks-api.d.ts for the broader public API specification.
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
type HostSuccess<T> = { readonly ok: true; readonly value: T };
type HostFailure = { readonly ok: false; readonly code: HookReturnCode };
type HostResult<T> = HostSuccess<T> | HostFailure;

/** Compile-time transaction codes; const-enum values emit no runtime lookup. */
declare const enum TransactionType {
  Payment = 0,
  EscrowCreate = 1,
  EscrowFinish = 2,
  AccountSet = 3,
  EscrowCancel = 4,
  SetRegularKey = 5,
  NicknameSet = 6,
  OfferCreate = 7,
  OfferCancel = 8,
  Contract = 9,
  TicketCreate = 10,
  SpinalTap = 11,
  SignerListSet = 12,
  PaymentChannelCreate = 13,
  PaymentChannelFund = 14,
  PaymentChannelClaim = 15,
  CheckCreate = 16,
  CheckCash = 17,
  CheckCancel = 18,
  DepositPreauth = 19,
  TrustSet = 20,
  AccountDelete = 21,
  SetHook = 22,
  NFTokenMint = 25,
  NFTokenBurn = 26,
  NFTokenCreateOffer = 27,
  NFTokenCancelOffer = 28,
  NFTokenAcceptOffer = 29,
  Clawback = 30,
  AMMClawback = 31,
  AMMCreate = 35,
  AMMDeposit = 36,
  AMMWithdraw = 37,
  AMMVote = 38,
  AMMBid = 39,
  AMMDelete = 40,
  URITokenMint = 45,
  URITokenBurn = 46,
  URITokenBuy = 47,
  URITokenCreateSellOffer = 48,
  URITokenCancelSellOffer = 49,
  XChainCreateClaimID = 50,
  XChainCommit = 51,
  XChainClaim = 52,
  XChainAccountCreateCommit = 53,
  XChainAddClaimAttestation = 54,
  XChainAddAccountCreateAttestation = 55,
  XChainModifyBridge = 56,
  XChainCreateBridge = 57,
  DIDSet = 58,
  DIDDelete = 59,
  OracleSet = 60,
  OracleDelete = 61,
  LedgerStateFix = 62,
  MPTokenIssuanceCreate = 63,
  MPTokenIssuanceDestroy = 64,
  MPTokenIssuanceSet = 65,
  MPTokenAuthorize = 66,
  CredentialCreate = 67,
  CredentialAccept = 68,
  CredentialDelete = 69,
  NFTokenModify = 70,
  PermissionedDomainSet = 71,
  PermissionedDomainDelete = 72,
  Cron = 92,
  CronSet = 93,
  SetRemarks = 94,
  Remit = 95,
  GenesisMint = 96,
  Import = 97,
  ClaimReward = 98,
  Invoke = 99,
  EnableAmendment = 100,
  SetFee = 101,
  UNLModify = 102,
  EmitFailure = 103,
  UNLReport = 104,
}

declare class STBlob {
  readonly byteLength: number;
  static from(value: BytesLike): STBlob;
  byteAt(index: number): number;
  toBytes(): Uint8Array;
  toHex(): string;
  equals(other: BytesLike | STBlob): boolean;
}

/** Runtime name used by xahau-quickjs-v1; the canonical API names this STHash. */
declare class Hash256 {
  static from(value: BytesLike): Hash256;
  toHex(): string;
  toBytes(): Uint8Array;
  isZero(): boolean;
  equals(other: Hash256): boolean;
}

/** Runtime name used by xahau-quickjs-v1; the canonical API names this STAddress. */
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

declare namespace lifecycle {
  function account(): HostResult<AccountID>;
  function accept(message?: string | Uint8Array | ArrayBuffer, code?: number): never;
  function rollback(message?: string | Uint8Array | ArrayBuffer, code?: number): never;
}

/** Compatibility terminal aliases retained by the v1 provider. */
declare function accept(
  message?: string | Uint8Array | ArrayBuffer,
  code?: number,
): never;
declare function rollback(
  message?: string | Uint8Array | ArrayBuffer,
  code?: number,
): never;

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

declare function trace(label: string, value?: unknown): void;

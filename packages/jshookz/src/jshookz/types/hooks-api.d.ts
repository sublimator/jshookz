/**
 * The canonical JavaScript/TypeScript API declaration for Xahau Hooks.
 *
 * Shaped by lazy rich native values and batched host round-trips. Consequently:
 * no buffer parameters and no output-length arguments. Ordinary negative host
 * statuses are tagged `HostResult` values; exceptions are reserved for hook
 * termination and JavaScript/runtime programming faults.
 *
 * This file is the public specification. Runtime profiles may implement a
 * deliberately versioned subset; tooling must not silently widen that subset.
 */

type BytesLike = Uint8Array | ArrayBuffer | ArrayBufferView | string | readonly number[];
type HexString = string;
type UInt8 = number;
type UInt16 = number;
type UInt32 = number;
type UInt64 = bigint;
type Drops = bigint;
type LedgerSequence = number;
type RippleTime = number;
type UInt32OrHash = UInt32 | STHash<32>;
type BytePart = BytesLike | STBlob | STHash | STAddress | STCurrency | number | bigint;
type StateKeyLike = BytesLike | string | STBlob | STHash | STAddress | STCurrency;
type StateValueLike = BytesLike | STBlob | STHash | STAddress | STCurrency | STAmount;
type BatchKeys = Record<string, StateKeyLike>;
type BatchValues<T extends Record<string, unknown>> = { readonly [K in keyof T]: STBlob | undefined };

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
type HostSuccess<T> = { readonly ok: true; readonly value: T };
type HostFailure = { readonly ok: false; readonly code: HookReturnCode };
type HostResult<T> = HostSuccess<T> | HostFailure;

/** A pure binary-codec validation result; no Hooks host call occurred. */
type ParseResult<T> =
  | { readonly ok: true; readonly value: T }
  | {
      readonly ok: false;
      readonly issue: "wrong-length";
      readonly expectedLength: number;
      readonly actualLength: number;
    }
  | {
      readonly ok: false;
      readonly issue: "invalid-value";
    }
  | {
      readonly ok: false;
      readonly issue: "invalid-field";
      readonly field: string;
    };

interface RecordField<T, Width extends number = number> {
  readonly kind: string;
  readonly offset: number;
  readonly byteLength: Width;

  /** Permit overlap only with fields carrying the same non-empty group. */
  overlay(group: string): RecordField<T, Width>;
}

/** Produce one configured record field at the requested offset. */
type RootFieldFactory<T, Width extends number> =
  (offset: number) => RecordField<T, Width>;

/**
 * A named scalar schema produced from one configured root-field factory.
 * Parsing requires exactly `byteLength`; encoding and parsing reuse the
 * returned field's representation logic. Runtime schema objects are frozen.
 */
interface ScalarSchema<
  Name extends string,
  T,
  Width extends number,
> {
  readonly name: Name;
  readonly byteLength: Width;
  safeParse(value: BytesLike | STBlob): ParseResult<T>;
  parse(value: BytesLike | STBlob): T;
  encode(value: T): STBlob;
}

/**
 * Define a named scalar schema from either a field factory or an already
 * configured root field. `cell` invokes a factory with zero, then requires a
 * genuine record-field descriptor rooted at offset zero in either form.
 */
declare function cell<
  const Name extends string,
  T,
  const Width extends number,
>(
  name: Name,
  factory: RootFieldFactory<T, Width>,
): ScalarSchema<Name, T, Width>;

declare function cell<
  const Name extends string,
  T,
  const Width extends number,
>(
  name: Name,
  rootField: RecordField<T, Width>,
): ScalarSchema<Name, T, Width>;

type RecordShape = Readonly<Record<string, RecordField<unknown, number>>>;
type RecordFieldValue<T> = T extends RecordField<infer V, number> ? V : never;
type RecordValue<T extends RecordShape> = {
  -readonly [K in keyof T as RecordFieldValue<T[K]> extends never ? never : K]: RecordFieldValue<T[K]>;
};

interface RecordSchema<
  Name extends string,
  Size extends number,
  Shape extends RecordShape,
> {
  readonly name: Name;
  readonly byteLength: Size;
  readonly fields: Shape;

  /**
   * Decode a record after validating its size and field representations.
   * Prefer this result-valued form for state or transaction-derived bytes.
   */
  safeParse(value: BytesLike | STBlob): ParseResult<RecordValue<Shape>>;

  /**
   * Assertion form for a programmer-guaranteed record. Throws on malformed
   * input; it must not become the default for untrusted persisted bytes.
   */
  parse(value: BytesLike | STBlob): RecordValue<Shape>;

  encode(value: RecordValue<Shape>): STBlob;
  patch(
    source: BytesLike | STBlob,
    values: Partial<RecordValue<Shape>>,
  ): ParseResult<STBlob>;
}

/**
 * Define a checked fixed-width composite binary record at hook runtime.
 *
 * Construction validates bounds, coverage, and overlap. Overlapping fields
 * are rejected unless every participant names the same explicit overlay
 * group. The default coverage policy is `complete`; use `record.padding(...)`
 * for reserved bytes or opt into `allow-gaps` deliberately.
 *
 * This is a runtime API first. A future compiler may recognize and lower the
 * same declarative schema to generated TypeScript, WASM, or native metadata
 * without changing hook source.
 */
declare function record<
  const Name extends string,
  const Size extends number,
  const Shape extends RecordShape,
>(
  name: Name,
  byteLength: Size,
  fields: Shape,
  options?: record.Options,
): RecordSchema<Name, Size, Shape>;

declare namespace record {
  interface Options {
    readonly coverage?: "complete" | "allow-gaps";
  }

  function u8(offset: number): RecordField<UInt8, 1>;
  function u16be(offset: number): RecordField<UInt16, 2>;
  function u16le(offset: number): RecordField<UInt16, 2>;
  function u32be(offset: number): RecordField<UInt32, 4>;
  function u32le(offset: number): RecordField<UInt32, 4>;
  function i32be(offset: number): RecordField<number, 4>;
  function i32le(offset: number): RecordField<number, 4>;
  function u64be(offset: number): RecordField<UInt64, 8>;
  function u64le(offset: number): RecordField<UInt64, 8>;
  function i64be(offset: number): RecordField<bigint, 8>;
  function i64le(offset: number): RecordField<bigint, 8>;
  function xflbe(offset: number): RecordField<XFL, 8>;
  function xflle(offset: number): RecordField<XFL, 8>;
  function bytes<const Width extends number>(offset: number, byteLength: Width): RecordField<STBlob, Width>;
  function hash<const Width extends number>(offset: number, byteLength: Width): RecordField<STHash<Width>, Width>;
  function address(offset: number): RecordField<STAddress, 20>;
  function currency(offset: number): RecordField<STCurrency, 20>;

  /** A named field that participates in coverage but is omitted from values. */
  function padding<const Width extends number>(offset: number, byteLength: Width): RecordField<never, Width>;
}

interface ByteCompareOptions {
  readonly caseSensitive?: boolean;
}

interface ByteFindOptions extends ByteCompareOptions {
  readonly start?: number;
}

interface STSerializeOptions {
  readonly field?: string | number;
  readonly includeFieldHeader?: boolean;
}

/** @serial Blob */
declare class STBlob {
  readonly byteLength: number;
  static from(value: BytesLike): STBlob;
  static concat(...parts: (BytesLike | STBlob)[]): STBlob;
  static fromUint8(value: number): STBlob;
  static fromUint32(value: number, endian?: "big" | "little"): STBlob;
  static fromUint64(value: bigint | number, endian?: "big" | "little"): STBlob;
  byteAt(index: number): number;
  slice(start: number, end?: number): STBlob;
  toBytes(): Uint8Array;
  toHex(): HexString;
  toUint8(): number;
  toUint32(endian?: "big" | "little"): number;
  toUint64(endian?: "big" | "little"): bigint;
  toXFL(endian?: "big" | "little"): XFL;
  toAddress(): STAddress;
  toCurrency(): STCurrency;
  toHash<N extends number = number>(): STHash<N>;
  isZero(): boolean;
  equals(other: BytesLike | STBlob): boolean;
  compare(other: BytesLike | STBlob, options?: ByteCompareOptions): -1 | 0 | 1;
  indexOf(needle: BytesLike | STBlob, options?: ByteFindOptions): number | undefined;
}

/** @serial Hash128 Hash160 Hash192 Hash256 Hash384 Hash512
 *  @inner-rich-type STHash */
declare class STHash<N extends number = number> {
  readonly byteLength: N;
  static readonly zero256: STHash<32>;
  static from(value: BytesLike): STHash<32>;
  constructor(value: BytesLike);
  toBytes(): Uint8Array;
  toHex(): HexString;
  isZero(): boolean;
  equals(other: BytesLike | STHash<N>): boolean;
  compare(other: STHash<N>): number;
}

/** @serial AccountID
 *  @inner-rich-type STAddress */
declare class STAddress {
  readonly byteLength: 20;
  readonly r: string;
  static readonly zero: STAddress;
  static from(value: BytesLike | string): STAddress;
  static fromRAddress(value: string): STAddress;
  toBytes(): Uint8Array;
  toHex(): HexString;
  toString(): string;
  equals(other: STAddress | BytesLike | string): boolean;
  compare(other: STAddress): number;
}

/** @serial Currency */
declare class STCurrency {
  readonly byteLength: 20;
  readonly isNative: boolean;
  static readonly native: STCurrency;
  static from(value: BytesLike | string): STCurrency;
  toBytes(): Uint8Array;
  toHex(): HexString;
  toString(): string;
  equals(other: STCurrency | BytesLike | string): boolean;
}

/** @serial Issue */
declare class STIssue {
  readonly kind: "native" | "iou" | "mpt";
  readonly currency?: STCurrency;
  readonly issuer?: STAddress;
  readonly mptIssuanceId?: STHash<32>;
  static native(): STIssue;
  static iou(currency: STCurrency, issuer: STAddress): STIssue;
  static mpt(mptIssuanceId: STHash<32>): STIssue;
  equals(other: STIssue): boolean;
}

/** @inner-rich-type XFL */
declare class XFL {
  readonly raw: bigint;
  static readonly zero: XFL;
  static readonly one: XFL;
  static fromRaw(raw: bigint): XFL;
  /**
   * Construct `mantissa × 10^exponent`. The value comes first so ordinary
   * calls read in the same order as the decimal quantity they express.
   */
  static from(mantissa: bigint | number, exponent: number): XFL;
  mantissa(): bigint;
  exponent(): number;
  isNegative(): boolean;
  isZero(): boolean;
  sign(): -1 | 0 | 1;
  log(): XFL;
  root(degree: number): XFL;
  /**
   * Apply the bounded Hooks `float_int` projection. Conversion failures remain
   * ordinary Hook statuses even when a provider evaluates the rule locally.
   */
  toInt(decimalPlaces?: number, absolute?: boolean): HostResult<bigint>;
  toString(): string;
  equals(other: XFL): boolean;
  compare(other: XFL): number;
}

/** @serial Amount */
declare class STAmount {
  readonly kind: "native" | "iou" | "mpt";
  readonly issue: STIssue;
  readonly currency?: STCurrency;
  readonly issuer?: STAddress;
  readonly mptIssuanceId?: STHash<32>;
  readonly xfl?: XFL;
  readonly drops?: Drops;
  readonly byteLength: 8 | 33 | 48;
  static from(value: BytesLike | STBlob): STAmount;
  static drops(value: Drops): STNativeAmount;
  static iou(value: XFL, currency: STCurrency, issuer: STAddress): STAmount;
  static mpt(value: XFL, mptIssuanceId: STHash<32>): STAmount;
  toBytes(options?: STSerializeOptions): Uint8Array;
  toXFL(): XFL;
  toString(): string;
  isNative(): this is STNativeAmount;
  isIOU(): this is STIOUAmount;
  isMPT(): this is STMPTAmount;
  equals(other: STAmount): boolean;
  compare(other: STAmount): number;
}

declare interface STNativeAmount extends STAmount {
  readonly kind: "native";
  readonly drops: Drops;
  readonly currency: undefined;
  readonly issuer: undefined;
  readonly xfl: undefined;
}

declare interface STIOUAmount extends STAmount {
  readonly kind: "iou";
  readonly drops: undefined;
  readonly currency: STCurrency;
  readonly issuer: STAddress;
  readonly xfl: XFL;
}

declare interface STMPTAmount extends STAmount {
  readonly kind: "mpt";
  readonly drops: undefined;
  readonly mptIssuanceId: STHash<32>;
  readonly xfl: XFL;
}

declare interface STPathHop {
  readonly account?: STAddress;
  readonly currency?: STCurrency;
  readonly issuer?: STAddress;
}

declare interface STPath extends Iterable<STPathHop> {
  readonly length: number;
  at(index: number): STPathHop | undefined;
}

/** @serial PathSet */
declare class STPathSet implements Iterable<STPath> {
  readonly length: number;
  at(index: number): STPath | undefined;
  [Symbol.iterator](): IterableIterator<STPath>;
}

/**
 * A protocol field descriptor derived from Xahau definitions.
 *
 * `T` is type-only; the runtime value contains the three numeric codes. The
 * literal code parameters let drift checkers retain exact source equality
 * while `T` lets generic object access infer the decoded value. `Code` is
 * redundant by design: it equals `(TypeCode << 16) | FieldCode`, but
 * TypeScript cannot express that numeric-literal arithmetic directly. The
 * declaration checker enforces the relationship instead.
 */
declare interface SerializedField<
  T,
  Code extends number = number,
  TypeCode extends number = number,
  FieldCode extends number = number,
> {
  readonly code: Code;
  readonly typeCode: TypeCode;
  readonly fieldCode: FieldCode;
  readonly __valueType?: T;
}

type SerializedFieldValue<T> =
  T extends SerializedField<infer V> ? unknown extends V ? never : V : never;

/** Values derived mechanically from the protocol `Field` descriptor table. */
type ProtocolFieldValue = {
  [K in keyof typeof Field]: SerializedFieldValue<(typeof Field)[K]>;
}[keyof typeof Field];

/** A decoded protocol field value, plus absence for an unset field. */
type STFieldValue = ProtocolFieldValue | undefined;

/**
 * Immutable decoded-object view.
 *
 * `withField` and `withoutField` return a new logical value and never mutate
 * this object. Implementations may structurally share backing bytes, decoded
 * values, and patch overlays so long as that sharing is unobservable.
 *
 * @serial STObject LedgerEntry
 * @inner-rich-type STObject
 */
declare interface STObject {
  has(field: string | SerializedField<unknown>): boolean;
  get<T>(field: SerializedField<T>): T | undefined;
  get(field: string): STFieldValue;
  fieldBytes(field: string | number | SerializedField<unknown>): STBlob | undefined;
  withField(field: string | number | SerializedField<unknown>, value: STFieldValue | BytesLike): STObject;
  withoutField(field: string | number | SerializedField<unknown>): STObject;
  toBytes(options?: STSerializeOptions): Uint8Array;
  toJSON(): unknown;
}

/** @serial STArray
 *  @inner-rich-type STArray */
declare interface STArray<T extends STObject = STObject> extends Iterable<T> {
  readonly length: number;
  at(index: number): T | undefined;
}

/** @serial Transaction */
declare interface STTransaction extends STObject {
  readonly TransactionType: TransactionType;
  readonly Account: STAddress;
  readonly Destination?: STAddress;
  readonly Amount?: STAmount;
  readonly Amounts?: STArray;
  readonly Fee?: STAmount;
  readonly Flags?: UInt32;
  readonly Sequence?: UInt32;
  readonly Blob?: STBlob;
  readonly NFTokenID?: STHash<32>;
  readonly HookParameters?: STArray;
}

declare interface STAccountRoot extends STObject {
  readonly LedgerEntryType: "AccountRoot";
  readonly Account: STAddress;
  readonly Balance: STNativeAmount;
  readonly Flags: UInt32;
  readonly ImportSequence?: UInt32;
  readonly RewardAccumulator?: UInt64;
  readonly RewardLgrFirst?: LedgerSequence;
  readonly RewardLgrLast?: LedgerSequence;
  readonly RewardTime?: RippleTime;
  readonly Sequence: UInt32;
  readonly OwnerCount: UInt32;
  readonly PreviousTxnID: STHash<32>;
  readonly PreviousTxnLgrSeq: UInt32;
  readonly AccountTxnID?: STHash<32>;
  readonly RegularKey?: STAddress;
  readonly EmailHash?: STHash<16>;
  readonly WalletLocator?: STHash<32>;
  readonly WalletSize?: UInt32;
  readonly MessageKey?: STBlob;
  readonly TransferRate?: UInt32;
  readonly Domain?: STBlob;
  readonly TickSize?: UInt8;
  readonly TicketCount?: UInt32;
  readonly NFTokenMinter?: STAddress;
  readonly MintedNFTokens?: UInt32;
  readonly BurnedNFTokens?: UInt32;
  readonly HookStateCount?: UInt32;
  readonly FirstNFTokenSequence?: UInt32;
  readonly GovernanceFlags?: STHash<32>;
  readonly GovernanceMarks?: STHash<32>;
  readonly AccountIndex?: UInt64;
  readonly TouchCount?: UInt64;
  readonly HookStateScale?: UInt16;
  readonly Cron?: STHash<32>;
  readonly AMMID?: STHash<32>;
  readonly LedgerIndex?: STHash<32>;
  readonly Remarks?: STArray;
}

declare interface STActiveValidator {
  readonly account: STAddress;
  readonly publicKey?: STBlob;
}

declare interface STUNLReport extends STObject {
  readonly LedgerEntryType: "UNLReport";
  readonly ActiveValidators: readonly STActiveValidator[];
}

declare interface STNFToken extends STObject {
  readonly NFTokenID: STHash<32>;
  readonly URI?: STBlob;
}

/** @serial Metadata */
declare interface STMetadata extends STObject {
  readonly TransactionResult: TransactionResult;
  findNFToken(id: STHash<32>): STNFToken | undefined;
}

declare interface STXPop {
  readonly transaction: STTransaction;
  readonly metadata: STMetadata;
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

declare const enum TransactionResult {
  tesSUCCESS = 0,
  tecLAST_POSSIBLE_ENTRY = 255,
  tecBAD_CREDENTIALS = 199,
  tecLOCKED = 198,
  tecARRAY_TOO_LARGE = 197,
  tecARRAY_EMPTY = 196,
  tecTOKEN_PAIR_NOT_FOUND = 195,
  tecINVALID_UPDATE_TIME = 194,
  tecEMPTY_DID = 193,
  tecXCHAIN_BAD_PUBLIC_KEY_ACCOUNT_PAIR = 192,
  tecINCOMPLETE = 191,
  tecHAS_HOOK_STATE = 190,
  tecTOO_MANY_REMARKS = 189,
  tecIMMUTABLE = 188,
  tecINSUF_RESERVE_SELLER = 187,
  tecXCHAIN_CREATE_ACCOUNT_DISABLED = 186,
  tecXCHAIN_SELF_COMMIT = 185,
  tecXCHAIN_PAYMENT_FAILED = 184,
  tecXCHAIN_ACCOUNT_CREATE_TOO_MANY = 183,
  tecXCHAIN_ACCOUNT_CREATE_PAST = 182,
  tecXCHAIN_INSUFF_CREATE_AMOUNT = 181,
  tecXCHAIN_SENDING_ACCOUNT_MISMATCH = 180,
  tecXCHAIN_NO_SIGNERS_LIST = 179,
  tecXCHAIN_REWARD_MISMATCH = 178,
  tecXCHAIN_WRONG_CHAIN = 177,
  tecXCHAIN_CREATE_ACCOUNT_NONXRP_ISSUE = 176,
  tecXCHAIN_PROOF_UNKNOWN_KEY = 175,
  tecXCHAIN_CLAIM_NO_QUORUM = 174,
  tecXCHAIN_BAD_CLAIM_ID = 173,
  tecXCHAIN_NO_CLAIM_ID = 172,
  tecXCHAIN_BAD_TRANSFER_ISSUE = 171,
  tecPRECISION_LOSS = 170,
  tecREQUIRES_FLAG = 169,
  tecAMM_ACCOUNT = 168,
  tecAMM_NOT_EMPTY = 167,
  tecAMM_EMPTY = 166,
  tecAMM_INVALID_TOKENS = 165,
  tecAMM_FAILED = 164,
  tecAMM_BALANCE = 163,
  tecUNFUNDED_AMM = 162,
  tecINSUFFICIENT_PAYMENT = 161,
  tecOBJECT_NOT_FOUND = 160,
  tecINSUFFICIENT_FUNDS = 159,
  tecCANT_ACCEPT_OWN_NFTOKEN_OFFER = 158,
  tecNFTOKEN_OFFER_TYPE_MISMATCH = 157,
  tecNFTOKEN_BUY_SELL_MISMATCH = 156,
  tecNO_SUITABLE_NFTOKEN_PAGE = 155,
  tecMAX_SEQUENCE_REACHED = 154,
  tecHOOK_REJECTED = 153,
  tecTOO_SOON = 152,
  tecHAS_OBLIGATIONS = 151,
  tecKILLED = 150,
  tecDUPLICATE = 149,
  tecEXPIRED = 148,
  tecINVARIANT_FAILED = 147,
  tecCRYPTOCONDITION_ERROR = 146,
  tecOVERSIZE = 145,
  tecINTERNAL = 144,
  tecDST_TAG_NEEDED = 143,
  tecNEED_MASTER_KEY = 142,
  tecINSUFFICIENT_RESERVE = 141,
  tecNO_ENTRY = 140,
  tecNO_PERMISSION = 139,
  tecNO_TARGET = 138,
  tecFROZEN = 137,
  tecINSUFF_FEE = 136,
  tecNO_LINE = 135,
  tecNO_AUTH = 134,
  tecNO_ISSUER = 133,
  tecOWNERS = 132,
  tecNO_REGULAR_KEY = 131,
  tecNO_ALTERNATIVE_KEY = 130,
  tecUNFUNDED = 129,
  tecPATH_DRY = 128,
  tecNO_LINE_REDUNDANT = 127,
  tecNO_LINE_INSUF_RESERVE = 126,
  tecNO_DST_INSUF_NATIVE = 125,
  tecNO_DST = 124,
  tecINSUF_RESERVE_OFFER = 123,
  tecINSUF_RESERVE_LINE = 122,
  tecDIR_FULL = 121,
  tecFAILED_PROCESSING = 105,
  tecUNFUNDED_PAYMENT = 104,
  tecUNFUNDED_OFFER = 103,
  tecUNFUNDED_ADD = 102,
  tecPATH_PARTIAL = 101,
  tecCLAIM = 100,
  tesPARTIAL = 1,
  terNO_HOOK = -86,
  terNO_AMM = -87,
  terPRE_TICKET = -88,
  terQUEUED = -89,
  terNO_RIPPLE = -90,
  terLAST = -91,
  terPRE_SEQ = -92,
  terOWNERS = -93,
  terNO_LINE = -94,
  terNO_AUTH = -95,
  terNO_ACCOUNT = -96,
  terINSUF_FEE_B = -97,
  terFUNDS_SPENT = -98,
  terRETRY = -99,
  tefINVALID_LEDGER_FIX_TYPE = -174,
  tefIMPORT_BLACKHOLED = -175,
  tefNONDIR_EMIT = -176,
  tefPAST_IMPORT_VL_SEQ = -177,
  tefPAST_IMPORT_SEQ = -178,
  tefNFTOKEN_IS_NOT_TRANSFERABLE = -179,
  tefNO_TICKET = -180,
  tefTOO_BIG = -181,
  tefINVARIANT_FAILED = -182,
  tefBAD_AUTH_MASTER = -183,
  tefNOT_MULTI_SIGNING = -184,
  tefBAD_QUORUM = -185,
  tefBAD_SIGNATURE = -186,
  tefMAX_LEDGER = -187,
  tefMASTER_DISABLED = -188,
  tefWRONG_PRIOR = -189,
  tefPAST_SEQ = -190,
  tefNO_AUTH_REQUIRED = -191,
  tefINTERNAL = -192,
  tefEXCEPTION = -193,
  tefCREATED = -194,
  tefBAD_LEDGER = -195,
  tefBAD_AUTH = -196,
  tefBAD_ADD_AUTH = -197,
  tefALREADY = -198,
  tefFAILURE = -199,
  temBAD_TRANSFER_FEE = -249,
  temARRAY_TOO_LARGE = -250,
  temARRAY_EMPTY = -251,
  temEMPTY_DID = -252,
  temHOOK_DATA_TOO_LARGE = -253,
  temXCHAIN_TOO_MANY_ATTESTATIONS = -254,
  temXCHAIN_BRIDGE_BAD_REWARD_AMOUNT = -255,
  temXCHAIN_BRIDGE_BAD_MIN_ACCOUNT_CREATE_AMOUNT = -256,
  temXCHAIN_BRIDGE_NONDOOR_OWNER = -257,
  temXCHAIN_BRIDGE_BAD_ISSUES = -258,
  temXCHAIN_BAD_PROOF = -259,
  temXCHAIN_EQUAL_DOOR_ACCOUNTS = -260,
  temBAD_AMM_TOKENS = -261,
  temBAD_NFTOKEN_TRANSFER_FEE = -262,
  temSEQ_AND_TICKET = -263,
  temUNKNOWN = -264,
  temUNCERTAIN = -265,
  temINVALID_COUNT = -266,
  temCANNOT_PREAUTH_SELF = -267,
  temINVALID_ACCOUNT_ID = -268,
  temBAD_TICK_SIZE = -269,
  temBAD_WEIGHT = -270,
  temBAD_QUORUM = -271,
  temBAD_SIGNER = -272,
  temDISABLED = -273,
  temRIPPLE_EMPTY = -274,
  temREDUNDANT = -275,
  temINVALID_FLAG = -276,
  temINVALID = -277,
  temDST_NEEDED = -278,
  temDST_IS_SRC = -279,
  temBAD_TRANSFER_RATE = -280,
  temBAD_SRC_ACCOUNT = -281,
  temBAD_SIGNATURE = -282,
  temBAD_SEQUENCE = -283,
  temBAD_SEND_NATIVE_PATHS = -284,
  temBAD_SEND_NATIVE_PARTIAL = -285,
  temBAD_SEND_NATIVE_NO_DIRECT = -286,
  temBAD_SEND_NATIVE_MAX = -287,
  temBAD_SEND_NATIVE_LIMIT = -288,
  temBAD_REGKEY = -289,
  temBAD_PATH_LOOP = -290,
  temBAD_PATH = -291,
  temBAD_OFFER = -292,
  temBAD_LIMIT = -293,
  temBAD_ISSUER = -294,
  temBAD_FEE = -295,
  temBAD_EXPIRATION = -296,
  temBAD_CURRENCY = -297,
  temBAD_AMOUNT = -298,
  temMALFORMED = -299,
  telENV_RPC_FAILED = -380,
  telCAN_NOT_QUEUE_IMPORT = -381,
  telIMPORT_VL_KEY_NOT_RECOGNISED = -382,
  telNON_LOCAL_EMITTED_TXN = -383,
  telNETWORK_ID_MAKES_TX_NON_CANONICAL = -384,
  telREQUIRES_NETWORK_ID = -385,
  telWRONG_NETWORK = -386,
  telCAN_NOT_QUEUE_FULL = -387,
  telCAN_NOT_QUEUE_FEE = -388,
  telCAN_NOT_QUEUE_BLOCKED = -389,
  telCAN_NOT_QUEUE_BLOCKS = -390,
  telCAN_NOT_QUEUE_BALANCE = -391,
  telCAN_NOT_QUEUE = -392,
  telNO_DST_PARTIAL = -393,
  telINSUF_FEE_P = -394,
  telFAILED_PROCESSING = -395,
  telBAD_PUBLIC_KEY = -396,
  telBAD_PATH_COUNT = -397,
  telBAD_DOMAIN = -398,
  telLOCAL_ERROR = -399,
}

declare const enum HookExecutionMode {
  /** Strong pre-apply execution. */
  Strong = "strong",

  /**
   * Weak post-apply execution. Xahau has no `hefWEAK` symbol: this is the
   * execution state with neither `hefSTRONG` nor `hefCALLBACK` set.
   */
  Weak = "weak",

  /** Emitted-transaction callback execution. */
  Callback = "callback",
}

declare class LedgerKeylet {
  readonly byteLength: 34;
  readonly type: number;
  constructor(value: BytesLike);
  toBytes(): Uint8Array;
  toHex(): HexString;
}

declare namespace otxn {
  function raw(): HostResult<STBlob>;
  function current(): HostResult<STTransaction>;
  function object(): HostResult<STObject>;
  function type(): HostResult<TransactionType>;
  function id(flags?: number): HostResult<STHash<32>>;
  function generation(): HostResult<number>;
  function burden(): HostResult<bigint>;
  function param(name: StateKeyLike): HostResult<STBlob | undefined>;
  function params(names: readonly StateKeyLike[]): HostResult<readonly (STBlob | undefined)[]>;
  function params<const T extends BatchKeys>(names: T): HostResult<BatchValues<T>>;
  function meta(): HostResult<STObject | undefined>;
  function xpop(): HostResult<STXPop | undefined>;
}

declare namespace hook {
  function account(): HostResult<STAddress>;
  function hash(): HostResult<STHash<32>>;
  function position(): HostResult<number>;
  function mode(): HostResult<HookExecutionMode>;
  function hashAt(position: number): HostResult<STHash<32> | undefined>;
  function param(name: StateKeyLike): HostResult<STBlob | undefined>;
  function params(names: readonly StateKeyLike[]): HostResult<readonly (STBlob | undefined)[]>;
  function params<const T extends BatchKeys>(names: T): HostResult<BatchValues<T>>;
  function paramSet(targetHook: STHash<32>, name: StateKeyLike, value: BytesLike): HostResult<void>;
  function skip(targetHook: STHash<32>, remove?: boolean): HostResult<void>;
  function again(): HostResult<void>;
}

declare namespace state {
  interface KeyOptions {
    readonly length?: number;
    readonly pad?: "left" | "right" | "none";
  }

  interface Put {
    readonly key: StateKeyLike;
    readonly value?: StateValueLike;
  }

  interface Accessor {
    get(key: StateKeyLike): HostResult<STBlob | undefined>;
    getMany(keys: readonly StateKeyLike[]): HostResult<readonly (STBlob | undefined)[]>;
    getMany<const T extends BatchKeys>(keys: T): HostResult<BatchValues<T>>;
    set(key: StateKeyLike, value: StateValueLike): HostResult<void>;
    del(key: StateKeyLike): HostResult<void>;
    setMany(items: readonly Put[]): HostResult<void>;
  }

  function key(part: BytePart, options?: KeyOptions): STBlob;
  function key(parts: readonly BytePart[], options?: KeyOptions): STBlob;
  function get(key: StateKeyLike): HostResult<STBlob | undefined>;
  function getMany(keys: readonly StateKeyLike[]): HostResult<readonly (STBlob | undefined)[]>;
  function getMany<const T extends BatchKeys>(keys: T): HostResult<BatchValues<T>>;
  function set(key: StateKeyLike, value: StateValueLike): HostResult<void>;
  function del(key: StateKeyLike): HostResult<void>;
  function setMany(items: readonly Put[]): HostResult<void>;
  function foreign(account: STAddress, namespace: STHash<32>): Accessor;
}

declare namespace slot {
  type SlotValue = STObject | STArray | STFieldValue;
  function fromLedger(keylet: LedgerKeylet): HostResult<STObject | undefined>;
  function meta(): HostResult<STObject | undefined>;
  function xpop(): HostResult<STXPop | undefined>;
  function clear(value: SlotValue): HostResult<void>;
}

declare namespace emit {
  interface EmittedTransaction {
    readonly blob: STBlob;
    readonly kind: TransactionType;
  }

  /** Host stage that failed while finalizing fee/details for an emission. */
  type BuildStage = "details" | "fee";
  type BuildResult =
    | HostSuccess<EmittedTransaction>
    | (HostFailure & { readonly stage: BuildStage });

  interface HookParameter {
    readonly name: StateKeyLike;
    readonly value: StateValueLike;
  }

  /**
   * Selected typed emitted-transaction builders.
   *
   * This is not yet the complete Xahau transaction catalogue. Every omitted
   * transaction type must eventually be projected or carry an explicit
   * unsupported/not-emittable disposition; see the builder coverage gate.
   */
  namespace build {
    interface InvokeOptions {
      readonly destination?: STAddress;
      readonly hookParameters?: readonly HookParameter[];
      readonly blob?: StateValueLike;
    }

    interface HookSetOptions {
      readonly account?: STAddress;
      readonly hooks: readonly {
        readonly position: number;
        readonly hookHash: STHash<32> | null;
      }[];
    }

    interface PaymentOptions {
      readonly destination: STAddress;
      readonly amount: STAmount;
      readonly sourceTag?: UInt32;
      readonly destinationTag?: UInt32;
      readonly flags?: UInt32;
      readonly invoiceId?: STHash<32>;
      readonly sendMax?: STAmount;
      readonly deliverMin?: STAmount;
      readonly hookParameters?: readonly HookParameter[];
    }

    /** Build an OfferCreate for direct DEX placement. */
    interface OfferCreateOptions {
      readonly account?: STAddress;
      readonly takerPays: STAmount;
      readonly takerGets: STAmount;
      readonly expiration?: RippleTime;
      readonly flags?: UInt32;
      readonly hookParameters?: readonly HookParameter[];
    }

    /** Build a TrustSet for trustline limits, qualities, and flags. */
    interface TrustSetOptions {
      readonly account?: STAddress;
      readonly limitAmount?: STAmount;
      readonly qualityIn?: UInt32;
      readonly qualityOut?: UInt32;
      readonly flags?: UInt32;
      readonly hookParameters?: readonly HookParameter[];
    }

    interface RemitOptions {
      readonly destination: STAddress;
      readonly uri?: StateValueLike;
      readonly amounts?: readonly STAmount[];
      readonly sourceTag?: UInt32;
      readonly destinationTag?: UInt32;
      readonly flags?: UInt32;
      readonly hookParameters?: readonly HookParameter[];
    }

    interface ClaimRewardOptions {
      readonly account?: STAddress;
      readonly issuer: STAddress;
      readonly flags?: UInt32;
      readonly hookParameters?: readonly HookParameter[];
    }

    interface SignerEntry {
      readonly account: STAddress;
      readonly weight: UInt16;
    }

    interface SignerListSetOptions {
      readonly account?: STAddress;
      readonly signerQuorum: UInt32;
      readonly signerEntries: readonly SignerEntry[];
      readonly flags?: UInt32;
      readonly hookParameters?: readonly HookParameter[];
    }

    interface URITokenMintOptions {
      readonly account?: STAddress;
      readonly destination?: STAddress;
      readonly uri: StateValueLike;
      readonly amount?: STAmount;
      readonly digest?: STHash<32>;
      readonly flags?: UInt32;
      readonly hookParameters?: readonly HookParameter[];
    }

    interface GenesisMintBaseOptions {
      readonly account?: STAddress;
      readonly flags?: UInt32;
      readonly hookParameters?: readonly HookParameter[];
    }

    interface GenesisMintEntry {
      readonly account: STAddress;
      readonly amount: STNativeAmount | Drops;
    }

    interface GenesisMintRawOptions extends GenesisMintBaseOptions {
      readonly rawMints: StateValueLike;
      readonly mints?: never;
    }

    interface GenesisMintEntriesOptions extends GenesisMintBaseOptions {
      readonly mints: readonly GenesisMintEntry[];
      readonly rawMints?: never;
    }

    type GenesisMintOptions = GenesisMintRawOptions | GenesisMintEntriesOptions;

    /** Final builders acquire host-derived fee/details and identify a failed stage. */
    function invoke(options: InvokeOptions): BuildResult;
    function hookSet(options: HookSetOptions): BuildResult;
    function payment(options: PaymentOptions): BuildResult;
    function offerCreate(options: OfferCreateOptions): BuildResult;
    function trustSet(options: TrustSetOptions): BuildResult;
    function remit(options: RemitOptions): BuildResult;
    function claimReward(options: ClaimRewardOptions): BuildResult;
    function signerListSet(options: SignerListSetOptions): BuildResult;
    function genesisMint(options: GenesisMintOptions): BuildResult;
    function uriTokenMint(options: URITokenMintOptions): BuildResult;
  }

  function reserve(count: number): HostResult<void>;
  function tx(transaction: BytesLike | STBlob | EmittedTransaction): HostResult<STHash<32>>;
  function txMany(transactions: readonly (BytesLike | STBlob | EmittedTransaction)[]): HostResult<readonly STHash<32>[]>;
  function prepare(partial: BytesLike | STBlob | STObject): HostResult<STBlob>;
  function details(): HostResult<STBlob>;
  function feeBase(transaction: BytesLike | STBlob | EmittedTransaction): HostResult<Drops>;
  function nonce(): HostResult<STHash<32>>;
  function generation(): HostResult<number>;
  function burden(): HostResult<bigint>;
}

declare namespace util {
  namespace keylet {
    function account(account: STAddress): LedgerKeylet;
    function hook(account: STAddress): LedgerKeylet;
    function hookDefinition(hash: STHash<32>): LedgerKeylet;
    function hookState(account: STAddress, key: STHash<32>, namespace: STHash<32>): LedgerKeylet;
    function hookStateDir(account: STAddress, namespace: STHash<32>): LedgerKeylet;
    /** Account order is normalized by the host when deriving the trust-line key. */
    function line(accountA: STAddress, accountB: STAddress, currency: STCurrency): LedgerKeylet;
    function ownerDir(account: STAddress): LedgerKeylet;
    function signers(account: STAddress): LedgerKeylet;
    function did(account: STAddress): LedgerKeylet;
    function oracle(account: STAddress, sequence: UInt32): LedgerKeylet;
    function offer(account: STAddress, sequence: UInt32OrHash): LedgerKeylet;
    function check(account: STAddress, sequence: UInt32OrHash): LedgerKeylet;
    function escrow(account: STAddress, sequence: UInt32OrHash): LedgerKeylet;
    function nftOffer(account: STAddress, sequence: UInt32OrHash): LedgerKeylet;
    function cron(account: STAddress, sequence: UInt32): LedgerKeylet;
    function paychan(source: STAddress, destination: STAddress, sequence: UInt32OrHash): LedgerKeylet;
    function depositPreauth(owner: STAddress, authorized: STAddress): LedgerKeylet;
    function child(hash: STHash<32>): LedgerKeylet;
    function emittedTxn(hash: STHash<32>): LedgerKeylet;
    function unchecked(hash: STHash<32>): LedgerKeylet;
    function page(hash: STHash<32>, index: UInt64 | number): LedgerKeylet;
    function quality(directory: LedgerKeylet, quality: UInt64 | number): LedgerKeylet;
    function skip(position?: UInt32): LedgerKeylet;
    function amendments(): LedgerKeylet;
    function fees(): LedgerKeylet;
    function negativeUNL(): LedgerKeylet;
    function emittedDir(): LedgerKeylet;
    function amm(left: STIssue, right: STIssue): LedgerKeylet;
  }

  function sha512h(data: BytesLike | STBlob): STHash<32>;
  function verify(publicKey: BytesLike, signature: BytesLike, message: BytesLike | STBlob): boolean;
  function bytes(...parts: readonly BytePart[]): STBlob;
  function toRAddress(account: STAddress | BytesLike): string;
  function fromRAddress(account: string): STAddress;
  function encodeObject(value: STObject): STBlob;
  function decodeObject(value: BytesLike | STBlob): STObject;
  function validateObject(value: BytesLike | STBlob): boolean;
}

declare namespace float {
  const zero: XFL;
  const one: XFL;
  function set(exponent: number, mantissa: bigint | number): HostResult<XFL>;
  function sum(left: XFL, right: XFL): HostResult<XFL>;
  function multiply(left: XFL, right: XFL): HostResult<XFL>;
  function multiplyRatio(value: XFL, opts: { readonly numerator: number; readonly denominator: number; readonly roundUp?: boolean }): HostResult<XFL>;
  function divide(left: XFL, right: XFL): HostResult<XFL>;
  function negate(value: XFL): HostResult<XFL>;
  function invert(value: XFL): HostResult<XFL>;
  function compare(left: XFL, right: XFL): HostResult<number>;
  function sign(value: XFL): HostResult<-1 | 0 | 1>;
  function mantissa(value: XFL): HostResult<bigint>;
  function log(value: XFL): HostResult<XFL>;
  function root(value: XFL, degree: number): HostResult<XFL>;
  function amount(value: XFL, issue?: STIssue): HostResult<STAmount>;
}

declare namespace ledger {
  const sequence: LedgerSequence;
  const lastTime: RippleTime;
  const lastHash: STHash<32>;
  const feeBase: Drops;
  function nonce(): HostResult<STHash<32>>;
  function accountRoot(account: STAddress): HostResult<STAccountRoot | undefined>;
  function unlReport(): HostResult<STUNLReport | undefined>;
  function lookup(locator: LedgerKeylet | STHash<32>): HostResult<STObject | undefined>;
  function lookupMany(locators: readonly (LedgerKeylet | STHash<32>)[]): HostResult<readonly (STObject | undefined)[]>;
  function nextKeylet(lo: LedgerKeylet, hi: LedgerKeylet): HostResult<LedgerKeylet | undefined>;
}

/**
 * Legacy compatibility with the C Hooks `_g` verifier surface.
 *
 * This is not the target execution meter. Runtime pricing may count cheap
 * interpreter/wasm instructions, complexity-priced host calls, or both; the
 * executor and tariff are still design choices. Ports must not mechanically
 * translate entry `_g(1, 1)` calls or ordinary loop guards into this
 * namespace. Use normal bounded JavaScript control flow, and require every
 * host operation to be bounded and charged by the eventual consensus meter.
 * Keep these helpers only for a deliberate, observable C-guard-parity case or
 * a diagnostic probe.
 *
 * @deprecated Prefer normal JavaScript control flow under runtime metering.
 * Remove this compatibility surface if no explicit guard-parity case remains.
 */
declare namespace guard {
  function hit(id: number, maxIterations: number): number;
  function loop<T>(id: number, maxIterations: number, body: (index: number) => T | void): void;
}

declare namespace lifecycle {
  function account(): HostResult<STAddress>;
  function hash(): HostResult<STHash<32>>;
  function position(): HostResult<number>;
  function mode(): HostResult<HookExecutionMode>;
  function hashAt(position: number): HostResult<STHash<32> | undefined>;
  function param(name: StateKeyLike): HostResult<STBlob | undefined>;
  function params(names: readonly StateKeyLike[]): HostResult<readonly (STBlob | undefined)[]>;
  function params<const T extends BatchKeys>(names: T): HostResult<BatchValues<T>>;
  function paramSet(targetHook: STHash<32>, name: StateKeyLike, value: BytesLike): HostResult<void>;
  function skip(targetHook: STHash<32>, remove?: boolean): HostResult<void>;
  function again(): HostResult<void>;

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
 * A supplied `code` becomes the integer HookReturnCode recorded on-ledger;
 * omit it only when that value is not part of the contract's intended
 * interface. C Hooks commonly pass `__LINE__` as a source-location breadcrumb,
 * but TypeScript translations are not required to preserve that
 * source-language convention. Explicitly meaningful codes remain caller-owned.
 */
declare function accept(message?: string | BytesLike | STBlob, code?: number): never;

/**
 * Reject, atomically roll back, and terminate this hook execution. Like
 * `accept`, the host unwinds the Wasm invocation and this call never returns.
 */
declare function rollback(message?: string | BytesLike | STBlob, code?: number): never;

declare namespace rollback {
  /**
   * Return a successful host value, including a legitimate `undefined`, or
   * atomically roll back with the exact failed host status as the terminal
   * code. Supply `message` when the surrounding contract gives that failure
   * more useful context than the default host-status diagnostic.
   */
  function onHostFailure<T>(result: HostResult<T>, message?: string | BytesLike | STBlob): T;
}

/** Trace a value; callable namespace members provide explicit encodings. */
declare function trace(label: string, value?: unknown): void;

declare namespace trace {
  function hex(label: string, value: BytesLike | STBlob | STHash | STAddress | STCurrency): void;
  function number(label: string, value: number | bigint | XFL): void;
}
/**
 * Serialized types with no declaration of their own, recorded so coverage over
 * definitions.json is total rather than silently partial.
 *
 * @serial-scalar   UInt8 UInt16 UInt32 as number
 * @serial-scalar   UInt64 as bigint
 * @serial-sentinel Done NotPresent Unknown
 * @serial-unmapped UInt96 UInt192 UInt384 UInt512 Vector256 Validation
 * @serial-unmapped Number XChainBridge
 *
 * The unmapped lines are a decision list, not a hole. Nothing on the first is
 * exotic — Vector256 is structurally an array of 32-byte hashes, and it covers
 * seven real fields (Indexes, Hashes, Amendments, NFTokenOffers,
 * HookNamespaces, CredentialIDs, URITokenIDs). The open question is what a type
 * should *prevent*: STHash<32>[] reads fine and lets you append a currency code
 * to it; an STVector256 does not. Declared unmapped rather than quietly absent,
 * so the choice stays visible.
 *
 * The second line arrived with the 2026-07-26 definitions refresh, which took
 * the protocol from 260 to 337 fields in total — 325 of them serialized, which
 * is what the Field table below declares. Currency, Issue and XChainBridge are
 * structured rather than scalar, so they are the ones likely to want real
 * declarations; Hash192/384/512 are the existing generic STHash at other
 * widths. They are listed to keep coverage total — that the refresh surfaced
 * them automatically is the point of parsing this file rather than keeping the
 * mapping in the checker.
 */

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


/**
 * Every serialized protocol field, by name.
 *
 * `code` is what a host function expects: (typeCode << 16) | fieldCode,
 * the same encoding as sfAccount in the C headers. Plain data, not a
 * rich type — a contract that needs to name a field it has no typed
 * accessor for can reach for this.
 */
declare const Field: {
  readonly LedgerEntryType: SerializedField<UInt16, 65537, 1, 1>;
  readonly TransactionType: SerializedField<TransactionType, 65538, 1, 2>;
  readonly SignerWeight: SerializedField<UInt16, 65539, 1, 3>;
  readonly TransferFee: SerializedField<UInt16, 65540, 1, 4>;
  readonly Version: SerializedField<UInt16, 65552, 1, 16>;
  readonly HookStateChangeCount: SerializedField<UInt16, 65553, 1, 17>;
  readonly HookEmitCount: SerializedField<UInt16, 65554, 1, 18>;
  readonly HookExecutionIndex: SerializedField<UInt16, 65555, 1, 19>;
  readonly HookApiVersion: SerializedField<UInt16, 65556, 1, 20>;
  readonly HookStateScale: SerializedField<UInt16, 65557, 1, 21>;
  readonly NetworkID: SerializedField<UInt32, 131073, 2, 1>;
  readonly Flags: SerializedField<UInt32, 131074, 2, 2>;
  readonly SourceTag: SerializedField<UInt32, 131075, 2, 3>;
  readonly Sequence: SerializedField<UInt32, 131076, 2, 4>;
  readonly PreviousTxnLgrSeq: SerializedField<UInt32, 131077, 2, 5>;
  readonly LedgerSequence: SerializedField<UInt32, 131078, 2, 6>;
  readonly CloseTime: SerializedField<UInt32, 131079, 2, 7>;
  readonly ParentCloseTime: SerializedField<UInt32, 131080, 2, 8>;
  readonly SigningTime: SerializedField<UInt32, 131081, 2, 9>;
  readonly Expiration: SerializedField<UInt32, 131082, 2, 10>;
  readonly TransferRate: SerializedField<UInt32, 131083, 2, 11>;
  readonly WalletSize: SerializedField<UInt32, 131084, 2, 12>;
  readonly OwnerCount: SerializedField<UInt32, 131085, 2, 13>;
  readonly DestinationTag: SerializedField<UInt32, 131086, 2, 14>;
  readonly HighQualityIn: SerializedField<UInt32, 131088, 2, 16>;
  readonly HighQualityOut: SerializedField<UInt32, 131089, 2, 17>;
  readonly LowQualityIn: SerializedField<UInt32, 131090, 2, 18>;
  readonly LowQualityOut: SerializedField<UInt32, 131091, 2, 19>;
  readonly QualityIn: SerializedField<UInt32, 131092, 2, 20>;
  readonly QualityOut: SerializedField<UInt32, 131093, 2, 21>;
  readonly StampEscrow: SerializedField<UInt32, 131094, 2, 22>;
  readonly BondAmount: SerializedField<UInt32, 131095, 2, 23>;
  readonly LoadFee: SerializedField<UInt32, 131096, 2, 24>;
  readonly OfferSequence: SerializedField<UInt32, 131097, 2, 25>;
  readonly FirstLedgerSequence: SerializedField<UInt32, 131098, 2, 26>;
  readonly LastLedgerSequence: SerializedField<UInt32, 131099, 2, 27>;
  readonly TransactionIndex: SerializedField<UInt32, 131100, 2, 28>;
  readonly OperationLimit: SerializedField<UInt32, 131101, 2, 29>;
  readonly ReferenceFeeUnits: SerializedField<UInt32, 131102, 2, 30>;
  readonly ReserveBase: SerializedField<UInt32, 131103, 2, 31>;
  readonly ReserveIncrement: SerializedField<UInt32, 131104, 2, 32>;
  readonly SetFlag: SerializedField<UInt32, 131105, 2, 33>;
  readonly ClearFlag: SerializedField<UInt32, 131106, 2, 34>;
  readonly SignerQuorum: SerializedField<UInt32, 131107, 2, 35>;
  readonly CancelAfter: SerializedField<UInt32, 131108, 2, 36>;
  readonly FinishAfter: SerializedField<UInt32, 131109, 2, 37>;
  readonly SignerListID: SerializedField<UInt32, 131110, 2, 38>;
  readonly SettleDelay: SerializedField<UInt32, 131111, 2, 39>;
  readonly TicketCount: SerializedField<UInt32, 131112, 2, 40>;
  readonly TicketSequence: SerializedField<UInt32, 131113, 2, 41>;
  readonly NFTokenTaxon: SerializedField<UInt32, 131114, 2, 42>;
  readonly MintedNFTokens: SerializedField<UInt32, 131115, 2, 43>;
  readonly BurnedNFTokens: SerializedField<UInt32, 131116, 2, 44>;
  readonly HookStateCount: SerializedField<UInt32, 131117, 2, 45>;
  readonly EmitGeneration: SerializedField<UInt32, 131118, 2, 46>;
  readonly LockCount: SerializedField<UInt32, 131121, 2, 49>;
  readonly FirstNFTokenSequence: SerializedField<UInt32, 131122, 2, 50>;
  readonly StartTime: SerializedField<UInt32, 131165, 2, 93>;
  readonly RepeatCount: SerializedField<UInt32, 131166, 2, 94>;
  readonly DelaySeconds: SerializedField<UInt32, 131167, 2, 95>;
  readonly XahauActivationLgrSeq: SerializedField<UInt32, 131168, 2, 96>;
  readonly ImportSequence: SerializedField<UInt32, 131169, 2, 97>;
  readonly RewardTime: SerializedField<UInt32, 131170, 2, 98>;
  readonly RewardLgrFirst: SerializedField<UInt32, 131171, 2, 99>;
  readonly RewardLgrLast: SerializedField<UInt32, 131172, 2, 100>;
  readonly IndexNext: SerializedField<UInt64, 196609, 3, 1>;
  readonly IndexPrevious: SerializedField<UInt64, 196610, 3, 2>;
  readonly BookNode: SerializedField<UInt64, 196611, 3, 3>;
  readonly OwnerNode: SerializedField<UInt64, 196612, 3, 4>;
  readonly BaseFee: SerializedField<UInt64, 196613, 3, 5>;
  readonly ExchangeRate: SerializedField<UInt64, 196614, 3, 6>;
  readonly LowNode: SerializedField<UInt64, 196615, 3, 7>;
  readonly HighNode: SerializedField<UInt64, 196616, 3, 8>;
  readonly DestinationNode: SerializedField<UInt64, 196617, 3, 9>;
  readonly Cookie: SerializedField<UInt64, 196618, 3, 10>;
  readonly ServerVersion: SerializedField<UInt64, 196619, 3, 11>;
  readonly NFTokenOfferNode: SerializedField<UInt64, 196620, 3, 12>;
  readonly EmitBurden: SerializedField<UInt64, 196621, 3, 13>;
  readonly HookInstructionCount: SerializedField<UInt64, 196625, 3, 17>;
  readonly HookReturnCode: SerializedField<UInt64, 196626, 3, 18>;
  readonly ReferenceCount: SerializedField<UInt64, 196627, 3, 19>;
  readonly TouchCount: SerializedField<UInt64, 196705, 3, 97>;
  readonly AccountIndex: SerializedField<UInt64, 196706, 3, 98>;
  readonly AccountCount: SerializedField<UInt64, 196707, 3, 99>;
  readonly RewardAccumulator: SerializedField<UInt64, 196708, 3, 100>;
  readonly EmailHash: SerializedField<STHash<16>, 262145, 4, 1>;
  readonly LedgerHash: SerializedField<STHash<32>, 327681, 5, 1>;
  readonly ParentHash: SerializedField<STHash<32>, 327682, 5, 2>;
  readonly TransactionHash: SerializedField<STHash<32>, 327683, 5, 3>;
  readonly AccountHash: SerializedField<STHash<32>, 327684, 5, 4>;
  readonly PreviousTxnID: SerializedField<STHash<32>, 327685, 5, 5>;
  readonly LedgerIndex: SerializedField<STHash<32>, 327686, 5, 6>;
  readonly WalletLocator: SerializedField<STHash<32>, 327687, 5, 7>;
  readonly RootIndex: SerializedField<STHash<32>, 327688, 5, 8>;
  readonly AccountTxnID: SerializedField<STHash<32>, 327689, 5, 9>;
  readonly NFTokenID: SerializedField<STHash<32>, 327690, 5, 10>;
  readonly EmitParentTxnID: SerializedField<STHash<32>, 327691, 5, 11>;
  readonly EmitNonce: SerializedField<STHash<32>, 327692, 5, 12>;
  readonly EmitHookHash: SerializedField<STHash<32>, 327693, 5, 13>;
  readonly ObjectID: SerializedField<STHash<32>, 327694, 5, 14>;
  readonly BookDirectory: SerializedField<STHash<32>, 327696, 5, 16>;
  readonly InvoiceID: SerializedField<STHash<32>, 327697, 5, 17>;
  readonly Nickname: SerializedField<STHash<32>, 327698, 5, 18>;
  readonly Amendment: SerializedField<STHash<32>, 327699, 5, 19>;
  readonly HookOn: SerializedField<STHash<32>, 327700, 5, 20>;
  readonly Digest: SerializedField<STHash<32>, 327701, 5, 21>;
  readonly Channel: SerializedField<STHash<32>, 327702, 5, 22>;
  readonly ConsensusHash: SerializedField<STHash<32>, 327703, 5, 23>;
  readonly CheckID: SerializedField<STHash<32>, 327704, 5, 24>;
  readonly ValidatedHash: SerializedField<STHash<32>, 327705, 5, 25>;
  readonly PreviousPageMin: SerializedField<STHash<32>, 327706, 5, 26>;
  readonly NextPageMin: SerializedField<STHash<32>, 327707, 5, 27>;
  readonly NFTokenBuyOffer: SerializedField<STHash<32>, 327708, 5, 28>;
  readonly NFTokenSellOffer: SerializedField<STHash<32>, 327709, 5, 29>;
  readonly HookStateKey: SerializedField<STHash<32>, 327710, 5, 30>;
  readonly HookHash: SerializedField<STHash<32>, 327711, 5, 31>;
  readonly HookNamespace: SerializedField<STHash<32>, 327712, 5, 32>;
  readonly HookSetTxnID: SerializedField<STHash<32>, 327713, 5, 33>;
  readonly OfferID: SerializedField<STHash<32>, 327714, 5, 34>;
  readonly EscrowID: SerializedField<STHash<32>, 327715, 5, 35>;
  readonly URITokenID: SerializedField<STHash<32>, 327716, 5, 36>;
  readonly Cron: SerializedField<STHash<32>, 327775, 5, 95>;
  readonly HookCanEmit: SerializedField<STHash<32>, 327776, 5, 96>;
  readonly EmittedTxnID: SerializedField<STHash<32>, 327777, 5, 97>;
  readonly GovernanceMarks: SerializedField<STHash<32>, 327778, 5, 98>;
  readonly GovernanceFlags: SerializedField<STHash<32>, 327779, 5, 99>;
  readonly Amount: SerializedField<STAmount, 393217, 6, 1>;
  readonly Balance: SerializedField<STAmount, 393218, 6, 2>;
  readonly LimitAmount: SerializedField<STAmount, 393219, 6, 3>;
  readonly TakerPays: SerializedField<STAmount, 393220, 6, 4>;
  readonly TakerGets: SerializedField<STAmount, 393221, 6, 5>;
  readonly LowLimit: SerializedField<STAmount, 393222, 6, 6>;
  readonly HighLimit: SerializedField<STAmount, 393223, 6, 7>;
  readonly Fee: SerializedField<STAmount, 393224, 6, 8>;
  readonly SendMax: SerializedField<STAmount, 393225, 6, 9>;
  readonly DeliverMin: SerializedField<STAmount, 393226, 6, 10>;
  readonly MinimumOffer: SerializedField<STAmount, 393232, 6, 16>;
  readonly RippleEscrow: SerializedField<STAmount, 393233, 6, 17>;
  readonly DeliveredAmount: SerializedField<STAmount, 393234, 6, 18>;
  readonly NFTokenBrokerFee: SerializedField<STAmount, 393235, 6, 19>;
  readonly HookCallbackFee: SerializedField<STAmount, 393236, 6, 20>;
  readonly LockedBalance: SerializedField<STAmount, 393237, 6, 21>;
  readonly BaseFeeDrops: SerializedField<STAmount, 393238, 6, 22>;
  readonly ReserveBaseDrops: SerializedField<STAmount, 393239, 6, 23>;
  readonly ReserveIncrementDrops: SerializedField<STAmount, 393240, 6, 24>;
  readonly PublicKey: SerializedField<STBlob, 458753, 7, 1>;
  readonly MessageKey: SerializedField<STBlob, 458754, 7, 2>;
  readonly SigningPubKey: SerializedField<STBlob, 458755, 7, 3>;
  readonly TxnSignature: SerializedField<STBlob, 458756, 7, 4>;
  readonly URI: SerializedField<STBlob, 458757, 7, 5>;
  readonly Signature: SerializedField<STBlob, 458758, 7, 6>;
  readonly Domain: SerializedField<STBlob, 458759, 7, 7>;
  readonly FundCode: SerializedField<STBlob, 458760, 7, 8>;
  readonly RemoveCode: SerializedField<STBlob, 458761, 7, 9>;
  readonly ExpireCode: SerializedField<STBlob, 458762, 7, 10>;
  readonly CreateCode: SerializedField<STBlob, 458763, 7, 11>;
  readonly MemoType: SerializedField<STBlob, 458764, 7, 12>;
  readonly MemoData: SerializedField<STBlob, 458765, 7, 13>;
  readonly MemoFormat: SerializedField<STBlob, 458766, 7, 14>;
  readonly Fulfillment: SerializedField<STBlob, 458768, 7, 16>;
  readonly Condition: SerializedField<STBlob, 458769, 7, 17>;
  readonly MasterSignature: SerializedField<STBlob, 458770, 7, 18>;
  readonly UNLModifyValidator: SerializedField<STBlob, 458771, 7, 19>;
  readonly ValidatorToDisable: SerializedField<STBlob, 458772, 7, 20>;
  readonly ValidatorToReEnable: SerializedField<STBlob, 458773, 7, 21>;
  readonly HookStateData: SerializedField<STBlob, 458774, 7, 22>;
  readonly HookReturnString: SerializedField<STBlob, 458775, 7, 23>;
  readonly HookParameterName: SerializedField<STBlob, 458776, 7, 24>;
  readonly HookParameterValue: SerializedField<STBlob, 458777, 7, 25>;
  readonly Blob: SerializedField<STBlob, 458778, 7, 26>;
  readonly RemarkValue: SerializedField<STBlob, 458850, 7, 98>;
  readonly RemarkName: SerializedField<STBlob, 458851, 7, 99>;
  readonly Account: SerializedField<STAddress, 524289, 8, 1>;
  readonly Owner: SerializedField<STAddress, 524290, 8, 2>;
  readonly Destination: SerializedField<STAddress, 524291, 8, 3>;
  readonly Issuer: SerializedField<STAddress, 524292, 8, 4>;
  readonly Authorize: SerializedField<STAddress, 524293, 8, 5>;
  readonly Unauthorize: SerializedField<STAddress, 524294, 8, 6>;
  readonly RegularKey: SerializedField<STAddress, 524296, 8, 8>;
  readonly NFTokenMinter: SerializedField<STAddress, 524297, 8, 9>;
  readonly EmitCallback: SerializedField<STAddress, 524298, 8, 10>;
  readonly HookAccount: SerializedField<STAddress, 524304, 8, 16>;
  readonly Inform: SerializedField<STAddress, 524387, 8, 99>;
  readonly TransactionMetaData: SerializedField<STObject, 917506, 14, 2>;
  readonly CreatedNode: SerializedField<STObject, 917507, 14, 3>;
  readonly DeletedNode: SerializedField<STObject, 917508, 14, 4>;
  readonly ModifiedNode: SerializedField<STObject, 917509, 14, 5>;
  readonly PreviousFields: SerializedField<STObject, 917510, 14, 6>;
  readonly FinalFields: SerializedField<STObject, 917511, 14, 7>;
  readonly NewFields: SerializedField<STObject, 917512, 14, 8>;
  readonly TemplateEntry: SerializedField<STObject, 917513, 14, 9>;
  readonly Memo: SerializedField<STObject, 917514, 14, 10>;
  readonly SignerEntry: SerializedField<STObject, 917515, 14, 11>;
  readonly NFToken: SerializedField<STObject, 917516, 14, 12>;
  readonly EmitDetails: SerializedField<STObject, 917517, 14, 13>;
  readonly Hook: SerializedField<STObject, 917518, 14, 14>;
  readonly Signer: SerializedField<STObject, 917520, 14, 16>;
  readonly Majority: SerializedField<STObject, 917522, 14, 18>;
  readonly DisabledValidator: SerializedField<STObject, 917523, 14, 19>;
  readonly EmittedTxn: SerializedField<STObject, 917524, 14, 20>;
  readonly HookExecution: SerializedField<STObject, 917525, 14, 21>;
  readonly HookParameter: SerializedField<STObject, 917527, 14, 23>;
  readonly HookGrant: SerializedField<STObject, 917528, 14, 24>;
  readonly AmountEntry: SerializedField<STObject, 917595, 14, 91>;
  readonly MintURIToken: SerializedField<STObject, 917596, 14, 92>;
  readonly HookEmission: SerializedField<STObject, 917597, 14, 93>;
  readonly ImportVLKey: SerializedField<STObject, 917598, 14, 94>;
  readonly ActiveValidator: SerializedField<STObject, 917599, 14, 95>;
  readonly GenesisMint: SerializedField<STObject, 917600, 14, 96>;
  readonly Remark: SerializedField<STObject, 917601, 14, 97>;
  readonly Signers: SerializedField<STArray, 983043, 15, 3>;
  readonly SignerEntries: SerializedField<STArray, 983044, 15, 4>;
  readonly Template: SerializedField<STArray, 983045, 15, 5>;
  readonly Necessary: SerializedField<STArray, 983046, 15, 6>;
  readonly Sufficient: SerializedField<STArray, 983047, 15, 7>;
  readonly AffectedNodes: SerializedField<STArray, 983048, 15, 8>;
  readonly Memos: SerializedField<STArray, 983049, 15, 9>;
  readonly NFTokens: SerializedField<STArray, 983050, 15, 10>;
  readonly Hooks: SerializedField<STArray, 983051, 15, 11>;
  readonly Majorities: SerializedField<STArray, 983056, 15, 16>;
  readonly DisabledValidators: SerializedField<STArray, 983057, 15, 17>;
  readonly HookExecutions: SerializedField<STArray, 983058, 15, 18>;
  readonly HookParameters: SerializedField<STArray, 983059, 15, 19>;
  readonly HookGrants: SerializedField<STArray, 983060, 15, 20>;
  readonly Amounts: SerializedField<STArray, 983132, 15, 92>;
  readonly HookEmissions: SerializedField<STArray, 983133, 15, 93>;
  readonly ImportVLKeys: SerializedField<STArray, 983134, 15, 94>;
  readonly ActiveValidators: SerializedField<STArray, 983135, 15, 95>;
  readonly GenesisMints: SerializedField<STArray, 983136, 15, 96>;
  readonly Remarks: SerializedField<STArray, 983137, 15, 97>;
  readonly CloseResolution: SerializedField<UInt8, 1048577, 16, 1>;
  readonly Method: SerializedField<UInt8, 1048578, 16, 2>;
  readonly TransactionResult: SerializedField<TransactionResult, 1048579, 16, 3>;
  readonly TickSize: SerializedField<UInt8, 1048592, 16, 16>;
  readonly UNLModifyDisabling: SerializedField<UInt8, 1048593, 16, 17>;
  readonly HookResult: SerializedField<UInt8, 1048594, 16, 18>;
  readonly TakerPaysCurrency: SerializedField<STHash<20>, 1114113, 17, 1>;
  readonly TakerPaysIssuer: SerializedField<STHash<20>, 1114114, 17, 2>;
  readonly TakerGetsCurrency: SerializedField<STHash<20>, 1114115, 17, 3>;
  readonly TakerGetsIssuer: SerializedField<STHash<20>, 1114116, 17, 4>;
  readonly Paths: SerializedField<STPathSet, 1179649, 18, 1>;
  readonly Indexes: SerializedField<unknown, 1245185, 19, 1>;
  readonly Hashes: SerializedField<unknown, 1245186, 19, 2>;
  readonly Amendments: SerializedField<unknown, 1245187, 19, 3>;
  readonly NFTokenOffers: SerializedField<unknown, 1245188, 19, 4>;
  readonly HookNamespaces: SerializedField<unknown, 1245189, 19, 5>;
  readonly URITokenIDs: SerializedField<unknown, 1245283, 19, 99>;
  readonly TradingFee: SerializedField<UInt16, 65541, 1, 5>;
  readonly DiscountedFee: SerializedField<UInt16, 65542, 1, 6>;
  readonly LedgerFixType: SerializedField<UInt16, 65558, 1, 22>;
  readonly LastUpdateTime: SerializedField<UInt32, 131087, 2, 15>;
  readonly VoteWeight: SerializedField<UInt32, 131120, 2, 48>;
  readonly OracleDocumentID: SerializedField<UInt32, 131123, 2, 51>;
  readonly XChainClaimID: SerializedField<UInt64, 196628, 3, 20>;
  readonly XChainAccountCreateCount: SerializedField<UInt64, 196629, 3, 21>;
  readonly XChainAccountClaimCount: SerializedField<UInt64, 196630, 3, 22>;
  readonly AssetPrice: SerializedField<UInt64, 196631, 3, 23>;
  readonly MaximumAmount: SerializedField<UInt64, 196632, 3, 24>;
  readonly OutstandingAmount: SerializedField<UInt64, 196633, 3, 25>;
  readonly MPTAmount: SerializedField<UInt64, 196634, 3, 26>;
  readonly IssuerNode: SerializedField<UInt64, 196635, 3, 27>;
  readonly SubjectNode: SerializedField<UInt64, 196636, 3, 28>;
  readonly AMMID: SerializedField<STHash<32>, 327695, 5, 15>;
  readonly DomainID: SerializedField<STHash<32>, 327717, 5, 37>;
  readonly HookOnOutgoing: SerializedField<STHash<32>, 327773, 5, 93>;
  readonly HookOnIncoming: SerializedField<STHash<32>, 327774, 5, 94>;
  readonly Amount2: SerializedField<STAmount, 393227, 6, 11>;
  readonly BidMin: SerializedField<STAmount, 393228, 6, 12>;
  readonly BidMax: SerializedField<STAmount, 393229, 6, 13>;
  readonly LPTokenOut: SerializedField<STAmount, 393241, 6, 25>;
  readonly LPTokenIn: SerializedField<STAmount, 393242, 6, 26>;
  readonly EPrice: SerializedField<STAmount, 393243, 6, 27>;
  readonly Price: SerializedField<STAmount, 393244, 6, 28>;
  readonly SignatureReward: SerializedField<STAmount, 393245, 6, 29>;
  readonly MinAccountCreateAmount: SerializedField<STAmount, 393246, 6, 30>;
  readonly LPTokenBalance: SerializedField<STAmount, 393247, 6, 31>;
  readonly TrustLineRewardAccumulator: SerializedField<STAmount, 393315, 6, 99>;
  readonly DIDDocument: SerializedField<STBlob, 458779, 7, 27>;
  readonly Data: SerializedField<STBlob, 458780, 7, 28>;
  readonly AssetClass: SerializedField<STBlob, 458781, 7, 29>;
  readonly Provider: SerializedField<STBlob, 458782, 7, 30>;
  readonly MPTokenMetadata: SerializedField<STBlob, 458783, 7, 31>;
  readonly CredentialType: SerializedField<STBlob, 458784, 7, 32>;
  readonly HookName: SerializedField<STBlob, 458849, 7, 97>;
  readonly Holder: SerializedField<STAddress, 524299, 8, 11>;
  readonly OtherChainSource: SerializedField<STAddress, 524306, 8, 18>;
  readonly OtherChainDestination: SerializedField<STAddress, 524307, 8, 19>;
  readonly AttestationSignerAccount: SerializedField<STAddress, 524308, 8, 20>;
  readonly AttestationRewardAccount: SerializedField<STAddress, 524309, 8, 21>;
  readonly LockingChainDoor: SerializedField<STAddress, 524310, 8, 22>;
  readonly IssuingChainDoor: SerializedField<STAddress, 524311, 8, 23>;
  readonly Subject: SerializedField<STAddress, 524312, 8, 24>;
  readonly Number: SerializedField<unknown, 589825, 9, 1>;
  readonly VoteEntry: SerializedField<STObject, 917529, 14, 25>;
  readonly AuctionSlot: SerializedField<STObject, 917530, 14, 26>;
  readonly AuthAccount: SerializedField<STObject, 917531, 14, 27>;
  readonly XChainClaimProofSig: SerializedField<STObject, 917532, 14, 28>;
  readonly XChainCreateAccountProofSig: SerializedField<STObject, 917533, 14, 29>;
  readonly XChainClaimAttestationCollectionElement: SerializedField<STObject, 917534, 14, 30>;
  readonly XChainCreateAccountAttestationCollectionElement: SerializedField<STObject, 917535, 14, 31>;
  readonly PriceData: SerializedField<STObject, 917536, 14, 32>;
  readonly Credential: SerializedField<STObject, 917537, 14, 33>;
  readonly HighReward: SerializedField<STObject, 917602, 14, 98>;
  readonly LowReward: SerializedField<STObject, 917603, 14, 99>;
  readonly VoteSlots: SerializedField<STArray, 983052, 15, 12>;
  readonly XChainClaimAttestations: SerializedField<STArray, 983061, 15, 21>;
  readonly XChainCreateAccountAttestations: SerializedField<STArray, 983062, 15, 22>;
  readonly PriceDataSeries: SerializedField<STArray, 983064, 15, 24>;
  readonly AuthAccounts: SerializedField<STArray, 983065, 15, 25>;
  readonly AuthorizeCredentials: SerializedField<STArray, 983066, 15, 26>;
  readonly UnauthorizeCredentials: SerializedField<STArray, 983067, 15, 27>;
  readonly AcceptedCredentials: SerializedField<STArray, 983068, 15, 28>;
  readonly Scale: SerializedField<UInt8, 1048580, 16, 4>;
  readonly AssetScale: SerializedField<UInt8, 1048581, 16, 5>;
  readonly WasLockingChainSend: SerializedField<UInt8, 1048595, 16, 19>;
  readonly CredentialIDs: SerializedField<unknown, 1245190, 19, 6>;
  readonly MPTokenIssuanceID: SerializedField<STHash<24>, 1376257, 21, 1>;
  readonly LockingChainIssue: SerializedField<STIssue, 1572865, 24, 1>;
  readonly IssuingChainIssue: SerializedField<STIssue, 1572866, 24, 2>;
  readonly Asset: SerializedField<STIssue, 1572867, 24, 3>;
  readonly Asset2: SerializedField<STIssue, 1572868, 24, 4>;
  readonly ClaimCurrency: SerializedField<STIssue, 1572869, 24, 5>;
  readonly XChainBridge: SerializedField<unknown, 1638401, 25, 1>;
  readonly BaseAsset: SerializedField<STCurrency, 1703937, 26, 1>;
  readonly QuoteAsset: SerializedField<STCurrency, 1703938, 26, 2>;
};

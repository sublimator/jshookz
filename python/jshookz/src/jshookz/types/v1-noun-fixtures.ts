/**
 * v1 runtime-noun conformance fixtures (0088 third turn, 2026-08-24).
 *
 * Compiled by tsconfig.v1-root.json with the exact v1 declaration as the
 * only other root, so these prove the SHIPPED surface: every selected
 * noun is a runtime value — `instanceof` classifies and narrows. A-prime
 * classifier nouns expose no prototype; the serialized-object classes expose
 * their real prototype hierarchy but keep `new` inaccessible.
 */
export {};

declare const unknownValue: unknown;

// ---- instanceof narrows every published v1 noun ----

if (unknownValue instanceof AccountID) { const v: AccountID = unknownValue; void v; }
if (unknownValue instanceof Hash) { const v: Hash = unknownValue; void v; }
if (unknownValue instanceof Hash128) { const v: Hash128 = unknownValue; void v; }
if (unknownValue instanceof Hash160) { const v: Hash160 = unknownValue; void v; }
if (unknownValue instanceof Hash192) { const v: Hash192 = unknownValue; void v; }
if (unknownValue instanceof Hash256) { const v: Hash256 = unknownValue; void v; }
if (unknownValue instanceof STBlob) { const v: STBlob = unknownValue; void v; }
if (unknownValue instanceof UInt) { const v: UInt = unknownValue; void v; }
if (unknownValue instanceof UInt8) { const v: UInt8 = unknownValue; void v; }
if (unknownValue instanceof UInt16) { const v: UInt16 = unknownValue; void v; }
if (unknownValue instanceof UInt32) { const v: UInt32 = unknownValue; void v; }
if (unknownValue instanceof UInt64) { const v: UInt64 = unknownValue; void v; }
if (unknownValue instanceof Amount) { const v: Amount = unknownValue; void v; }
if (unknownValue instanceof Currency) { const v: Currency = unknownValue; void v; }
if (unknownValue instanceof IOUAmount) { const v: IOUAmount = unknownValue; void v; }
if (unknownValue instanceof Issue) { const v: Issue = unknownValue; void v; }
if (unknownValue instanceof MPTAmount) { const v: MPTAmount = unknownValue; void v; }
if (unknownValue instanceof NativeAmount) { const v: NativeAmount = unknownValue; void v; }
if (unknownValue instanceof Path) { const v: Path = unknownValue; void v; }
if (unknownValue instanceof PathHop) { const v: PathHop = unknownValue; void v; }
if (unknownValue instanceof PathSet) { const v: PathSet = unknownValue; void v; }
if (unknownValue instanceof Result) { const v: Result<unknown, unknown> = unknownValue; void v; }
if (unknownValue instanceof VoidResult) { const v: VoidResult<unknown> = unknownValue; void v; }
if (unknownValue instanceof STArray) { const v: STArray<STObject> = unknownValue; void v; }
if (unknownValue instanceof STObject) { const v: STObject = unknownValue; void v; }
if (unknownValue instanceof Transaction) { const v: Transaction = unknownValue; void v; }
if (unknownValue instanceof Payment) { const v: Payment = unknownValue; void v; }
if (unknownValue instanceof LedgerEntry) { const v: LedgerEntry = unknownValue; void v; }
if (unknownValue instanceof AccountRoot) { const v: AccountRoot = unknownValue; void v; }
if (unknownValue instanceof URIToken) { const v: URIToken = unknownValue; void v; }
if (unknownValue instanceof SerializedField) {
  const v: SerializedField<unknown, number, number, number> = unknownValue; void v;
}
if (unknownValue instanceof Vector256) { const v: Vector256 = unknownValue; void v; }
if (unknownValue instanceof XChainBridge) { const v: XChainBridge = unknownValue; void v; }
if (unknownValue instanceof XFLDecimal) { const v: XFLDecimal = unknownValue; void v; }

// ---- `new` stays red on every published v1 noun ----

// @ts-expect-error the noun is not constructible
const c01 = new AccountID(); void c01;
// @ts-expect-error the noun is not constructible
const c02 = new Hash(); void c02;
// @ts-expect-error the noun is not constructible
const c03 = new Hash128(); void c03;
// @ts-expect-error the noun is not constructible
const c04 = new Hash160(); void c04;
// @ts-expect-error the noun is not constructible
const c05 = new Hash192(); void c05;
// @ts-expect-error the noun is not constructible
const c06 = new Hash256(); void c06;
// @ts-expect-error the noun is not constructible
const c07 = new STBlob(); void c07;
// @ts-expect-error the noun is not constructible
const c08 = new UInt(); void c08;
// @ts-expect-error the noun is not constructible
const c09 = new UInt8(); void c09;
// @ts-expect-error the noun is not constructible
const c10 = new UInt16(); void c10;
// @ts-expect-error the noun is not constructible
const c11 = new UInt32(); void c11;
// @ts-expect-error the noun is not constructible
const c12 = new UInt64(); void c12;
// @ts-expect-error the noun is not constructible
const c13 = new Amount(); void c13;
// @ts-expect-error the noun is not constructible
const c14 = new Currency(); void c14;
// @ts-expect-error the noun is not constructible
const c15 = new IOUAmount(); void c15;
// @ts-expect-error the noun is not constructible
const c16 = new Issue(); void c16;
// @ts-expect-error the noun is not constructible
const c17 = new MPTAmount(); void c17;
// @ts-expect-error the noun is not constructible
const c18 = new NativeAmount(); void c18;
// @ts-expect-error the noun is not constructible
const c19 = new Path(); void c19;
// @ts-expect-error the noun is not constructible
const c20 = new PathHop(); void c20;
// @ts-expect-error the noun is not constructible
const c21 = new PathSet(); void c21;
// @ts-expect-error the noun is not constructible
const c22 = new Result(); void c22;
// @ts-expect-error the noun is not constructible
const c23 = new VoidResult(); void c23;
// @ts-expect-error the noun is not constructible
const c24 = new STArray(); void c24;
// @ts-expect-error the noun is not constructible
const c25 = new STObject(); void c25;
// @ts-expect-error the noun is not constructible
const c26 = new SerializedField(); void c26;
// @ts-expect-error the noun is not constructible
const c27 = new Vector256(); void c27;
// @ts-expect-error the noun is not constructible
const c28 = new XChainBridge(); void c28;
// @ts-expect-error the noun is not constructible
const c29 = new XFLDecimal(); void c29;
// @ts-expect-error protected provider-only constructor
const c30 = new Transaction(); void c30;
// @ts-expect-error private provider-only constructor
const c31 = new Payment(); void c31;
// @ts-expect-error protected provider-only constructor
const c32 = new LedgerEntry(); void c32;
// @ts-expect-error private provider-only constructor
const c33 = new AccountRoot(); void c33;
// @ts-expect-error private provider-only constructor
const c34 = new URIToken(); void c34;

// ---- only the real serialized-object classes expose `.prototype` ----

// @ts-expect-error no prototype is promised
const p01 = AccountID.prototype; void p01;
// @ts-expect-error no prototype is promised
const p02 = Hash.prototype; void p02;
// @ts-expect-error no prototype is promised
const p03 = Hash128.prototype; void p03;
// @ts-expect-error no prototype is promised
const p04 = Hash160.prototype; void p04;
// @ts-expect-error no prototype is promised
const p05 = Hash192.prototype; void p05;
// @ts-expect-error no prototype is promised
const p06 = Hash256.prototype; void p06;
// @ts-expect-error no prototype is promised
const p07 = STBlob.prototype; void p07;
// @ts-expect-error no prototype is promised
const p08 = UInt.prototype; void p08;
// @ts-expect-error no prototype is promised
const p09 = UInt8.prototype; void p09;
// @ts-expect-error no prototype is promised
const p10 = UInt16.prototype; void p10;
// @ts-expect-error no prototype is promised
const p11 = UInt32.prototype; void p11;
// @ts-expect-error no prototype is promised
const p12 = UInt64.prototype; void p12;
// @ts-expect-error no prototype is promised
const p13 = Amount.prototype; void p13;
// @ts-expect-error no prototype is promised
const p14 = Currency.prototype; void p14;
// @ts-expect-error no prototype is promised
const p15 = IOUAmount.prototype; void p15;
// @ts-expect-error no prototype is promised
const p16 = Issue.prototype; void p16;
// @ts-expect-error no prototype is promised
const p17 = MPTAmount.prototype; void p17;
// @ts-expect-error no prototype is promised
const p18 = NativeAmount.prototype; void p18;
// @ts-expect-error no prototype is promised
const p19 = Path.prototype; void p19;
// @ts-expect-error no prototype is promised
const p20 = PathHop.prototype; void p20;
// @ts-expect-error no prototype is promised
const p21 = PathSet.prototype; void p21;
// @ts-expect-error no prototype is promised
const p22 = Result.prototype; void p22;
// @ts-expect-error no prototype is promised
const p23 = VoidResult.prototype; void p23;
// @ts-expect-error no prototype is promised
const p24 = STArray.prototype; void p24;
const p25: STObject = STObject.prototype; void p25;
// @ts-expect-error no prototype is promised
const p26 = SerializedField.prototype; void p26;
// @ts-expect-error no prototype is promised
const p27 = Vector256.prototype; void p27;
// @ts-expect-error no prototype is promised
const p28 = XChainBridge.prototype; void p28;
// @ts-expect-error no prototype is promised
const p29 = XFLDecimal.prototype; void p29;
const p30: Transaction = Transaction.prototype; void p30;
const p31: Payment = Payment.prototype; void p31;
const p32: LedgerEntry = LedgerEntry.prototype; void p32;
const p33: AccountRoot = AccountRoot.prototype; void p33;
const p34: URIToken = URIToken.prototype; void p34;
const paymentIsTransaction: Transaction = Payment.prototype; void paymentIsTransaction;
const accountRootIsLedgerEntry: LedgerEntry = AccountRoot.prototype;
void accountRootIsLedgerEntry;
const uriTokenIsLedgerEntry: LedgerEntry = URIToken.prototype;
void uriTokenIsLedgerEntry;

// ── Schema reads, decimal authoring, and concrete lookup ────────────

declare const decimalStateSchema: BinarySchema<{ readonly value: XFLDecimal }>;
declare const foreignState: state.ForeignAccessor;

const localSchemaRead: StateReadResult<{ readonly value: XFLDecimal }> =
  state.get("schema-value", decimalStateSchema);
const foreignSchemaRead: StateReadResult<{ readonly value: XFLDecimal }> =
  foreignState.get("schema-value", decimalStateSchema);
const foreignStateWrite: HostVoidResult = foreignState.set(
  "schema-value",
  STBlob.from(new Uint8Array(8)),
);
const foreignStateDelete: HostVoidResult = foreignState.del("schema-value");
void localSchemaRead;
void foreignSchemaRead;
void foreignStateWrite;
void foreignStateDelete;

const baseFee: Drops = ledger.feeBase;
const decimalZero: XFLDecimal = XFLDecimal.zero;
const scaledDecimal = XFLDecimal.from(11, -1);
const persistedDecimalField: RecordField<XFLDecimal, 8> = record.xflle();
void baseFee;
void decimalZero;
void persistedDecimalField;

const issuedCurrency: Currency = Currency.from("USD");
const issuedAsset: Issue = Issue.iou(issuedCurrency, AccountID.one);
if (scaledDecimal.ok) {
  const issuedAmount: Result<IOUAmount, EncodeError> = Amount.iou(
    scaledDecimal.value,
    issuedAsset,
  );
  void issuedAmount;
}

const uriTokenKeylet: LedgerKeylet<URIToken> =
  util.keylet.uriToken(Hash256.zero);
const uriTokenRead: HostResult<URIToken | undefined> =
  ledger.lookup(uriTokenKeylet);
if (uriTokenRead.ok && uriTokenRead.value !== undefined) {
  const uriTokenOwner: AccountID = uriTokenRead.value.Owner;
  const uriTokenType: typeof LedgerEntryType.URIToken =
    uriTokenRead.value.LedgerEntryType;
  void uriTokenOwner;
  void uriTokenType;
}

// @ts-expect-error the decimal factory publishes zero but no one constant
void XFLDecimal.one;
// @ts-expect-error exact v1 accepts a complete Issue, not three IOU parts
Amount.iou(XFLDecimal.zero, issuedCurrency, AccountID.one);
declare const unspecializedLedgerKeylet: LedgerKeylet<LedgerEntry>;
const unspecializedLedgerRead: HostResult<LedgerEntry | undefined> =
  ledger.lookup(unspecializedLedgerKeylet);
void unspecializedLedgerRead;

// ── 0060 activation: profile-configuration vocabulary ────────────────

type ActivationEqual<X, Y> =
  (<T>() => T extends X ? 1 : 2) extends (<T>() => T extends Y ? 1 : 2)
    ? true
    : false;
declare function activationExpectTrue<T extends true>(): void;

activationExpectTrue<ActivationEqual<typeof XFLProfile.xahauFloatV1, 1>>();
activationExpectTrue<ActivationEqual<typeof XFLProfile.nearestEvenV1, 2>>();
activationExpectTrue<ActivationEqual<XFLProfile, 1 | 2>>();

const activationConfig = defineHookConfig({
  xflArithmetic: XFLProfile.xahauFloatV1,
});
activationExpectTrue<ActivationEqual<typeof activationConfig.xflArithmetic, 1>>();
const activationHookConfig: HookConfig = activationConfig;
void activationHookConfig;

// @ts-expect-error 0 means absence and is never a declarable profile
defineHookConfig({ xflArithmetic: 0 });
// @ts-expect-error the enum namespace is not constructible
new XFLProfile();
// @ts-expect-error the member inventory interface is module-scope and un-nameable
type ActivationLeak = XFLProfileEnum;
// @ts-expect-error XFLMath is not selected into v1
void XFLMath;
declare const activationDecimal: XFLDecimal;
const activationAdd: XFLResult<XFLDecimal> = activationDecimal.add(activationDecimal);
const activationSubtract: XFLResult<XFLDecimal> = activationDecimal.subtract(activationDecimal);
const activationMultiply: XFLResult<XFLDecimal> = activationDecimal.multiply(activationDecimal);
const activationDivide: XFLResult<XFLDecimal> = activationDecimal.divide(activationDecimal);
void activationAdd; void activationSubtract; void activationMultiply; void activationDivide;

// ── 0102: runtime enum namespaces, literal unions, hidden helpers ─────

activationExpectTrue<ActivationEqual<typeof TransactionType.Payment, 0>>();
activationExpectTrue<ActivationEqual<typeof LedgerEntryType.AccountRoot, 97>>();
activationExpectTrue<ActivationEqual<typeof TransactionResult.tesSUCCESS, 0>>();
activationExpectTrue<ActivationEqual<typeof HookReturnCode.TOO_BIG, -3>>();

const transactionTypeValue: TransactionType = TransactionType.Invoke;
const ledgerEntryTypeValue: LedgerEntryType = LedgerEntryType.AccountRoot;
const transactionResultValue: TransactionResult = TransactionResult.tecNO_ENTRY;
const hookReturnCodeValue: HookReturnCode = HookReturnCode.INVALID_FLOAT;
void transactionTypeValue; void ledgerEntryTypeValue;
void transactionResultValue; void hookReturnCodeValue;

// @ts-expect-error 123456 is not a declared transaction type
const badTransactionType: TransactionType = 123456; void badTransactionType;
// @ts-expect-error 123456 is not a declared ledger-entry type
const badLedgerEntryType: LedgerEntryType = 123456; void badLedgerEntryType;
// @ts-expect-error 123456 is not a declared transaction result
const badTransactionResult: TransactionResult = 123456; void badTransactionResult;
// @ts-expect-error -24 is the intentionally unused Hook return-code gap
const badHookReturnCode: HookReturnCode = -24; void badHookReturnCode;

// @ts-expect-error enum namespace objects are not constructible
new TransactionType();
// @ts-expect-error enum namespace objects are not constructible
new TransactionResult();
// @ts-expect-error enum namespace objects are not constructible
new HookReturnCode();

// @ts-expect-error module-scope helper must not leak
type TransactionTypeEnumLeak = TransactionTypeEnum;
// @ts-expect-error module-scope helper must not leak
type TransactionResultEnumLeak = TransactionResultEnum;
// @ts-expect-error module-scope helper must not leak
type HookReturnCodeEnumLeak = HookReturnCodeEnum;

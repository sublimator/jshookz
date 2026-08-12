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
  function type(): HostResult<number>;
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

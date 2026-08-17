declare function require(id: string): any;
declare namespace require {
  let main: unknown;
}
declare const module: { exports: any; parent?: unknown };
declare const process: {
  argv: string[];
  env: { [key: string]: string | undefined };
  exit(code?: number): never;
};
declare const console: { error(...args: any[]): void };

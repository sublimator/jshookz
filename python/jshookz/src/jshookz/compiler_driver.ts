/* One Program: graph diagnostics/policy, Result, callback type, emit. */

import { checkResultOwnership } from "./result_validator";
import { checkEntrySignatures, checkHookImports } from "./entry_policy";
import {
  checkXFLProfilePolicy,
  SourceXFLProfile,
} from "./xfl_profile_policy";

const path = require("path") as {
  resolve(...parts: string[]): string;
  dirname(p: string): string;
  relative(from: string, to: string): string;
  isAbsolute(p: string): boolean;
};

type TS = any;
type Kind = "typescript" | "result" | "entry" | "xfl" | "ok";

interface DriverResult {
  ok: boolean;
  kind: Kind;
  diagnostics: string[];
  createCount: number;
  allowMalformed: boolean;
  xflProfile: SourceXFLProfile;
}

function formatDiagnostic(ts: TS, diagnostic: TS): string {
  const message = ts.flattenDiagnosticMessageText(diagnostic.messageText, "\n");
  if (!diagnostic.file || diagnostic.start === undefined) return message;
  const { line, character } = diagnostic.file.getLineAndCharacterOfPosition(
    diagnostic.start,
  );
  return `${diagnostic.file.fileName}(${line + 1},${character + 1}): ${message}`;
}

function sourceAllowsMalformed(ts: TS, source: TS): boolean {
  const text = source.text;
  let found = false;
  const consider = (pos: number) => {
    for (const range of ts.getLeadingCommentRanges(text, pos) || []) {
      if (range.kind !== ts.SyntaxKind.SingleLineCommentTrivia) continue;
      const body = text.slice(range.pos, range.end);
      if (/^\/\/[ \t]*@jshookz-allow-malformed[ \t]*$/.test(body)) found = true;
    }
  };
  const visit = (node: TS) => {
    consider(node.pos);
    consider(node.getFullStart());
    ts.forEachChild(node, visit);
  };
  visit(source);
  return found;
}

export function compile(
  ts: TS,
  configPath: string,
  sourcePath: string,
  declarationPath: string,
): DriverResult {
  let createCount = 0;
  const createProgram = (...args: unknown[]) => {
    createCount += 1;
    return ts.createProgram(...args);
  };

  const fail = (
    kind: Kind,
    diagnostics: string[],
    extra: Partial<DriverResult> = {},
  ): DriverResult => ({
    ok: false,
    kind,
    diagnostics,
    createCount,
    allowMalformed: false,
    xflProfile: "none",
    ...extra,
  });

  const configFile = ts.readConfigFile(configPath, ts.sys.readFile);
  if (configFile.error) {
    return fail("typescript", [formatDiagnostic(ts, configFile.error)]);
  }
  const parsed = ts.parseJsonConfigFileContent(
    configFile.config,
    ts.sys,
    path.dirname(configPath),
  );
  if (parsed.errors.length) {
    return fail(
      "typescript",
      parsed.errors.map((diagnostic: TS) => formatDiagnostic(ts, diagnostic)),
    );
  }

  const options = { ...parsed.options, noEmitOnError: true };
  const program = createProgram(parsed.fileNames, options);
  const source = program.getSourceFile(sourcePath);
  if (!source) {
    return fail("typescript", [
      `source file is absent from TypeScript program: ${sourcePath}`,
    ]);
  }
  const declaration = program.getSourceFile(declarationPath);
  if (!declaration) {
    return fail("typescript", [
      `declaration file is absent from TypeScript program: ${declarationPath}`,
    ]);
  }

  const xfl = checkXFLProfilePolicy(ts, program, source, declaration);
  if (xfl.diagnostics.length) {
    return fail("xfl", [...xfl.diagnostics], { xflProfile: xfl.profile });
  }

  const authoringRoot = path.dirname(path.resolve(sourcePath));
  const withinAuthoringRoot = (candidate: TS): boolean => {
    const relative = path.relative(
      authoringRoot,
      path.resolve(candidate.fileName),
    );
    return (
      relative === "" ||
      (!relative.startsWith("..") && !path.isAbsolute(relative))
    );
  };
  const policySources = program
    .getSourceFiles()
    .filter(
      (candidate: TS) =>
        !candidate.isDeclarationFile ||
        (path.resolve(candidate.fileName) !== path.resolve(declarationPath) &&
          withinAuthoringRoot(candidate)),
    )
    .sort((left: TS, right: TS) => left.fileName.localeCompare(right.fileName));
  const allowStaticRelativeImports = sourcePath.endsWith(".ts");
  const importDiagnostics = policySources.flatMap((candidate: TS) =>
    checkHookImports(ts, candidate, allowStaticRelativeImports),
  );
  if (importDiagnostics.length) {
    return fail("typescript", importDiagnostics);
  }

  const tsDiagnostics = ts.getPreEmitDiagnostics(program);
  if (tsDiagnostics.length) {
    return fail(
      "typescript",
      tsDiagnostics.map((diagnostic: TS) => formatDiagnostic(ts, diagnostic)),
    );
  }

  const resultDiagnostics = policySources
    .filter((candidate: TS) => !candidate.isDeclarationFile)
    .flatMap((candidate: TS) => checkResultOwnership(ts, program, candidate));
  if (resultDiagnostics.length) {
    return fail("result", resultDiagnostics);
  }

  const allowMalformed = sourceAllowsMalformed(ts, source);
  const entryDiagnostics = allowMalformed
    ? []
    : checkEntrySignatures(ts, program, source);
  if (entryDiagnostics.length) {
    return fail("entry", entryDiagnostics);
  }

  const emitted = program.emit();
  if (emitted.emitSkipped || emitted.diagnostics.length) {
    return fail(
      "typescript",
      emitted.diagnostics.map((diagnostic: TS) => formatDiagnostic(ts, diagnostic)),
    );
  }

  return {
    ok: true,
    kind: "ok",
    diagnostics: [],
    createCount,
    allowMalformed,
    xflProfile: xfl.profile,
  };
}

function main() {
  const ts = require(process.argv[2]);
  const configPath = process.argv[3];
  const sourcePath = path.resolve(process.argv[4]);
  const declarationPath = path.resolve(process.argv[5]);
  const result = compile(ts, configPath, sourcePath, declarationPath);
  console.error(`kind=${result.kind}`);
  console.error(`allowMalformed=${result.allowMalformed ? "1" : "0"}`);
  console.error(`xflProfile=${result.xflProfile}`);
  if (process.env.JSHOOKZ_COUNT_PROGRAM === "1") {
    console.error(`createProgram=${result.createCount}`);
  }
  if (result.ok) return;
  console.error(result.diagnostics.join("\n"));
  process.exit(result.kind === "typescript" ? 1 : 2);
}

if (require.main === module) {
  main();
}

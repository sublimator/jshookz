/* XFL artifact-profile policy over the compiler's one TypeScript Program. */

import {
  XFL_PROFILE_IMPLEMENTATIONS,
  XFL_PROFILE_LEDGER_SCHEMA,
  XFL_PROFILE_SENSITIVE_MEMBERS,
} from "./xfl_profile_ledger";

type TS = any;

export type SourceXFLProfile =
  | "none"
  | "xahauFloatV1"
  | "nearestEvenV1";

interface PolicyDiagnostic {
  readonly code: "JSH-XFL001" | "JSH-XFL002";
  readonly node: TS;
  readonly detail: string;
}

export interface XFLPolicyResult {
  readonly profile: SourceXFLProfile;
  readonly diagnostics: readonly string[];
}

interface CanonicalSymbols {
  readonly declaration: TS;
  readonly xflProfile: TS;
  readonly xahauFloatV1: TS;
  readonly nearestEvenV1: TS;
  readonly defineHookConfig: TS;
  readonly sensitive: ReadonlyMap<TS, string>;
}

function symbolAt(checker: TS, node: TS): TS {
  return node ? checker.getSymbolAtLocation(node) : undefined;
}

function formatAt(node: TS, code: PolicyDiagnostic["code"], detail: string): string {
  const file = node.getSourceFile ? node.getSourceFile() : undefined;
  if (!file) return `${code}: ${detail}`;
  const { line, character } = file.getLineAndCharacterOfPosition(
    node.getStart(file),
  );
  return `${file.fileName}(${line + 1},${character + 1}): ${code}: ${detail}`;
}

function globalStatements(ts: TS, declaration: TS): readonly TS[] {
  const blocks: TS[] = [];
  const visit = (node: TS) => {
    if (
      ts.isModuleDeclaration(node) &&
      node.name &&
      node.name.text === "global" &&
      node.body &&
      ts.isModuleBlock(node.body)
    ) {
      blocks.push(node.body);
      return;
    }
    ts.forEachChild(node, visit);
  };
  visit(declaration);
  if (blocks.length !== 1) {
    throw new Error(
      `XFL declaration integrity: expected one direct declare global block, found ${blocks.length}`,
    );
  }
  return blocks[0].statements;
}

function declarationNamed(ts: TS, statement: TS, name: string): TS[] {
  if (
    (ts.isInterfaceDeclaration(statement) ||
      ts.isTypeAliasDeclaration(statement) ||
      ts.isEnumDeclaration(statement) ||
      ts.isFunctionDeclaration(statement)) &&
    statement.name &&
    statement.name.text === name
  ) {
    return [statement.name];
  }
  if (ts.isVariableStatement(statement)) {
    return statement.declarationList.declarations
      .filter(
        (declaration: TS) =>
          ts.isIdentifier(declaration.name) && declaration.name.text === name,
      )
      .map((declaration: TS) => declaration.name);
  }
  return [];
}

function exactlyOne(nodes: readonly TS[], label: string): TS {
  if (nodes.length !== 1) {
    throw new Error(
      `XFL declaration integrity: expected one ${label}, found ${nodes.length}`,
    );
  }
  return nodes[0];
}

function memberSymbol(
  ts: TS,
  checker: TS,
  nameNode: TS,
  memberName: string,
): TS {
  const parent = nameNode.parent;
  if (ts.isEnumDeclaration(parent)) {
    const member = parent.members.find(
      (candidate: TS) => candidate.name && candidate.name.text === memberName,
    );
    return member ? symbolAt(checker, member.name) : undefined;
  }
  const container = symbolAt(checker, nameNode);
  if (!container) return undefined;
  const type = checker.getTypeOfSymbolAtLocation(container, nameNode);
  return checker.getPropertyOfType(type, memberName);
}

function canonicalSymbols(
  ts: TS,
  program: TS,
  declaration: TS,
): CanonicalSymbols | undefined {
  if (XFL_PROFILE_LEDGER_SCHEMA !== "jshookz.xfl-profile-ledger.v1") {
    throw new Error("XFL declaration integrity: unsupported profile ledger schema");
  }
  const checker = program.getTypeChecker();
  const statements = globalStatements(ts, declaration);
  const profileNodes = statements
    .flatMap((statement: TS) => declarationNamed(ts, statement, "XFLProfile"))
    .filter(
      (node: TS) =>
        ts.isEnumDeclaration(node.parent) ||
        ts.isVariableDeclaration(node.parent),
    );
  const configNodes = statements
    .flatMap((statement: TS) => declarationNamed(ts, statement, "HookConfig"))
    .filter((node: TS) => ts.isInterfaceDeclaration(node.parent));
  const helperNodes = statements
    .flatMap((statement: TS) =>
      declarationNamed(ts, statement, "defineHookConfig"),
    )
    .filter((node: TS) => ts.isFunctionDeclaration(node.parent));

  if (
    profileNodes.length === 0 &&
    configNodes.length === 0 &&
    helperNodes.length === 0
  ) {
    // The pre-activation exact-v1 declaration is a supported control input.
    return undefined;
  }

  const profileNode = exactlyOne(profileNodes, "global XFLProfile value");
  exactlyOne(configNodes, "global HookConfig interface");
  const helperNode = exactlyOne(helperNodes, "global defineHookConfig function");
  const xflProfile = symbolAt(checker, profileNode);
  const defineHookConfig = symbolAt(checker, helperNode);
  const xahauFloatV1 = memberSymbol(
    ts,
    checker,
    profileNode,
    "xahauFloatV1",
  );
  const nearestEvenV1 = memberSymbol(
    ts,
    checker,
    profileNode,
    "nearestEvenV1",
  );
  if (!xflProfile || !defineHookConfig || !xahauFloatV1 || !nearestEvenV1) {
    throw new Error("XFL declaration integrity: canonical profile symbols are missing");
  }
  if (ts.isVariableDeclaration(profileNode.parent)) {
    const xahauType = checker.getTypeOfSymbolAtLocation(
      xahauFloatV1,
      xahauFloatV1.valueDeclaration || profileNode,
    );
    const nearestType = checker.getTypeOfSymbolAtLocation(
      nearestEvenV1,
      nearestEvenV1.valueDeclaration || profileNode,
    );
    if (xahauType.value !== 1 || nearestType.value !== 2) {
      throw new Error(
        "XFL declaration integrity: runtime profile values must be exactly 1 and 2",
      );
    }
  }

  const sensitive = new Map<TS, string>();
  for (const path of XFL_PROFILE_SENSITIVE_MEMBERS) {
    const [typeName, memberName, ...extra] = path.split(".");
    if (extra.length || !typeName || !memberName) {
      throw new Error(`XFL declaration integrity: malformed ledger path ${path}`);
    }
    const typeDeclarations = statements.filter(
      (statement: TS) =>
        ts.isInterfaceDeclaration(statement) &&
        statement.name &&
        statement.name.text === typeName,
    );
    if (typeDeclarations.length !== 1) {
      // Exact v1 intentionally excludes the sensitive arithmetic quartet.
      continue;
    }
    const typeDeclaration = typeDeclarations[0];
    const memberDeclaration =
      ts.isInterfaceDeclaration(typeDeclaration) &&
      typeDeclaration.members.find(
        (candidate: TS) => candidate.name && candidate.name.text === memberName,
      );
    const member = memberDeclaration
      ? symbolAt(checker, memberDeclaration.name)
      : undefined;
    if (!member) continue;
    const owned = (member.getDeclarations() || []).some(
      (candidate: TS) => candidate.getSourceFile() === declaration,
    );
    if (!owned) {
      throw new Error(
        `XFL declaration integrity: ledger member ${path} is not canonical`,
      );
    }
    sensitive.set(member, path);
    sensitive.set(memberDeclaration, path);
  }

  return {
    declaration,
    xflProfile,
    xahauFloatV1,
    nearestEvenV1,
    defineHookConfig,
    sensitive,
  };
}

function hasOnlyExportModifier(ts: TS, statement: TS): boolean {
  const modifiers = statement.modifiers || [];
  return (
    modifiers.length === 1 &&
    modifiers[0].kind === ts.SyntaxKind.ExportKeyword
  );
}

function directSymbol(checker: TS, node: TS): TS {
  return symbolAt(checker, node);
}

function resolvedDeclarationSymbol(checker: TS, call: TS): TS {
  const signature = checker.getResolvedSignature(call);
  const declaration = signature && signature.getDeclaration();
  return declaration && declaration.name
    ? symbolAt(checker, declaration.name)
    : undefined;
}

function isCanonicalHelperCall(
  ts: TS,
  checker: TS,
  call: TS,
  canonical: CanonicalSymbols,
): boolean {
  const expression = call.expression;
  return (
    (ts.isIdentifier(expression) &&
      directSymbol(checker, expression) === canonical.defineHookConfig) ||
    resolvedDeclarationSymbol(checker, call) === canonical.defineHookConfig
  );
}

function acceptedConfig(
  ts: TS,
  checker: TS,
  declaration: TS,
  entry: TS,
  canonical: CanonicalSymbols,
): SourceXFLProfile | undefined {
  const list = declaration.parent;
  const statement = list && list.parent;
  if (
    !list ||
    !statement ||
    !ts.isVariableDeclarationList(list) ||
    !ts.isVariableStatement(statement) ||
    statement.getSourceFile() !== entry ||
    list.declarations.length !== 1 ||
    (list.flags & ts.NodeFlags.Const) === 0 ||
    !hasOnlyExportModifier(ts, statement) ||
    !ts.isIdentifier(declaration.name) ||
    declaration.name.text !== "hookConfig" ||
    declaration.type ||
    declaration.exclamationToken
  ) {
    return undefined;
  }
  const call = declaration.initializer;
  if (
    !call ||
    !ts.isCallExpression(call) ||
    call.typeArguments?.length ||
    !ts.isIdentifier(call.expression) ||
    directSymbol(checker, call.expression) !== canonical.defineHookConfig ||
    call.arguments.length !== 1
  ) {
    return undefined;
  }
  const object = call.arguments[0];
  if (!ts.isObjectLiteralExpression(object) || object.properties.length !== 1) {
    return undefined;
  }
  const property = object.properties[0];
  if (
    !ts.isPropertyAssignment(property) ||
    !ts.isIdentifier(property.name) ||
    property.name.text !== "xflArithmetic"
  ) {
    return undefined;
  }
  const value = property.initializer;
  if (
    !ts.isPropertyAccessExpression(value) ||
    value.questionDotToken ||
    !ts.isIdentifier(value.expression) ||
    directSymbol(checker, value.expression) !== canonical.xflProfile
  ) {
    return undefined;
  }
  const member = directSymbol(checker, value.name);
  if (member === canonical.xahauFloatV1) return "xahauFloatV1";
  if (member === canonical.nearestEvenV1) return "nearestEvenV1";
  return undefined;
}

function executableFiles(program: TS): TS[] {
  return program
    .getSourceFiles()
    .filter((source: TS) => !source.isDeclarationFile)
    .sort((left: TS, right: TS) => left.fileName.localeCompare(right.fileName));
}

function collectAttempts(
  ts: TS,
  checker: TS,
  files: readonly TS[],
  canonical: CanonicalSymbols,
): { configs: TS[]; helperCalls: TS[]; otherAttempts: TS[] } {
  const configs: TS[] = [];
  const helperCalls: TS[] = [];
  const otherAttempts: TS[] = [];
  for (const source of files) {
    const visit = (node: TS) => {
      if (
        ts.isVariableDeclaration(node) &&
        ts.isIdentifier(node.name) &&
        node.name.text === "hookConfig"
      ) {
        configs.push(node);
      }
      if (
        ts.isExportSpecifier(node) &&
        node.name &&
        node.name.text === "hookConfig"
      ) {
        otherAttempts.push(node);
      }
      if (
        ts.isBinaryExpression(node) &&
        ts.isIdentifier(node.left) &&
        node.left.text === "hookConfig" &&
        node.operatorToken.kind >= ts.SyntaxKind.FirstAssignment &&
        node.operatorToken.kind <= ts.SyntaxKind.LastAssignment
      ) {
        otherAttempts.push(node);
      }
      if (
        ts.isExportAssignment(node) &&
        ts.isObjectLiteralExpression(node.expression) &&
        node.expression.properties.some(
          (property: TS) => property.name && property.name.text === "xflArithmetic",
        )
      ) {
        otherAttempts.push(node);
      }
      if (
        ts.isCallExpression(node) &&
        isCanonicalHelperCall(ts, checker, node, canonical)
      ) {
        helperCalls.push(node);
      }
      ts.forEachChild(node, visit);
    };
    visit(source);
  }
  return { configs, helperCalls, otherAttempts };
}

function sensitiveCalls(
  ts: TS,
  checker: TS,
  files: readonly TS[],
  canonical: CanonicalSymbols,
): { call: TS; member: string; moduleEvaluation: boolean }[] {
  const calls: { call: TS; member: string; moduleEvaluation: boolean }[] = [];
  for (const source of files) {
    const visit = (node: TS) => {
      if (ts.isCallExpression(node)) {
        const signature = checker.getResolvedSignature(node);
        const declaration = signature && signature.getDeclaration();
        const symbol = resolvedDeclarationSymbol(checker, node);
        const member =
          (declaration && canonical.sensitive.get(declaration)) ||
          (symbol && canonical.sensitive.get(symbol));
        if (member) {
          let parent = node.parent;
          while (parent && !ts.isFunctionLike(parent)) parent = parent.parent;
          calls.push({ call: node, member, moduleEvaluation: !parent });
        }
      }
      ts.forEachChild(node, visit);
    };
    visit(source);
  }
  return calls;
}

export function checkXFLProfilePolicy(
  ts: TS,
  program: TS,
  entry: TS,
  declaration: TS,
): XFLPolicyResult {
  const canonical = canonicalSymbols(ts, program, declaration);
  if (!canonical) return { profile: "none", diagnostics: [] };

  const checker = program.getTypeChecker();
  const files = executableFiles(program);
  const attempts = collectAttempts(ts, checker, files, canonical);
  const diagnostics: PolicyDiagnostic[] = [];
  let profile: SourceXFLProfile = "none";

  const onlyConfig = attempts.configs.length === 1 ? attempts.configs[0] : undefined;
  const accepted = onlyConfig
    ? acceptedConfig(ts, checker, onlyConfig, entry, canonical)
    : undefined;
  const acceptedCall = onlyConfig?.initializer;
  const exactAttempt =
    !!accepted &&
    attempts.configs.length === 1 &&
    attempts.helperCalls.length === 1 &&
    attempts.helperCalls[0] === acceptedCall &&
    attempts.otherAttempts.length === 0;

  const attempted =
    attempts.configs.length > 0 ||
    attempts.helperCalls.length > 0 ||
    attempts.otherAttempts.length > 0;
  if (attempted && !exactAttempt) {
    const node =
      attempts.configs[0] || attempts.helperCalls[0] || attempts.otherAttempts[0];
    diagnostics.push({
      code: "JSH-XFL001",
      node,
      detail:
        "hookConfig must be one direct source-entry export const initialized by canonical defineHookConfig with one canonical XFLProfile member",
    });
  } else if (exactAttempt && accepted) {
    profile = accepted;
  }

  if (!attempted || exactAttempt) {
    const implemented = new Set<string>(XFL_PROFILE_IMPLEMENTATIONS[profile]);
    for (const sensitive of sensitiveCalls(ts, checker, files, canonical)) {
      if (sensitive.moduleEvaluation) {
        diagnostics.push({
          code: "JSH-XFL002",
          node: sensitive.call,
          detail: `${sensitive.member} cannot execute during module evaluation under profile ${profile}`,
        });
        continue;
      }
      if (profile !== "none" && implemented.has(sensitive.member)) continue;
      diagnostics.push({
        code: profile === "none" ? "JSH-XFL001" : "JSH-XFL002",
        node: sensitive.call,
        detail:
          profile === "none"
            ? `${sensitive.member} requires one canonical hookConfig declaration`
            : `${sensitive.member} is not implemented for profile ${profile}`,
      });
    }
  }

  const formatted = diagnostics
    .sort((left, right) => {
      const leftFile = left.node.getSourceFile().fileName;
      const rightFile = right.node.getSourceFile().fileName;
      return leftFile.localeCompare(rightFile) || left.node.pos - right.node.pos;
    })
    .map((diagnostic) =>
      formatAt(diagnostic.node, diagnostic.code, diagnostic.detail),
    );
  return { profile, diagnostics: formatted };
}

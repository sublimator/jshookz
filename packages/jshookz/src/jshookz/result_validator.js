/* Typed ownership/dataflow check for the closed Hook Result exit set.
 *
 * Invoked by hook_compiler.py with:
 *   node result_validator.js <typescript.js> <tsconfig.json> <source>
 */
"use strict";

const ts = require(process.argv[2]);
const path = require("path");
const configPath = process.argv[3];
const sourcePath = path.resolve(process.argv[4]);

const configFile = ts.readConfigFile(configPath, ts.sys.readFile);
if (configFile.error) {
  console.error(ts.flattenDiagnosticMessageText(configFile.error.messageText, "\n"));
  process.exit(2);
}
const parsed = ts.parseJsonConfigFileContent(
  configFile.config,
  ts.sys,
  path.dirname(configPath),
);
const program = ts.createProgram(parsed.fileNames, parsed.options);
const checker = program.getTypeChecker();
const source = program.getSourceFile(sourcePath);
if (!source) {
  console.error(`source file is absent from TypeScript program: ${sourcePath}`);
  process.exit(2);
}

const EXIT_METHODS = new Set(["okOr", "okOrHandle", "okMapOr", "moot"]);
const violations = new Map();

function unwrap(node) {
  while (
    ts.isParenthesizedExpression(node) ||
    ts.isAsExpression(node) ||
    ts.isTypeAssertionExpression(node) ||
    ts.isNonNullExpression(node) ||
    ts.isSatisfiesExpression(node)
  ) {
    node = node.expression;
  }
  return node;
}

function isResult(type) {
  if (type.flags & (ts.TypeFlags.Any | ts.TypeFlags.Unknown)) return false;
  if (type.isUnion()) return type.types.some(isResult);
  const apparent = checker.getApparentType(type);
  return ["ok", "okOr", "okOrHandle", "okMapOr"].every(name =>
    checker.getPropertyOfType(apparent, name)
  );
}

function expressionIsResult(node) {
  return isResult(checker.getTypeAtLocation(node));
}

function reject(node, detail) {
  const position = source.getLineAndCharacterOfPosition(node.getStart(source));
  const message = `${position.line + 1}:${position.character + 1}: ${detail}`;
  violations.set(message, message);
}

function newState() {
  return { live: new Set(), owners: new Map() };
}

function cloneState(state) {
  return {
    live: new Set(state.live),
    owners: new Map(
      [...state.owners].map(([symbol, tokens]) => [symbol, new Set(tokens)]),
    ),
  };
}

function replaceState(target, sourceState) {
  target.live = sourceState.live;
  target.owners = sourceState.owners;
}

function mergeStates(states) {
  const merged = newState();
  for (const state of states) {
    for (const token of state.live) merged.live.add(token);
    for (const [symbol, tokens] of state.owners) {
      if (!merged.owners.has(symbol)) merged.owners.set(symbol, new Set());
      for (const token of tokens) merged.owners.get(symbol).add(token);
    }
  }
  return merged;
}

function symbolAt(node) {
  return ts.isIdentifier(node) ? checker.getSymbolAtLocation(node) : undefined;
}

function tokensForValue(node, state) {
  node = unwrap(node);
  if (ts.isIdentifier(node)) {
    const symbol = symbolAt(node);
    if (symbol && state.owners.has(symbol)) {
      return new Set(state.owners.get(symbol));
    }
    const token = node;
    state.live.add(token);
    if (symbol) state.owners.set(symbol, new Set([token]));
    return new Set([token]);
  }
  state.live.add(node);
  return new Set([node]);
}

function consume(tokens, state) {
  for (const token of tokens) state.live.delete(token);
}

function bind(tokens, identifier, state) {
  const symbol = symbolAt(identifier);
  if (!symbol) {
    reject(identifier, "Result owner must be a plain identifier");
    return;
  }
  const previous = state.owners.get(symbol);
  if (previous && [...previous].some(token => state.live.has(token))) {
    reject(identifier, "assignment overwrites an unconsumed Result");
  }
  state.owners.set(symbol, new Set(tokens));
}

function applyResultUse(node, state, use) {
  const tokens = tokensForValue(node, state);
  if (use.kind === "bind") {
    bind(tokens, use.target, state);
  } else if (use.kind === "consume" || use.kind === "transfer") {
    consume(tokens, state);
  } else if (use.kind === "discard") {
    reject(node, "Result reaches a use that is not a legal exit or checked transfer");
    consume(tokens, state);
  }
  return tokens;
}

function isRollbackMember(call, name) {
  const target = unwrap(call.expression);
  return (
    ts.isPropertyAccessExpression(target) &&
    ts.isIdentifier(unwrap(target.expression)) &&
    unwrap(target.expression).text === "rollback" &&
    target.name.text === name
  );
}

function isTerminalCall(expression) {
  expression = unwrap(expression);
  if (!ts.isCallExpression(expression)) return false;
  const target = unwrap(expression.expression);
  return (
    ts.isIdentifier(target) &&
    (target.text === "accept" || target.text === "rollback")
  );
}

function definitelyTerminates(statement) {
  if (ts.isReturnStatement(statement) || ts.isThrowStatement(statement)) return true;
  if (ts.isExpressionStatement(statement)) return isTerminalCall(statement.expression);
  if (ts.isBlock(statement)) {
    return statement.statements.length > 0 &&
      definitelyTerminates(statement.statements[statement.statements.length - 1]);
  }
  if (ts.isIfStatement(statement) && statement.elseStatement) {
    return definitelyTerminates(statement.thenStatement) &&
      definitelyTerminates(statement.elseStatement);
  }
  return false;
}

function localResultParameter(call, index) {
  const signature = checker.getResolvedSignature(call);
  const declaration = signature && signature.getDeclaration();
  if (!declaration || declaration.getSourceFile() !== source || !declaration.body) {
    return false;
  }
  const parameters = declaration.parameters || [];
  let parameter = parameters[index];
  if (!parameter && parameters.length && parameters[parameters.length - 1].dotDotDotToken) {
    parameter = parameters[parameters.length - 1];
  }
  return !!parameter && isResult(checker.getTypeAtLocation(parameter));
}

function analyzeCallInputs(call, state) {
  const target = unwrap(call.expression);
  if (ts.isPropertyAccessExpression(target) && expressionIsResult(target.expression)) {
    if (EXIT_METHODS.has(target.name.text)) {
      analyzeExpression(target.expression, state, { kind: "consume" });
    } else {
      analyzeExpression(target.expression, state, { kind: "observe" });
    }
    for (const argument of call.arguments) {
      analyzeExpression(argument, state, { kind: "discard" });
    }
    return;
  }

  if (
    ts.isPropertyAccessExpression(target) &&
    ts.isIdentifier(unwrap(target.expression)) &&
    unwrap(target.expression).text === "rollback"
  ) {
    const name = target.name.text;
    if (call.arguments.length) {
      const first = unwrap(call.arguments[0]);
      if (
        (name === "onAnyFail" || name === "onAllFail") &&
        ts.isArrayLiteralExpression(first)
      ) {
        for (const element of first.elements) {
          analyzeExpression(element, state, { kind: "consume" });
        }
      } else {
        analyzeExpression(first, state, { kind: "consume" });
      }
    }
    for (let index = 1; index < call.arguments.length; ++index) {
      analyzeExpression(call.arguments[index], state, { kind: "discard" });
    }
    return;
  }

  analyzeExpression(target, state, { kind: "observe" });
  for (let index = 0; index < call.arguments.length; ++index) {
    const argument = call.arguments[index];
    if (expressionIsResult(argument) && localResultParameter(call, index)) {
      analyzeExpression(argument, state, { kind: "transfer" });
    } else {
      analyzeExpression(argument, state, { kind: "discard" });
    }
  }
}

function okTests(node, state, negated = false, tests = []) {
  node = unwrap(node);
  if (ts.isPrefixUnaryExpression(node) && node.operator === ts.SyntaxKind.ExclamationToken) {
    return okTests(node.operand, state, !negated, tests);
  }
  if (ts.isPropertyAccessExpression(node) && node.name.text === "ok" &&
      expressionIsResult(node.expression)) {
    tests.push({
      tokens: tokensForValue(node.expression, state),
      failureWhenTrue: negated,
    });
    return tests;
  }
  if (ts.isBinaryExpression(node)) {
    okTests(node.left, state, negated, tests);
    okTests(node.right, state, negated, tests);
  }
  return tests;
}

function analyzeCondition(node, state) {
  node = unwrap(node);
  if (ts.isPropertyAccessExpression(node) && node.name.text === "ok" &&
      expressionIsResult(node.expression)) {
    tokensForValue(node.expression, state);
    return;
  }
  if (ts.isPrefixUnaryExpression(node)) {
    analyzeCondition(node.operand, state);
    return;
  }
  if (ts.isBinaryExpression(node)) {
    analyzeCondition(node.left, state);
    analyzeCondition(node.right, state);
    return;
  }
  analyzeExpression(node, state, { kind: "discard" });
}

function analyzeConditional(expression, state, use) {
  analyzeCondition(expression.condition, state);
  const tests = okTests(expression.condition, state);
  for (const test of tests) consume(test.tokens, state);

  const whenTrue = cloneState(state);
  const whenFalse = cloneState(state);
  const resultTyped = expressionIsResult(expression);
  analyzeExpression(
    expression.whenTrue,
    whenTrue,
    resultTyped ? { kind: "transfer" } : { kind: "discard" },
  );
  analyzeExpression(
    expression.whenFalse,
    whenFalse,
    resultTyped ? { kind: "transfer" } : { kind: "discard" },
  );
  replaceState(state, mergeStates([whenTrue, whenFalse]));
  if (resultTyped) applyResultUse(expression, state, use);
}

function analyzeExpression(expression, state, use = { kind: "discard" }) {
  expression = unwrap(expression);

  if (ts.isBinaryExpression(expression) &&
      expression.operatorToken.kind === ts.SyntaxKind.EqualsToken) {
    const left = unwrap(expression.left);
    if (expressionIsResult(expression.right)) {
      if (!ts.isIdentifier(left)) {
        reject(left, "Result assignment target must be a plain identifier");
        analyzeExpression(expression.right, state, { kind: "discard" });
      } else {
        analyzeExpression(expression.right, state, { kind: "bind", target: left });
      }
    } else {
      analyzeExpression(expression.right, state, { kind: "discard" });
    }
    return;
  }

  if (ts.isConditionalExpression(expression)) {
    analyzeConditional(expression, state, use);
    return;
  }

  if (ts.isCallExpression(expression)) {
    analyzeCallInputs(expression, state);
    if (expressionIsResult(expression)) applyResultUse(expression, state, use);
    return;
  }

  if (ts.isPropertyAccessExpression(expression)) {
    if (expressionIsResult(expression.expression)) {
      applyResultUse(expression.expression, state, { kind: "observe" });
    } else {
      analyzeExpression(expression.expression, state, { kind: "observe" });
    }
    if (expressionIsResult(expression)) applyResultUse(expression, state, use);
    return;
  }

  if (ts.isElementAccessExpression(expression)) {
    analyzeExpression(expression.expression, state, { kind: "observe" });
    if (expression.argumentExpression) {
      analyzeExpression(expression.argumentExpression, state, { kind: "discard" });
    }
    if (expressionIsResult(expression)) applyResultUse(expression, state, use);
    return;
  }

  if (ts.isIdentifier(expression)) {
    if (expressionIsResult(expression)) applyResultUse(expression, state, use);
    return;
  }

  if (ts.isArrayLiteralExpression(expression)) {
    for (const element of expression.elements) {
      analyzeExpression(element, state, { kind: "discard" });
    }
    if (expressionIsResult(expression)) applyResultUse(expression, state, use);
    return;
  }

  if (ts.isObjectLiteralExpression(expression)) {
    for (const property of expression.properties) {
      if (ts.isPropertyAssignment(property)) {
        analyzeExpression(property.initializer, state, { kind: "discard" });
      } else if (ts.isShorthandPropertyAssignment(property)) {
        analyzeExpression(property.name, state, { kind: "discard" });
      } else if (ts.isSpreadAssignment(property)) {
        analyzeExpression(property.expression, state, { kind: "discard" });
      }
    }
    if (expressionIsResult(expression)) applyResultUse(expression, state, use);
    return;
  }

  if (ts.isArrowFunction(expression) || ts.isFunctionExpression(expression) ||
      ts.isClassExpression(expression)) {
    return;
  }

  if (ts.isBinaryExpression(expression)) {
    const comma = expression.operatorToken.kind === ts.SyntaxKind.CommaToken;
    analyzeExpression(expression.left, state, { kind: "discard" });
    analyzeExpression(expression.right, state, comma ? use : { kind: "discard" });
    if (!comma && expressionIsResult(expression)) applyResultUse(expression, state, use);
    return;
  }

  if (ts.isPrefixUnaryExpression(expression) || ts.isPostfixUnaryExpression(expression) ||
      ts.isVoidExpression(expression) || ts.isAwaitExpression(expression) ||
      ts.isDeleteExpression(expression) || ts.isTypeOfExpression(expression)) {
    analyzeExpression(expression.operand || expression.expression, state, { kind: "discard" });
    if (expressionIsResult(expression)) applyResultUse(expression, state, use);
    return;
  }

  ts.forEachChild(expression, child => {
    if (ts.isExpression(child)) analyzeExpression(child, state, { kind: "discard" });
  });
  if (expressionIsResult(expression)) applyResultUse(expression, state, use);
}

function rejectLive(state, node) {
  for (const token of state.live) {
    reject(token, "Result is not consumed on every reachable path");
  }
  state.live.clear();
}

function analyzeVariableDeclaration(declaration, state) {
  if (!declaration.initializer) return;
  if (expressionIsResult(declaration.initializer)) {
    if (!ts.isIdentifier(declaration.name)) {
      reject(declaration.name, "destructuring does not consume a Result");
      analyzeExpression(declaration.initializer, state, { kind: "discard" });
    } else {
      analyzeExpression(
        declaration.initializer,
        state,
        { kind: "bind", target: declaration.name },
      );
    }
  } else {
    analyzeExpression(declaration.initializer, state, { kind: "discard" });
  }
}

function analyzeStatement(statement, state) {
  if (ts.isBlock(statement)) return analyzeStatements(statement.statements, state);

  if (ts.isVariableStatement(statement)) {
    for (const declaration of statement.declarationList.declarations) {
      analyzeVariableDeclaration(declaration, state);
    }
    return true;
  }

  if (ts.isFunctionDeclaration(statement) || ts.isClassDeclaration(statement)) {
    return true;
  }

  if (ts.isExpressionStatement(statement)) {
    analyzeExpression(statement.expression, state, { kind: "discard" });
    if (isTerminalCall(statement.expression)) {
      rejectLive(state, statement);
      return false;
    }
    return true;
  }

  if (ts.isReturnStatement(statement)) {
    if (statement.expression) {
      analyzeExpression(
        statement.expression,
        state,
        expressionIsResult(statement.expression)
          ? { kind: "transfer" }
          : { kind: "discard" },
      );
    }
    rejectLive(state, statement);
    return false;
  }

  if (ts.isThrowStatement(statement)) {
    if (statement.expression) {
      analyzeExpression(statement.expression, state, { kind: "discard" });
    }
    rejectLive(state, statement);
    return false;
  }

  if (ts.isIfStatement(statement)) {
    analyzeCondition(statement.expression, state);
    const tests = okTests(statement.expression, state);
    if (statement.elseStatement) {
      for (const test of tests) consume(test.tokens, state);
    } else if (definitelyTerminates(statement.thenStatement)) {
      for (const test of tests) {
        if (test.failureWhenTrue) consume(test.tokens, state);
      }
    }

    const whenTrue = cloneState(state);
    const whenFalse = cloneState(state);
    const trueReachable = analyzeStatement(statement.thenStatement, whenTrue);
    const falseReachable = statement.elseStatement
      ? analyzeStatement(statement.elseStatement, whenFalse)
      : true;
    const reachableStates = [];
    if (trueReachable) reachableStates.push(whenTrue);
    if (falseReachable) reachableStates.push(whenFalse);
    replaceState(state, mergeStates(reachableStates));
    return reachableStates.length > 0;
  }

  if (ts.isWhileStatement(statement) || ts.isDoStatement(statement)) {
    analyzeCondition(statement.expression, state);
    const body = cloneState(state);
    analyzeStatement(statement.statement, body);
    replaceState(state, mergeStates([state, body]));
    return true;
  }

  if (ts.isForStatement(statement)) {
    if (statement.initializer) {
      if (ts.isVariableDeclarationList(statement.initializer)) {
        for (const declaration of statement.initializer.declarations) {
          analyzeVariableDeclaration(declaration, state);
        }
      } else {
        analyzeExpression(statement.initializer, state, { kind: "discard" });
      }
    }
    if (statement.condition) analyzeCondition(statement.condition, state);
    const body = cloneState(state);
    analyzeStatement(statement.statement, body);
    if (statement.incrementor) {
      analyzeExpression(statement.incrementor, body, { kind: "discard" });
    }
    replaceState(state, mergeStates([state, body]));
    return true;
  }

  if (ts.isForOfStatement(statement) || ts.isForInStatement(statement)) {
    analyzeExpression(statement.expression, state, { kind: "discard" });
    const body = cloneState(state);
    analyzeStatement(statement.statement, body);
    replaceState(state, mergeStates([state, body]));
    return true;
  }

  if (ts.isTryStatement(statement)) {
    const paths = [];
    const tried = cloneState(state);
    if (analyzeStatement(statement.tryBlock, tried)) paths.push(tried);
    if (statement.catchClause) {
      const caught = cloneState(state);
      if (analyzeStatement(statement.catchClause.block, caught)) paths.push(caught);
    } else {
      paths.push(cloneState(state));
    }
    let merged = mergeStates(paths);
    if (statement.finallyBlock) analyzeStatement(statement.finallyBlock, merged);
    replaceState(state, merged);
    return true;
  }

  ts.forEachChild(statement, child => {
    if (ts.isExpression(child)) analyzeExpression(child, state, { kind: "discard" });
  });
  return true;
}

function analyzeStatements(statements, state) {
  let reachable = true;
  for (const statement of statements) {
    if (!reachable) break;
    reachable = analyzeStatement(statement, state);
  }
  return reachable;
}

function analyzeFunction(node) {
  if (!node.body) return;
  const state = newState();
  for (const parameter of node.parameters || []) {
    if (!isResult(checker.getTypeAtLocation(parameter))) continue;
    if (!ts.isIdentifier(parameter.name)) {
      reject(parameter.name, "Result parameter owner must be a plain identifier");
      continue;
    }
    const token = parameter;
    state.live.add(token);
    bind(new Set([token]), parameter.name, state);
  }
  const reachable = ts.isBlock(node.body)
    ? analyzeStatements(node.body.statements, state)
    : (analyzeExpression(
        node.body,
        state,
        expressionIsResult(node.body) ? { kind: "transfer" } : { kind: "discard" },
      ), true);
  if (reachable) rejectLive(state, node.body);
}

const topLevel = newState();
if (analyzeStatements(source.statements, topLevel)) rejectLive(topLevel, source);

function visitFunctions(node) {
  if (ts.isFunctionLike(node) && node.body) analyzeFunction(node);
  ts.forEachChild(node, visitFunctions);
}
visitFunctions(source);

if (violations.size) {
  console.error([...violations.values()].join("\n"));
  process.exit(2);
}

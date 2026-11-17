// vyrn --smoke-test <project> [--check] [--train-seconds N] [--inject-error] [--screenshot out.png]
// vyrn --smoke-test --open-package <file.vrlp> [--trust] [--check] [...] [--export-brain out.vrl] [--import-brain file.vrl]
// ... [--compile <new folder>]   (the toolbar's Compile; a <project> copy then keeps its brains/)
// (docs/dev/app-internals.md section 9). Prints one JSON line; returns the process exit code.
#pragma once

#include <QStringList>

int runSmokeTest(const QStringList& args);   // QApplication already exists; args after "--smoke-test"

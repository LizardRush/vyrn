// The only place QScintilla headers are included. The app is built with QT_NO_KEYWORDS (so Qt's `slots` macro
// can never clash with anything), but QScintilla's headers use the lowercase keywords: map them here.
#pragma once

#define signals Q_SIGNALS
#define slots Q_SLOTS
#include <Qsci/qsciapis.h>
#include <Qsci/qscilexerpython.h>
#include <Qsci/qsciscintilla.h>
#undef signals
#undef slots

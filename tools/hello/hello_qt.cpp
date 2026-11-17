// Qt side of the M0 toolchain proof. Never include torch headers here.
#include <QApplication>
#include <QtGlobal>

// QScintilla's headers use the lowercase signals/slots keywords.
#include <Qsci/qsciapis.h>
#include <Qsci/qscilexerpython.h>
#include <Qsci/qsciscintilla.h>

#include <string>

std::string hello_qt() {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  int argc = 1;
  char arg0[] = "hello";
  char* argv[] = {arg0, nullptr};
  QApplication app(argc, argv);
  QsciScintilla editor;
  auto* lexer = new QsciLexerPython(&editor);
  editor.setLexer(lexer);
  editor.setText("def see():\n    return vyrn.screen()\n");
  return std::string("qt ") + qVersion() + " ok, QScintilla " + QSCINTILLA_VERSION_STR + " ok (" +
         std::to_string(editor.lines()) + " lines)";
}

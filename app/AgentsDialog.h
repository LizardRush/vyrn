// Help -> "Connect an AI coding agent…": copyable setup lines for Claude Code, Codex and Gemini CLI
// (docs/dev/mcp.md section 2), what agents may and may not do, and an "Add to Claude Code" button.
#pragma once

#include <QDialog>

class AgentsDialog : public QDialog {
  Q_OBJECT

 public:
  explicit AgentsDialog(QWidget* parent = nullptr);
  ~AgentsDialog() override;   // an "Add to Claude Code" still running is left to finish on its own
};

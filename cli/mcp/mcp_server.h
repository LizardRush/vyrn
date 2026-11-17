// The Vyrn MCP server (docs/dev/mcp.md): JSON-RPC over stdio for coding agents. Dual-era: modern MCP
// (2026-07-28: server/discover, per-request _meta) and legacy MCP (initialize handshake, 2025-11-25 and older).
#pragma once

// Runs until stdin closes or a stop request (SIGINT/SIGTERM/SIGHUP; Windows: Ctrl+C, Ctrl+Break, closing the console);
// stops every running session gracefully (saving brains) first. A second stop request quits at once.
// Arguments (after "mcp"): [--projects-dir DIR] [--allow-dir DIR]... [--data-dir DIR] [--max-trainings N]
// Logs go to stderr only (stdout carries the protocol). Returns the process exit code.
int run_mcp_server(int argc, char** argv);

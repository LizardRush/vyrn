// vyrn-mcp: the Vyrn MCP server as a program of its own (docs/dev/mcp.md). `vyrn-cli mcp` runs the same server.
//
//   vyrn-mcp [--projects-dir DIR] [--allow-dir DIR]... [--data-dir DIR] [--max-trainings N]
#include "mcp_server.h"

#include "vyrn/platform.h"

int main(int argc, char** argv) {
  // libgomp reads OMP_WAIT_POLICY when it loads, so it must be set before this program starts (like vyrn-cli):
  // set it and run this program again, once; if that fails, carry on with the default.
  vyrn::platform::ensure_passive_openmp(argv);
  return run_mcp_server(argc, argv);
}

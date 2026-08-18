#pragma once

namespace TonDron::mcp {

// Attach to a running TonDron MCP server over stdio (Content-Length framed JSON-RPC).
// Returns a process exit code. Does not start the GUI.
int runStdioAttach();

} // namespace TonDron::mcp

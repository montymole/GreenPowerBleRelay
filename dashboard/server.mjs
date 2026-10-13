import { createServer, request as httpRequest } from "node:http";
import { request as httpsRequest } from "node:https";
import { readFile } from "node:fs/promises";
import { extname, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(fileURLToPath(new URL(".", import.meta.url)));
const mimeTypes = {
  ".css": "text/css; charset=utf-8",
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
};
const port = Number(process.env.PORT || 4173);
const relay = process.env.RELAY_URL ? new URL(process.env.RELAY_URL) : null;
if (relay && (!["http:", "https:"].includes(relay.protocol) || relay.pathname !== "/" || relay.search || relay.hash || relay.username)) {
  throw new Error("RELAY_URL must be an http(s) origin without a path or credentials");
}

createServer(async (request, response) => {
  const pathname = new URL(request.url || "/", "http://localhost").pathname;
  if (pathname.startsWith("/api/v1/")) {
    if (!relay) {
      response.writeHead(503, { "Content-Type": "application/json" })
        .end(JSON.stringify({ error: "Set RELAY_URL when starting the local development server" }));
      return;
    }
    const upstream = (relay.protocol === "https:" ? httpsRequest : httpRequest)(
      new URL(request.url, relay),
      { method: request.method, headers: { ...request.headers, host: relay.host } },
      result => {
        response.writeHead(result.statusCode, result.headers);
        result.pipe(response);
      });
    upstream.setTimeout(45000, () => upstream.destroy(new Error("Relay timeout")));
    upstream.on("error", () => {
      if (!response.headersSent) response.writeHead(502, { "Content-Type": "application/json" });
      response.end(JSON.stringify({ error: "Relay unavailable" }));
    });
    request.pipe(upstream);
    return;
  }
  let filePath = resolve(root, `.${decodeURIComponent(pathname)}`);
  if (filePath !== root && !filePath.startsWith(`${root}${sep}`)) {
    response.writeHead(403).end("Forbidden");
    return;
  }
  if (pathname === "/") filePath = resolve(root, "index.html");

  try {
    const body = await readFile(filePath);
    response.writeHead(200, {
      "Content-Type": mimeTypes[extname(filePath)] || "application/octet-stream",
      "Cache-Control": "no-store",
      "X-Content-Type-Options": "nosniff",
    });
    response.end(body);
  } catch (error) {
    if (error.code === "ENOENT" || error.code === "EISDIR") {
      response.writeHead(404).end("Not found");
      return;
    }
    console.error("Dashboard request failed:", error);
    response.writeHead(500).end("Dashboard server error");
  }
}).listen(port, "127.0.0.1", () => {
  console.log(`BLE relay dashboard: http://127.0.0.1:${port}`);
});

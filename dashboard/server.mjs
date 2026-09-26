import { createServer } from "node:http";
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

createServer(async (request, response) => {
  const pathname = new URL(request.url || "/", "http://localhost").pathname;
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

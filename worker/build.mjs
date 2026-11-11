// Gera dist/worker.js (um arquivo só, com o painel embutido) para colar no editor da Cloudflare.
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
const html = readFileSync("src/painel.html", "utf8");
const js = readFileSync("src/index.js", "utf8").replace(
  'import PAINEL_HTML from "./painel.html";',
  "const PAINEL_HTML = " + JSON.stringify(html) + ";"
);
mkdirSync("dist", { recursive: true });
writeFileSync("dist/worker.js", js);
console.log("dist/worker.js", js.length, "bytes");

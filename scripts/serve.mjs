import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { resolve, extname } from 'node:path';
const root = resolve('dist');
const mime = { '.html':'text/html', '.css':'text/css', '.js':'text/javascript', '.mjs':'text/javascript', '.wasm':'application/wasm', '.svg':'image/svg+xml', '.mp4':'video/mp4', '.webm':'video/webm', '.json':'application/json' };
createServer(async(req,res) => {
  try {
    let path = resolve(root, '.' + decodeURIComponent(new URL(req.url, 'http://localhost').pathname));
    if (!path.startsWith(root + '/') && path !== root) throw new Error('Invalid path');
    if ((await stat(path)).isDirectory()) path += '/index.html';
    res.setHeader('Content-Type',mime[extname(path)] || 'application/octet-stream');
    res.setHeader('Cache-Control','no-store');
    res.end(await readFile(path));
  } catch { res.writeHead(404); res.end('Not found'); }
}).listen(Number(process.env.PORT) || 4173, '0.0.0.0', () => console.log('Jolt Studio preview ready'));

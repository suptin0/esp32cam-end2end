// backend/server.js
const express = require('express');
const cors = require('cors');
const fs = require('fs');
const path = require('path');

const PORT = process.env.PORT || 3000;
const UPLOAD_DIR = path.join(__dirname, 'uploads');
if (!fs.existsSync(UPLOAD_DIR)) fs.mkdirSync(UPLOAD_DIR, { recursive: true });

const app = express();
app.disable('x-powered-by');
app.set('etag', false);

// log ทุก request
app.use((req,res,next)=>{
  console.log(`[${new Date().toISOString()}] ${req.method} ${req.url}`);
  next();
});

app.use(cors({
  origin: true, methods: ['GET','POST','OPTIONS'], allowedHeaders: ['Content-Type'], maxAge: 86400,
}));

app.use('/uploads', express.static(UPLOAD_DIR, { immutable: false, maxAge: 0 }));

app.get(['/','/healthz'], (req, res) => res.json({ ok: true, msg: 'esp32cam-server up' }));

// อัปโหลดแบบ stream (อัปได้ไม่จำกัดครั้ง)
app.post('/api/upload', (req, res) => {
  const ct = String(req.headers['content-type'] || '').toLowerCase();
  if (!ct.includes('application/octet-stream')) {
    res.status(415).json({ ok: false, err: 'bad_content_type' }); return;
  }
  const rawName  = (req.query.filename || `shot_${Date.now()}.jpg`).toString();
  const safeName = path.basename(rawName).replace(/[^a-zA-Z0-9._-]/g, '_');
  const finalPath = path.join(UPLOAD_DIR, safeName);

  req.setTimeout(120000);
  const ws = fs.createWriteStream(finalPath);
  req.pipe(ws);

  let finished = false;
  req.on('aborted', ()=>{ try{ ws.destroy(); }catch{} });

  ws.on('finish', () => {
    if (finished) return; finished = true;
    const baseURL = `${req.protocol}://${req.headers.host}`;
    res.set('Connection', 'close');           // ปิด keep-alive
    console.log(`Upload OK: ${safeName}`);
    res.json({ ok: true, filename: safeName, url: `${baseURL}/uploads/${encodeURIComponent(safeName)}` });
  });
  ws.on('error', (e) => {
    console.error('write error', e);
    if (!finished) { finished = true; res.status(500).json({ ok: false, err: 'write_fail' }); }
  });
});

// list ไฟล์
app.get('/api/list', async (req, res) => {
  try {
    const files = await fs.promises.readdir(UPLOAD_DIR);
    const stats = await Promise.all(files.map(async (name) => {
      const p = path.join(UPLOAD_DIR, name);
      const st = await fs.promises.stat(p);
      return { name, size: st.size, mtime: st.mtimeMs };
    }));
    const items = stats
      .filter(f => /\.(jpe?g|png|gif|bmp)$/i.test(f.name))
      .sort((a,b)=> b.mtime - a.mtime)
      .map(f => ({ filename: f.name, size: f.size, mtime: f.mtime,
                   url: `${req.protocol}://${req.headers.host}/uploads/${encodeURIComponent(f.name)}` }));
    res.json({ ok: true, items });
  } catch (e) {
    console.error(e); res.status(500).json({ ok: false, err: 'list_fail' });
  }
});

const server = app.listen(PORT, '0.0.0.0', () => {
  console.log(`[esp32cam-server] listening on http://0.0.0.0:${PORT}`);
});
server.keepAliveTimeout = 5000;
server.headersTimeout   = 7000;
//npm start
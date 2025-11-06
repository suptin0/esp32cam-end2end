# Backend (Node.js + Express)
Run:
  npm install
  npm start

Health:
  http://localhost:3000/healthz

Allow Windows Firewall (Private) for TCP 3000 if needed:
  netsh advfirewall firewall add rule name="Node 3000" dir=in action=allow protocol=TCP localport=3000 profile=Private

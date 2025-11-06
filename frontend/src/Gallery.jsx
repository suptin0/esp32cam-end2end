import { useEffect, useState } from "react"
const BACKEND = import.meta.env.VITE_BACKEND || "http://10.48.11.71:3000"
export default function Gallery(){
  const [items,setItems]=useState([]); const [loading,setLoading]=useState(true); const [error,setError]=useState("");
  useEffect(()=>{ let stop=false; const load=async()=>{ try{ const r=await fetch(`${BACKEND}/api/list`,{cache:"no-store"}); const j=await r.json(); if(!j.ok) throw new Error(j.err||"load_fail"); if(!stop) setItems(j.items||[]); }catch(e){ if(!stop) setError(e.message);}finally{ if(!stop) setLoading(false);} }; load(); const t=setInterval(load,3000); return ()=>{ stop=true; clearInterval(t); }; },[]);
  return (<div style={{padding:16,color:"#fff",background:"#111",minHeight:"100vh"}}>
    <h2 style={{margin:0}}>ESP32-CAM Uploads</h2>
    <div style={{color:"#9aa",marginBottom:12}}>Backend: <code style={{background:"#000",border:"1px solid #222",padding:"2px 6px",borderRadius:6}}>{BACKEND}</code></div>
    {loading && <div>Loading...</div>}{error && <div style={{color:"#e88"}}>Error: {error}</div>}
    <div style={{display:"grid",gridTemplateColumns:"repeat(auto-fill,minmax(220px,1fr))",gap:12}}>
    {items.map(it=>(<div key={it.url} style={{background:"#1b1b1b",border:"1px solid #222",borderRadius:12,padding:8}}>
      <a href={it.url} target="_blank"><img src={it.url} style={{width:"100%",height:180,objectFit:"cover",borderRadius:10,background:"#000"}}/></a>
      <div style={{marginTop:6}}><div style={{overflow:"hidden",textOverflow:"ellipsis"}}>{it.filename}</div>
      <small style={{color:"#9aa"}}>{new Date(it.mtime).toLocaleString()} · {(it.size/1024).toFixed(1)} KB</small></div></div>))}
    </div></div>);
}
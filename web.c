/*
 * Embedded web dashboard with live map
 *
 * Serves a Leaflet-based map showing decoded STD-C and Aero messages.
 * Uses Server-Sent Events (SSE) for live updates.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include "web.h"

extern volatile sig_atomic_t running;

/* ---- Ring buffer storage ---- */

#define MAX_STDC_MSGS   200
#define MAX_AIRCRAFT    256
#define MAX_FIXES       8

typedef struct {
    double timestamp;
    double lat, lon;
    int has_position;
    int service_code;
    int msg_type;
    char text[512];
    int text_len;
} stdc_entry_t;

typedef struct {
    char reg[16];
    char flight[16];
    char label[4];
    double fix_lat[MAX_FIXES];
    double fix_lon[MAX_FIXES];
    int fix_alt[MAX_FIXES];
    double fix_t[MAX_FIXES];
    int n_fixes;
    double last_seen;
    char last_text[256];
    int channel_id;
} aircraft_entry_t;

static struct {
    pthread_mutex_t lock;

    stdc_entry_t stdc[MAX_STDC_MSGS];
    int stdc_head;
    int stdc_count;

    aircraft_entry_t aircraft[MAX_AIRCRAFT];
    int num_aircraft;
} state;

/* ---- Time ---- */

static double now_unix(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---- State update functions ---- */

void web_add_stdc(const stdc_message_t *msg) {
    pthread_mutex_lock(&state.lock);

    stdc_entry_t *e = &state.stdc[state.stdc_head];
    e->timestamp = now_unix();
    e->lat = msg->lat;
    e->lon = msg->lon;
    e->has_position = msg->has_position;
    e->service_code = msg->service_code;
    e->msg_type = msg->type;

    int tlen = msg->text_len;
    if (tlen > (int)sizeof(e->text) - 1)
        tlen = (int)sizeof(e->text) - 1;
    memcpy(e->text, msg->text, tlen);
    e->text[tlen] = '\0';
    e->text_len = tlen;

    state.stdc_head = (state.stdc_head + 1) % MAX_STDC_MSGS;
    if (state.stdc_count < MAX_STDC_MSGS)
        state.stdc_count++;

    pthread_mutex_unlock(&state.lock);
}

void web_add_aero(const aero_message_t *msg) {
    pthread_mutex_lock(&state.lock);

    double now = now_unix();

    /* Find existing aircraft by registration */
    aircraft_entry_t *ac = NULL;
    for (int i = 0; i < state.num_aircraft; i++) {
        if (strcmp(state.aircraft[i].reg, msg->reg) == 0) {
            ac = &state.aircraft[i];
            break;
        }
    }

    if (!ac) {
        /* Create new entry */
        if (state.num_aircraft < MAX_AIRCRAFT) {
            ac = &state.aircraft[state.num_aircraft++];
            memset(ac, 0, sizeof(*ac));
            strncpy(ac->reg, msg->reg, sizeof(ac->reg) - 1);
        } else {
            /* Evict oldest */
            double oldest_t = 1e18;
            int oldest_i = 0;
            for (int i = 0; i < state.num_aircraft; i++) {
                if (state.aircraft[i].last_seen < oldest_t) {
                    oldest_t = state.aircraft[i].last_seen;
                    oldest_i = i;
                }
            }
            ac = &state.aircraft[oldest_i];
            memset(ac, 0, sizeof(*ac));
            strncpy(ac->reg, msg->reg, sizeof(ac->reg) - 1);
        }
    }

    strncpy(ac->flight, msg->flight, sizeof(ac->flight) - 1);
    strncpy(ac->label, msg->label, sizeof(ac->label) - 1);
    ac->last_seen = now;
    ac->channel_id = msg->channel_id;

    if (msg->text_len > 0) {
        int tlen = msg->text_len;
        if (tlen > (int)sizeof(ac->last_text) - 1)
            tlen = (int)sizeof(ac->last_text) - 1;
        memcpy(ac->last_text, msg->text, tlen);
        ac->last_text[tlen] = '\0';
    }

    /* Add position fix if available */
    if (msg->has_position && !isnan(msg->lat) && !isnan(msg->lon)) {
        if (ac->n_fixes >= MAX_FIXES) {
            memmove(ac->fix_lat, ac->fix_lat + 1, (MAX_FIXES - 1) * sizeof(double));
            memmove(ac->fix_lon, ac->fix_lon + 1, (MAX_FIXES - 1) * sizeof(double));
            memmove(ac->fix_alt, ac->fix_alt + 1, (MAX_FIXES - 1) * sizeof(int));
            memmove(ac->fix_t, ac->fix_t + 1, (MAX_FIXES - 1) * sizeof(double));
            ac->n_fixes = MAX_FIXES - 1;
        }
        ac->fix_lat[ac->n_fixes] = msg->lat;
        ac->fix_lon[ac->n_fixes] = msg->lon;
        ac->fix_alt[ac->n_fixes] = msg->alt_ft;
        ac->fix_t[ac->n_fixes] = now;
        ac->n_fixes++;
    }

    pthread_mutex_unlock(&state.lock);
}

/* ---- JSON serialization ---- */

static int json_escape_str(char *out, int maxlen, const char *in, int inlen) {
    int pos = 0;
    for (int i = 0; i < inlen && pos < maxlen - 6; i++) {
        char c = in[i];
        switch (c) {
        case '"':  out[pos++] = '\\'; out[pos++] = '"'; break;
        case '\\': out[pos++] = '\\'; out[pos++] = '\\'; break;
        case '\n': out[pos++] = '\\'; out[pos++] = 'n'; break;
        case '\r': out[pos++] = '\\'; out[pos++] = 'r'; break;
        case '\t': out[pos++] = '\\'; out[pos++] = 't'; break;
        default:
            if (c >= 0x20)
                out[pos++] = c;
            break;
        }
    }
    out[pos] = '\0';
    return pos;
}

#define JSON_BUF_SIZE 524288  /* 512 KB — room for 512 aircraft + 200 STD-C */

static int build_json(char *buf, int maxlen) {
    pthread_mutex_lock(&state.lock);

    int pos = 0;
    pos += snprintf(buf + pos, maxlen - pos, "{\"t\":%.3f,", now_unix());

    /* STD-C messages */
    pos += snprintf(buf + pos, maxlen - pos, "\"stdc\":[");
    int first = 1;
    for (int i = 0; i < state.stdc_count && pos < maxlen - 1024; i++) {
        int idx = (state.stdc_head - state.stdc_count + i + MAX_STDC_MSGS) % MAX_STDC_MSGS;
        stdc_entry_t *e = &state.stdc[idx];

        char escaped[1024];
        int elen = e->text_len;
        if (elen > 400) elen = 400;
        json_escape_str(escaped, sizeof(escaped), e->text, elen);

        if (!first) buf[pos++] = ',';
        first = 0;

        if (e->has_position) {
            pos += snprintf(buf + pos, maxlen - pos,
                "{\"t\":%.3f,\"svc\":%d,\"type\":%d,"
                "\"lat\":%.6f,\"lon\":%.6f,\"text\":\"%s\"}",
                e->timestamp, e->service_code, e->msg_type,
                e->lat, e->lon, escaped);
        } else {
            pos += snprintf(buf + pos, maxlen - pos,
                "{\"t\":%.3f,\"svc\":%d,\"type\":%d,\"text\":\"%s\"}",
                e->timestamp, e->service_code, e->msg_type, escaped);
        }
    }
    pos += snprintf(buf + pos, maxlen - pos, "],");

    /* Aircraft — only send entries seen in the last 10 minutes */
    double cutoff = now_unix() - 600.0;
    pos += snprintf(buf + pos, maxlen - pos, "\"aircraft\":[");
    first = 1;
    for (int i = 0; i < state.num_aircraft && pos < maxlen - 2048; i++) {
        aircraft_entry_t *ac = &state.aircraft[i];
        if (ac->last_seen < cutoff) continue;
        if (!first) buf[pos++] = ',';
        first = 0;

        char escaped_text[512];
        json_escape_str(escaped_text, sizeof(escaped_text),
                         ac->last_text, (int)strlen(ac->last_text));

        pos += snprintf(buf + pos, maxlen - pos,
            "{\"reg\":\"%s\",\"flight\":\"%s\",\"label\":\"%s\","
            "\"last_seen\":%.3f,\"ch\":%d,\"text\":\"%s\",\"fixes\":[",
            ac->reg, ac->flight, ac->label,
            ac->last_seen, ac->channel_id, escaped_text);

        for (int j = 0; j < ac->n_fixes; j++) {
            if (j > 0) buf[pos++] = ',';
            pos += snprintf(buf + pos, maxlen - pos,
                "[%.6f,%.6f,%d,%.3f]",
                ac->fix_lat[j], ac->fix_lon[j],
                ac->fix_alt[j], ac->fix_t[j]);
        }

        pos += snprintf(buf + pos, maxlen - pos, "]}");
    }
    pos += snprintf(buf + pos, maxlen - pos, "],");

    /* Per-channel status (from jaero_chans in main.c) */
    {
        extern int num_jaero_chans;
        typedef struct {
            int channel_id;
            int baud_rate;
            unsigned long msg_count;
            unsigned long burst_count;
            double last_msg_time;
            unsigned long drops;
            double mse;
            double ebno;
        } chan_web_info_t;

        /* Gather channel info without holding main's lock */
        extern void web_get_channel_info(chan_web_info_t *out, int *n);
        chan_web_info_t chinfo[32];
        int nch = 0;
        web_get_channel_info(chinfo, &nch);

        pos += snprintf(buf + pos, maxlen - pos, "\"channels\":[");
        for (int i = 0; i < nch && pos < maxlen - 256; i++) {
            if (i > 0) buf[pos++] = ',';
            double age = now_unix() - chinfo[i].last_msg_time;
            if (chinfo[i].last_msg_time < 1) age = -1;
            pos += snprintf(buf + pos, maxlen - pos,
                "{\"ch\":%d,\"baud\":%d,\"msgs\":%lu,\"age\":%.0f,\"mse\":%.3f,\"ebno\":%.1f}",
                chinfo[i].channel_id, chinfo[i].baud_rate,
                chinfo[i].msg_count, age, chinfo[i].mse, chinfo[i].ebno);
        }
        pos += snprintf(buf + pos, maxlen - pos, "]");
    }

    pos += snprintf(buf + pos, maxlen - pos, "}");

    pthread_mutex_unlock(&state.lock);
    return pos;
}

/* ---- Embedded HTML page ---- */

static const char HTML_PAGE[] =
"<!DOCTYPE html>\n"
"<html><head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>inmarsat-sniffer</title>\n"
"<link rel=\"stylesheet\" href=\"https://unpkg.com/leaflet@1.9.4/dist/leaflet.css\">\n"
"<script src=\"https://unpkg.com/leaflet@1.9.4/dist/leaflet.js\"></script>\n"
"<style>\n"
"*{margin:0;padding:0;box-sizing:border-box}\n"
"body{font-family:system-ui,-apple-system,sans-serif;background:#0f172a;color:#e2e8f0}\n"
"#map{width:100vw;height:calc(100vh - 44px)}\n"
"#bar{height:44px;background:#1e293b;color:#e2e8f0;display:flex;\n"
"  align-items:center;padding:0 16px;gap:20px;font-size:13px;\n"
"  border-bottom:1px solid #334155}\n"
"#bar .title{font-weight:600;color:#f8fafc;letter-spacing:0.5px}\n"
".stat{color:#94a3b8}\n"
".val{color:#38bdf8;font-weight:600;font-variant-numeric:tabular-nums}\n"
"#status{margin-left:auto;font-size:12px}\n"
".leaflet-popup-content{font-family:'SF Mono',Consolas,monospace;\n"
"  font-size:12px;line-height:1.6}\n"
".popup-title{font-weight:700;font-size:13px;margin-bottom:4px;\n"
"  padding-bottom:4px;border-bottom:1px solid #e2e8f0}\n"
".popup-ac{color:#38bdf8;font-weight:600}\n"
".popup-egc{color:#4ade80;font-weight:600}\n"
".leaflet-container{background:#0f172a}\n"
".leaflet-control-layers{background:rgba(15,23,42,0.92)!important;\n"
"  color:#e2e8f0!important;border:1px solid #334155!important}\n"
".leaflet-control-layers label{color:#e2e8f0}\n"
"#side{position:absolute;right:0;top:44px;bottom:0;width:320px;\n"
"  background:rgba(15,23,42,0.92);border-left:1px solid #334155;\n"
"  font-size:11px;display:flex;flex-direction:column;\n"
"  font-family:'SF Mono',Consolas,monospace;color:#cbd5e1;z-index:400}\n"
"#tabs{display:flex;border-bottom:1px solid #334155;flex-shrink:0}\n"
".tab{flex:1;padding:6px 0;text-align:center;font-size:10px;cursor:pointer;\n"
"  color:#64748b;text-transform:uppercase;letter-spacing:1px;font-weight:600;\n"
"  border-bottom:2px solid transparent}\n"
".tab.active{color:#38bdf8;border-bottom-color:#38bdf8}\n"
".tab:hover{color:#94a3b8}\n"
".tab-content{flex:1;overflow-y:auto;padding:8px;display:none}\n"
".tab-content.active{display:block}\n"
".msg{background:#1e293b;border-left:2px solid #38bdf8;margin:3px 0;\n"
"  padding:4px 6px;word-wrap:break-word;border-radius:2px}\n"
".msg.egc{border-color:#4ade80}\n"
".msg .hdr{color:#38bdf8;font-weight:600;font-size:11px}\n"
".msg .ts{color:#64748b;font-size:10px}\n"
".msg .txt{color:#cbd5e1;font-size:11px;margin-top:2px}\n"
"@media(max-width:900px){#side{display:none}#map{width:100vw}}\n"
"</style></head><body>\n"
"<div id=\"bar\">\n"
"  <span class=\"title\">inmarsat-sniffer</span>\n"
"  <span class=\"stat\">Locked <span id=\"n-ac\" class=\"val\">0</span></span>\n"
"  <span class=\"stat\">ACARS <span id=\"n-acars\" class=\"val\">0</span></span>\n"
"  <span class=\"stat\">Positions <span id=\"n-pos\" class=\"val\">0</span></span>\n"
"  <span class=\"stat\">STD-C <span id=\"n-stdc\" class=\"val\">0</span></span>\n"
"  <button id=\"btn-export\" onclick=\"exportAircraft()\" style=\"background:#334155;color:#e2e8f0;border:1px solid #475569;border-radius:4px;padding:2px 8px;cursor:pointer;font-size:11px;margin-left:4px\">Export CSV</button>\n"
"  <span id=\"status\" style=\"color:#64748b\">connecting...</span>\n"
"</div>\n"
"<div id=\"map\"></div>\n"
"<div id=\"side\">\n"
"  <div id=\"tabs\">\n"
"    <div class=\"tab active\" onclick=\"switchTab('acars')\">ACARS</div>\n"
"    <div class=\"tab\" onclick=\"switchTab('stdc')\">STD-C</div>\n"
"    <div class=\"tab\" onclick=\"switchTab('channels')\">Channels</div>\n"
"  </div>\n"
"  <div id=\"tab-acars\" class=\"tab-content active\"><div id=\"aero-list\"></div></div>\n"
"  <div id=\"tab-stdc\" class=\"tab-content\"><div id=\"stdc-list\"></div></div>\n"
"  <div id=\"tab-channels\" class=\"tab-content\"><div id=\"ch-panel\" style=\"font-size:10px;line-height:1.6\"></div></div>\n"
"</div>\n"
"<script>"
"function switchTab(name){"
"  var tabs=document.querySelectorAll('.tab');"
"  var panes=document.querySelectorAll('.tab-content');"
"  tabs.forEach(function(t){t.classList.remove('active')});"
"  panes.forEach(function(p){p.classList.remove('active')});"
"  document.querySelector('.tab[onclick*=\"'+name+'\"]').classList.add('active');"
"  document.getElementById('tab-'+name).classList.add('active');"
"}\n"
"var map=L.map('map',{center:[20,0],zoom:3});"
"L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png',"
"{attribution:'CartoDB',maxZoom:19}).addTo(map);"
""
"var stdc_layer=L.layerGroup().addTo(map);"
"var ac_layer=L.layerGroup().addTo(map);"
"var trail_layer=L.layerGroup().addTo(map);"
"L.control.layers(null,{'STD-C':stdc_layer,'Aircraft':ac_layer,"
"'Trails':trail_layer},{position:'bottomleft'}).addTo(map);"
""
"var ac_markers={};"
"var allAcars=[];"
"var acarsKeys={};"
"function exportAircraft(){"
"  if(allAcars.length===0){alert('No ACARS messages collected yet.');return;}"
"  var csv='timestamp,reg,flight,label,channel,lat,lon,text\\n';"
"  allAcars.forEach(function(a){"
"    var d=new Date(a.t*1000);"
"    var txt=(a.text||'').replace(/[\"\\n\\r,]/g,' ').substring(0,200);"
"    csv+=d.toISOString()+','+a.reg+','+a.flight+','+a.label+','+a.ch"
"      +','+(a.lat||'')+','+(a.lon||'')+',\"'+txt+'\"\\n';"
"  });"
"  var blob=new Blob([csv],{type:'text/csv'});"
"  var a=document.createElement('a');"
"  a.href=URL.createObjectURL(blob);"
"  a.download='inmarsat_acars_'+Date.now()+'.csv';"
"  a.click();"
"}"
"var stdc_markers=[];"
""
"function fmtTime(ts){"
"var d=new Date(ts*1000);"
"return d.toUTCString().slice(17,25)+'Z'}"
""
"function update(d){"
"var now=d.t;"
"var nPos=0;"
"if(d.aircraft){d.aircraft.forEach(function(a){if(a.fixes&&a.fixes.length)nPos++})}"
"document.getElementById('n-ac').textContent=d.aircraft?d.aircraft.length:0;"
"document.getElementById('n-acars').textContent=d.total_acars||(d.aircraft?d.aircraft.length:0);"
"document.getElementById('n-pos').textContent=nPos;"
"document.getElementById('n-stdc').textContent=d.stdc?d.stdc.length:0;"
"document.getElementById('status').style.color='#22c55e';"
"document.getElementById('status').textContent='live';"
"if(d.channels){"
"  var cp=document.getElementById('ch-panel');"
"  var locked=0;"
"  var html='';"
"  function fmtN(n){return n>=10000?(n/1000).toFixed(1)+'k':n>=1000?(n/1000).toFixed(1)+'k':''+n}"
"  d.channels.forEach(function(c){"
"    var active=c.msgs>0&&c.age>=0&&c.age<120;"
"    if(active)locked++;"
"    var baud=c.baud>=8400?(c.baud/1000+'k OQPSK'):(c.baud+' MSK');"
"    var dot=active?'\\u25CF':'\\u25CB';"
"    var color=active?'#38bdf8':'#475569';"
"    var msgs=c.msgs>0?fmtN(c.msgs):'\\u2014';"
"    var eb=c.ebno.toFixed(1);"
"    var ebc=c.msgs>0?'#38bdf8':'#64748b';"
"    html+='<div style=\"color:'+color+';display:flex;gap:6px;padding:1px 0;align-items:center\">'"
"      +'<span>'+dot+'</span>'"
"      +'<span style=\"min-width:32px\">ch'+c.ch+'</span>'"
"      +'<span style=\"min-width:78px\">'+baud+'</span>'"
"      +'<span style=\"min-width:38px;text-align:right\">'+msgs+'</span>'"
"      +'<span style=\"min-width:55px;color:'+ebc+'\">'+eb+' dB</span>'"
"      +'</div>';"
"  });"
"  document.getElementById('n-ac').textContent=locked+'/'+d.channels.length;"
"  cp.innerHTML=html;"
"}"
"if(d.aircraft){d.aircraft.forEach(function(a){"
"  var key=a.reg+'|'+a.last_seen.toFixed(1);"
"  if(!acarsKeys[key]){"
"    acarsKeys[key]=1;"
"    allAcars.push({t:a.last_seen,reg:a.reg,flight:a.flight,"
"      label:a.label,ch:a.ch,text:a.text,"
"      lat:a.fixes&&a.fixes.length?a.fixes[a.fixes.length-1][0]:null,"
"      lon:a.fixes&&a.fixes.length?a.fixes[a.fixes.length-1][1]:null});"
"    if(allAcars.length>5000){allAcars=allAcars.slice(-2500);acarsKeys={};allAcars.forEach(function(e){acarsKeys[e.reg+'|'+e.t.toFixed(1)]=1;});}"
"  }"
"})}"
""
"/* STD-C messages */"
"var sl=document.getElementById('stdc-list');"
"sl.innerHTML='';"
"stdc_layer.clearLayers();"
"var recent=d.stdc.slice(-50).reverse();"
"for(var i=0;i<recent.length;i++){"
"var m=recent[i];"
"var div=document.createElement('div');"
"div.className='msg egc';"
"div.innerHTML='<div class=\"ts\">'+fmtTime(m.t)+'</div>'"
"+m.text.substring(0,200);"
"sl.appendChild(div);"
"if(m.lat!==undefined){"
"L.circleMarker([m.lat,m.lon],{radius:5,color:'#0f0',"
"fillColor:'#0f0',fillOpacity:0.7}).addTo(stdc_layer)"
".bindPopup('<b>STD-C</b><br>'+m.text.substring(0,100));"
"}}"
""
"/* Aircraft */"
"var al=document.getElementById('aero-list');"
"al.innerHTML='';"
"var seen={};"
"for(var i=0;i<d.aircraft.length;i++){"
"var ac=d.aircraft[i];"
"seen[ac.reg]=1;"
"var div=document.createElement('div');"
"div.className='msg acars';"
"div.innerHTML='<div class=\"hdr\">'+ac.reg+' '+ac.flight"
"+'</div><div class=\"ts\">'+fmtTime(ac.last_seen)"
"+'</div>'+ac.text.substring(0,150);"
"al.appendChild(div);"
""
"if(ac.fixes.length>0){"
"var last=ac.fixes[ac.fixes.length-1];"
"var latlng=[last[0],last[1]];"
"if(ac_markers[ac.reg]){"
"ac_markers[ac.reg].setLatLng(latlng);"
"}else{"
"ac_markers[ac.reg]=L.circleMarker(latlng,{radius:6,"
"color:'#38bdf8',fillColor:'#38bdf8',fillOpacity:0.8})"
".addTo(ac_layer);}"
"ac_markers[ac.reg].bindPopup('<b>'+ac.reg+'</b><br>'"
"+ac.flight+'<br>Alt: '+last[2]+' ft<br>'+ac.text.substring(0,80));"
"}}"
""
"/* Remove stale markers */"
"for(var r in ac_markers){"
"if(!seen[r]){ac_layer.removeLayer(ac_markers[r]);"
"delete ac_markers[r];}}"
""
"/* Rebuild trails (clear first to avoid layer accumulation) */"
"trail_layer.clearLayers();"
"for(var i=0;i<d.aircraft.length;i++){"
"var ac=d.aircraft[i];"
"if(ac.fixes.length>1){"
"var pts=ac.fixes.map(function(f){return[f[0],f[1]]});"
"L.polyline(pts,{color:'#38bdf8',weight:1,opacity:0.5,"
"dashArray:'4 4'}).addTo(trail_layer);}}"
"}"
""
"var base=location.protocol+'//'+location.host+'/';"
"var es=new EventSource(base+'api/events');"
"es.addEventListener('update',function(e){"
"try{update(JSON.parse(e.data))}catch(err){}});"
"es.onerror=function(){"
"document.getElementById('stats').innerHTML='Reconnecting...';};"
"</script></body></html>";

/* ---- HTTP server ---- */

static int server_fd = -1;
static pthread_t server_tid;

static void send_response(int fd, const char *status, const char *content_type,
                            const char *body, int body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n\r\n",
        status, content_type, body_len);

    struct iovec iov[2];
    iov[0].iov_base = header;
    iov[0].iov_len = hlen;
    iov[1].iov_base = (void *)body;
    iov[1].iov_len = body_len;
    if (writev(fd, iov, 2) < 0) { /* ignore broken pipe */ }
}

static void handle_sse(int fd) {
    const char *header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "X-Accel-Buffering: no\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n";

    if (write(fd, header, strlen(header)) < 0) {
        close(fd);
        return;
    }

    char *json = malloc(JSON_BUF_SIZE);
    if (!json) { close(fd); return; }

    while (running) {
        usleep(1000000);  /* 1 Hz updates */

        int jlen = build_json(json, JSON_BUF_SIZE - 64);

        char prefix[32];
        int plen = snprintf(prefix, sizeof(prefix), "event: update\ndata: ");

        struct iovec iov[3];
        iov[0].iov_base = prefix;
        iov[0].iov_len = plen;
        iov[1].iov_base = json;
        iov[1].iov_len = jlen;
        iov[2].iov_base = (void *)"\n\n";
        iov[2].iov_len = 2;

        if (writev(fd, iov, 3) < 0)
            break;
    }

    free(json);
    close(fd);
}

static void *client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;

    char req[4096];
    int rlen = read(fd, req, sizeof(req) - 1);
    if (rlen <= 0) {
        close(fd);
        return NULL;
    }
    req[rlen] = '\0';

    /* Parse request path */
    char *path = NULL;
    if (strncmp(req, "GET ", 4) == 0) {
        path = req + 4;
        char *end = strchr(path, ' ');
        if (end) *end = '\0';
    }

    if (!path) {
        send_response(fd, "400 Bad Request", "text/plain", "Bad Request", 11);
        close(fd);
        return NULL;
    }

    if (strcmp(path, "/") == 0) {
        send_response(fd, "200 OK", "text/html",
                       HTML_PAGE, (int)sizeof(HTML_PAGE) - 1);
        close(fd);
    } else if (strcmp(path, "/api/events") == 0) {
        handle_sse(fd);  /* blocks until disconnect */
    } else if (strcmp(path, "/api/state") == 0) {
        char *json = malloc(JSON_BUF_SIZE);
        if (json) {
            int jlen = build_json(json, JSON_BUF_SIZE - 64);
            send_response(fd, "200 OK", "application/json", json, jlen);
            free(json);
        }
        close(fd);
    } else {
        send_response(fd, "404 Not Found", "text/plain", "Not Found", 9);
        close(fd);
    }

    return NULL;
}

static void *server_thread(void *arg) {
    (void)arg;

    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr,
                                &client_len);
        if (client_fd < 0)
            continue;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, client_thread,
                        (void *)(intptr_t)client_fd);
        pthread_attr_destroy(&attr);
    }

    return NULL;
}

int web_init(int port) {
    memset(&state, 0, sizeof(state));
    pthread_mutex_init(&state.lock, NULL);

    /* Ignore SIGPIPE for broken SSE connections */
    signal(SIGPIPE, SIG_IGN);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("web: socket");
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("web: bind");
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    if (listen(server_fd, 8) < 0) {
        perror("web: listen");
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    fprintf(stderr, "Web dashboard: http://localhost:%d/\n", port);

    pthread_create(&server_tid, NULL, server_thread, NULL);
    return 0;
}

void web_shutdown(void) {
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
    pthread_mutex_destroy(&state.lock);
}

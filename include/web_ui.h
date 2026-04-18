#pragma once

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>FSD 控制器</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,system-ui,"PingFang SC","Microsoft YaHei",sans-serif;background:radial-gradient(circle at top,#15213a 0,#0b1120 45%,#090e1a 100%);color:#e2e8f0;min-height:100vh;padding:16px}
.page{width:min(1180px,100%);margin:0 auto}
.page-header{text-align:center;padding:10px 0 22px}
h1{font-size:22px;color:#38bdf8;font-weight:700;letter-spacing:1px}
.page-subtitle{margin-top:8px;font-size:13px;line-height:1.6;color:#94a3b8}
.dashboard{display:grid;grid-template-columns:1fr;gap:16px;align-items:start}
.card{background:rgba(19,29,50,.96);border:1px solid rgba(56,189,248,.08);border-radius:16px;padding:18px;backdrop-filter:blur(8px);box-shadow:0 18px 40px rgba(2,8,23,.22)}
.card-title{font-size:12px;font-weight:700;color:#64748b;letter-spacing:2px;margin-bottom:14px}
.row{display:flex;align-items:center;justify-content:space-between;padding:12px 0;border-bottom:1px solid #1e293b;gap:12px}
.row:last-child{border-bottom:none}
.row-label{font-size:14px;font-weight:500;flex:1;min-width:0}
.toggle{position:relative;width:50px;height:28px;flex:0 0 auto}
.toggle input{opacity:0;width:0;height:0}
.slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background:#334155;border-radius:28px;transition:.3s}
.slider:before{content:"";position:absolute;height:22px;width:22px;left:3px;bottom:3px;background:#fff;border-radius:50%;transition:.3s}
input:checked+.slider{background:#22c55e}
input:checked+.slider:before{transform:translateX(22px)}
select,.text-input,.picker-trigger{width:100%;background:#1e293b;color:#e2e8f0;border:1px solid #334155;border-radius:8px;padding:10px 12px;font-size:13px}
.text-area{min-height:120px;resize:vertical;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
select{padding-right:32px;appearance:none;-webkit-appearance:none;background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='12' height='12' fill='%2394a3b8' viewBox='0 0 16 16'%3E%3Cpath d='M8 11L3 6h10z'/%3E%3C/svg%3E");background-repeat:no-repeat;background-position:right 10px center;min-width:110px;cursor:pointer}
select:focus,.text-input:focus,.picker-trigger:focus{outline:none;border-color:#38bdf8}
select:disabled,.text-input:disabled,.toggle input:disabled+.slider{opacity:.45;cursor:not-allowed}
.picker-wrap{width:100%;min-width:110px}
.picker-native{display:none}
.picker-trigger{display:flex;align-items:center;justify-content:space-between;gap:12px;text-align:left;cursor:pointer}
.picker-trigger:after{content:"";flex:0 0 auto;width:0;height:0;border-left:5px solid transparent;border-right:5px solid transparent;border-top:6px solid #94a3b8}
.picker-trigger:disabled{opacity:.45;cursor:not-allowed}
.picker-trigger:disabled:after{opacity:.45}
.field{padding:10px 0}
.field+.field{border-top:1px solid #1e293b}
.field-label{display:block;font-size:12px;color:#94a3b8;margin-bottom:8px;letter-spacing:.5px}
.hint{font-size:12px;line-height:1.5;color:#94a3b8;margin-top:10px}
.actions{display:flex;gap:10px;margin-top:14px}
.actions>button{flex:1}
.save-btn,.upload-btn,.ghost-btn{border-radius:8px;padding:10px 14px;font-size:14px;font-weight:600;cursor:pointer;letter-spacing:.5px}
.save-btn,.upload-btn{background:#e31937;color:#fff;border:none}
.ghost-btn{background:#1e293b;color:#e2e8f0;border:1px solid #334155}
.save-btn{width:100%}
.upload-btn{width:100%;margin-top:10px}
.ghost-btn.small-btn{padding:8px 12px;font-size:12px;flex:0 0 auto}
.save-btn:disabled,.upload-btn:disabled,.ghost-btn:disabled{opacity:.4;cursor:not-allowed}
.save-btn:hover:not(:disabled),.upload-btn:hover:not(:disabled){background:#c41530}
.ghost-btn:hover:not(:disabled){background:#24324d}
.stats{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:14px}
.stat{background:#1a2740;border-radius:10px;padding:14px;text-align:center}
.stat-val{font-size:28px;font-weight:800;color:#38bdf8}
.stat-label{font-size:11px;color:#64748b;margin-top:2px;letter-spacing:1px}
.stat-val.green{color:#22c55e}
.stat-val.amber{color:#eab308}
.status-row{display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid #1e293b;font-size:14px;gap:12px}
.status-row:last-child{border-bottom:none}
.status-text{max-width:55%;text-align:right;word-break:break-all}
.status-wide{max-width:68%}
.status-ok{color:#22c55e;font-weight:700}
.status-err{color:#ef4444;font-weight:700}
.status-warn{color:#f59e0b;font-weight:700}
.status-yes{color:#22c55e;font-weight:700}
.status-no{color:#64748b;font-weight:700}
.ota-row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
.file-btn{background:#1e293b;color:#94a3b8;border:1px solid #334155;border-radius:8px;padding:8px 14px;font-size:12px;cursor:pointer}
.file-name{font-size:12px;color:#64748b;flex:1}
.progress{width:100%;height:6px;background:#1e293b;border-radius:3px;margin-top:10px;display:none}
.progress-bar{height:100%;background:#22c55e;border-radius:3px;width:0%;transition:width .3s}
.msg{text-align:center;font-size:12px;margin-top:8px;min-height:16px}
.msg.ok{color:#22c55e}
.msg.err{color:#ef4444}
.inline-actions{display:flex;align-items:center;justify-content:space-between;gap:10px}
.inline-actions .field-label{margin-bottom:0}
.section-head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-top:14px}
.section-head .field-label{margin-bottom:0}
.saved-list{display:flex;flex-direction:column;gap:10px;margin-top:12px}
.saved-item{display:flex;align-items:center;justify-content:space-between;gap:12px;background:#1a2740;border:1px solid #334155;border-radius:10px;padding:12px}
.saved-main{min-width:0;flex:1}
.saved-name{font-size:14px;font-weight:600;word-break:break-all}
.saved-tags{display:flex;gap:6px;flex-wrap:wrap;margin-top:6px}
.tag{display:inline-flex;align-items:center;padding:4px 8px;border-radius:999px;background:#273449;color:#94a3b8;font-size:11px;line-height:1}
.tag.ok{background:rgba(34,197,94,.16);color:#86efac}
.tag.busy{background:rgba(56,189,248,.16);color:#7dd3fc}
.tag.warn{background:rgba(234,179,8,.16);color:#fde68a}
.empty-box{border:1px dashed #334155;border-radius:10px;padding:12px;text-align:center;font-size:12px;color:#64748b}
.picker-modal{position:fixed;inset:0;display:none;align-items:flex-end;justify-content:center;background:rgba(2,8,23,.72);z-index:9999;padding:16px}
.picker-modal.open{display:flex}
.picker-sheet{width:min(560px,100%);max-height:min(80vh,640px);background:#111827;border:1px solid rgba(56,189,248,.12);border-radius:18px;box-shadow:0 28px 70px rgba(2,8,23,.45);overflow:hidden}
.picker-head{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:16px 18px;border-bottom:1px solid #1e293b}
.picker-title{font-size:15px;font-weight:700;color:#e2e8f0}
.picker-close{background:transparent;border:none;color:#94a3b8;font-size:22px;line-height:1;cursor:pointer;padding:4px 6px}
.picker-body{padding:10px;overflow:auto;max-height:min(60vh,520px)}
.picker-option{width:100%;display:flex;align-items:center;justify-content:space-between;gap:12px;padding:14px 16px;border:none;border-radius:12px;background:#172033;color:#e2e8f0;font-size:15px;text-align:left;cursor:pointer}
.picker-option+.picker-option{margin-top:8px}
.picker-option.active{background:rgba(56,189,248,.18);color:#7dd3fc}
.picker-option:after{content:"";width:10px;height:10px;border-radius:999px;border:2px solid currentColor;opacity:.25}
.picker-option.active:after{opacity:1;background:currentColor}
@media (min-width:980px){
body{padding:24px}
.page-header{padding:18px 0 28px}
h1{font-size:30px}
.page-subtitle{font-size:14px}
.dashboard{grid-template-columns:1fr 1.1fr 1.1fr;gap:18px}
.card-controls{grid-column:1;grid-row:1}
.card-status{grid-column:2 / span 2;grid-row:1}
.card-hotspot{grid-column:1 / -1;grid-row:2}
.card-dns{grid-column:1 / -1;grid-row:3}
.card-ota{grid-column:1 / -1;grid-row:4}
.card{padding:20px}
.row,.status-row{padding:13px 0}
.row select{width:clamp(132px,42%,190px);flex:0 0 clamp(132px,42%,190px)}
.status-text{max-width:60%}
}
@media (max-width:640px){
body{padding:12px}
.page-header{padding:6px 0 18px}
.page-subtitle{font-size:12px}
.inline-actions,.section-head{align-items:flex-start;flex-direction:column}
.actions{flex-direction:column}
.saved-item{align-items:flex-start;flex-direction:column}
.saved-item .ghost-btn.small-btn{align-self:flex-end}
}
</style>
</head>
<body>

<div class="page">
<div class="page-header">
  <h1>FSD 控制器</h1>
  <div class="page-subtitle">桌面端与移动端统一适配，便于管理热点转发、DNS 白名单和设备状态。</div>
</div>
<div class="dashboard">

<div class="card card-controls">
  <div class="card-title">控制</div>
  <div class="row">
    <span class="row-label">FSD 开关</span>
    <label class="toggle"><input type="checkbox" id="fsdEnable" checked onchange="setVal('fsdEnable',this.checked?1:0)"><span class="slider"></span></label>
  </div>
	<div class="row">
	    <span class="row-label">硬件版本</span>
	    <div class="picker-wrap">
	      <select id="hwMode" class="picker-native" data-picker-title="硬件版本" data-picker-trigger="hwModeBtn" onchange="setVal('hwMode',this.value)">
	        <option value="0">LEGACY</option>
	        <option value="1">HW3</option>
	        <option value="2" selected>HW4</option>
	      </select>
	      <button type="button" class="picker-trigger" id="hwModeBtn" onclick="openPicker('hwMode')"></button>
	    </div>
	  </div>
	  <div class="row">
	    <span class="row-label">速度模式</span>
	    <div class="picker-wrap">
	      <select id="speedProfile" class="picker-native" data-picker-title="速度模式" data-picker-trigger="speedProfileBtn" onchange="setVal('speedProfile',this.value)">
	        <option value="0">保守</option>
	        <option value="1" selected>默认</option>
	        <option value="2">适中</option>
	        <option value="3">激进</option>
	        <option value="4">最大</option>
	      </select>
	      <button type="button" class="picker-trigger" id="speedProfileBtn" onclick="openPicker('speedProfile')"></button>
	    </div>
	  </div>
	  <div class="row">
	    <span class="row-label">模式来源</span>
	    <div class="picker-wrap">
	      <select id="profileMode" class="picker-native" data-picker-title="模式来源" data-picker-trigger="profileModeBtn" onchange="setVal('profileMode',this.value)">
	        <option value="1" selected>自动（拨杆）</option>
	        <option value="0">手动</option>
	      </select>
	      <button type="button" class="picker-trigger" id="profileModeBtn" onclick="openPicker('profileMode')"></button>
	    </div>
	  </div>
  <div class="row">
    <span class="row-label">HW3 速度偏移</span>
    <label class="toggle"><input type="checkbox" id="speedOffsetEnable" onchange="setVal('speedOffsetEnable',this.checked?1:0)"><span class="slider"></span></label>
  </div>
	  <div class="field">
	    <label class="field-label" for="speedOffsetPct">偏移百分比（+10% 表示高于当前限速 10%）</label>
	    <div class="picker-wrap">
	      <select id="speedOffsetPct" class="picker-native" data-picker-title="偏移百分比" data-picker-trigger="speedOffsetPctBtn" onchange="setVal('speedOffsetPct',this.value)">
	        <option value="0" selected>+0%</option>
	        <option value="5">+5%</option>
	        <option value="10">+10%</option>
	        <option value="15">+15%</option>
	        <option value="20">+20%</option>
	        <option value="25">+25%</option>
	        <option value="30">+30%</option>
	        <option value="35">+35%</option>
	        <option value="40">+40%</option>
	        <option value="45">+45%</option>
	        <option value="50">+50%</option>
	      </select>
	      <button type="button" class="picker-trigger" id="speedOffsetPctBtn" onclick="openPicker('speedOffsetPct')"></button>
	    </div>
	  </div>
  <div class="hint">仅 HW3 生效。启用后会按 release 版同样的编码方式注入速度偏移，建议先从 +5% 或 +10% 开始测试。</div>
  <div class="row">
    <span class="row-label">限速提示音抑制</span>
    <label class="toggle"><input type="checkbox" id="isaChime" onchange="setVal('isaChime',this.checked?1:0)"><span class="slider"></span></label>
  </div>
  <div class="row">
    <span class="row-label">紧急车辆检测</span>
    <label class="toggle"><input type="checkbox" id="emergencyDet" checked onchange="setVal('emergencyDet',this.checked?1:0)"><span class="slider"></span></label>
  </div>
  <div class="row">
    <span class="row-label">中国模式 🇨🇳</span>
    <label class="toggle"><input type="checkbox" id="chinaMode" onchange="setVal('chinaMode',this.checked?1:0)"><span class="slider"></span></label>
  </div>
</div>

<div class="card card-feature card-hotspot">
  <div class="card-title">上游热点</div>
  <div class="field">
    <label class="field-label" for="apSSID">本地热点名称</label>
    <input class="text-input" type="text" id="apSSID" maxlength="32" placeholder="例如：FSD-Controller" oninput="markApDirty()">
  </div>
  <div class="field">
    <label class="field-label" for="apPass">本地热点密码</label>
    <input class="text-input" type="password" id="apPass" maxlength="63" placeholder="8-63 个字符" oninput="markApDirty()">
  </div>
  <div class="actions">
    <button class="save-btn" onclick="saveApConfig()">保存本地热点</button>
  </div>
  <div class="msg" id="apMsg"></div>
  <div class="hint">保存后设备会重新启动本地 AP。当前连接会断开，请用新的热点名称和密码重新连接。</div>
  <div class="row">
    <span class="row-label">启用手机热点接入</span>
    <label class="toggle"><input type="checkbox" id="upstreamEnable" onchange="setUpstreamEnabled(this.checked)"><span class="slider"></span></label>
  </div>
	  <div class="field">
	    <div class="inline-actions">
	      <label class="field-label" for="scanResults">搜索附近热点</label>
	      <button class="ghost-btn small-btn" id="scanBtn" onclick="scanUpstreamNetworks()">搜索热点</button>
	    </div>
	    <div class="picker-wrap">
	      <select id="scanResults" class="picker-native" data-picker-title="选择附近热点" data-picker-trigger="scanResultsBtn">
	        <option value="">点击“搜索热点”查看附近可用热点</option>
	      </select>
	      <button type="button" class="picker-trigger" id="scanResultsBtn" onclick="openPicker('scanResults')"></button>
	    </div>
	  </div>
  <div class="field">
    <label class="field-label" for="upstreamPass">热点密码</label>
    <input class="text-input" type="password" id="upstreamPass" maxlength="63" placeholder="首次添加或更新密码时填写">
  </div>
  <div class="actions">
    <button class="save-btn" onclick="saveSelectedUpstream()">保存选中热点</button>
  </div>
  <div class="hint">支持保存多个热点。设备会自动跳过本机发射的 AP，并在已保存热点里优先尝试当前能连上的那个。</div>
  <div class="hint">上游热点连通后，ESP32 会把本地 AP 的客户端流量做 NAT 转发到外网。</div>
  <div class="hint">如需配合 DNS 白名单，请让客户端保持默认 DNS，不要手动改成公网 DNS。</div>
  <div class="saved-list" id="savedNetworks"></div>
  <div class="msg" id="netMsg"></div>
  <div class="status-row"><span>上游状态</span><span id="sUpstream" class="status-no status-text">--</span></div>
  <div class="status-row"><span>当前热点</span><span id="sCurrentUpstream" class="status-no status-text status-wide">--</span></div>
  <div class="status-row"><span>已保存热点</span><span id="sSavedUpstreams" class="status-no status-text">0</span></div>
  <div class="status-row"><span>上游 RSSI</span><span id="sUpstreamRSSI" class="status-no status-text">--</span></div>
  <div class="status-row"><span>信号质量</span><span id="sUpstreamSignal" class="status-no status-text">--</span></div>
  <div class="status-row"><span>当前信道</span><span id="sWiFiChannel" class="status-no status-text">--</span></div>
  <div class="status-row"><span>AP 客户端</span><span id="sAPClients" class="status-no status-text">0</span></div>
  <div class="status-row"><span>上游 IP</span><span id="sUpstreamIP" class="status-no status-text">--</span></div>
  <div class="status-row"><span>NAT 转发</span><span id="sNAT" class="status-no status-text">--</span></div>
  <div class="status-row"><span>本地 AP</span><span id="sAP" class="status-ok status-text">--</span></div>
  <div class="status-row"><span>AP 地址</span><span id="sAPIP" class="status-ok status-text">--</span></div>
</div>

<div class="card card-feature card-dns">
  <div class="card-title">DNS 规则</div>
  <div class="row">
    <span class="row-label">启用 DNS 规则</span>
    <label class="toggle"><input type="checkbox" id="dnsWhitelistEnable" onchange="markDnsDirty()"><span class="slider"></span></label>
  </div>
  <div class="field">
    <label class="field-label" for="dnsAllowlist">允许解析的域名</label>
    <textarea class="text-input text-area" id="dnsAllowlist" maxlength="384" placeholder="每行一个域名，例如：&#10;tesla.com&#10;apple.com" oninput="markDnsDirty()"></textarea>
  </div>
  <div class="field">
    <label class="field-label" for="dnsBlocklist">禁止解析的域名</label>
    <textarea class="text-input text-area" id="dnsBlocklist" maxlength="384" placeholder="每行一个域名，例如：&#10;google.com&#10;doubleclick.net" oninput="markDnsDirty()"></textarea>
  </div>
  <div class="hint">支持逗号、空格或换行分隔。填写 `tesla.com` 会同时允许 `api.tesla.com` 这类子域名。</div>
  <div class="hint">黑名单优先于白名单。若白名单留空，则表示“除黑名单外全部放行”；白名单非空时，只允许解析白名单域名。</div>
  <div class="hint">这项规则只影响连到 ESP32 本地 AP 且把 ESP32 当成 DNS 的设备；命中黑名单或未命中白名单的域名会被拒绝。</div>
  <div class="actions">
    <button class="save-btn" onclick="saveDns()">保存 DNS 规则</button>
  </div>
  <div class="msg" id="dnsMsg"></div>
  <div class="status-row"><span>规则状态</span><span id="sDNSMode" class="status-no status-text">--</span></div>
  <div class="status-row"><span>白名单数量</span><span id="sDNSCount" class="status-no status-text">0</span></div>
  <div class="status-row"><span>黑名单数量</span><span id="sDNSBlockCount" class="status-no status-text">0</span></div>
  <div class="status-row"><span>拦截总数</span><span id="sDNSBlocked" class="status-no status-text">0</span></div>
  <div class="section-head">
    <div class="field-label">最近被拦截请求</div>
    <button class="ghost-btn small-btn" onclick="clearBlockedDns()">清空记录</button>
  </div>
  <div class="hint">这里按域名聚合统计被拦截次数，并按次数从小到大显示；设备重启后会清空。</div>
  <div class="saved-list" id="dnsBlockedList"></div>
</div>

<div class="card card-status">
  <div class="card-title">状态</div>
  <div class="stats">
    <div class="stat"><div class="stat-val green" id="sModified">0</div><div class="stat-label">已修改</div></div>
    <div class="stat"><div class="stat-val" id="sRX">0</div><div class="stat-label">已接收</div></div>
    <div class="stat"><div class="stat-val amber" id="sErrors">0</div><div class="stat-label">错误</div></div>
    <div class="stat"><div class="stat-val" id="sUptime">0秒</div><div class="stat-label">运行时间</div></div>
  </div>
  <div class="status-row"><span>CAN 总线</span><span id="sCAN" class="status-no status-text">--</span></div>
  <div class="status-row"><span>FSD 已触发</span><span id="sFSD" class="status-no status-text">--</span></div>
  <div class="status-row"><span>芯片温度</span><span id="sChipTemp" class="status-no status-text">--</span></div>
  <div class="status-row"><span>温控状态</span><span id="sThermal" class="status-no status-text status-wide">--</span></div>
</div>

<div class="card card-full card-ota">
  <div class="card-title">固件更新</div>
  <div class="ota-row">
    <label class="file-btn" for="fwFile">选择文件</label>
    <input type="file" id="fwFile" accept=".bin" style="display:none" onchange="fileChosen(this)">
    <span class="file-name" id="fileName">未选择文件</span>
  </div>
  <button class="upload-btn" id="uploadBtn" disabled onclick="doOTA()">上传固件</button>
  <div class="progress" id="progWrap"><div class="progress-bar" id="progBar"></div></div>
  <div class="msg" id="otaMsg"></div>
</div>

</div>
</div>

<div class="picker-modal" id="pickerModal" onclick="closePicker(event)">
  <div class="picker-sheet" onclick="event.stopPropagation()">
    <div class="picker-head">
      <div class="picker-title" id="pickerTitle">请选择</div>
      <button type="button" class="picker-close" onclick="closePicker()">&times;</button>
    </div>
    <div class="picker-body" id="pickerBody"></div>
  </div>
</div>

<script>
let dnsDirty=false;
let apDirty=false;
let scanResults=[];
let pendingScanResultsRender=false;
let latestBlockedDnsRequests=[];
let latestStatusUptime=0;
let activePickerId='';

function markDnsDirty(){
  dnsDirty=true;
  renderBlockedDnsRequests(latestBlockedDnsRequests,latestStatusUptime);
}

function markApDirty(){
  apDirty=true;
}

function setStatusText(id,text,className){
  const el=document.getElementById(id);
  el.textContent=text;
  el.className=className+' status-text';
}

function setWideStatusText(id,text,className){
  const el=document.getElementById(id);
  el.textContent=text;
  el.className=className+' status-text status-wide';
}

function formatChipTemp(current,average){
  const currentValid=typeof current==='number'&&Number.isFinite(current);
  const avgValid=typeof average==='number'&&Number.isFinite(average);
  if(!currentValid&&!avgValid)return '--';
  if(currentValid&&avgValid)return current.toFixed(1)+'°C / 平均 '+average.toFixed(1)+'°C';
  if(currentValid)return current.toFixed(1)+'°C';
  return '平均 '+average.toFixed(1)+'°C';
}

function getSignalClass(rssi){
  if(typeof rssi!=='number'||!Number.isFinite(rssi))return 'status-no';
  if(rssi>=-55)return 'status-ok';
  if(rssi>=-67)return 'status-ok';
  if(rssi>=-75)return 'status-warn';
  return 'status-err';
}

function syncNetworkForm(d){
  if(!apDirty){
    document.getElementById('apSSID').value=d.apSSID||'';
    document.getElementById('apPass').value=d.apPassword||'';
  }
  document.getElementById('upstreamEnable').checked=!!d.upstreamEnable;
  const savedNetworks=Array.isArray(d.upstreamNetworks)?d.upstreamNetworks:[];
  let scanSavedStateChanged=false;
  scanResults.forEach(net=>{
    const nextSaved=savedNetworks.some(saved=>saved.ssid===net.ssid);
    if(net.saved!==nextSaved){
      net.saved=nextSaved;
      scanSavedStateChanged=true;
    }
  });
  if(scanSavedStateChanged){
    refreshScanResultsSelect();
  }
  renderSavedNetworks(savedNetworks);
}

function syncDnsForm(d){
  if(dnsDirty)return;
  document.getElementById('dnsWhitelistEnable').checked=!!d.dnsWhitelistEnable;
  document.getElementById('dnsAllowlist').value=d.dnsAllowlist||'';
  document.getElementById('dnsBlocklist').value=d.dnsBlocklist||'';
}

function setNetMessage(text,type){
  const msg=document.getElementById('netMsg');
  msg.textContent=text;
  msg.className='msg'+(type?' '+type:'');
}

function setApMessage(text,type){
  const msg=document.getElementById('apMsg');
  msg.textContent=text;
  msg.className='msg'+(type?' '+type:'');
}

function normalizeDomain(domain){
  let normalized=(domain||'').trim().toLowerCase();
  while(normalized.endsWith('.'))normalized=normalized.slice(0,-1);
  return normalized;
}

function getDnsRulesFromTextarea(id){
  return (document.getElementById(id).value||'')
    .split(/[\s,;]+/)
    .map(normalizeDomain)
    .filter(Boolean);
}

function ruleMatchesDomain(domain,rule){
  if(!domain||!rule)return false;
  return domain===rule||(domain.length>rule.length&&domain.endsWith('.'+rule));
}

function isDomainAlreadyAllowed(domain){
  const normalizedDomain=normalizeDomain(domain);
  if(!normalizedDomain)return false;
  return getDnsRulesFromTextarea('dnsAllowlist').some(rule=>ruleMatchesDomain(normalizedDomain,rule));
}

function isDomainBlockedByBlacklist(domain){
  const normalizedDomain=normalizeDomain(domain);
  if(!normalizedDomain)return false;
  return getDnsRulesFromTextarea('dnsBlocklist').some(rule=>ruleMatchesDomain(normalizedDomain,rule));
}

function renderScanResults(){
  const select=document.getElementById('scanResults');
  const current=select.value;
  select.innerHTML='';

  if(!scanResults.length){
    const opt=document.createElement('option');
    opt.value='';
    opt.textContent='点击“搜索热点”查看附近可用热点';
    select.appendChild(opt);
    syncPickerButton('scanResults');
    return;
  }

  const placeholder=document.createElement('option');
  placeholder.value='';
  placeholder.textContent='请选择要保存的热点';
  select.appendChild(placeholder);

  scanResults.forEach(net=>{
    const opt=document.createElement('option');
    opt.value=net.ssid;
    let label=net.ssid;
    if(typeof net.rssi==='number')label+=' · '+net.rssi+' dBm';
    if(net.saved)label+=' · 已保存';
    opt.textContent=label;
    select.appendChild(opt);
  });

  if(scanResults.some(net=>net.ssid===current)){
    select.value=current;
  }
  syncPickerButton('scanResults');
}

function refreshScanResultsSelect(force){
  const select=document.getElementById('scanResults');
  if(!force&&document.activeElement===select){
    pendingScanResultsRender=true;
    return;
  }
  pendingScanResultsRender=false;
  renderScanResults();
}

function appendTag(parent,text,className){
  const tag=document.createElement('span');
  tag.className='tag'+(className?' '+className:'');
  tag.textContent=text;
  parent.appendChild(tag);
}

function renderSavedNetworks(networks){
  const wrap=document.getElementById('savedNetworks');
  wrap.innerHTML='';

  if(!Array.isArray(networks)||!networks.length){
    const empty=document.createElement('div');
    empty.className='empty-box';
    empty.textContent='还没有保存热点。先搜索附近热点，再保存需要自动连接的候选热点。';
    wrap.appendChild(empty);
    return;
  }

  networks.forEach(net=>{
    const item=document.createElement('div');
    item.className='saved-item';

    const main=document.createElement('div');
    main.className='saved-main';

    const name=document.createElement('div');
    name.className='saved-name';
    name.textContent=net.ssid;
    main.appendChild(name);

    const tags=document.createElement('div');
    tags.className='saved-tags';
    if(net.connected)appendTag(tags,'已连接','ok');
    else if(net.active)appendTag(tags,'连接中','busy');
    else appendTag(tags,'已保存','');
    if(!net.hasPass)appendTag(tags,'无密码','warn');
    main.appendChild(tags);

    const delBtn=document.createElement('button');
    delBtn.className='ghost-btn small-btn';
    delBtn.textContent='删除';
    delBtn.onclick=()=>deleteSavedUpstream(net.ssid);

    item.appendChild(main);
    item.appendChild(delBtn);
    wrap.appendChild(item);
  });
}

function formatRelativeTime(seconds){
  const delta=Math.max(0,seconds||0);
  if(delta<60)return delta+'秒前';
  if(delta<3600)return Math.floor(delta/60)+'分前';
  if(delta<86400)return Math.floor(delta/3600)+'小时前';
  return Math.floor(delta/86400)+'天前';
}

function renderBlockedDnsRequests(requests,currentUptime){
  const wrap=document.getElementById('dnsBlockedList');
  wrap.innerHTML='';

  if(!Array.isArray(requests)||!requests.length){
    const empty=document.createElement('div');
    empty.className='empty-box';
    empty.textContent='暂时没有被拦截域名统计。';
    wrap.appendChild(empty);
    return;
  }

  requests.forEach(item=>{
    const row=document.createElement('div');
    row.className='saved-item';
    const normalizedDomain=normalizeDomain(item.domain);
    const blockedByBlacklist=isDomainBlockedByBlacklist(normalizedDomain);
    const alreadyAllowed=isDomainAlreadyAllowed(normalizedDomain);

    const main=document.createElement('div');
    main.className='saved-main';

    const name=document.createElement('div');
    name.className='saved-name';
    name.textContent=item.domain||'(未知域名)';
    main.appendChild(name);

    const tags=document.createElement('div');
    tags.className='saved-tags';
    appendTag(tags,String(item.count||0)+'次','warn');
    appendTag(tags,formatRelativeTime((currentUptime||0)-(item.lastBlockedAt||0)),'busy');
    main.appendChild(tags);

    const addBtn=document.createElement('button');
    addBtn.className='ghost-btn small-btn';
    addBtn.textContent=blockedByBlacklist?'已在黑名单':(alreadyAllowed?'已在白名单':'加入白名单');
    addBtn.disabled=blockedByBlacklist||alreadyAllowed||!normalizedDomain;
    addBtn.onclick=()=>addBlockedDomainToAllowlist(normalizedDomain);

    row.appendChild(main);
    row.appendChild(addBtn);
    wrap.appendChild(row);
  });
}

function poll(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('sModified').textContent=d.modified;
    document.getElementById('sRX').textContent=d.rx;
    document.getElementById('sErrors').textContent=d.errors;
    let u=d.uptime;
    let h=Math.floor(u/3600),m=Math.floor((u%3600)/60),s=u%60;
    document.getElementById('sUptime').textContent=h>0?h+'时'+m+'分':m>0?m+'分'+s+'秒':s+'秒';

    let canEl=document.getElementById('sCAN');
    canEl.textContent=d.canOK?'正常':'异常';
    canEl.className=(d.canOK?'status-ok':'status-err')+' status-text';

    let fsdEl=document.getElementById('sFSD');
    fsdEl.textContent=d.fsdTriggered?'是':'否';
    fsdEl.className=(d.fsdTriggered?'status-yes':'status-no')+' status-text';

    let thermalClass='status-ok';
    if(d.thermalProtect)thermalClass='status-err';
    else if((d.thermalStatus||'').includes('降频')||(d.thermalStatus||'').includes('偏高'))thermalClass='status-warn';
    let signalClass=getSignalClass(d.upstreamRSSI);

    document.getElementById('fsdEnable').checked=!!d.fsdEnable;
    document.getElementById('hwMode').value=d.hwMode;
    document.getElementById('speedProfile').value=d.speedProfile;
    document.getElementById('profileMode').value=d.profileMode?'1':'0';
    document.getElementById('speedOffsetEnable').checked=!!d.speedOffsetEnable;
    document.getElementById('speedOffsetPct').value=String(d.speedOffsetPct||0);
    document.getElementById('isaChime').checked=!!d.isaChime;
    document.getElementById('emergencyDet').checked=!!d.emergencyDet;
    document.getElementById('chinaMode').checked=!!d.chinaMode;

    const isHw3=String(d.hwMode)==='1';
    document.getElementById('speedOffsetEnable').disabled=!isHw3;
    document.getElementById('speedOffsetPct').disabled=!isHw3||!d.speedOffsetEnable;
    syncPickerButton('hwMode');
    syncPickerButton('speedProfile');
    syncPickerButton('profileMode');
    syncPickerButton('speedOffsetPct');
    syncPickerButton('scanResults');

    syncNetworkForm(d);
    syncDnsForm(d);
    setStatusText('sUpstream',d.upstreamStatus||'--',d.upstreamConnected?'status-ok':(d.upstreamEnable?'status-err':'status-no'));
    setWideStatusText('sCurrentUpstream',d.connectedUpstreamSSID||d.upstreamSSID||'--',d.upstreamConnected?'status-ok':'status-no');
    setStatusText('sSavedUpstreams',String(d.upstreamSavedCount||0),d.upstreamSavedCount?'status-ok':'status-no');
    setStatusText('sUpstreamRSSI',typeof d.upstreamRSSI==='number'&&Number.isFinite(d.upstreamRSSI)?String(d.upstreamRSSI)+' dBm':'--',signalClass);
    setStatusText('sUpstreamSignal',d.upstreamSignal||'--',signalClass);
    setStatusText('sWiFiChannel',d.wifiChannel?String(d.wifiChannel):'--',d.wifiChannel?'status-ok':'status-no');
    setStatusText('sAPClients',String(d.apClients||0),(d.apClients||0)>1?'status-warn':'status-ok');
    setStatusText('sUpstreamIP',d.upstreamIP||'--',d.upstreamConnected?'status-ok':'status-no');
    setStatusText('sNAT',d.natStatus||'--',d.natEnabled?'status-ok':(d.upstreamConnected?'status-err':'status-no'));
    setStatusText('sAP',d.apSSID||'--','status-ok');
    setStatusText('sAPIP',d.apIP||'--','status-ok');
    setWideStatusText('sDNSMode',d.dnsWhitelistEnable?'已启用':'未启用',d.dnsWhitelistEnable?'status-ok':'status-no');
    setStatusText('sDNSCount',String(d.dnsWhitelistCount||0),d.dnsWhitelistCount?'status-ok':'status-no');
    setStatusText('sDNSBlockCount',String(d.dnsBlacklistCount||0),d.dnsBlacklistCount?'status-err':'status-no');
    setStatusText('sDNSBlocked',String(d.dnsBlockedCount||0),d.dnsBlockedCount?'status-err':'status-no');
    setWideStatusText('sChipTemp',formatChipTemp(d.chipTempC,d.chipTempAvgC),thermalClass);
    setWideStatusText('sThermal',d.thermalStatus||'--',thermalClass);
    latestBlockedDnsRequests=Array.isArray(d.dnsBlockedRequests)?d.dnsBlockedRequests:[];
    latestStatusUptime=d.uptime||0;
    renderBlockedDnsRequests(latestBlockedDnsRequests,latestStatusUptime);
  }).catch(()=>{});
}

function setVal(key,val){
  fetch('/api/set?'+key+'='+val).catch(()=>{});
}

function getSelectDisplayText(select){
  if(!select||!select.options.length)return '请选择';
  const option=select.options[select.selectedIndex>=0?select.selectedIndex:0];
  return option&&option.textContent?option.textContent:'请选择';
}

function syncPickerButton(selectId){
  const select=document.getElementById(selectId);
  if(!select)return;
  const btnId=select.dataset.pickerTrigger;
  if(!btnId)return;
  const btn=document.getElementById(btnId);
  if(!btn)return;
  btn.textContent=getSelectDisplayText(select);
  btn.disabled=!!select.disabled;
}

function openPicker(selectId){
  const select=document.getElementById(selectId);
  if(!select||select.disabled)return;
  activePickerId=selectId;
  document.getElementById('pickerTitle').textContent=select.dataset.pickerTitle||'请选择';
  const body=document.getElementById('pickerBody');
  body.innerHTML='';
  for(let i=0;i<select.options.length;i++){
    const option=select.options[i];
    if(option.disabled)continue;
    const btn=document.createElement('button');
    btn.type='button';
    btn.className='picker-option'+(option.value===select.value?' active':'');
    btn.textContent=option.textContent;
    btn.onclick=()=>choosePickerValue(selectId,option.value);
    body.appendChild(btn);
  }
  document.getElementById('pickerModal').classList.add('open');
}

function choosePickerValue(selectId,value){
  const select=document.getElementById(selectId);
  if(!select)return;
  select.value=value;
  syncPickerButton(selectId);
  select.dispatchEvent(new Event('change',{bubbles:true}));
  closePicker();
}

function closePicker(evt){
  if(evt&&evt.target&&evt.target!==document.getElementById('pickerModal'))return;
  activePickerId='';
  document.getElementById('pickerModal').classList.remove('open');
}

function setUpstreamEnabled(enabled){
  setNetMessage('保存中...','');
  fetch('/api/set?upstreamEnable='+(enabled?'1':'0')).then(r=>{
    if(!r.ok)throw new Error('save failed');
    return r.text();
  }).then(()=>{
    setNetMessage(enabled?'热点接入已启用':'热点接入已关闭','ok');
    poll();
  }).catch(()=>{
    setNetMessage('保存失败','err');
    poll();
  });
}

function saveApConfig(){
  const ssid=(document.getElementById('apSSID').value||'').trim();
  const pass=document.getElementById('apPass').value||'';
  if(!ssid){
    setApMessage('热点名称不能为空','err');
    return;
  }
  if(ssid.length>32){
    setApMessage('热点名称最多 32 个字符','err');
    return;
  }
  if(pass.length<8||pass.length>63){
    setApMessage('热点密码长度必须为 8-63 个字符','err');
    return;
  }
  const params=new URLSearchParams();
  params.set('apSSID',ssid);
  params.set('apPass',pass);
  setApMessage('保存中，热点将重新启动...','');
  fetch('/api/set?'+params.toString()).then(async r=>{
    if(!r.ok){
      const txt=await r.text();
      throw new Error(txt||'save failed');
    }
    return r.text();
  }).then(()=>{
    apDirty=false;
    setApMessage('本地热点设置已保存，约 1 秒后会切换到新的名称和密码','ok');
  }).catch(err=>{
    setApMessage(err.message||'保存失败','err');
  });
}

function scanUpstreamNetworks(){
  const btn=document.getElementById('scanBtn');
  btn.disabled=true;
  setNetMessage('搜索中...','');

  fetch('/api/upstream/scan').then(r=>{
    if(!r.ok){
      return r.text().then(t=>{throw new Error(t||'scan failed');});
    }
    return r.json();
  }).then(d=>{
    scanResults=Array.isArray(d.results)?d.results:[];
    refreshScanResultsSelect(true);
    setNetMessage(scanResults.length?'已更新附近热点':'没有搜索到可用热点','ok');
  }).catch(err=>{
    setNetMessage(err.message||'搜索失败','err');
  }).finally(()=>{
    btn.disabled=false;
  });
}

function saveSelectedUpstream(){
  const ssid=document.getElementById('scanResults').value;
  const pass=document.getElementById('upstreamPass').value;

  if(!ssid){
    setNetMessage('请先选择要保存的热点','err');
    return;
  }

  const params=new URLSearchParams();
  params.set('ssid',ssid);
  if(pass)params.set('pass',pass);

  setNetMessage('保存中...','');

  fetch('/api/upstream/add?'+params.toString()).then(r=>{
    if(!r.ok){
      return r.text().then(t=>{throw new Error(t||'save failed');});
    }
    return r.text();
  }).then(()=>{
    document.getElementById('upstreamPass').value='';
    setNetMessage(document.getElementById('upstreamEnable').checked?'热点已保存，设备会自动切换到可连接的热点':'热点已保存，启用后会自动连接','ok');
    poll();
  }).catch(err=>{
    setNetMessage(err.message||'保存失败','err');
  });
}

function deleteSavedUpstream(ssid){
  if(!window.confirm('确定删除这个已保存热点吗？'))return;

  setNetMessage('删除中...','');
  fetch('/api/upstream/delete?ssid='+encodeURIComponent(ssid)).then(r=>{
    if(!r.ok){
      return r.text().then(t=>{throw new Error(t||'delete failed');});
    }
    return r.text();
  }).then(()=>{
    setNetMessage('已删除热点','ok');
    poll();
  }).catch(err=>{
    setNetMessage(err.message||'删除失败','err');
  });
}

['hwMode','speedProfile','profileMode','speedOffsetPct','scanResults'].forEach(syncPickerButton);

function persistDnsRules(successText){
  const msg=document.getElementById('dnsMsg');
  const params=new URLSearchParams();

  params.set('dnsWhitelistEnable',document.getElementById('dnsWhitelistEnable').checked?'1':'0');
  params.set('dnsAllowlist',document.getElementById('dnsAllowlist').value.trim());
  params.set('dnsBlocklist',document.getElementById('dnsBlocklist').value.trim());

  msg.textContent='保存中...';
  msg.className='msg';

  fetch('/api/set?'+params.toString()).then(r=>{
    if(!r.ok)throw new Error('save failed');
    return r.text();
  }).then(()=>{
    dnsDirty=false;
    msg.textContent=successText||'DNS 规则已保存';
    msg.className='msg ok';
    poll();
  }).catch(()=>{
    msg.textContent='保存失败';
    msg.className='msg err';
  });
}

function saveDns(){
  persistDnsRules('DNS 规则已保存');
}

function addBlockedDomainToAllowlist(domain){
  const normalizedDomain=normalizeDomain(domain);
  const msg=document.getElementById('dnsMsg');

  if(!normalizedDomain){
    msg.textContent='域名无效，无法加入白名单';
    msg.className='msg err';
    return;
  }

  if(isDomainAlreadyAllowed(normalizedDomain)){
    msg.textContent='这个域名已经在白名单里了';
    msg.className='msg ok';
    renderBlockedDnsRequests(latestBlockedDnsRequests,latestStatusUptime);
    return;
  }

  if(isDomainBlockedByBlacklist(normalizedDomain)){
    msg.textContent='这个域名已在黑名单里，黑名单优先，请先移除黑名单规则';
    msg.className='msg err';
    renderBlockedDnsRequests(latestBlockedDnsRequests,latestStatusUptime);
    return;
  }

  const textarea=document.getElementById('dnsAllowlist');
  const current=textarea.value.trim();
  textarea.value=current?current+'\n'+normalizedDomain:normalizedDomain;
  dnsDirty=true;
  renderBlockedDnsRequests(latestBlockedDnsRequests,latestStatusUptime);
  persistDnsRules('已加入白名单: '+normalizedDomain);
}

function clearBlockedDns(){
  const msg=document.getElementById('dnsMsg');
  msg.textContent='清空中...';
  msg.className='msg';

  fetch('/api/dns/blocked/clear').then(r=>{
    if(!r.ok)throw new Error('clear failed');
    return r.text();
  }).then(()=>{
    msg.textContent='拦截记录已清空';
    msg.className='msg ok';
    poll();
  }).catch(()=>{
    msg.textContent='清空失败';
    msg.className='msg err';
  });
}

function fileChosen(inp){
  document.getElementById('fileName').textContent=inp.files[0]?inp.files[0].name:'未选择文件';
  document.getElementById('uploadBtn').disabled=!inp.files[0];
}

function doOTA(){
  let file=document.getElementById('fwFile').files[0];
  if(!file)return;
  let xhr=new XMLHttpRequest();
  let prog=document.getElementById('progWrap');
  let bar=document.getElementById('progBar');
  let msg=document.getElementById('otaMsg');
  prog.style.display='block';
  bar.style.width='0%';
  msg.textContent='';
  msg.className='msg';
  document.getElementById('uploadBtn').disabled=true;
  xhr.upload.addEventListener('progress',e=>{if(e.lengthComputable)bar.style.width=Math.round(e.loaded/e.total*100)+'%';});
  xhr.onload=function(){
    if(xhr.status===200){msg.textContent='上传成功，正在重启...';msg.className='msg ok';}
    else{msg.textContent='上传失败: '+xhr.statusText;msg.className='msg err';document.getElementById('uploadBtn').disabled=false;}
  };
  xhr.onerror=function(){msg.textContent='连接失败';msg.className='msg err';document.getElementById('uploadBtn').disabled=false;};
  let form=new FormData();
  form.append('firmware',file);
  xhr.open('POST','/api/ota');
  xhr.send(form);
}

setInterval(poll,1000);
poll();
</script>
</body>
</html>
)rawliteral";

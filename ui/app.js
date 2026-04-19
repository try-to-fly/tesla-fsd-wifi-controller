let dnsDirty=false;
let apDirty=false;
let scanResults=[];
let pendingScanResultsRender=false;
let latestBlockedDnsRequests=[];
let latestStatusUptime=0;
let activePickerId='';
const hwModeLabels={
  '0':'LEGACY',
  '1':'HW3',
  '2':'HW4'
};

function setTextIfPresent(id,text){
  const el=document.getElementById(id);
  if(el)el.textContent=text;
}

function markDnsDirty(){
  dnsDirty=true;
  renderBlockedDnsRequests(latestBlockedDnsRequests,latestStatusUptime);
  syncDashboardSummary();
}

function markApDirty(){
  apDirty=true;
}

function setStatusText(id,text,className){
  const el=document.getElementById(id);
  if(!el)return;
  el.textContent=text;
  el.className=className+' status-text';
}

function setWideStatusText(id,text,className){
  const el=document.getElementById(id);
  if(!el)return;
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

function getHwModeLabel(value){
  return hwModeLabels[String(value)]||'--';
}

function syncDashboardSummary(data){
  const hwSelect=document.getElementById('hwMode');
  const offsetSelect=document.getElementById('speedOffsetPct');
  const offsetToggle=document.getElementById('speedOffsetEnable');
  const fsdToggle=document.getElementById('fsdEnable');
  const dnsToggle=document.getElementById('dnsWhitelistEnable');
  const currentUpstream=document.getElementById('sCurrentUpstream');
  const upstreamStatus=document.getElementById('sUpstream');
  const hwLabel=data?getHwModeLabel(data.hwMode):getSelectDisplayText(hwSelect);
  const offsetLabel=data?('+'+String(Number(data.speedOffsetPct||0))+'%'):getSelectDisplayText(offsetSelect);
  const isHw3=data?String(data.hwMode)==='1':hwLabel==='HW3';
  const offsetEnabled=data?!!data.speedOffsetEnable:!!(offsetToggle&&offsetToggle.checked);
  const fsdEnabled=data?!!data.fsdEnable:!!(fsdToggle&&fsdToggle.checked);
  const dnsEnabled=data?!!data.dnsWhitelistEnable:!!(dnsToggle&&dnsToggle.checked);
  const dnsCount=data?Number(data.dnsWhitelistCount||0):getDnsRulesFromTextarea('dnsAllowlist').length;
  const currentUpstreamText=(data?(data.connectedUpstreamSSID||data.upstreamSSID||''):((currentUpstream&&currentUpstream.textContent)||'')).trim();
  const upstreamStatusText=(data?(data.upstreamStatus||'--'):((upstreamStatus&&upstreamStatus.textContent)||'--')).trim();
  let offsetState='已关闭';
  if(!isHw3)offsetState='当前为 '+hwLabel+'，仅 HW3 可用';
  else if(offsetEnabled)offsetState='已启用，按当前限速上浮';

  setTextIfPresent('hwModeBadge','硬件 '+hwLabel);
  setTextIfPresent('speedOffsetDisplay',offsetLabel);
  setTextIfPresent('speedOffsetState',offsetState);
  setTextIfPresent('toolbarFsdMeta','硬件 '+hwLabel+' · '+(fsdEnabled?'已启用':'已关闭'));
  setTextIfPresent('toolbarDnsMeta',(dnsEnabled?'已启用':'未启用')+' · 白名单 '+dnsCount+' 条');
  setTextIfPresent('toolbarNetworkMeta',currentUpstreamText&&currentUpstreamText!=='--'?currentUpstreamText:'上游 '+(upstreamStatusText||'--'));
}

function syncSpeedOffsetOptions(){
  const select=document.getElementById('speedOffsetPct');
  const wrap=document.getElementById('speedOffsetOptions');
  if(!select||!wrap)return;
  const buttons=wrap.querySelectorAll('.offset-chip');
  buttons.forEach(btn=>{
    const isActive=btn.dataset.value===String(select.value);
    btn.classList.toggle('active',isActive);
    btn.disabled=!!select.disabled;
    btn.setAttribute('aria-pressed',isActive?'true':'false');
  });
}

function chooseSpeedOffsetPreset(value){
  const select=document.getElementById('speedOffsetPct');
  if(!select||select.disabled)return;
  select.value=String(value);
  select.dispatchEvent(new Event('change',{bubbles:true}));
  syncSpeedOffsetOptions();
  syncDashboardSummary();
}

function openDialog(id){
  const dialog=document.getElementById(id);
  if(dialog)dialog.classList.add('open');
}

function closeDialog(evt,id){
  const dialog=id?document.getElementById(id):(evt&&evt.target&&evt.target.classList.contains('dialog-modal')?evt.target:null);
  if(!dialog)return;
  if(evt&&evt.target&&evt.target!==dialog)return;
  dialog.classList.remove('open');
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
    syncSpeedOffsetOptions();
    syncPickerButton('hwMode');
    syncPickerButton('speedProfile');
    syncPickerButton('profileMode');
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
    syncDashboardSummary(d);
  }).catch(()=>{});
}

function setVal(key,val){
  fetch('/api/set?'+key+'='+val).catch(()=>{});
  if(key==='speedOffsetEnable'||key==='hwMode'){
    const select=document.getElementById('speedOffsetPct');
    const hwSelect=document.getElementById('hwMode');
    const offsetToggle=document.getElementById('speedOffsetEnable');
    if(select&&hwSelect){
      const nextHwMode=key==='hwMode'?String(val):String(hwSelect.value);
      const nextOffsetEnabled=key==='speedOffsetEnable'?!!Number(val):!!(offsetToggle&&offsetToggle.checked);
      select.disabled=nextHwMode!=='1'||!nextOffsetEnabled;
      syncSpeedOffsetOptions();
    }
  }
  syncDashboardSummary();
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
  syncDashboardSummary();
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

['hwMode','speedProfile','profileMode','scanResults'].forEach(syncPickerButton);
syncSpeedOffsetOptions();
syncDashboardSummary();

document.addEventListener('keydown',evt=>{
  if(evt.key!=='Escape')return;
  closePicker();
  document.querySelectorAll('.dialog-modal.open').forEach(dialog=>dialog.classList.remove('open'));
});

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

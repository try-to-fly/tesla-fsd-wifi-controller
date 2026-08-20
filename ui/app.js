let dnsDirty=false;
let apDirty=false;
let scanResults=[];
let pendingScanResultsRender=false;
let latestBlockedDnsRequests=[];
let latestStatusUptime=0;
let lastCanCounters=null;
let activePickerId='';
let vehicleSpeedHistory=[];
let lastVehicleSpeedUptimeMs=null;
let lastValidVehicleSpeedKph=null;
let diagnosticsPinned=false;
let diagnosticsHideTimer=0;
const COUNTER_WRAP=4294967296;
const VEHICLE_SPEED_HISTORY_WINDOW_MS=60000;
const MAX_VEHICLE_SPEED_POINTS=75;
const VEHICLE_SPEED_META_MAX_AGE_MS=30000;
const SVG_NS='http://www.w3.org/2000/svg';
const OTA_TRACE_POLL_MS=1200;
const OTA_TRACE_TIMEOUT_MS=20000;
let otaTraceTimer=0;
let otaTraceStartedAt=0;
let otaTraceGeneration=0;
let otaTraceController=null;
let otaTraceClientUploadComplete=false;
let otaTraceSawLikelySuccessPhase=false;
let otaClientUploadComplete=false;
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

function setMetricValue(id,text,className){
  const el=document.getElementById(id);
  if(!el)return;
  el.textContent=text;
  el.className='metric-value '+className;
}

function setSpeedInfoValue(id,text,className){
  const el=document.getElementById(id);
  if(!el)return;
  el.textContent=text;
  el.className='speed-info-value '+className;
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

function formatHeap(freeHeap,minFreeHeap){
  if(typeof freeHeap!=='number'||!Number.isFinite(freeHeap))return '--';
  const freeText=Math.round(freeHeap/1024)+'KB';
  if(typeof minFreeHeap==='number'&&Number.isFinite(minFreeHeap)){
    return freeText+' / min '+Math.round(minFreeHeap/1024)+'KB';
  }
  return freeText;
}

function heapClass(freeHeap,minFreeHeap){
  const value=typeof minFreeHeap==='number'&&Number.isFinite(minFreeHeap)?minFreeHeap:freeHeap;
  if(typeof value!=='number'||!Number.isFinite(value))return 'status-no';
  if(value<20000)return 'status-err';
  if(value<40000)return 'status-warn';
  return 'status-ok';
}

function resetReasonClass(reason){
  if(!reason||reason==='poweron'||reason==='software'||reason==='ext')return 'status-ok';
  if(reason==='unknown')return 'status-no';
  return 'status-err';
}

function formatDnsPolicy(d){
  if(!d||!d.dnsPolicyEnabled)return '关闭';
  if(d.dnsStrictAllow||d.dnsForwardPolicy==='strict-allow')return 'DNS 白名单';
  if(d.dnsForwardPolicy==='blocklist-only')return '仅拦黑名单';
  return '已启用';
}

function formatDnsIpCache(d){
  const allow=Number(d&&d.dnsAllowIpCount||0);
  const block=Number(d&&d.dnsBlockIpCount||0);
  return allow+' / '+block;
}

function formatActivityAge(ageMs){
  if(typeof ageMs!=='number'||!Number.isFinite(ageMs))return '--';
  if(ageMs<1000)return '不到 1 秒';
  if(ageMs<60000)return String(Math.max(1,Math.round(ageMs/1000)))+' 秒';
  if(ageMs<3600000)return String(Math.round(ageMs/60000))+' 分钟';
  return String(Math.round(ageMs/3600000))+' 小时';
}

function getCounterDelta(current,previous){
  if(typeof current!=='number'||!Number.isFinite(current))return null;
  if(typeof previous!=='number'||!Number.isFinite(previous))return null;
  if(current>=previous)return current-previous;
  return COUNTER_WRAP-previous+current;
}

function updateCounterCard(id,metaId,delta,isActive,ageMs,noun){
  if(delta===null){
    if(isActive){
      setMetricValue(id,'变化中','status-ok');
      setTextIfPresent(metaId,'已检测到'+noun+'，等待下一次速率采样');
    }else if(typeof ageMs==='number'&&Number.isFinite(ageMs)){
      setMetricValue(id,'空闲','status-no');
      setTextIfPresent(metaId,'上次'+noun+' '+formatActivityAge(ageMs)+'前');
    }else{
      setMetricValue(id,'等待','status-no');
      setTextIfPresent(metaId,'尚未检测到'+noun);
    }
    return;
  }

  if(delta>0){
    setMetricValue(id,'+'+String(delta)+'/s','status-ok');
    setTextIfPresent(metaId,'最近 1 秒持续'+noun);
    return;
  }

  setMetricValue(id,'空闲','status-no');
  if(typeof ageMs==='number'&&Number.isFinite(ageMs)){
    setTextIfPresent(metaId,'上次'+noun+' '+formatActivityAge(ageMs)+'前');
  }else{
    setTextIfPresent(metaId,'尚未检测到'+noun);
  }
}

function updateCanActivity(data){
  const rx=typeof data.rx==='number'?data.rx:Number(data.rx||0);
  const modified=typeof data.modified==='number'?data.modified:Number(data.modified||0);
  const rxDelta=lastCanCounters?getCounterDelta(rx,lastCanCounters.rx):null;
  const modifiedDelta=lastCanCounters?getCounterDelta(modified,lastCanCounters.modified):null;

  updateCounterCard('sRX','sRXMeta',rxDelta,!!data.rxActive,data.rxAgeMs,'收包');
  updateCounterCard('sModified','sModifiedMeta',modifiedDelta,!!data.modifiedActive,data.modifiedAgeMs,'改包');

  lastCanCounters={rx,modified};
}

function updateCanStatus(data){
  const canEl=document.getElementById('sCAN');
  if(!canEl)return;

  let text='异常';
  let className='status-err';
  let meta='TWAI 初始化失败';

  if(data.canReady){
    if(data.rxActive){
      text='活跃';
      className='status-ok';
      meta='最近'+formatActivityAge(data.rxAgeMs)+'有收包';
    }else if((typeof data.rx==='number'?data.rx:Number(data.rx||0))>0){
      text='静默';
      className='status-warn';
      meta='驱动已就绪，但最近 2 秒没有新 CAN';
    }else{
      text='等待';
      className='status-no';
      meta='驱动已就绪，等待第一帧 CAN';
    }
  }

  canEl.textContent=text;
  canEl.className=className+' status-text';
  setTextIfPresent('sCANMeta',meta);
}

function getHwModeLabel(value){
  return hwModeLabels[String(value)]||'--';
}

function getSpeedSourceLabel(value){
  switch(String(value)){
    case '1': return '视觉限速';
    case '2': return '融合限速';
    case '3': return '地图限速';
    default: return '';
  }
}

function getVehicleSpeedSourceLabel(value){
  switch(String(value)){
    case '1': return 'ESP 车身速度';
    case '2': return 'DI 仪表车速';
    default: return '';
  }
}

function formatVehicleSpeedAge(ageMs){
  if(typeof ageMs!=='number'||!Number.isFinite(ageMs)||ageMs<0)return '';
  if(ageMs<120)return '刚更新';
  if(ageMs<1000)return String(Math.round(ageMs/10)*10)+' ms前';
  if(ageMs<10000)return (ageMs/1000).toFixed(1)+' 秒前';
  if(ageMs<VEHICLE_SPEED_META_MAX_AGE_MS)return String(Math.round(ageMs/1000))+' 秒前';
  return '';
}

function formatVehicleSpeedValue(speedKph){
  if(typeof speedKph!=='number'||!Number.isFinite(speedKph))return '--';
  return speedKph>=100?String(Math.round(speedKph)):speedKph.toFixed(1);
}

function formatVehicleSpeedLabel(speedKph){
  if(typeof speedKph!=='number'||!Number.isFinite(speedKph))return '--';
  return formatVehicleSpeedValue(speedKph)+' km/h';
}

function trimVehicleSpeedHistory(uptimeMs){
  const minTime=Math.max(0,uptimeMs-VEHICLE_SPEED_HISTORY_WINDOW_MS);
  vehicleSpeedHistory=vehicleSpeedHistory.filter(point=>point.timeMs>=minTime);
  if(vehicleSpeedHistory.length>MAX_VEHICLE_SPEED_POINTS){
    vehicleSpeedHistory=vehicleSpeedHistory.slice(-MAX_VEHICLE_SPEED_POINTS);
  }
}

function resetVehicleSpeedHistory(){
  vehicleSpeedHistory=[];
  lastVehicleSpeedUptimeMs=null;
  lastValidVehicleSpeedKph=null;
}

function updateVehicleSpeedHistory(data){
  const uptimeMs=Math.max(0,Number(data&&data.uptime||0)*1000);
  if(lastVehicleSpeedUptimeMs!==null&&uptimeMs<lastVehicleSpeedUptimeMs){
    resetVehicleSpeedHistory();
  }
  lastVehicleSpeedUptimeMs=uptimeMs;
  trimVehicleSpeedHistory(uptimeMs);

  const speedKph=Number(data&&data.vehicleSpeedKph);
  const speedValid=!!(data&&data.vehicleSpeedValid)&&Number.isFinite(speedKph);
  if(!speedValid)return;

  const lastPoint=vehicleSpeedHistory[vehicleSpeedHistory.length-1];
  if(lastPoint&&lastPoint.timeMs===uptimeMs){
    lastPoint.kph=speedKph;
    return;
  }

  vehicleSpeedHistory.push({timeMs:uptimeMs,kph:speedKph});
  trimVehicleSpeedHistory(uptimeMs);
}

function computeVehicleAccelerationG(){
  if(vehicleSpeedHistory.length<2)return null;
  const latest=vehicleSpeedHistory[vehicleSpeedHistory.length-1];
  let anchor=null;

  for(let i=vehicleSpeedHistory.length-2;i>=0;i--){
    const candidate=vehicleSpeedHistory[i];
    const dtSec=(latest.timeMs-candidate.timeMs)/1000;
    if(dtSec>=1.5&&dtSec<=4){
      anchor=candidate;
      break;
    }
  }

  if(!anchor)anchor=vehicleSpeedHistory[vehicleSpeedHistory.length-2];
  const dtSec=(latest.timeMs-anchor.timeMs)/1000;
  if(dtSec<=0||dtSec>5)return null;

  const accelMps2=((latest.kph-anchor.kph)/3.6)/dtSec;
  return accelMps2/9.80665;
}

function clearSvgChildren(el){
  while(el.firstChild)el.removeChild(el.firstChild);
}

function addVehicleSpeedLabelCandidate(candidates,index,priority){
  const current=candidates.get(index)||0;
  if(priority>current)candidates.set(index,priority);
}

function renderVehicleSpeedChartSamples(samplesEl,labelsEl,points,minKph,maxKph,startTime){
  clearSvgChildren(samplesEl);
  clearSvgChildren(labelsEl);

  points.forEach(point=>{
    const circle=document.createElementNS(SVG_NS,'circle');
    circle.setAttribute('cx',point.x.toFixed(1));
    circle.setAttribute('cy',point.y.toFixed(1));
    circle.setAttribute('r','2.2');
    samplesEl.appendChild(circle);
  });

  const candidates=new Map();
  const lastIndex=points.length-1;
  addVehicleSpeedLabelCandidate(candidates,0,35);
  addVehicleSpeedLabelCandidate(candidates,lastIndex,100);

  const minIndex=points.findIndex(point=>point.kph===minKph);
  const maxIndex=points.findIndex(point=>point.kph===maxKph);
  if(minIndex>=0)addVehicleSpeedLabelCandidate(candidates,minIndex,70);
  if(maxIndex>=0)addVehicleSpeedLabelCandidate(candidates,maxIndex,70);

  let nextTimeMark=startTime+10000;
  points.forEach((point,index)=>{
    const previous=points[index-1];
    if(previous){
      const deltaKph=Math.abs(point.kph-previous.kph);
      if(deltaKph>=3)addVehicleSpeedLabelCandidate(candidates,index,65);
      else if(deltaKph>=1.5)addVehicleSpeedLabelCandidate(candidates,index,50);
    }
    while(point.timeMs>=nextTimeMark){
      addVehicleSpeedLabelCandidate(candidates,index,30);
      nextTimeMark+=10000;
    }
  });

  const selected=[];
  Array.from(candidates.entries())
    .map(([index,priority])=>({point:points[index],priority}))
    .sort((a,b)=>b.priority-a.priority)
    .forEach(candidate=>{
      const overlapIndex=selected.findIndex(item=>Math.abs(item.point.x-candidate.point.x)<42);
      if(overlapIndex>=0){
        if(candidate.priority>selected[overlapIndex].priority){
          selected.splice(overlapIndex,1,candidate);
        }
        return;
      }
      selected.push(candidate);
    });

  selected
    .sort((a,b)=>b.priority-a.priority)
    .slice(0,10)
    .sort((a,b)=>a.point.x-b.point.x)
    .forEach(({point})=>{
      const label=document.createElement('span');
      let y=point.y-8;
      const x=Math.max(8,Math.min(312,point.x));
      if(y<14)y=point.y+16;
      label.style.left=(x/320*100).toFixed(2)+'%';
      label.style.top=(Math.max(12,Math.min(122,y))/128*100).toFixed(2)+'%';
      if(point.x<18)label.className='anchor-start';
      else if(point.x>302)label.className='anchor-end';
      label.textContent=formatVehicleSpeedValue(point.kph);
      labelsEl.appendChild(label);
    });
}

function updateVehicleSpeedChart(){
  const areaEl=document.getElementById('vehicleSpeedChartArea');
  const lineEl=document.getElementById('vehicleSpeedChartLine');
  const samplesEl=document.getElementById('vehicleSpeedChartSamples');
  const labelsEl=document.getElementById('vehicleSpeedChartLabels');
  const dotEl=document.getElementById('vehicleSpeedChartDot');
  const emptyEl=document.getElementById('vehicleSpeedChartEmpty');
  if(!areaEl||!lineEl||!samplesEl||!labelsEl||!dotEl||!emptyEl)return;

  if(vehicleSpeedHistory.length<2){
    areaEl.setAttribute('d','');
    lineEl.setAttribute('points','');
    clearSvgChildren(samplesEl);
    clearSvgChildren(labelsEl);
    dotEl.style.opacity='0';
    emptyEl.classList.add('visible');
    setTextIfPresent('vehicleSpeedTrendMeta','波动 --');
    setTextIfPresent('vehicleSpeedChartMin','最低 --');
    setTextIfPresent('vehicleSpeedChartMax','最高 --');
    return;
  }

  const width=320;
  const top=16;
  const bottom=108;
  const height=bottom-top;
  const latestTime=vehicleSpeedHistory[vehicleSpeedHistory.length-1].timeMs;
  const startTime=Math.max(0,latestTime-VEHICLE_SPEED_HISTORY_WINDOW_MS);
  const minKph=Math.min(...vehicleSpeedHistory.map(point=>point.kph));
  const maxKph=Math.max(...vehicleSpeedHistory.map(point=>point.kph));
  const rangeKph=Math.max(4,maxKph-minKph);

  const points=vehicleSpeedHistory.map(point=>{
    const x=((point.timeMs-startTime)/VEHICLE_SPEED_HISTORY_WINDOW_MS)*width;
    const y=bottom-((point.kph-minKph)/rangeKph)*height;
    return {
      x:Math.max(0,Math.min(width,x)),
      y:Math.max(top,Math.min(bottom,y)),
      kph:point.kph,
      timeMs:point.timeMs
    };
  });

  const steppedPoints=[];
  points.forEach((point,index)=>{
    if(index===0){
      steppedPoints.push(point);
      return;
    }
    const previous=points[index-1];
    steppedPoints.push({x:point.x,y:previous.y});
    steppedPoints.push(point);
  });

  const polylinePoints=steppedPoints.map(point=>point.x.toFixed(1)+','+point.y.toFixed(1)).join(' ');
  const linePath=steppedPoints.map((point,index)=>(index===0?'M ':' L ')+point.x.toFixed(1)+' '+point.y.toFixed(1)).join('');
  const firstPoint=points[0];
  const lastPoint=points[points.length-1];

  areaEl.setAttribute('d',linePath+' L '+lastPoint.x.toFixed(1)+' '+String(bottom)+' L '+firstPoint.x.toFixed(1)+' '+String(bottom)+' Z');
  lineEl.setAttribute('points',polylinePoints);
  renderVehicleSpeedChartSamples(samplesEl,labelsEl,points,minKph,maxKph,startTime);
  dotEl.setAttribute('cx',lastPoint.x.toFixed(1));
  dotEl.setAttribute('cy',lastPoint.y.toFixed(1));
  dotEl.style.opacity='1';
  emptyEl.classList.remove('visible');
  setTextIfPresent('vehicleSpeedTrendMeta','波动 '+formatVehicleSpeedLabel(maxKph-minKph));
  setTextIfPresent('vehicleSpeedChartMin','最低 '+formatVehicleSpeedLabel(minKph));
  setTextIfPresent('vehicleSpeedChartMax','最高 '+formatVehicleSpeedLabel(maxKph));
}

function updateVehicleAcceleration(speedValid){
  if(!speedValid){
    setSpeedInfoValue('vehicleAccelerationDisplay','--','status-no');
    setTextIfPresent('vehicleAccelerationMeta','等待连续车速样本');
    return;
  }

  const accelG=computeVehicleAccelerationG();
  if(accelG===null||!Number.isFinite(accelG)){
    setSpeedInfoValue('vehicleAccelerationDisplay','--','status-no');
    setTextIfPresent('vehicleAccelerationMeta','至少需要 2 个连续速度点');
    return;
  }

  let className='status-no';
  if(accelG>=0.02)className='status-ok';
  else if(accelG<=-0.02)className='status-warn';

  const accelLabel=(accelG>0?'+':'')+accelG.toFixed(2)+' g';
  setSpeedInfoValue('vehicleAccelerationDisplay',accelLabel,className);
  setTextIfPresent('vehicleAccelerationMeta','按最近 2-4 秒速度变化估算');
}

function updateVehicleSpeedTelemetry(data){
  updateVehicleSpeedHistory(data);

  const speedKph=Number(data&&data.vehicleSpeedKph);
  const speedValid=!!(data&&data.vehicleSpeedValid)&&Number.isFinite(speedKph);
  const speedEl=document.getElementById('vehicleSpeedDisplay');
  if(speedEl){
    if(speedValid){
      lastValidVehicleSpeedKph=speedKph;
      speedEl.textContent=formatVehicleSpeedValue(speedKph);
      speedEl.classList.remove('is-stale');
      speedEl.classList.remove('is-empty');
    }else if(lastValidVehicleSpeedKph!==null){
      speedEl.textContent=formatVehicleSpeedValue(lastValidVehicleSpeedKph);
      speedEl.classList.add('is-stale');
      speedEl.classList.remove('is-empty');
    }else{
      speedEl.textContent='等待';
      speedEl.classList.add('is-stale');
      speedEl.classList.add('is-empty');
    }
  }

  if(speedValid){
    const sourceLabel=getVehicleSpeedSourceLabel(data.vehicleSpeedSource)||'CAN 车速';
    const ageLabel=formatVehicleSpeedAge(Number(data.vehicleSpeedAgeMs));
    setTextIfPresent('vehicleSpeedMeta',[sourceLabel,ageLabel].filter(Boolean).join(' · '));
  }else if(typeof data.vehicleSpeedAgeMs==='number'&&Number.isFinite(data.vehicleSpeedAgeMs)){
    const ageLabel=formatVehicleSpeedAge(data.vehicleSpeedAgeMs);
    setTextIfPresent('vehicleSpeedMeta',ageLabel?'车速数据暂停 · 上次更新 '+ageLabel:'车速数据暂停');
  }else{
    setTextIfPresent('vehicleSpeedMeta','等待 CAN 车速');
  }

  updateVehicleSpeedChart();
  updateVehicleAcceleration(speedValid);
}

function syncDashboardSummary(data){
  const hwSelect=document.getElementById('hwMode');
  const fsdToggle=document.getElementById('fsdEnable');
  const dnsToggle=document.getElementById('dnsWhitelistEnable');
  const currentUpstream=document.getElementById('sCurrentUpstream');
  const upstreamStatus=document.getElementById('sUpstream');
  const hwLabel=data?getHwModeLabel(data.hwMode):getSelectDisplayText(hwSelect);
  const isHw3=data?String(data.hwMode)==='1':hwLabel==='HW3';
  const fsdEnabled=data?!!data.fsdEnable:!!(fsdToggle&&fsdToggle.checked);
  const fsdTriggered=data?!!data.fsdTriggered:false;
  const dnsEnabled=data?!!data.dnsWhitelistEnable:!!(dnsToggle&&dnsToggle.checked);
  const dnsCount=data?Number(data.dnsWhitelistCount||0):getDnsRulesFromTextarea('dnsAllowlist').length;
  const currentUpstreamText=(data?(data.connectedUpstreamSSID||data.upstreamSSID||''):((currentUpstream&&currentUpstream.textContent)||'')).trim();
  const upstreamStatusText=(data?(data.upstreamStatus||'--'):((upstreamStatus&&upstreamStatus.textContent)||'--')).trim();
  const detectedLimitKph=data?Number(data.detectedSpeedLimitKph||0):0;
  const detectedSpeedSource=data?getSpeedSourceLabel(data.detectedSpeedSource):'';
  const appliedOffsetKph=data&&isHw3&&fsdEnabled&&fsdTriggered?Number(data.appliedSpeedOffsetKph||0):0;
  const appliedOffsetLabel='+'+String(appliedOffsetKph)+' km/h';
  const sourceLabel=detectedSpeedSource||'--';
  let offsetState='读取中';
  let policySummary='低于30→30，30→45，40→55，50→65，60→72，高于60固定10%';
  let limitLabel='--';

  if(data&&detectedLimitKph>0){
    limitLabel=String(detectedLimitKph)+' km/h';
  }

  if(!isHw3){
    offsetState='当前为 '+hwLabel+'，自动限速仅在 HW3 生效';
    policySummary='当前硬件不使用这套 HW3 自动限速策略';
  }else if(data&&!fsdEnabled){
    offsetState='FSD 已关闭，当前仅监测限速';
  }else if(data&&!fsdTriggered){
    offsetState=detectedLimitKph>0?'未触发 FSD，当前仅监测限速':'未触发 FSD，等待车机进入 FSD';
  }else if(data&&detectedLimitKph>0){
    offsetState='按识别限速自动上浮';
  }else if(data){
    offsetState='未识别到有效限速，回退到温和默认增量';
    policySummary+=' · 识别丢失时回退到 +'+String(10)+' km/h 内';
  }

  setTextIfPresent('hwModeBadge','硬件 '+hwLabel);
  setTextIfPresent('speedOffsetDisplay',appliedOffsetLabel);
  setTextIfPresent('speedOffsetState',offsetState);
  setTextIfPresent('detectedSpeedLimitDisplay',limitLabel);
  setTextIfPresent('detectedSpeedSourceDisplay',sourceLabel);
  setTextIfPresent('speedPolicySummary',policySummary);
  setTextIfPresent('toolbarFsdMeta','硬件 '+hwLabel+' · '+(fsdEnabled?'已启用':'已关闭'));
  setTextIfPresent('toolbarDnsMeta',(dnsEnabled?'已启用':'未启用')+' · 白名单 '+dnsCount+' 条');
  setTextIfPresent('toolbarNetworkMeta',currentUpstreamText&&currentUpstreamText!=='--'?currentUpstreamText:'上游 '+(upstreamStatusText||'--'));
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

function setExpanded(id,expanded){
  const el=document.getElementById(id);
  if(el)el.setAttribute('aria-expanded',expanded?'true':'false');
}

function closeSettingsMenu(){
  const menu=document.getElementById('settingsMenu');
  const toggle=document.getElementById('settingsToggle');
  if(menu)menu.classList.remove('open');
  if(toggle)toggle.classList.remove('active');
  setExpanded('settingsToggle',false);
}

function closeDiagnosticsPanel(){
  diagnosticsPinned=false;
  const panel=document.getElementById('diagnosticsPanel');
  const toggle=document.getElementById('diagnosticsToggle');
  if(panel)panel.classList.remove('open');
  if(toggle)toggle.classList.remove('active');
  setExpanded('diagnosticsToggle',false);
}

function showDiagnosticsPanel(){
  clearTimeout(diagnosticsHideTimer);
  closeSettingsMenu();
  const panel=document.getElementById('diagnosticsPanel');
  if(panel)panel.classList.add('open');
  const toggle=document.getElementById('diagnosticsToggle');
  if(toggle)toggle.classList.add('active');
  setExpanded('diagnosticsToggle',true);
}

function hideDiagnosticsPanelSoon(){
  clearTimeout(diagnosticsHideTimer);
  diagnosticsHideTimer=setTimeout(()=>{
    if(!diagnosticsPinned)closeDiagnosticsPanel();
  },120);
}

function toggleDiagnosticsPanel(evt){
  if(evt)evt.stopPropagation();
  closeSettingsMenu();
  const panel=document.getElementById('diagnosticsPanel');
  const shouldOpen=!(panel&&panel.classList.contains('open')&&diagnosticsPinned);
  if(shouldOpen){
    diagnosticsPinned=true;
    showDiagnosticsPanel();
  }else{
    closeDiagnosticsPanel();
  }
}

function toggleSettingsMenu(evt){
  if(evt)evt.stopPropagation();
  closeDiagnosticsPanel();
  const menu=document.getElementById('settingsMenu');
  const toggle=document.getElementById('settingsToggle');
  const shouldOpen=!(menu&&menu.classList.contains('open'));
  if(menu)menu.classList.toggle('open',shouldOpen);
  if(toggle)toggle.classList.toggle('active',shouldOpen);
  setExpanded('settingsToggle',shouldOpen);
}

function closeTransientPanels(){
  closeSettingsMenu();
  closeDiagnosticsPanel();
}

function openDashboardDialog(id){
  closeTransientPanels();
  openDialog(id);
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
    updateCanActivity(d);
    setTextIfPresent('sErrors',d.errors);
    let u=d.uptime;
    let h=Math.floor(u/3600),m=Math.floor((u%3600)/60),s=u%60;
    setTextIfPresent('sUptime',h>0?h+'时'+m+'分':m>0?m+'分'+s+'秒':s+'秒');

    updateCanStatus(d);
    updateVehicleSpeedTelemetry(d);

    let fsdEl=document.getElementById('sFSD');
    if(fsdEl){
      fsdEl.textContent=d.fsdTriggered?'是':'否';
      fsdEl.className=(d.fsdTriggered?'status-yes':'status-no')+' status-text';
    }

    let thermalClass='status-ok';
    if(d.thermalProtect)thermalClass='status-err';
    else if((d.thermalStatus||'').includes('降频')||(d.thermalStatus||'').includes('偏高'))thermalClass='status-warn';
    let signalClass=getSignalClass(d.upstreamRSSI);

    document.getElementById('fsdEnable').checked=!!d.fsdEnable;
    document.getElementById('hwMode').value=d.hwMode;
    document.getElementById('speedProfile').value=d.speedProfile;
    document.getElementById('profileMode').value=d.profileMode?'1':'0';
    document.getElementById('isaChime').checked=!!d.isaChime;
    document.getElementById('emergencyDet').checked=!!d.emergencyDet;
    document.getElementById('chinaMode').checked=!!d.chinaMode;

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
    setStatusText('sAPRunning',d.apRunning?'运行中':'已停止',d.apRunning?'status-ok':'status-err');
    setStatusText('sHeap',formatHeap(d.freeHeap,d.minFreeHeap),heapClass(d.freeHeap,d.minFreeHeap));
    setStatusText('sResetReason',d.resetReason||'--',resetReasonClass(d.resetReason));
    setStatusText('sUpstreamIP',d.upstreamIP||'--',d.upstreamConnected?'status-ok':'status-no');
    setStatusText('sNAT',d.natStatus||'--',d.natEnabled?'status-ok':(d.upstreamConnected?'status-err':'status-no'));
    setStatusText('sUpstreamPhase',d.upstreamPhase||'--',d.upstreamConnected?'status-ok':(d.upstreamEnable?'status-warn':'status-no'));
    setStatusText('sUpstreamRetry',String(d.upstreamRetryCount||0),(d.upstreamRetryCount||0)>6?'status-warn':'status-ok');
    setStatusText('sAP',d.apSSID||'--',d.apRunning?'status-ok':'status-err');
    setStatusText('sAPIP',d.apIP||'--','status-ok');
    setWideStatusText('sDNSMode',formatDnsPolicy(d),d.dnsStrictAllow?'status-ok':(d.dnsWhitelistEnable?'status-warn':'status-no'));
    setStatusText('sDNSCount',String(d.dnsWhitelistCount||0),d.dnsWhitelistCount?'status-ok':'status-no');
    setStatusText('sDNSBlockCount',String(d.dnsBlacklistCount||0),d.dnsBlacklistCount?'status-err':'status-no');
    setStatusText('sDNSBlocked',String(d.dnsBlockedCount||0),d.dnsBlockedCount?'status-err':'status-no');
    setStatusText('sDNSPolicy',formatDnsPolicy(d),d.dnsStrictAllow?'status-ok':(d.dnsPolicyEnabled?'status-warn':'status-no'));
    setStatusText('sDNSIpCache',formatDnsIpCache(d),(d.dnsAllowIpCount||d.dnsBlockIpCount)?'status-ok':'status-no');
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
syncDashboardSummary();

document.addEventListener('keydown',evt=>{
  if(evt.key!=='Escape')return;
  closeTransientPanels();
  closePicker();
  document.querySelectorAll('.dialog-modal.open').forEach(dialog=>dialog.classList.remove('open'));
});

document.addEventListener('click',evt=>{
  if(evt.target&&evt.target.closest&&evt.target.closest('.hud-popover-wrap'))return;
  closeTransientPanels();
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

function formatBytes(bytes){
  if(typeof bytes!=='number'||!Number.isFinite(bytes)||bytes<0)return '--';
  if(bytes<1024)return String(Math.round(bytes))+' B';
  if(bytes<1024*1024)return (bytes/1024).toFixed(bytes<10*1024?1:0)+' KB';
  return (bytes/(1024*1024)).toFixed(bytes<10*1024*1024?2:1)+' MB';
}

function setOtaMessage(text,kind){
  const msg=document.getElementById('otaMsg');
  if(!msg)return;
  msg.textContent=text||'';
  msg.className='msg'+(kind?' '+kind:'');
}

function setOtaProgress(percent){
  const prog=document.getElementById('progWrap');
  const bar=document.getElementById('progBar');
  if(!prog||!bar)return;
  prog.style.display='block';
  bar.style.width=Math.max(0,Math.min(100,percent))+'%';
}

function setOtaUploadEnabled(enabled){
  const btn=document.getElementById('uploadBtn');
  const file=document.getElementById('fwFile').files[0];
  if(btn)btn.disabled=enabled?!file:true;
}

function stopOtaTrace(){
  otaTraceGeneration++;
  if(otaTraceTimer){
    clearTimeout(otaTraceTimer);
    otaTraceTimer=0;
  }
  if(otaTraceController){
    otaTraceController.abort();
    otaTraceController=null;
  }
  otaTraceStartedAt=0;
  otaTraceClientUploadComplete=false;
  otaTraceSawLikelySuccessPhase=false;
}

function scheduleOtaTrace(traceId){
  otaTraceTimer=setTimeout(()=>traceOtaStatus(traceId),OTA_TRACE_POLL_MS);
}

function parseJsonSafe(text){
  if(!text)return null;
  try{
    return JSON.parse(text);
  }catch(_err){
    return null;
  }
}

function getWrongOtaFileMessage(file){
  if(!file||!file.name)return '';
  const lower=file.name.trim().toLowerCase();
  if(lower.endsWith('full.bin'))return 'WebUI OTA 只能上传 firmware.bin，不能直接上传 full.bin。';
  if(lower.endsWith('bootloader.bin')||lower.endsWith('partitions.bin'))return 'bootloader.bin / partitions.bin 不能通过 WebUI OTA 更新。请选择应用固件 firmware.bin。';
  return '';
}

function buildOtaStatusLines(status){
  const lines=[];
  if(status&&status.errorMessage)lines.push(status.errorMessage);
  if(status&&typeof status.bytesReceived==='number'&&Number.isFinite(status.bytesReceived)&&status.bytesReceived>0){
    let progress='设备侧已接收 '+formatBytes(status.bytesReceived);
    if(typeof status.totalBytes==='number'&&Number.isFinite(status.totalBytes)&&status.totalBytes>0){
      progress+=' / HTTP 请求体 '+formatBytes(status.totalBytes);
    }
    lines.push(progress);
  }
  if(status&&status.hint)lines.push(status.hint);
  return lines.filter(Boolean);
}

function renderOtaStatus(status,fallbackMessage){
  const phase=status&&status.phase?status.phase:'idle';
  const hasBytes=!!status&&typeof status.bytesReceived==='number'&&Number.isFinite(status.bytesReceived)&&status.bytesReceived>0;
  const hasTotal=!!status&&typeof status.totalBytes==='number'&&Number.isFinite(status.totalBytes)&&status.totalBytes>0;
  if(hasBytes&&hasTotal)setOtaProgress(Math.round(status.bytesReceived/status.totalBytes*100));

  switch(phase){
    case 'uploading':
      setOtaMessage(['设备正在接收固件...'].concat(buildOtaStatusLines(status)).join('\n'),'');
      return false;
    case 'finishing':
      setOtaProgress(100);
      setOtaMessage(['固件已传到设备，正在完成校验...'].concat(buildOtaStatusLines(status)).join('\n'),'');
      return false;
    case 'success-rebooting':
      setOtaProgress(100);
      setOtaMessage(buildOtaStatusLines(status).join('\n')||'固件写入完成，设备正在重启。','ok');
      stopOtaTrace();
      return true;
    case 'failed-begin':
    case 'failed-write':
    case 'failed-end':
    case 'aborted':
      setOtaMessage(buildOtaStatusLines(status).join('\n')||fallbackMessage||'升级失败','err');
      setOtaUploadEnabled(true);
      stopOtaTrace();
      return true;
    case 'idle':
    default:
      if(fallbackMessage){
        setOtaMessage(fallbackMessage,'err');
        setOtaUploadEnabled(true);
      }
      stopOtaTrace();
      return true;
  }
}

function finishOtaTraceAfterIdle(){
  const lines=[];
  if(otaTraceClientUploadComplete||otaTraceSawLikelySuccessPhase){
    setOtaProgress(100);
    lines.push('设备已重新上线，上一轮 OTA 响应大概率被重启打断。');
    lines.push('如果页面功能正常，本次升级通常已经完成。');
    setOtaMessage(lines.join('\n'),'');
  }else{
    lines.push('设备已重新上线，但本次 OTA 状态已在重启后丢失。');
    lines.push('浏览器端上传没有完整结束，更像是链路中断或设备异常重启。');
    lines.push('建议重试一次，并结合串口日志确认原因。');
    setOtaMessage(lines.join('\n'),'');
  }
  setOtaUploadEnabled(true);
  stopOtaTrace();
}

function traceOtaStatus(traceId){
  if(traceId!==otaTraceGeneration)return;
  const controller=typeof AbortController==='function'?new AbortController():null;
  otaTraceController=controller;
  const requestInit={cache:'no-store'};
  if(controller)requestInit.signal=controller.signal;

  fetch('/api/ota/status',requestInit).then(r=>{
    if(traceId!==otaTraceGeneration)return null;
    if(!r.ok)throw new Error('status unavailable');
    return r.json();
  }).then(status=>{
    if(traceId!==otaTraceGeneration||!status)return;
    if(otaTraceController===controller)otaTraceController=null;
    const phase=status&&status.phase?status.phase:'idle';
    if(phase==='finishing'||phase==='success-rebooting'){
      otaTraceSawLikelySuccessPhase=true;
    }
    if(phase==='idle'){
      finishOtaTraceAfterIdle();
      return;
    }
    if(renderOtaStatus(status,''))return;
    if(Date.now()-otaTraceStartedAt>OTA_TRACE_TIMEOUT_MS){
      const lines=['设备暂时没有返回明确失败原因。'].concat(buildOtaStatusLines(status));
      lines.push('请结合串口日志继续排查。');
      setOtaMessage(lines.join('\n'),'err');
      setOtaUploadEnabled(true);
      stopOtaTrace();
      return;
    }
    scheduleOtaTrace(traceId);
  }).catch(err=>{
    if(traceId!==otaTraceGeneration)return;
    if(otaTraceController===controller)otaTraceController=null;
    if(err&&err.name==='AbortError')return;
    if(Date.now()-otaTraceStartedAt>OTA_TRACE_TIMEOUT_MS){
      setOtaMessage('连接已断开，但设备长时间没有恢复在线，未能拿到诊断结果。请查看串口日志。','err');
      setOtaUploadEnabled(true);
      stopOtaTrace();
      return;
    }
    setOtaMessage('连接已断开，正在等待设备重新上线并回传诊断信息...','err');
    scheduleOtaTrace(traceId);
  });
}

function startOtaTrace(initialMessage){
  stopOtaTrace();
  otaTraceStartedAt=Date.now();
  otaTraceClientUploadComplete=otaClientUploadComplete;
  if(initialMessage)setOtaMessage(initialMessage,'err');
  traceOtaStatus(otaTraceGeneration);
}

function fileChosen(inp){
  const file=inp.files[0];
  document.getElementById('fileName').textContent=file?file.name:'未选择文件';
  stopOtaTrace();
  setOtaProgress(0);
  document.getElementById('progWrap').style.display='none';
  const wrongFileMessage=getWrongOtaFileMessage(file);
  if(wrongFileMessage){
    setOtaMessage(wrongFileMessage,'err');
    document.getElementById('uploadBtn').disabled=true;
    return;
  }
  setOtaMessage('','');
  document.getElementById('uploadBtn').disabled=!file;
}

function doOTA(){
  let file=document.getElementById('fwFile').files[0];
  if(!file)return;
  const wrongFileMessage=getWrongOtaFileMessage(file);
  if(wrongFileMessage){
    setOtaMessage(wrongFileMessage,'err');
    return;
  }
  stopOtaTrace();
  otaClientUploadComplete=false;
  let xhr=new XMLHttpRequest();
  setOtaProgress(0);
  setOtaMessage('开始上传，设备正在准备 OTA...','');
  document.getElementById('uploadBtn').disabled=true;
  xhr.upload.addEventListener('progress',e=>{
    if(e.lengthComputable){
      const percent=Math.round(e.loaded/e.total*100);
      otaClientUploadComplete=e.loaded>=e.total||percent>=100;
      setOtaProgress(percent);
      setOtaMessage('浏览器正在上传固件...','');
    }
  });
  xhr.onload=function(){
    const result=parseJsonSafe(xhr.responseText);
    if(result&&typeof result==='object'){
      renderOtaStatus(result,xhr.status===200?'固件写入完成，设备正在重启。':'升级失败');
      return;
    }
    if(xhr.status===200){
      setOtaProgress(100);
      setOtaMessage('固件写入完成，设备正在重启。','ok');
      return;
    }
    setOtaMessage('升级失败，设备没有返回可解析的诊断信息。','err');
    setOtaUploadEnabled(true);
  };
  xhr.onerror=function(){
    startOtaTrace('连接已中断，正在读取设备侧诊断信息...');
  };
  let form=new FormData();
  form.append('firmware',file);
  xhr.open('POST','/api/ota');
  xhr.send(form);
}

setInterval(poll,1000);
poll();

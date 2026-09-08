from pathlib import Path
import sys,hashlib,json
root=Path(sys.argv[1])/'samples/basic_sample_frame_generation'
source=Path(__file__).parent/'NativeOutputMailbox.h'
s=source.read_text(encoding='utf-8')
def once(a,b):
 global s
 if s.count(a)!=1:raise RuntimeError('mailbox control anchor: '+a[:100])
 s=s.replace(a,b)
once('inline void DebugSetup(){}', '''inline double OutputPeriod() {
 static const double configured=[] {char b[64]{};if(!GetEnvironmentVariableA("XEFG_LAB_OUTPUT_PERIOD_MS",b,sizeof(b)))return 0.0;
  char* e=nullptr;double x=strtod(b,&e);return e!=b&&*e==0&&std::isfinite(x)&&x>=2&&x<=100?x:0.0;}();
 return configured>0?configured:sourcePeriod.load();
}
inline void DebugSetup(){}''')
once('r.period=sourcePeriod.load();r.paced=', 'r.period=OutputPeriod();r.paced=')
once('''  hr=dev->CreateCommandQueue(&desc,IID_PPV_ARGS(&display));if(FAILED(hr))return hr;''','''  const INT inheritedPriority=desc.Priority;
  char value[32]{};
  if(GetEnvironmentVariableA("XEFG_LAB_DISPLAY_PRIORITY",value,sizeof(value))) {
   char* end=nullptr;long selected=strtol(value,&end,10);
   if(end==value||*end||!(selected==0||selected==100||selected==10000))return E_INVALIDARG;
   desc.Priority=static_cast<INT>(selected);
  }
  D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY support{desc.Type,static_cast<UINT>(desc.Priority),FALSE};
  HRESULT checked=dev->CheckFeatureSupport(D3D12_FEATURE_COMMAND_QUEUE_PRIORITY,&support,sizeof(support));
  std::ofstream queueLog(EnvPath()+"\\\\queue-priority.json");
  queueLog<<"{\\\"sdk_priority\\\":"<<inheritedPriority<<",\\\"requested_display_priority\\\":"<<desc.Priority
          <<",\\\"support_query_hresult\\\":"<<static_cast<long>(checked)<<",\\\"supported\\\":"<<(support.PriorityForTypeIsSupported?"true":"false");
  // No silent downgrade, token adjustment, or global scheduling setting.
  if(FAILED(checked)||!support.PriorityForTypeIsSupported){queueLog<<",\\\"creation_attempted\\\":false}\\n";return DXGI_ERROR_UNSUPPORTED;}
  hr=dev->CreateCommandQueue(&desc,IID_PPV_ARGS(&display));
  queueLog<<",\\\"creation_attempted\\\":true,\\\"creation_hresult\\\":"<<static_cast<long>(hr)<<"}\\n";queueLog.flush();
  if(FAILED(hr))return hr;''')
(root/'NativeGate.h').write_text(s,encoding='utf-8')
m=root/'NATIVE-GATE-MANIFEST.json';d=json.loads(m.read_text())
d.update(kind='isolated_native_output_fifo_not_game_patch',deadline_placement='CPU_before_real_native_Present_after_completed_raw_copy',native_submission='bounded_acceptance_then_real_Present_with_sticky_asynchronous_errors',explicit_deadline_period_optional=True,display_priority_requires_supported_API_creation=True,game_modified=False,resize_supported=False,scanout_or_pixel_quality_certified=False)
d['NativeGate.h_sha256']=hashlib.sha256((root/'NativeGate.h').read_bytes()).hexdigest()
m.write_text(json.dumps(d,indent=2),encoding='utf-8')
print('Selected complete-image native output FIFO with independent test controls; no game patch.')

import fs from 'node:fs/promises';
import { Presentation, PresentationFile } from '@oai/artifact-tool';
import { finalizePresentation } from '/home/sch/.cache/codex-runtimes/codex-primary-runtime/plugins/openai-primary-runtime/plugins/presentations/skills/presentations/container_tools/artifact_tool_utils.mjs';

const ROOT='/home/sch/nebula/nebula-3.6-tantivy';
const TMP=process.env.DECK_BUILD_DIR || ROOT+'/task-logs/tantivy-presentation/final-r1';
const SKILL='/home/sch/.cache/codex-runtimes/codex-primary-runtime/plugins/openai-primary-runtime/plugins/presentations/skills/presentations';
const FINAL=process.env.DECK_OUTPUT || ROOT+'/docs/tantivy/nebula-tantivy-competition-v1.pptx';
const performance=JSON.parse(await fs.readFile(process.env.PERFORMANCE_SLIDE_JSON || ROOT+'/task-logs/tantivy-presentation/performance-slide.pending.json','utf8'));
for(const key of ['title','headline','headlineLabel','detail','footer','notes']) if(typeof performance[key]!=='string') throw new Error('Missing performance slide text: '+key);
if(!Array.isArray(performance.table)||performance.table.length<2||performance.table.length>5) throw new Error('Performance slide needs header plus 1–4 rows');
const performanceColumns=performance.table[0].length;
if(![2,3].includes(performanceColumns)||performance.table.some(row=>row.length!==performanceColumns||row.some(cell=>typeof cell!=='string'))) throw new Error('Performance table must have 2 or 3 text columns');
const FONT='Noto Sans CJK SC';
const C={navy:'#102A43',ink:'#183B56',muted:'#526778',blue:'#126E82',orange:'#AD5F22',paper:'#F5F7F9',line:'#C6D3DE',white:'#FFFFFF'};
const p=Presentation.create({slideSize:{width:1280,height:720}});
p.theme.defaultFont=FONT;
function text(s,str,x,y,w,h,size=28,color=C.ink,bold=false){
 const q=s.shapes.add({geometry:'textbox',position:{left:x,top:y,width:w,height:h},fill:'none',line:{fill:'none',width:0}});
 q.text=str; q.text.style={typeface:FONT,fontSize:size,color,bold}; return q;
}
function slide(title,n,notes){const s=p.slides.add();s.background.fill=C.paper;text(s,title,72,48,1140,76,44,C.navy,true);text(s,String(n).padStart(2,'0'),1150,652,70,30,18,C.muted);s.speakerNotes.text=notes;return s;}
function table(s,values,widths,y=180,rowHeight=74,fontSize=26){
 const t=s.tables.add({rows:values.length,columns:values[0].length,left:72,top:y,width:1136,height:values.length*rowHeight,columnWidths:widths,values});
 t.borders.assign({fill:C.line,width:1,style:'solid'});
 for(let r=0;r<values.length;r++)for(let c=0;c<values[0].length;c++){
  const cell=t.getCell(r,c);cell.fill=r===0?C.navy:(r%2?C.white:C.paper);cell.text.style={typeface:FONT,fontSize,color:r===0?C.white:C.ink,bold:r===0};
 }
 return t;
}
function box(s,str,x,y,w,h){const q=s.shapes.add({geometry:'rect',position:{left:x,top:y,width:w,height:h},fill:C.white,line:{fill:C.blue,width:2}});q.text=str;q.text.style={typeface:FONT,fontSize:27,color:C.ink,bold:true};return q;}
function connect(s,a,b,fromSide='right',toSide='left'){s.shapes.connect(a,b,{kind:'straight',fromSide,toSide,line:{fill:C.blue,width:2},tail:{type:'arrow',width:'med',length:'med'}});}

{
const s=p.slides.add();s.background.fill=C.navy;
text(s,'NebulaGraph 内置全文索引',72,160,1136,110,58,C.white,true);
text(s,'基于 Tantivy 的无感模糊查询下推',76,292,1080,70,35,'#8DD6DA');
text(s,'软件创客竞赛  /  可演示原型',76,472,1000,48,26,'#C4D6E3');
text(s,'2026 年 11 月',76,552,600,40,24,'#C4D6E3');
s.speakerNotes.text='开场约25秒：我们已有商用NebulaGraph基础，希望把全文检索做成数据库自身能力，减少独立ES和Listener部署，并让业务继续使用字符串谓词。交付定位为次日可演示原型。此版本未完成商业可靠性或亿级导入，百万ES与原型已有绝对实测，但原生查询语义和索引复制条件不同，不能算跨引擎速度或降配比例。源码：src/common/plugin/fulltext/tantivy/。';
}
{
const s=slide('部署与原生检索观测',2,'约45秒。同一百万真实唯一PID、128字符投影、20图分区3图副本，原生三查询每项4000次、预热20轮、LIMIT10，两组每个并发档12000次均0失败。内置与ES的绝对观测分别列出，不计算比值。Tantivy C1/C4为36.2224/52.7110QPS，C1 P95/P99=55.8222/63.3650ms，ES C1/C4为129.1826/662.1524QPS，C1 P95/P99=14.2067/17.4246ms。Tantivy五服务与ES七服务C1同轮PSS采样峰值分别0.6487/2.3076GiB，采样窗口含初始化和预热，不含全部文件缓存或客户端，不等于可配置内存。两组均在一台机器串行运行，未清缓存，旧Nebula基线二进制Git不同，均Debug。Tantivy三个storage各保存词法和trigram/原文结构，ES7.17.29只词法索引、1primary/0replicas、512MiB heap，并有Listener WAL。water、New York短语、water AND river完整差集分别54/77/2，均仅Tantivy命中，相同语法fixture两端18项通过不能抹去真实语料差异。来源：docs/tantivy/benchmark-report.md，task-logs/tantivy-benchmark/tantivy-topk-1m-c1-long-independent-audit.json、tantivy-topk-1m-c4-long-independent-audit.json、es-1m-long-query-independent-audit.json、native-1m-comparison/comparison.json。');
table(s,[['百万原生检索','内置 Tantivy','Listener + ES'],['服务数（均单台）','5 个','7 个'],['QPS：单 / 四并发','36.22 / 52.71','129.18 / 662.15'],['单并发 P95 / P99 ms','55.82 / 63.37','14.21 / 17.42'],['单并发 PSS 采样峰值 GiB','0.6487（5 服务）','2.3076（7 服务）'],['全文索引结构','3 份，词法 + trigram','1 主 0 副本，仅词法']], [320,408,408],162,68,24);
text(s,'三项差集 54 / 77 / 2，语义与全文复制不同，不计算速度或降配比。',72,598,1070,50,23,C.orange,true);
}
{
const s=slide('内置检索架构',3,'约50秒。每个storaged内持有图数据和检索索引。Tantivy只承担本地检索，分区路由、复制和结果合并由Nebula承担。单机三个storage进程、20分区、3副本已通过真实集群验收，主演示每分区12条人工记录，覆盖全部20分区，不等同于多物理故障域。Graph合并各分区的实际分数，未做全局IDF校准，逻辑数据相同的副本也可能因segment/deletes统计产生不同BM25分数。来源：task-logs/tantivy-demo/verified-final/report.json。Tantivy明确不提供分布式搜索：https://github.com/quickwit-oss/tantivy 。');
const a=box(s,'业务查询\n字符串谓词',72,194,235,122),b=box(s,'graphd\n规划与分区路由',354,194,270,122),c=box(s,'storaged\nTantivyManager',675,194,270,122),d=box(s,'Tantivy 静态库\n进程内倒排',994,194,214,122);
connect(s,a,b);connect(s,b,c);connect(s,c,d);
const e=box(s,'Raft 日志应用',354,422,270,100),f=box(s,'RocksDB 图主数据',675,422,270,100);
connect(s,e,f);connect(s,f,c,'top','bottom');
text(s,'已提交变更',837,357,250,42,24,C.blue);
text(s,'候选 key 回查图记录，并重新计算原始谓词',72,562,1136,48,30,C.ink,true);
text(s,'已验证：单机 3 个 storage 进程，20 分区，3 副本',72,625,1050,36,24,C.muted);
}
{
const s=slide('业务查询保持字符串谓词',4,'约45秒。页上查询和对应MATCH已经在真实集群通过，并有执行计划确认Tantivy路径。主演示覆盖CONTAINS、STARTS WITH、ENDS WITH，中文、大小写、单字、空串、字面特殊字符、多字段、边和残余AND。Tantivy先用分区条件和Unicode三元组缩小候选，再对原文验证。短于三个字符的查询使用分区候选扫描，性能需要单独评估。来源：task-logs/tantivy-demo/verified-final/report.json。');
text(s,'LOOKUP ON article\nWHERE article.body CONTAINS "数据库"\nYIELD id(vertex) AS id;',72,180,1136,190,32,C.blue,true);
text(s,'规划器选择可用索引，业务保留原有表达式',72,420,1110,54,32,C.ink,true);
text(s,'候选过滤后执行原文复验，保留大小写与空值语义。\n短串可能退化为分区扫描，需要单独评估。',72,503,1100,100,28,C.muted);
}
{
const s=slide('索引与图主数据同步',5,'约45秒。RocksDB与Tantivy不是原子事务。写入成功后的回调排队，后台批量提交并刷新reader，查询等待提交屏障。队列背压会影响Raft提交延迟，须纳入评测。当前恢复策略为从RocksDB全量重建，不能声称已具备增量断点恢复。最新CLEAR SPACE在主数据清理后等待全文重建ready，失败不回滚清空，显式REBUILD读取Meta权威定义并复核ready，不能把空缓存当成成功。新版生命周期23项及立即重建3项已复验通过，后者在三storage任务FINISHED后首次直连storage读取200条完整记录，0重试。来源还包括task-logs/tantivy-lifecycle/post-topk/report.json、task-logs/tantivy-rebuild-immediate/verified-v2/report.json。来源：src/storage/fulltext/TantivyManager.cpp、src/kvstore/Part.cpp。');
const rows=[['01','Raft 应用成功','RocksDB 接受图记录变更'],['02','变更进入队列','批次队列与容量背压'],['03','Tantivy 批量提交','持久化并刷新查询 reader'],['04','查询等待屏障','候选回查主数据后返回']];
for(let i=0;i<rows.length;i++){const y=170+i*101;text(s,rows[i][0],72,y,85,60,37,C.blue,true);text(s,rows[i][1],190,y,420,60,32,C.ink,true);text(s,rows[i][2],655,y,550,65,27,C.muted);}
text(s,'恢复边界：原型重启后全量重建，尚未实现精确增量恢复。',72,607,1100,44,24,C.orange,true);
}
{
const s=slide('93 项不重复集群检查通过',6,'约45秒。主演示39/39：240条人工混合记录，每分区12条，覆盖普通MATCH/LOOKUP下推、中英文、边、多字段、评分分页、更新删除，生命周期新版复验23/23：初始42个INT64顶点和21条边，覆盖自动回填、三storage显式重建、删除后同名重建、清空后重插，原生语法18/18：8条人工记录，17项正确集合和1项预期语法错误，包含前缀、通配、正则、模糊、邻近短语、加权和范围。证据分别为task-logs/tantivy-demo/verified-final/report.json、task-logs/tantivy-lifecycle/post-topk/report.json、task-logs/tantivy-benchmark/regression-tantivy-final/regression.json。底层9项真实磁盘测试：task-logs/tantivy-bridge/query-compat-test.log，C++集成：cpp-test.log。这些是正确性验证，不是性能基准，也不表示全部ES/Lucene方言兼容。主演示39和语法18已最终复验，生命周期复验未重复计数。新增TopK10/10通过650条人工数据的恒定分数分页（六页含空页）、BM25与完整实际排序一致及不适用路径检查，报告task-logs/tantivy-topk/verified-v2/report.json，立即重建3/3通过200条数据、三任务完成后首次storage候选读取完整且0重试，报告task-logs/tantivy-rebuild-immediate/verified-v2/report.json。五组不重复合计93。最新TopK仅适用于无额外过滤/统计/自定义排序的单上下文FIXED_STRING顶点TEXT_SEARCH，普通谓词、INT64和边仍走完整候选，异常候选可回退。百万性能先于此优化，不能用来证明TopK收益。');
table(s,[['验收项目','已通过的检查'],['普通谓词与全文检索','39 / 39：含评分分页、增删改'],['索引生命周期','23 / 23：回填、重建、清空重插'],['原生查询语法','18 / 18：通配、正则、模糊等'],['TopK 与立即重建','10 / 10 + 3 / 3：新版实测']], [480,656],164,78,26);
text(s,'另有 9 项 Rust 及 C++ 集成，百万两引擎语义有差异，未完成亿级验证。',72,594,1110,44,24,C.orange,true);
}
{
const s=slide(performance.title,7,performance.notes);
text(s,performance.headline,72,160,670,95,66,C.blue,true);
text(s,performance.headlineLabel,745,190,450,70,28,C.ink,true);
text(s,performance.detail,72,272,1136,55,27,C.muted);
table(s,performance.table,performanceColumns===2?[300,836]:[360,388,388],344,56,25);
text(s,performance.footer,72,634,1050,62,21,C.orange,true);
}
{
const s=slide('比赛演示与落地路线',8,'约45秒。tantivy_demo_v3已恢复并最终复验39/39，ft_regression_final语法18/18也通过。当前保留ft_bench_5m五百万图数据。现场建议保留当前已就绪进程，若重启，storage会全量回填五百万全文索引，应预留数分钟，以ready日志与完整校验为准，进程存活不等于索引ready。现场演示已通过验收的路径：确认三storage、20分区、3副本，建全文索引，用普通字符串谓词查询，更新和删除后复验，展示查询计划与93项不重复功能检查报告。性能数据只展示已完成的独立基准结果。后续试点重点包括故障恢复、分区迁移、全原生功能对齐和长期压测。来源：docs/tantivy/fulltext-compatibility.md 及对应五组集群报告。');
text(s,'现场演示',72,169,1050,65,36,C.blue,true);
text(s,'部署拓扑  /  建立索引  /  原有谓词查询\n更新删除复验  /  查询计划与测试证据',72,250,1130,120,31,C.ink);
text(s,'试点前补齐',72,430,1050,65,36,C.blue,true);
text(s,'增量恢复与故障注入，分区迁移，功能兼容矩阵。\n用完整对照实验确定资源降配与性能收益。',72,509,1130,110,29,C.ink);
}

await fs.mkdir(TMP+'/previews',{recursive:true});
for(let i=0;i<p.slides.items.length;i++){
 const png=await p.export({slide:p.slides.items[i],format:'png',scale:1});
 await fs.writeFile(TMP+`/previews/slide-${String(i+1).padStart(2,'0')}.png`,new Uint8Array(await png.arrayBuffer()));
}
const candidate=TMP+'/candidate.pptx';
await (await PresentationFile.exportPptx(p)).save(candidate);
const result=await finalizePresentation({
 workspaceDir:ROOT,candidatePath:candidate,finalPath:FINAL,
 pythonExecutable:'/home/sch/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3',
 integrityValidatorPath:SKILL+'/container_tools/inspect_presentation_package_integrity.py',
 layoutValidatorPath:SKILL+'/container_tools/inspect_presentation_layout_geometry.py',
 layoutArgs:['--expected-slide-size-emu','12192000,6858000','--validate-bullet-geometry','--validate-heading-fit','--require-native-table-slide','2','--require-native-table-slide','6','--require-native-table-slide','7'],
 explicitTotalSlideCount:8,requiredNativeTableOwnerSlides:[2,6,7],fontPolicy:{basis:'design',families:[FONT]},
 verifyArtifactToolImport:true,receiptPath:TMP+'/validation.json'
});
console.log(JSON.stringify(result,null,2));

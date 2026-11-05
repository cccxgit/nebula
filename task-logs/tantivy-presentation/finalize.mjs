import { finalizePresentation } from '/home/sch/.cache/codex-runtimes/codex-primary-runtime/plugins/openai-primary-runtime/plugins/presentations/skills/presentations/container_tools/artifact_tool_utils.mjs';
const ROOT='/home/sch/nebula/nebula-3.6-tantivy';
const TMP=process.env.DECK_BUILD_DIR || ROOT+'/task-logs/tantivy-presentation/final-r1';
const SKILL='/home/sch/.cache/codex-runtimes/codex-primary-runtime/plugins/openai-primary-runtime/plugins/presentations/skills/presentations';
const FINAL=process.env.DECK_OUTPUT || ROOT+'/docs/tantivy/nebula-tantivy-competition-v1.pptx';
const FONT='Noto Sans CJK SC';
const candidate=TMP+'/candidate.pptx';
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

"""Explicit renderer fallback after bundled LibreOffice GLIBC incompatibility.

Uses the bundled Python and original skill renderer; changes only executable
selection in this local process. Does not edit the managed runtime.
"""
import importlib.util
import os
from pathlib import Path
import sys

renderer = Path('/home/sch/.codex/plugins/cache/openai-primary-runtime/documents/26.1009.11318/skills/documents/render_docx.py')
spec = importlib.util.spec_from_file_location('docx_skill_renderer', renderer)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
print('Explicit fallback: bundled LibreOffice requires GLIBC_2.38 / GLIBCXX_3.4.31 unavailable on this host.', flush=True)
print('Using host LibreOffice 7.3.7.2 and /usr/bin Poppler; bundled Python and original renderer retained.', flush=True)
module._resolve_soffice = lambda: '/usr/bin/soffice'
os.environ['PATH'] = '/usr/bin:/bin:' + os.environ.get('PATH', '')
module.main()

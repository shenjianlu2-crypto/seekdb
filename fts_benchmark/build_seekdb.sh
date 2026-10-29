#!/bin/bash
# Rebuild seekdb (stops the running instance first so the linker can replace the exe).
set -u
export VCToolsRedistDir="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\14.50.35710\\"
export PATH="/c/Users/31731/Documents/Qoder/2026-10-28/fa21b63f/toolchain/py312:$PATH"
cd "/c/Users/31731/Documents/Qoder/2026-10-28/fa21b63f/seekdb" || exit 1
taskkill //F //IM seekdb.exe >/dev/null 2>&1 || true
sleep 3
python build.py build -j12 --http_timeout_scaling=12 --experimental_repository_downloader_retries=8 seekdb 2>&1 \
  | grep -E "ERROR|error:|Build completed|Elapsed|action cache" | head -12

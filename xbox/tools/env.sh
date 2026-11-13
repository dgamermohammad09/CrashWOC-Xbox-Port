# source tools/env.sh  -- toolchain for the NuWorld Xbox port (all local to this folder)
CWX_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export CWX_ROOT
export PATH="$CWX_ROOT/tools/llvm-mingw/bin:$CWX_ROOT/tools/py/bin:$PATH"
export PYTHONPATH="$(cygpath -w "$CWX_ROOT/tools/py")"
export CWX_PY="/c/Users/User/AppData/Local/Programs/Python/Python312/python.exe"
export CWX_PYW="C:/Users/User/AppData/Local/Programs/Python/Python312/python.exe"
